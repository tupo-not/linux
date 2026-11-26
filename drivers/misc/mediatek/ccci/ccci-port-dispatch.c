// SPDX-License-Identifier: GPL-2.0-only
/* Accepted control transitions and normal ports; no CPU-release caller. */
#include <linux/bitops.h>
#include <linux/device.h>
#include <linux/err.h>
#include <linux/mutex.h>
#include <linux/slab.h>
#include <linux/spinlock.h>
#include <linux/string.h>
#include <linux/sysfs.h>
#include <linux/workqueue.h>

#include "ccci-ccif.h"
#include "ccci-md-at.h"
#include "ccci-md-net.h"
#include "ccci-ccism.h"
#include "ccci-ccism-bridge.h"
#include "ccci-port-dispatch.h"
#include "ccci-service-endpoint.h"

enum dispatch_state {
	DISPATCH_IDLE,
	DISPATCH_PREPARED,
	DISPATCH_WAIT_HS1,
	DISPATCH_WAIT_HS2,
	DISPATCH_HS2,
	DISPATCH_STOPPING,
	DISPATCH_STOPPED,
	DISPATCH_FAILED,
};

/* Same-board AT/MUX channels: UART2 is the ttyC0 path used by the RIL. */
#define CCCI_AT_RX	10
#define CCCI_AT_TX	12
#define CCCI_AT_PREVIEW	48
/* System channel: CCISM owns 0x11a/0x11b, everything else is observed only. */
#define CCCI_SYSTEM_RX	2
/* Same-board CCMNI1 data channels. */
#define CCCI_NET_RX	20
#define CCCI_NET_TX	22
/* Highest defined MD channel id in the same-board enum (CCCI_MAX_CH_NUM). */
#define CCCI_PORT_MAX	258

struct ccci_port_dispatch {
	struct device *dev;
	/* Parent lock may nest this mutex, callbacks never take the parent lock. */
	struct mutex lock;
	/* Control fault callbacks only latch and enqueue; no mutex or I/O. */
	spinlock_t fault_lock;
	struct work_struct fault_work;
	struct ccci_ccif *ccif;
	struct ccci_ccism_bridge *bridge;
	struct ccci_service_endpoint *endpoint;
	/* AT/MUX byte pipe owner; NULL when the port was not created. */
	struct ccci_md_at *at;
	/* CCMNI netdev owner; NULL when the port was not created. */
	struct ccci_md_net *net;
	u64 generation;
	enum dispatch_state state;
	int error;
	bool attempted;
	bool fault_closed;
	bool ex_ack_logged;
	/* AT channel observation: counts always, payload only when enabled. */
	u32 at_frames;
	u32 at_bytes;
	u32 at_last_channel;
	u32 at_last_size;
	u32 at_preview_len;
	u8 at_preview[CCCI_AT_PREVIEW];
	bool at_preview_enabled;
	/*
	 * Every other MD port seen after HS2: one bit per channel below 64 plus
	 * a counter for the rest, so the boot order is visible without a reply.
	 */
	u64 observed;
	u32 observed_other;
	u32 last_channel;
	u32 last_size;
};

/*
 * Count every AT frame; keep a bounded printable preview only on request.
 * Nothing is transmitted back: the RIL owns this channel, not this driver.
 */
static void dispatch_at_record(struct ccci_port_dispatch *dispatch,
			       const struct ccci_ccif_event *event)
{
	const u8 *bytes = event->data;
	u32 start = CCCI_CCIF_HEADER_SIZE;
	u32 len = 0;

	dispatch->at_frames++;
	dispatch->at_bytes += event->size;
	dispatch->at_last_channel = event->header.channel;
	dispatch->at_last_size = event->size;
	if (!dispatch->at_preview_enabled || event->size <= start) {
		dispatch->at_preview_len = 0;
		return;
	}
	while (start + len < event->size && len < CCCI_AT_PREVIEW) {
		u8 c = bytes[start + len];

		if (c < 0x20 || c >= 0x7f)
			break;
		len++;
	}
	memcpy(dispatch->at_preview, bytes + start, len);
	dispatch->at_preview_len = len;
}

/* Record one observed MD port event: channel bit plus the last size. */
static void dispatch_observe(struct ccci_port_dispatch *dispatch,
			     const struct ccci_ccif_event *event)
{
	u32 channel = event->header.channel;

	if (channel < 64)
		dispatch->observed |= BIT_ULL(channel);
	else
		dispatch->observed_other++;
	dispatch->last_channel = channel;
	dispatch->last_size = event->size;
}

static void dispatch_fault(void *context, int error)
{
	struct ccci_port_dispatch *dispatch = context;
	unsigned long flags;

	spin_lock_irqsave(&dispatch->fault_lock, flags);
	if (!dispatch->error)
		dispatch->error = error < 0 ? error : -EIO;
	schedule_work(&dispatch->fault_work);
	spin_unlock_irqrestore(&dispatch->fault_lock, flags);
}

static void dispatch_fault_work(struct work_struct *work)
{
	struct ccci_port_dispatch *dispatch =
		container_of(work, struct ccci_port_dispatch, fault_work);

	mutex_lock(&dispatch->lock);
	if (dispatch->state != DISPATCH_STOPPED && !dispatch->fault_closed) {
		dispatch->state = DISPATCH_FAILED;
		dispatch->fault_closed = true;
		ccci_service_endpoint_abort(dispatch->endpoint, dispatch->generation,
					    READ_ONCE(dispatch->error));
		ccci_md_at_abort(dispatch->at, READ_ONCE(dispatch->error));
		ccci_md_net_abort(dispatch->net, READ_ONCE(dispatch->error));
		ccci_ccif_abort(dispatch->ccif, READ_ONCE(dispatch->error));
	}
	mutex_unlock(&dispatch->lock);
}

/* Called only by the real controller after its wire/state validation. */
static int dispatch_port(void *context, const struct ccci_ccif_event *event)
{
	struct ccci_port_dispatch *dispatch = context;
	struct ccci_ccif_header header;
	enum ccci_ccif_control kind = CCCI_CCIF_CONTROL_OTHER;
	int ret;

	memset(&header, 0, sizeof(header));
	header.channel = 0xffff;
	mutex_lock(&dispatch->lock);
	ret = READ_ONCE(dispatch->error);
	if (ret)
		goto out;
	if (dispatch->state < DISPATCH_WAIT_HS1 || dispatch->state > DISPATCH_HS2) {
		ret = -ESHUTDOWN;
		goto out;
	}
	if (!event) {
		ret = -EINVAL;
		goto out;
	}
	if (event->type == CCCI_CCIF_EVENT_NOTIFY) {
		if (event->notifications & ~CCCI_CCIF_NOTIFY_VALID) {
			ret = -EREMOTEIO;
			goto out;
		}
		/* The RX scanner already acknowledged these RCHNUM bits; no payload. */
		if (event->notifications & CCCI_CCIF_NOTIFY_CCB) {
			int ret = ccci_ccif_ccb_wakeup(dispatch->ccif);

			dev_info_ratelimited(dispatch->dev,
					     "CCB wakeup notification: reply=%d\n", ret);
		}
		if (event->notifications & CCCI_CCIF_NOTIFY_MD_EXCEPTION) {
			if (!dispatch->ex_ack_logged) {
				dispatch->ex_ack_logged = true;
				dev_err(dispatch->dev, "MD exception init notified (bit16)\n");
			}
		} else if (event->notifications & CCCI_CCIF_NOTIFY_EX_ACK) {
			dev_info(dispatch->dev, "notification: ex=0x%x\n",
					     event->notifications & CCCI_CCIF_NOTIFY_EX_ACK);
		}
		goto out;
	}
	ret = ccci_ccif_header_decode(event->data, event->size, &header);
	if (ret)
		goto out;
	if (event->type == CCCI_CCIF_EVENT_SRAM) {
		kind = ccci_ccif_control_decode(&header);
		if (dispatch->state != DISPATCH_WAIT_HS1 ||
		    event->size != CCCI_CCIF_HS1_SIZE ||
		    kind != CCCI_CCIF_CONTROL_HS1) {
			ret = -EPROTO;
			goto out;
		}
		/* Publication is complete before the controller forwards this event. */
		ret = ccci_service_endpoint_arm(dispatch->endpoint, dispatch->generation);
		if (!ret)
			dispatch->state = DISPATCH_WAIT_HS2;
		if (!ret)
			dev_info(dispatch->dev, "HS1 accepted: generation=%llu\n",
				 dispatch->generation);
		goto out;
	}
	if (event->type != CCCI_CCIF_EVENT_RING) {
		ret = -EPROTO;
		goto out;
	}
	if (!header.channel) {
		kind = ccci_ccif_control_decode(&header);
		if (dispatch->state != DISPATCH_WAIT_HS2 || event->queue ||
		    event->size != CCCI_CCIF_HEADER_SIZE ||
		    kind != CCCI_CCIF_CONTROL_HS2) {
			ret = -EPROTO;
			goto out;
		}
		ret = ccci_service_endpoint_hs2(dispatch->endpoint, dispatch->generation);
		if (!ret)
			ret = ccci_md_at_hs2(dispatch->at);
		if (!ret)
			ret = ccci_md_net_hs2(dispatch->net);
		if (!ret)
			ret = ccci_ccism_bridge_port(dispatch->bridge, event);
		if (!ret)
			dispatch->state = DISPATCH_HS2;
		if (!ret)
			dev_info(dispatch->dev, "HS2 accepted: generation=%llu\n",
				 dispatch->generation);
	} else if (header.channel == CCCI_FS_RX || header.channel == CCCI_RPC_RX) {
		ret = dispatch->state == DISPATCH_WAIT_HS1 ? -EPROTO :
			ccci_service_endpoint_receive(dispatch->endpoint,
						      dispatch->generation, event);
	} else if (header.channel == CCCI_SYSTEM_RX && dispatch->state == DISPATCH_HS2) {
		ret = ccci_ccism_bridge_port(dispatch->bridge, event);
		if (ret == -EOPNOTSUPP) {
			/* Not CCISM: observe it, keep the generation, answer nothing. */
			dispatch_observe(dispatch, event);
			ret = 0;
		}
	} else if ((header.channel == CCCI_AT_RX || header.channel == CCCI_AT_TX) &&
		   dispatch->state == DISPATCH_HS2) {
		/*
		 * The MD opens the AT/MUX channel right after HS2. Record it and
		 * hand the payload to the byte pipe; nothing is answered here.
		 */
		dispatch_at_record(dispatch, event);
		dispatch_observe(dispatch, event);
		ret = dispatch->at ? ccci_md_at_receive(dispatch->at, event) : 0;
	} else if ((header.channel == CCCI_NET_RX || header.channel == CCCI_NET_TX) &&
		   dispatch->state == DISPATCH_HS2) {
		/* Data frames go to the netdev; nothing is answered on this path. */
		dispatch_observe(dispatch, event);
		ret = dispatch->net ? ccci_md_net_receive(dispatch->net, event) : 0;
	} else if (dispatch->state == DISPATCH_HS2 && header.channel < CCCI_PORT_MAX) {
		/*
		 * Any other defined MD port opened after HS2. Observe the channel
		 * and size, then keep the generation: no reply is invented for a
		 * port this driver does not own, and the MD is not killed for
		 * opening it. Ids outside the same-board enum still fail closed.
		 */
		dispatch_observe(dispatch, event);
	} else {
		/* No silent consumption or fake reply for an unimplemented port. */
		ret = -EOPNOTSUPP;
	}
out:
	if (ret) {
		dev_err(dispatch->dev,
			"port event rejected: type=%u size=%u queue=%u channel=%u kind=%d state=%u error=%d\n",
			event->type, event->size, event->queue, header.channel, kind,
			dispatch->state, ret);
		dispatch_fault(dispatch, ret);
	}
	mutex_unlock(&dispatch->lock);
	return ret;
}

int ccci_port_dispatch_prepare(struct ccci_port_dispatch *dispatch, u64 generation)
{
	int ret;

	if (!dispatch || !generation)
		return -EINVAL;
	mutex_lock(&dispatch->lock);
	if (dispatch->attempted) {
		ret = -EALREADY;
		goto out;
	}
	if (generation <= dispatch->generation) {
		ret = -ESTALE;
		goto out;
	}
	dispatch->attempted = true;
	dispatch->generation = generation;
	ret = ccci_service_endpoint_reserve(dispatch->endpoint, generation);
	if (!ret)
		ret = ccci_ccism_bridge_arm(dispatch->bridge);
	if (!ret)
		dispatch->state = DISPATCH_PREPARED;
	else
		dispatch_fault(dispatch, ret);
out:
	mutex_unlock(&dispatch->lock);
	return ret;
}

int ccci_port_dispatch_start(struct ccci_port_dispatch *dispatch,
			     const struct ccci_runtime_offer *offers)
{
	int ret;

	mutex_lock(&dispatch->lock);
	ret = READ_ONCE(dispatch->error);
	if (!ret && dispatch->state != DISPATCH_PREPARED)
		ret = -EAGAIN;
	if (!ret)
		dispatch->state = DISPATCH_WAIT_HS1;
	mutex_unlock(&dispatch->lock);
	if (ret)
		return ret;
	/* Never nest dispatch->lock outside the controller's lock. */
	ret = ccci_ccif_set_fault(dispatch->ccif, dispatch_fault, dispatch);
	if (!ret)
		ret = ccci_ccif_handshake_start(dispatch->ccif, offers, dispatch_port, dispatch);
	if (ret)
		dispatch_fault(dispatch, ret);
	return ret;
}

int ccci_port_dispatch_stop(struct ccci_port_dispatch *dispatch, bool md_stopped)
{
	int ret;

	if (!md_stopped)
		return -EBUSY;
	/* Parent serializes lifecycle; no dispatch mutex while joining callbacks. */
	ret = ccci_ccif_stop(dispatch->ccif);
	if (ret)
		return ret;
	cancel_work_sync(&dispatch->fault_work);
	mutex_lock(&dispatch->lock);
	dispatch->state = DISPATCH_STOPPING;
	mutex_unlock(&dispatch->lock);
	ret = ccci_service_endpoint_stop(dispatch->endpoint, true);
	if (!ret)
		ret = ccci_md_at_stop(dispatch->at, true);
	if (!ret)
		ret = ccci_md_net_stop(dispatch->net, true);
	if (!ret)
		ret = ccci_ccism_bridge_stop(dispatch->bridge, true);
	if (!ret)
		ret = ccci_ccism_bridge_offline(dispatch->bridge);
	mutex_lock(&dispatch->lock);
	if (!ret)
		dispatch->state = DISPATCH_STOPPED;
	mutex_unlock(&dispatch->lock);
	return ret;
}

/* CCIF callbacks and fault work are already joined by STOP. */
int ccci_port_dispatch_rearm(struct ccci_port_dispatch *dispatch, bool commit)
{
	int ret;

	mutex_lock(&dispatch->lock);
	if (dispatch->state != DISPATCH_STOPPED || !dispatch->attempted ||
	    READ_ONCE(dispatch->error)) {
		ret = -EBUSY;
		goto out;
	}

	/* Reinitialize the service router first; reopen the AT file last. */
	ret = ccci_service_endpoint_rearm(dispatch->endpoint, commit);
	if (!ret)
		ret = ccci_ccism_bridge_rearm(dispatch->bridge, commit);
	if (!ret)
		ret = ccci_md_net_rearm(dispatch->net, commit);
	if (!ret)
		ret = ccci_md_at_rearm(dispatch->at, commit);
	if (ret || !commit)
		goto out;

	dispatch->state = DISPATCH_IDLE;
	dispatch->attempted = false;
	dispatch->fault_closed = false;
	dispatch->ex_ack_logged = false;
	dispatch->at_frames = 0;
	dispatch->at_bytes = 0;
	dispatch->at_last_channel = 0;
	dispatch->at_last_size = 0;
	dispatch->at_preview_len = 0;
	dispatch->at_preview_enabled = false;
	memzero_explicit(dispatch->at_preview, sizeof(dispatch->at_preview));
	dispatch->observed = 0;
	dispatch->observed_other = 0;
	dispatch->last_channel = 0;
	dispatch->last_size = 0;
out:
	mutex_unlock(&dispatch->lock);
	return ret;
}

int ccci_port_dispatch_offline(struct ccci_port_dispatch *dispatch)
{
	int ret;

	if (!dispatch)
		return 0;
	mutex_lock(&dispatch->lock);
	ret = dispatch->attempted && dispatch->state != DISPATCH_STOPPED ? -EBUSY : 0;
	mutex_unlock(&dispatch->lock);
	if (!ret) {
		ccci_md_at_offline(dispatch->at);
		ccci_md_net_offline(dispatch->net);
	}
	return ret;
}

/* Before start only: the netdev owner is immutable through a generation. */
int ccci_port_dispatch_set_net(struct ccci_port_dispatch *dispatch, struct ccci_md_net *net)
{
	int ret = 0;

	if (!dispatch)
		return -EINVAL;
	mutex_lock(&dispatch->lock);
	if (dispatch->attempted)
		ret = -EBUSY;
	else
		dispatch->net = net;
	mutex_unlock(&dispatch->lock);
	return ret;
}

/* Before start only: the AT pipe owner is immutable through a generation. */
int ccci_port_dispatch_set_at(struct ccci_port_dispatch *dispatch, struct ccci_md_at *at)
{
	int ret = 0;

	if (!dispatch)
		return -EINVAL;
	mutex_lock(&dispatch->lock);
	if (dispatch->attempted)
		ret = -EBUSY;
	else
		dispatch->at = at;
	mutex_unlock(&dispatch->lock);
	return ret;
}

ssize_t ccci_port_dispatch_status(struct ccci_port_dispatch *dispatch, char *buf)
{
	ssize_t ret;

	if (!dispatch)
		return -EOPNOTSUPP;
	mutex_lock(&dispatch->lock);
	ret = sysfs_emit(buf,
			 "state=%u generation=%llu error=%d attempted=%u at_frames=%u at_bytes=%u at_last_channel=%u at_last_size=%u observed=0x%llx observed_other=%u last_channel=%u last_size=%u cpu_release_issued=0 service_ready=0\n",
			 dispatch->state, dispatch->generation, READ_ONCE(dispatch->error),
			 dispatch->attempted, dispatch->at_frames, dispatch->at_bytes,
			 dispatch->at_last_channel, dispatch->at_last_size,
			 dispatch->observed, dispatch->observed_other,
			 dispatch->last_channel, dispatch->last_size);
	mutex_unlock(&dispatch->lock);
	return ret;
}

ssize_t ccci_port_dispatch_at(struct ccci_port_dispatch *dispatch, char *buf)
{
	ssize_t ret;

	if (!dispatch)
		return -EOPNOTSUPP;
	mutex_lock(&dispatch->lock);
	ret = sysfs_emit(buf,
			 "preview=%u frames=%u bytes=%u last_channel=%u last_size=%u text=%.*s\n",
			 dispatch->at_preview_enabled, dispatch->at_frames, dispatch->at_bytes,
			 dispatch->at_last_channel, dispatch->at_last_size,
			 dispatch->at_preview_len, dispatch->at_preview);
	mutex_unlock(&dispatch->lock);
	return ret;
}

int ccci_port_dispatch_at_preview(struct ccci_port_dispatch *dispatch, bool enable)
{
	if (!dispatch)
		return -EINVAL;
	mutex_lock(&dispatch->lock);
	dispatch->at_preview_enabled = enable;
	if (!enable)
		dispatch->at_preview_len = 0;
	mutex_unlock(&dispatch->lock);
	return 0;
}

static void dispatch_cleanup(void *context)
{
	struct ccci_port_dispatch *dispatch = context;

	/* Built-in, dormant unwind only; active unbind is not supported. */
	if (WARN_ON(ccci_port_dispatch_offline(dispatch)))
		return;
	cancel_work_sync(&dispatch->fault_work);
	kfree(dispatch);
}

struct ccci_port_dispatch *
ccci_port_dispatch_create(struct device *dev, struct ccci_ccif *ccif,
			  struct ccci_ccism_bridge *bridge,
			  struct ccci_service_endpoint *endpoint)
{
	struct ccci_port_dispatch *dispatch;
	int ret;

	if (!dev || !ccif || !bridge || !endpoint)
		return ERR_PTR(-EINVAL);
	dispatch = kzalloc_obj(*dispatch);
	if (!dispatch)
		return ERR_PTR(-ENOMEM);
	mutex_init(&dispatch->lock);
	spin_lock_init(&dispatch->fault_lock);
	INIT_WORK(&dispatch->fault_work, dispatch_fault_work);
	dispatch->dev = dev;
	dispatch->ccif = ccif;
	dispatch->bridge = bridge;
	dispatch->endpoint = endpoint;
	ret = devm_add_action_or_reset(dev, dispatch_cleanup, dispatch);
	if (ret)
		return ERR_PTR(ret);
	return dispatch;
}
