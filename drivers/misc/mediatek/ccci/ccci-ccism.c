// SPDX-License-Identifier: GPL-2.0-only
/* Same-board 6.6 CCISM exchange; runs after HS2, never a CPU-release gate. */
#include <linux/errno.h>
#include <linux/limits.h>
#include <linux/string.h>

#include "ccci-ccism.h"
#include "ccci-smem.h"

int ccci_ccism_plan(const struct ccci_smem_layout *smem, struct ccci_ccism_plan *plan)
{
	const struct ccci_smem_region *region;
	u32 address;
	int ret;

	if (!plan)
		return -EINVAL;
	memset(plan, 0, sizeof(*plan));
	if (!smem)
		return -EINVAL;
	region = &smem->users[CCCI_SMEM_CCISM_SCP];
	if (!region->present || region->cached || region->no_map || region->size < 32768 ||
	    !ccci_lk_contains(&smem->noncached, region->ap, region->size) ||
	    (u64)region->md != 0x40000000ULL + (smem->noncached.base & 0x1ffffff) +
		region->ap - smem->noncached.base ||
	    (u64)region->md + region->size > 0x100000000ULL)
		return -ERANGE;
	ret = ccci_scp_shm_u32(region->ap, region->size, &address);
	if (ret)
		return ret;
	*plan = (struct ccci_ccism_plan) {
		.ap = address, .md = region->md, .size = region->size,
	};
	return 0;
}

int ccci_ccism_fault(struct ccci_ccism *ccism, int error)
{
	if (!ccism->error)
		ccism->error = error < 0 ? error : -EPROTO;
	if (ccism->state == CCCI_CCISM_WAIT_STOP && !ccism->cleanup_error)
		ccism->cleanup_error = error < 0 ? error : -EPROTO;
	ccism->state = CCCI_CCISM_FAILED;
	return ccism->error;
}

static int ccism_check(struct ccci_ccism *ccism, u64 now_ms)
{
	int ret;

	if (now_ms < ccism->last_ms)
		return ccci_ccism_fault(ccism, -ERANGE);
	ccism->last_ms = now_ms;
	ret = ccism->ops->check(ccism->context, ccism->generation);
	if (ret)
		return ccci_ccism_fault(ccism, ret);
	return 0;
}

static int ccism_wait(struct ccci_ccism *ccism, enum ccci_ccism_state state,
		      u64 now_ms, u32 timeout)
{
	if (now_ms > U64_MAX - timeout)
		return ccci_ccism_fault(ccism, -ERANGE);
	ccism->state = state;
	ccism->deadline_ms = now_ms + timeout;
	return 0;
}

int ccci_ccism_arm(struct ccci_ccism *ccism, const struct ccci_ccism_ops *ops,
		   void *context, const struct ccci_ccism_plan *plan, u64 generation, u64 now_ms)
{
	u32 address;
	int ret;

	if (!ccism || !ops || !ops->check || !ops->bridge_on || !ops->md_send ||
	    !ops->clear || !ops->scp_send || !ops->bridge_off || !plan || !generation)
		return -EINVAL;
	if (ccism->state != CCCI_CCISM_IDLE)
		return -EALREADY;
	ret = ccci_scp_shm_u32(plan->ap, plan->size, &address);
	if (ret || plan->size < 32768 || !plan->md || (plan->md & 3) ||
	    (u64)plan->md + plan->size > 0x100000000ULL)
		return ret ? ret : -ERANGE;
	ret = ops->check(context, generation);
	if (ret)
		return ret;
	ccism->ops = ops;
	ccism->context = context;
	ccism->plan = *plan;
	ccism->generation = generation;
	ccism->last_ms = now_ms;
	ccism->state = CCCI_CCISM_ARMED;
	return 0;
}

int ccci_ccism_tick(struct ccci_ccism *ccism, u64 now_ms)
{
	int ret;

	if (ccism->state == CCCI_CCISM_FAILED)
		return ccism->error;
	if (ccism->state == CCCI_CCISM_IDLE || ccism->state == CCCI_CCISM_STOPPED)
		return 0;
	ret = ccism_check(ccism, now_ms);
	if (ret)
		return ret;
	if (ccism->state != CCCI_CCISM_ARMED && ccism->state != CCCI_CCISM_RUNNING &&
	    now_ms >= ccism->deadline_ms)
		return ccci_ccism_fault(ccism, -ETIMEDOUT);
	return 0;
}

int ccci_ccism_md_ready(struct ccci_ccism *ccism, u64 now_ms)
{
	int ret;

	if (ccism->state == CCCI_CCISM_IDLE || ccism->state == CCCI_CCISM_STOPPED)
		return -ESHUTDOWN;
	ret = ccci_ccism_tick(ccism, now_ms);
	if (ret)
		return ret;
	if (ccism->state != CCCI_CCISM_ARMED)
		return ccci_ccism_fault(ccism, -EPROTO);
	ret = ccism_wait(ccism, CCCI_CCISM_WAIT_MD_ACK, now_ms, CCCI_CCISM_TIMEOUT_MS);
	if (ret)
		return ret;
	/* A failed callback may already have enabled clocks or issued SMC13. */
	ccism->bridge_touched = true;
	ret = ccism->ops->bridge_on(ccism->context);
	if (!ret)
		ret = ccism->ops->md_send(ccism->context, CCCI_CCISM_INIT);
	return ret ? ccci_ccism_fault(ccism, ret) : 0;
}

int ccci_ccism_md_event(struct ccci_ccism *ccism, const struct ccci_ccif_event *event, u64 now_ms)
{
	struct ccci_ccif_header header;
	int ret;

	if (!event)
		return -EINVAL;
	if (ccism->state == CCCI_CCISM_IDLE || ccism->state == CCCI_CCISM_STOPPED)
		return -ESHUTDOWN;
	ret = ccci_ccif_header_decode(event->data, event->size, &header);
	if (ret)
		return ccci_ccism_fault(ccism, ret);
	if (header.data[1] != CCCI_CCISM_ACK && header.data[1] != CCCI_CCISM_DONE)
		return -EOPNOTSUPP;
	ret = ccci_ccism_tick(ccism, now_ms);
	if (ret)
		return ret;
	if (event->type != CCCI_CCIF_EVENT_RING || event->queue || header.channel != 2 ||
	    event->size != CCCI_CCIF_HEADER_SIZE)
		return ccci_ccism_fault(ccism, -EPROTO);
	if (header.data[1] == CCCI_CCISM_ACK) {
		if (ccism->state != CCCI_CCISM_WAIT_MD_ACK)
			return ccci_ccism_fault(ccism, -EPROTO);
		ret = ccism_wait(ccism, CCCI_CCISM_WAIT_RBREADY, now_ms, CCCI_CCISM_TIMEOUT_MS);
		if (!ret)
			ret = ccism->ops->clear(ccism->context, ccism->plan.ap, ccism->plan.size);
		/* The IPI uses the AP physical address, not the MD bank4 address. */
		if (!ret)
			ret = ccism->ops->scp_send(ccism->context, CCCI_SCP_OP_SHM_INIT,
						 ccism->plan.ap);
	} else {
		if (ccism->state != CCCI_CCISM_WAIT_MD_DONE)
			return ccci_ccism_fault(ccism, -EPROTO);
		ccism->md_done_seen = true;
		ret = ccism->ops->scp_send(ccism->context, CCCI_SCP_OP_MD_STATE, 2);
		if (!ret)
			ccism->state = CCCI_CCISM_RUNNING;
	}
	return ret ? ccci_ccism_fault(ccism, ret) : 0;
}

int ccci_ccism_scp_state(struct ccci_ccism *ccism, u64 generation, u32 state, u64 now_ms)
{
	int ret;

	if (ccism->state == CCCI_CCISM_IDLE || ccism->state == CCCI_CCISM_STOPPED)
		return -ESHUTDOWN;
	if (!generation || generation != ccism->generation)
		return ccci_ccism_fault(ccism, -ESTALE);
	ret = ccci_ccism_tick(ccism, now_ms);
	if (ret)
		return ret;
	if (state == CCCI_SCP_STATE_BOOTING && ccism->state == CCCI_CCISM_ARMED)
		return 0;
	if (state == CCCI_SCP_STATE_STOP && ccism->state == CCCI_CCISM_WAIT_STOP) {
		ccism->stop_seen = true;
		ret = ccism->ops->bridge_off(ccism->context);
		if (!ret) {
			ccism->bridge_touched = false;
			ccism->state = CCCI_CCISM_STOPPED;
		}
	} else if (state == CCCI_SCP_STATE_RBREADY && ccism->state == CCCI_CCISM_WAIT_RBREADY) {
		ccism->rbready_seen = true;
		ret = ccism_wait(ccism, CCCI_CCISM_WAIT_MD_DONE, now_ms, CCCI_CCISM_TIMEOUT_MS);
		if (!ret)
			ret = ccism->ops->md_send(ccism->context, CCCI_CCISM_DONE);
	} else {
		ret = state == CCCI_SCP_STATE_BOOTING ? -ECONNRESET : -EPROTO;
	}
	return ret ? ccci_ccism_fault(ccism, ret) : 0;
}

int ccci_ccism_stop(struct ccci_ccism *ccism, bool md_stopped, u64 now_ms)
{
	int ret;

	if (!md_stopped)
		return -EBUSY;
	if (ccism->state == CCCI_CCISM_STOPPED)
		return 0;
	if (ccism->stop_attempted)
		return -EALREADY;
	if (ccism->state == CCCI_CCISM_IDLE)
		return -ESHUTDOWN;
	if (!ccism->bridge_touched) {
		ccism->state = CCCI_CCISM_STOPPED;
		return 0;
	}
	ccism->stop_attempted = true;
	ret = ccism_check(ccism, now_ms);
	if (ret) {
		ccism->cleanup_error = ret;
		return ret;
	}
	ret = ccism_wait(ccism, CCCI_CCISM_WAIT_STOP, now_ms, CCCI_CCISM_STOP_MS);
	if (!ret)
		ret = ccism->ops->scp_send(ccism->context, CCCI_SCP_OP_MD_STATE, 0);
	if (ret && !ccism->cleanup_error)
		ccism->cleanup_error = ret;
	return ret ? ccci_ccism_fault(ccism, ret) : 0;
}
