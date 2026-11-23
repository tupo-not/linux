// SPDX-License-Identifier: GPL-2.0-only
/* Asynchronous FS/RPC session; no MD release or implicit userspace endpoint. */
#include <linux/err.h>
#include <linux/ktime.h>
#include <linux/mutex.h>
#include <linux/slab.h>
#include <linux/spinlock.h>
#include <linux/string.h>
#include <linux/printk.h>
#include <linux/unaligned.h>
#include <linux/vmalloc.h>
#include <linux/printk.h>
#include <linux/unaligned.h>
#include <linux/workqueue.h>

#include "ccci-ccif.h"
#include "ccci-service-session.h"

#define SERVICE_SESSION_EVENTS	32
#define SERVICE_SESSION_RETRY_MS	10
#define SERVICE_SESSION_POLL_MS	1000

enum service_event_type {
	SERVICE_EVENT_RECEIVE,
	SERVICE_EVENT_HS2,
	SERVICE_EVENT_REPLY,
};

struct service_event {
	enum service_event_type type;
	enum ccci_service_kind kind;
	u64 generation;
	u64 token;
	u64 now_ms;
	u32 slot;
	u32 queue;
	u32 length;
	u8 packet[CCCI_FS_MESSAGE_MAX];
};

struct ccci_service_session {
	/* No parent/endpoint lifecycle lock is acquired by this worker. */
	struct mutex lock;
	/* Only admission, scalar status and bounded copies; never nests mutex. */
	spinlock_t queue_lock;
	struct workqueue_struct *wq;
	struct delayed_work work;
	struct ccci_service_router router;
	struct ccci_ccif *ccif;
	const struct ccci_service_backend_ops *ops;
	void *context;
	struct service_event queue[SERVICE_SESSION_EVENTS];
	/* Heap resident, avoiding a 16 KiB event on the kernel stack. */
	struct service_event dispatch;
	u8 request[CCCI_SERVICE_SLOTS][CCCI_FS_MESSAGE_MAX];
	u8 reply[CCCI_SERVICE_SLOTS][CCCI_FS_MESSAGE_MAX];
	u8 tx[CCCI_SERVICE_TX_SIZE];
	u64 generation;
	u32 head;
	u32 count;
	u32 tx_packets;
	int tx_error;
	int queue_error;
	bool armed;
	bool accepting;
	bool stopping;
	bool parked;
	bool stopped;
	bool fault_notified;
};

static u64 service_now(void)
{
	return ktime_to_ms(ktime_get_boottime());
}

/* Final gate admits one in-flight callback; stopped close joins the worker. */
static int service_admit(struct ccci_service_session *session)
{
	unsigned long flags;
	int ret;

	spin_lock_irqsave(&session->queue_lock, flags);
	ret = session->queue_error;
	if (!ret && !session->accepting)
		ret = -ESHUTDOWN;
	spin_unlock_irqrestore(&session->queue_lock, flags);
	return ret;
}

static int service_submit(void *context, u64 generation, u64 token,
			  enum ccci_service_kind kind, u32 slot,
			  const void *packet, size_t length)
{
	struct ccci_service_session *session = context;
	int ret;

	ret = service_admit(session);
	if (!ret)
		ret = ccci_service_router_tick(&session->router, generation, service_now());
	if (!ret)
		ret = service_admit(session);
	if (ret)
		return ret;
	return session->ops->submit(session->context, generation, token,
				    kind, slot, packet, length);
}

static int service_send(void *context, const void *packet, size_t length, bool hs2_seen)
{
	struct ccci_service_session *session = context;
	int ret;

	ret = service_admit(session);
	if (!ret)
		ret = ccci_service_router_tick(&session->router, session->generation,
					       service_now());
	if (!ret)
		ret = service_admit(session);
	if (ret)
		return ret;
	ret = ccci_ccif_service_send(session->ccif, packet, length, hs2_seen);
	session->tx_error = ret;
	if (!ret)
		session->tx_packets++;
	return ret;
}

static const struct ccci_service_ops service_router_ops = {
	.submit = service_submit,
	.send = service_send,
};

static void service_report(struct ccci_service_session *session)
{
	unsigned long flags;
	int error = session->router.error;

	if (!error || session->fault_notified)
		return;
	session->fault_notified = true;
	spin_lock_irqsave(&session->queue_lock, flags);
	session->accepting = false;
	if (!session->queue_error)
		session->queue_error = error;
	spin_unlock_irqrestore(&session->queue_lock, flags);
	ccci_ccif_abort(session->ccif, error);
	if (session->ops->fault)
		session->ops->fault(session->context, session->generation, error);
}

/* Caller holds queue_lock. A lost event invalidates the entire generation. */
static int service_queue_fault(struct ccci_service_session *session, int error)
{
	if (!session->queue_error)
		session->queue_error = error < 0 ? error : -EIO;
	session->accepting = false;
	mod_delayed_work(session->wq, &session->work, 0);
	return session->queue_error;
}

static int service_enqueue(struct ccci_service_session *session,
			   enum service_event_type type, u64 generation,
			    u64 token, enum ccci_service_kind kind, u32 slot,
			    u32 queue, const void *packet, size_t length)
{
	struct service_event *event;
	unsigned long flags;
	int ret = 0;

	if (!session)
		return -EINVAL;
	spin_lock_irqsave(&session->queue_lock, flags);
	if (!generation || generation != session->generation) {
		ret = -ESTALE;
	} else if (!session->accepting) {
		ret = session->queue_error ? session->queue_error : -ESHUTDOWN;
	} else if (session->count == SERVICE_SESSION_EVENTS) {
		ret = service_queue_fault(session, -ENOSPC);
	} else {
		event = &session->queue[(session->head + session->count) % SERVICE_SESSION_EVENTS];
		event->type = type;
		event->generation = generation;
		event->token = token;
		event->kind = kind;
		event->slot = slot;
		event->queue = queue;
		event->now_ms = service_now();
		event->length = length;
		if (length)
			memcpy(event->packet, packet, length);
		session->count++;
		mod_delayed_work(session->wq, &session->work, 0);
	}
	spin_unlock_irqrestore(&session->queue_lock, flags);
	return ret;
}

static bool service_take(struct ccci_service_session *session)
{
	unsigned long flags;
	bool taken = false;
	int error;

	spin_lock_irqsave(&session->queue_lock, flags);
	error = session->queue_error;
	if (!error && session->count) {
		session->dispatch = session->queue[session->head];
		session->head = (session->head + 1) % SERVICE_SESSION_EVENTS;
		session->count--;
		taken = true;
	}
	spin_unlock_irqrestore(&session->queue_lock, flags);
	if (error)
		ccci_service_router_abort(&session->router, session->generation, error);
	return taken;
}

static void service_work(struct work_struct *work)
{
	struct ccci_service_session *session =
		container_of(to_delayed_work(work), struct ccci_service_session, work);
	struct service_event *event = &session->dispatch;
	unsigned long flags;
	u32 budget = SERVICE_SESSION_EVENTS;
	unsigned int delay = SERVICE_SESSION_POLL_MS;
	int ret = 0;

	mutex_lock(&session->lock);
	if (!session->armed || session->parked || session->stopping || session->stopped)
		goto unlock;
	ccci_service_router_tick(&session->router, session->generation, service_now());
	while (!session->router.error && budget-- && service_take(session)) {
		switch (event->type) {
		case SERVICE_EVENT_RECEIVE:
			/* Queue latency counts towards the first-fragment deadline. */
			if (service_now() - event->now_ms >= CCCI_SERVICE_TIMEOUT_MS) {
				ret = -ETIMEDOUT;
				break;
			}
			ret = ccci_service_router_receive(&session->router, event->generation,
							  event->queue, event->packet,
						  event->length, event->now_ms);
			break;
		case SERVICE_EVENT_HS2:
			ret = ccci_service_router_hs2(&session->router, event->generation);
			break;
		case SERVICE_EVENT_REPLY:
			ret = ccci_service_router_reply(&session->router, event->generation,
							event->token, event->kind, event->slot,
						 event->packet, event->length);
			break;
		}
		if (!ret && event->type == SERVICE_EVENT_RECEIVE &&
		    event->length >= CCCI_FS_FRAGMENT_PREFIX &&
		    (get_unaligned_le32(event->packet + 8) & 0xffff) == 14)
			pr_info("ccci: fs fragment accepted: queue=%u length=%u data0=0x%x slot=%u op=0x%x body_argc=%u body_arg0=%u\n",
				event->queue, event->length,
				get_unaligned_le32(event->packet),
				get_unaligned_le32(event->packet + 12),
				get_unaligned_le32(event->packet + 16),
				get_unaligned_le32(event->packet + 20),
				get_unaligned_le32(event->packet + 24));
		if (ret) {
			if (event->type == SERVICE_EVENT_RECEIVE &&
			    event->length >= CCCI_FS_FRAGMENT_PREFIX) {
				const u8 *raw = event->packet;
				u32 argc = get_unaligned_le32(raw + 20);
				u32 end = 24, first = 0, i;
				u32 head[2] = { 0, 0 };

				for (i = 0; i < argc && i < 6 && end + 4 <= event->length; i++) {
					u32 size = get_unaligned_le32(raw + end);

					if (!i)
						first = size;
					end += 4 + ((size + 3) & ~3U);
				}
				/* First argument head, so one rejected op is fully identified. */
				if (argc && first >= 4 && event->length >= 32)
					head[0] = get_unaligned_le32(raw + 28);
				if (argc && first >= 8 && event->length >= 36)
					head[1] = get_unaligned_le32(raw + 32);
				pr_err("ccci: service request rejected: queue=%u length=%u data0=0x%x wire_len=%u channel_seq=0x%x slot=%u op=0x%x argc=%u arg0=%u arg0_hex=%08x,%08x strict_end=%u wire_ok=%u chan_ok=%u slot_ok=%u argc_ok=%u op_ok=%u end_ok=%u error=%d\n",
				       event->queue, event->length,
				       get_unaligned_le32(raw), get_unaligned_le32(raw + 4),
				       get_unaligned_le32(raw + 8), get_unaligned_le32(raw + 12),
				       get_unaligned_le32(raw + 16), argc, first,
				       head[0], head[1], end,
				       get_unaligned_le32(raw + 4) == event->length,
				       (get_unaligned_le32(raw + 8) & 0xffff) == 14 ||
					       (get_unaligned_le32(raw + 8) & 0xffff) == 32,
				       get_unaligned_le32(raw + 12) <= 2,
				       argc <= 6,
				       !(get_unaligned_le32(raw + 16) & 0xffff0000U),
				       end == event->length, ret);
			}
			ccci_service_router_abort(&session->router, session->generation, ret);
		}
	}
	ccci_service_router_tick(&session->router, session->generation, service_now());
	if (!session->router.error) {
		ret = ccci_service_router_pump(&session->router, session->generation);
		if (ret == -EBUSY || ret == -ENOSPC)
			delay = SERVICE_SESSION_RETRY_MS;
	}
	service_report(session);
	spin_lock_irqsave(&session->queue_lock, flags);
	if (session->accepting)
		mod_delayed_work(session->wq, &session->work,
				 session->count ? 0 : msecs_to_jiffies(delay));
	spin_unlock_irqrestore(&session->queue_lock, flags);
unlock:
	mutex_unlock(&session->lock);
}

static int service_router_init(struct ccci_service_session *session)
{
	struct ccci_service_storage storage[CCCI_SERVICE_SLOTS];
	u32 i;

	for (i = 0; i < CCCI_SERVICE_SLOTS; i++)
		storage[i] = (struct ccci_service_storage) {
			.request = session->request[i],
			.reply = session->reply[i],
			.capacity = sizeof(session->request[i]),
		};
	return ccci_service_router_init(&session->router, &service_router_ops, session,
					storage, session->tx, sizeof(session->tx));
}

struct ccci_service_session *
ccci_service_session_create(struct device *dev, struct ccci_ccif *ccif,
			    const struct ccci_service_backend_ops *ops,
			    void *context)
{
	struct ccci_service_session *session;
	int ret;

	if (!dev || !ccif || !ops || !ops->submit)
		return ERR_PTR(-EINVAL);
	session = kvzalloc_obj(*session);
	if (!session)
		return ERR_PTR(-ENOMEM);
	mutex_init(&session->lock);
	spin_lock_init(&session->queue_lock);
	INIT_DELAYED_WORK(&session->work, service_work);
	session->wq = alloc_ordered_workqueue("ccci-service", WQ_MEM_RECLAIM);
	if (!session->wq) {
		kvfree(session);
		return ERR_PTR(-ENOMEM);
	}
	session->ccif = ccif;
	session->ops = ops;
	session->context = context;
	ret = service_router_init(session);
	if (ret) {
		destroy_workqueue(session->wq);
		kvfree(session);
		return ERR_PTR(ret);
	}
	return session;
}

int ccci_service_session_arm(struct ccci_service_session *session, u64 generation)
{
	unsigned long flags;
	int ret;

	if (!session)
		return -EINVAL;
	mutex_lock(&session->lock);
	if (session->stopped || session->stopping) {
		ret = -ESHUTDOWN;
		goto unlock;
	}
	if (!session->armed && generation && generation <= session->generation) {
		ret = -ESTALE;
		goto unlock;
	}
	ret = ccci_service_router_begin(&session->router, generation);
	if (ret)
		goto unlock;
	spin_lock_irqsave(&session->queue_lock, flags);
	session->generation = generation;
	session->armed = true;
	session->accepting = true;
	mod_delayed_work(session->wq, &session->work, msecs_to_jiffies(SERVICE_SESSION_POLL_MS));
	spin_unlock_irqrestore(&session->queue_lock, flags);
unlock:
	mutex_unlock(&session->lock);
	return ret;
}

int ccci_service_session_receive(struct ccci_service_session *session,
				 u64 generation, const struct ccci_ccif_event *event)
{
	u32 channel;

	if (!event || event->type != CCCI_CCIF_EVENT_RING || !event->data ||
	    event->size < CCCI_FS_HEADER_SIZE)
		return -EINVAL;
	channel = get_unaligned_le32((const u8 *)event->data + 8) & 0xffff;
	if (channel == CCCI_FS_RX) {
		if (event->size > CCCI_SERVICE_TX_SIZE)
			return -EMSGSIZE;
	} else if (channel == CCCI_RPC_RX) {
		if (event->size > CCCI_RPC_MAX_PACKET)
			return -EMSGSIZE;
	} else {
		return -EOPNOTSUPP;
	}
	return service_enqueue(session, SERVICE_EVENT_RECEIVE, generation, 0,
			       0, 0, event->queue, event->data, event->size);
}

int ccci_service_session_hs2(struct ccci_service_session *session, u64 generation)
{
	return service_enqueue(session, SERVICE_EVENT_HS2, generation, 0, 0, 0, 0, NULL, 0);
}

int ccci_service_session_reply(struct ccci_service_session *session, u64 generation,
			       u64 token, enum ccci_service_kind kind, u32 slot,
			      const void *packet, size_t length)
{
	if ((kind != CCCI_SERVICE_FS && kind != CCCI_SERVICE_RPC) ||
	    !packet || !token || length < 24 ||
	    length > (kind == CCCI_SERVICE_FS ? CCCI_FS_MESSAGE_MAX : CCCI_RPC_MAX_PACKET) ||
	    slot >= (kind == CCCI_SERVICE_FS ? CCCI_FS_SLOTS : CCCI_SERVICE_RPC_SLOTS))
		return -EINVAL;
	return service_enqueue(session, SERVICE_EVENT_REPLY, generation, token,
			       kind, slot, 0, packet, length);
}

int ccci_service_session_abort(struct ccci_service_session *session, u64 generation,
			       int error)
{
	unsigned long flags;
	int ret;

	if (!session)
		return -EINVAL;
	spin_lock_irqsave(&session->queue_lock, flags);
	if (!generation || generation != session->generation)
		ret = -ESTALE;
	else if (!session->accepting)
		ret = session->queue_error ? session->queue_error : -ESHUTDOWN;
	else
		ret = service_queue_fault(session, error);
	spin_unlock_irqrestore(&session->queue_lock, flags);
	return ret;
}

void ccci_service_session_status(struct ccci_service_session *session,
				 struct ccci_service_session_status *status)
{
	unsigned long flags;

	if (!status)
		return;
	memset(status, 0, sizeof(*status));
	if (!session)
		return;
	mutex_lock(&session->lock);
	spin_lock_irqsave(&session->queue_lock, flags);
	status->generation = session->generation;
	status->queued = session->count;
	status->tx_packets = session->tx_packets;
	status->tx_error = session->tx_error;
	status->error = session->router.error ? session->router.error : session->queue_error;
	status->armed = session->armed;
	status->accepting = session->accepting;
	status->stopped = session->stopped;
	status->hs2_seen = session->router.hs2_seen;
	spin_unlock_irqrestore(&session->queue_lock, flags);
	mutex_unlock(&session->lock);
}

int ccci_service_session_offline(struct ccci_service_session *session)
{
	int ret;

	if (!session)
		return 0;
	mutex_lock(&session->lock);
	ret = session->armed && !session->stopped ? -EBUSY : 0;
	mutex_unlock(&session->lock);
	return ret;
}

/* RX joined and parent lifecycle serialized; does not close the backend. */
void ccci_service_session_park(struct ccci_service_session *session)
{
	unsigned long flags;

	/* The worker holds this lock across its last possible TX. */
	mutex_lock(&session->lock);
	session->parked = true;
	spin_lock_irqsave(&session->queue_lock, flags);
	session->accepting = false;
	spin_unlock_irqrestore(&session->queue_lock, flags);
	mutex_unlock(&session->lock);
	cancel_delayed_work_sync(&session->work);
}

int ccci_service_session_stop(struct ccci_service_session *session, bool md_stopped)
{
	unsigned long flags;
	int ret = 0;

	if (!session)
		return -EINVAL;
	if (!md_stopped)
		return -EBUSY;
	mutex_lock(&session->lock);
	if (session->stopped) {
		mutex_unlock(&session->lock);
		return 0;
	}
	spin_lock_irqsave(&session->queue_lock, flags);
	session->accepting = false;
	session->stopping = true;
	spin_unlock_irqrestore(&session->queue_lock, flags);
	mutex_unlock(&session->lock);
	cancel_delayed_work_sync(&session->work);
	if (session->armed && session->ops->closing)
		session->ops->closing(session->context, session->generation);
	mutex_lock(&session->lock);
	if (session->armed)
		ret = ccci_service_router_close(&session->router, session->generation, true);
	memzero_explicit(session->queue, sizeof(session->queue));
	memzero_explicit(&session->dispatch, sizeof(session->dispatch));
	memzero_explicit(session->request, sizeof(session->request));
	memzero_explicit(session->reply, sizeof(session->reply));
	memzero_explicit(session->tx, sizeof(session->tx));
	spin_lock_irqsave(&session->queue_lock, flags);
	session->head = 0;
	session->count = 0;
	session->stopping = false;
	session->stopped = true;
	spin_unlock_irqrestore(&session->queue_lock, flags);
	mutex_unlock(&session->lock);
	return ret;
}

int ccci_service_session_rearm(struct ccci_service_session *session, bool commit)
{
	unsigned long flags;
	int ret;

	mutex_lock(&session->lock);
	spin_lock_irqsave(&session->queue_lock, flags);
	ret = session->stopped && !session->queue_error ? 0 : -EBUSY;
	spin_unlock_irqrestore(&session->queue_lock, flags);
	if (ret || !commit)
		goto out;
	ret = service_router_init(session);
	if (ret)
		goto out;
	spin_lock_irqsave(&session->queue_lock, flags);
	session->stopped = false;
	session->parked = false;
	session->armed = false;
	session->queue_error = 0;
	session->tx_packets = 0;
	session->tx_error = 0;
	session->fault_notified = false;
	spin_unlock_irqrestore(&session->queue_lock, flags);
out:
	mutex_unlock(&session->lock);
	return ret;
}

int ccci_service_session_destroy(struct ccci_service_session *session)
{
	if (!session)
		return 0;
	if (ccci_service_session_offline(session))
		return -EBUSY;
	cancel_delayed_work_sync(&session->work);
	destroy_workqueue(session->wq);
	kvfree_sensitive(session, sizeof(*session));
	return 0;
}
