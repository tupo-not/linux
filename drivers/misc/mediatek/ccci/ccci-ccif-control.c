// SPDX-License-Identifier: GPL-2.0-only
/* Dormant control consumer in the existing CCIF owner; no CPU-release caller. */
#include <linux/device.h>
#include <linux/err.h>
#include <linux/io.h>
#include <linux/ioport.h>
#include <linux/jiffies.h>
#include <linux/ktime.h>
#include <linux/mutex.h>
#include <linux/slab.h>
#include <linux/sysfs.h>
#include <linux/workqueue.h>

#include "ccci-ccif-control.h"
#include "ccci-ccif-tx.h"
#include "ccci-handshake.h"
#include "ccci-smem.h"

/* Same-board AP-written DBM area: two guards around a cleared body. */
#define CCCI_DBM_GUARD	0x44444444
#define CCCI_DBM_BODY	160

struct ccci_ccif_controller {
	struct device *dev;
	void __iomem *ap;
	void __iomem *runtime;
	struct ccci_ccif_rx *rx;
	/* Never acquire the parent MD mutex from a worker or port callback. */
	struct mutex lock;
	struct delayed_work timer;
	struct ccci_handshake hs;
	struct ccci_runtime_plan plan;
	struct ccci_runtime_offer offers[CCCI_RUNTIME_FEATURES];
	ccci_ccif_consume_fn port;
	void *context;
	ccci_ccif_control_fault_fn fault;
	void *fault_context;
	bool fault_notified;
	bool active;
	bool attempted;
	/* DBM guards and body, written before the runtime doorbell. */
	void __iomem *dbm;
	u32 dbm_bytes;
};

static int control_runtime_write(void *context, u32 offset, u32 value)
{
	struct ccci_ccif_controller *control = context;

	if ((offset & 3) || offset >= CCCI_RUNTIME_HALF)
		return -ERANGE;
	writel(value, control->runtime + offset);
	return 0;
}

static int control_read(void *context, u32 offset, u32 *value)
{
	struct ccci_ccif_controller *control = context;

	if (offset != 4 && ((offset & 3) || offset < 0x1f0 || offset >= 0x29c))
		return -ERANGE;
	*value = readl(control->ap + offset);
	return 0;
}

static int control_write(void *context, u32 offset, u32 value)
{
	struct ccci_ccif_controller *control = context;

	if ((offset == 4 && value == BIT(15)) || (offset == 0xc && value == 15) ||
	    (!(offset & 3) && offset >= 0x1f0 && offset < 0x29c)) {
		writel(value, control->ap + offset);
		return 0;
	}
	return -ERANGE;
}

static void control_fence(void *context)
{
	(void)context;
	/* Complete WC runtime stores and SRAM writes before the doorbell. */
	mb();
}

static const struct ccci_ccif_tx_ops control_tx_ops = {
	.runtime_write = control_runtime_write,
	.read = control_read,
	.write = control_write,
	.fence = control_fence,
};

/*
 * The same-board AP writes two guard words around a cleared 160-byte body in
 * SMEM user 0 before the runtime doorbell. The pattern is written whole so a
 * partially initialized area is never visible to the MD.
 */
static void control_dbm_pattern(void __iomem *dbm)
{
	writel(CCCI_DBM_GUARD, dbm);
	writel(CCCI_DBM_GUARD, dbm + 4);
	memset_io(dbm + 8, 0, CCCI_DBM_BODY);
	writel(CCCI_DBM_GUARD, dbm + 8 + CCCI_DBM_BODY);
	writel(CCCI_DBM_GUARD, dbm + 12 + CCCI_DBM_BODY);
}

static void control_dbm_init(struct ccci_ccif_controller *control)
{
	if (!control->dbm || control->dbm_bytes < 16 + CCCI_DBM_BODY)
		return;
	control_dbm_pattern(control->dbm);
}

static int control_publish(void *context, const void *runtime, u32 used, const void *upload)
{
	struct ccci_ccif_controller *control = context;

	control_dbm_init(control);
	return ccci_ccif_runtime_send(&control_tx_ops, context, runtime, used, upload);
}

static int control_port(void *context, const struct ccci_ccif_event *event)
{
	struct ccci_ccif_controller *control = context;

	return control->port(control->context, event);
}

static const struct ccci_handshake_ops control_ops = {
	.publish = control_publish,
	.port = control_port,
};

static void control_report(struct ccci_ccif_controller *control)
{
	if (!control->hs.error || control->fault_notified)
		return;
	control->fault_notified = true;
	if (control->fault)
		control->fault(control->fault_context, control->hs.error);
}

static int control_consume(void *context, const struct ccci_ccif_event *event)
{
	struct ccci_ccif_controller *control = context;
	int ret = 0;

	mutex_lock(&control->lock);
	if (control->active) {
		ret = ccci_handshake_consume(&control->hs, event,
					     ktime_to_ms(ktime_get_boottime()));
		if (ret) {
			dev_err(control->dev,
				"control event failed: type=%u size=%u state=%u error=%d\n",
				event->type, event->size, control->hs.state, ret);
			control->active = false;
		}
		control_report(control);
	}
	mutex_unlock(&control->lock);
	return ret;
}

static void control_timeout(struct work_struct *work)
{
	struct ccci_ccif_controller *control = container_of(to_delayed_work(work),
						       struct ccci_ccif_controller, timer);
	int ret = 0;

	mutex_lock(&control->lock);
	if (control->active) {
		ret = ccci_ccif_rx_error(control->rx);
		if (ret)
			ret = ccci_handshake_abort(&control->hs, ret);
		else
			ret = ccci_handshake_tick(&control->hs, ktime_to_ms(ktime_get_boottime()));
		if (ret)
			control->active = false;
		else
			/* Keep observing transport faults after accepted HS2. */
			mod_delayed_work(system_dfl_wq, &control->timer, msecs_to_jiffies(1000));
		control_report(control);
	}
	mutex_unlock(&control->lock);
	/* No waits under the control lock; the owner retains power until RX STOP. */
	if (ret)
		ccci_ccif_rx_abort(control->rx, ret);
}

int ccci_ccif_control_set_fault(struct ccci_ccif_controller *control,
				ccci_ccif_control_fault_fn fault, void *context)
{
	int ret = 0;

	if (!control)
		return -EINVAL;
	mutex_lock(&control->lock);
	if (control->attempted || control->active) {
		ret = -EBUSY;
	} else {
		control->fault = fault;
		control->fault_context = context;
	}
	mutex_unlock(&control->lock);
	return ret;
}

int ccci_ccif_control_start(struct ccci_ccif_controller *control,
			    const struct ccci_runtime_offer *offers,
			    ccci_ccif_consume_fn port, void *context)
{
	int ret;

	if (!offers || !port)
		return -EINVAL;
	mutex_lock(&control->lock);
	if (ccci_ccif_rx_error(control->rx)) {
		ret = -EIO;
		goto out;
	}
	if (control->attempted || ccci_ccif_rx_started(control->rx)) {
		ret = -EALREADY;
		goto out;
	}
	memcpy(control->offers, offers, sizeof(control->offers));
	ret = ccci_handshake_begin(&control->hs, &control_ops, control, &control->plan,
				   control->offers, ktime_to_ms(ktime_get_boottime()));
	if (ret)
		goto out;
	control->attempted = true;
	control->port = port;
	control->context = context;
	/* Both halves may be cleared only here, with stopped MD and masked IRQs. */
	memset_io(control->runtime, 0, 2 * CCCI_RUNTIME_HALF);
	/* Complete initialization before the RX worker or future CPU release. */
	mb();
	control->active = true;
	ret = ccci_ccif_rx_start(control->rx, control_consume, control);
	if (ret) {
		control->hs.error = ret;
		control->hs.state = CCCI_HANDSHAKE_FAILED;
		control->active = false;
		control_report(control);
		goto out;
	}
	mod_delayed_work(system_dfl_wq, &control->timer, msecs_to_jiffies(1000));
out:
	mutex_unlock(&control->lock);
	return ret;
}

void ccci_ccif_control_quiesce(struct ccci_ccif_controller *control)
{
	mutex_lock(&control->lock);
	control->active = false;
	ccci_handshake_stop(&control->hs);
	mutex_unlock(&control->lock);
	cancel_delayed_work_sync(&control->timer);
}

int ccci_ccif_control_error(struct ccci_ccif_controller *control)
{
	int ret;

	mutex_lock(&control->lock);
	ret = control->hs.error;
	mutex_unlock(&control->lock);
	return ret;
}

int ccci_ccif_control_rearm(struct ccci_ccif_controller *control, bool commit)
{
	int ret = 0;

	mutex_lock(&control->lock);
	if (control->active || control->hs.error) {
		ret = -EBUSY;
	} else if (commit) {
		memset(&control->hs, 0, sizeof(control->hs));
		memset(control->offers, 0, sizeof(control->offers));
		control->port = NULL;
		control->context = NULL;
		control->fault = NULL;
		control->fault_context = NULL;
		control->fault_notified = false;
		control->attempted = false;
	}
	mutex_unlock(&control->lock);
	return ret;
}

ssize_t ccci_ccif_control_status(struct ccci_ccif_controller *control, char *buf)
{
	char query[CCCI_RUNTIME_FEATURES * 2 + 1], negotiated[CCCI_RUNTIME_FEATURES * 2 + 1];
	ssize_t ret;

	mutex_lock(&control->lock);
	ccci_runtime_hex(query, control->hs.query);
	ccci_runtime_hex(negotiated, control->hs.negotiated);
	ret = sysfs_emit(buf,
			 "state=%u error=%d active=%u attempted=%u bytes=%u rejected=%u hs2_seen=%u dbm=%u md_head=0x%x md_word1=0x%x cpu_release_issued=0 query=%s negotiated=%s\n",
			 control->hs.state, control->hs.error, control->active, control->attempted,
			 control->hs.used, control->hs.rejected,
			 control->hs.hs2_seen, control->dbm ? control->dbm_bytes : 0,
			 readl(control->runtime + CCCI_RUNTIME_HALF),
			 readl(control->runtime + CCCI_RUNTIME_HALF + 4),
			 query, negotiated);
	mutex_unlock(&control->lock);
	return ret;
}

struct ccci_ccif_controller *ccci_ccif_control_create(struct device *dev, void __iomem *ap,
						      struct ccci_ccif_rx *rx,
						      const struct ccci_smem_layout *smem)
{
	struct ccci_ccif_controller *control;
	void __iomem *mapping;
	u64 page, end;
	int ret;

	control = devm_kzalloc(dev, sizeof(*control), GFP_KERNEL);
	if (!control)
		return ERR_PTR(-ENOMEM);
	control->dev = dev;
	ret = ccci_runtime_plan(smem, &control->plan);
	if (ret)
		return ERR_PTR(ret);
	/* Reserve/map the enclosing pages once; never access adjacent users. */
	page = control->plan.ap & ~0xfffULL;
	end = (control->plan.ap + 2 * CCCI_RUNTIME_HALF + 0xfff) & ~0xfffULL;
	if (!ccci_lk_contains(&smem->noncached, page, end - page))
		return ERR_PTR(-ERANGE);
	if (!devm_request_mem_region(dev, page, end - page, dev_name(dev)))
		return ERR_PTR(-EBUSY);
	mapping = devm_ioremap_wc(dev, page, end - page);
	if (!mapping)
		return ERR_PTR(-ENOMEM);
	control->runtime = mapping + control->plan.ap - page;
	if (smem->users[CCCI_SMEM_DBM].present) {
		const struct ccci_smem_region *dbm = &smem->users[CCCI_SMEM_DBM];
		u32 bytes = dbm->size;

		/*
		 * The DBM user sits immediately after the runtime pair, so it is
		 * already inside this reservation; never claim a second region.
		 */
		if (!dbm->cached && !dbm->no_map && bytes >= 16 + CCCI_DBM_BODY &&
		    dbm->ap >= page && dbm->ap + bytes <= end &&
		    ccci_lk_contains(&smem->noncached, dbm->ap, bytes)) {
			control->dbm = mapping + (dbm->ap - page);
			control->dbm_bytes = bytes;
		}
	}
	control->ap = ap;
	control->rx = rx;
	mutex_init(&control->lock);
	INIT_DELAYED_WORK(&control->timer, control_timeout);
	return control;
}
