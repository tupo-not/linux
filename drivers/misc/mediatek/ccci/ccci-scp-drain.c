// SPDX-License-Identifier: GPL-2.0-only
/* Process-context drain/lifetime backend; no IRQ or reset hardware caller. */
#include <linux/errno.h>
#include <linux/string.h>

#include "ccci-scp-drain.h"

static void ccci_scp_drain_invalidate(struct ccci_scp_drain *drain)
{
	u32 i;

	for (i = 0; i < CCCI_SCP_MAX_MBOXES; i++)
		ccci_scp_rx_invalidate(drain->rx[i]);
}

static int ccci_scp_drain_mask(struct ccci_scp_drain *drain, bool masked)
{
	int ret;

	/* In particular, do not claim containment after a failed unmask. */
	drain->masked = false;
	ret = drain->ops->set_masked(drain->context, masked);
	if (!ret)
		drain->masked = masked;
	return ret;
}

static int ccci_scp_drain_fault(struct ccci_scp_drain *drain, int error)
{
	drain->error = error;
	drain->state = CCCI_SCP_DRAIN_FAULT;
	ccci_scp_drain_invalidate(drain);
	return error;
}

int ccci_scp_drain_init(struct ccci_scp_drain *drain,
			const struct ccci_scp_drain_ops *ops, void *context)
{
	if (!drain)
		return -EINVAL;
	memset(drain, 0, sizeof(*drain));
	drain->state = CCCI_SCP_DRAIN_FAULT;
	if (!ops || !ops->set_masked || !ops->check)
		return -EINVAL;
	drain->ops = ops;
	drain->context = context;
	drain->state = CCCI_SCP_DRAIN_QUIESCED;
	return 0;
}

static int ccci_scp_drain_receivers(struct ccci_scp_rx * const rx[], u64 generation)
{
	u32 i;

	for (i = 0; i < CCCI_SCP_MAX_MBOXES; i++) {
		if (!rx[i] || rx[i]->fault || !rx[i]->layout || !rx[i]->ops ||
		    !rx[i]->ops->pending || !rx[i]->ops->read_words ||
		    !rx[i]->ops->post_capture || !rx[i]->ops->ack || !rx[i]->ops->deliver ||
		    rx[i]->mbox != i || rx[i]->generation != generation ||
		    !rx[i]->known_mask || rx[i]->supported_mask != rx[i]->known_mask ||
		    rx[i]->layout != rx[0]->layout)
			return -EINVAL;
	}
	return 0;
}

int ccci_scp_drain_prepare(struct ccci_scp_drain *drain,
			   struct ccci_scp_rx * const rx[CCCI_SCP_MAX_MBOXES],
			   u64 generation, u32 budget)
{
	u32 i;
	int ret;

	if (!drain || !drain->ops || !rx || !generation || !budget ||
	    budget > CCCI_SCP_DRAIN_MAX_ROUNDS)
		return -EINVAL;
	if (drain->state != CCCI_SCP_DRAIN_QUIESCED)
		return -EBUSY;
	if (generation <= drain->last_generation)
		return -ESTALE;
	ret = ccci_scp_drain_receivers(rx, generation);
	if (ret)
		return ret;
	for (i = 0; i < CCCI_SCP_MAX_MBOXES; i++)
		drain->rx[i] = rx[i];
	drain->generation = generation;
	/* A failed operational attempt still consumes this generation. */
	drain->last_generation = generation;
	drain->budget = budget;
	drain->rounds = 0;
	drain->error = 0;
	drain->containment_error = 0;
	ret = ccci_scp_drain_mask(drain, true);
	if (ret) {
		drain->containment_error = ret;
		return ccci_scp_drain_fault(drain, ret);
	}
	ret = drain->ops->check(drain->context, generation);
	if (ret)
		return ccci_scp_drain_fault(drain, ret);
	drain->state = CCCI_SCP_DRAIN_PREPARED;
	return 0;
}

int ccci_scp_drain_run(struct ccci_scp_drain *drain, u64 queued_generation)
{
	struct ccci_scp_rx_result result;
	bool busy;
	u32 i;
	int ret;

	if (!drain || !drain->ops)
		return -EINVAL;
	if (!queued_generation || queued_generation != drain->generation)
		return -ESTALE;
	if (drain->state != CCCI_SCP_DRAIN_PREPARED &&
	    drain->state != CCCI_SCP_DRAIN_ACTIVE)
		return -ESHUTDOWN;
	drain->rounds = 0;
	ret = ccci_scp_drain_mask(drain, true);
	if (ret) {
		drain->containment_error = ret;
		return ccci_scp_drain_fault(drain, ret);
	}
	/* A bounded call holds all sources masked until a complete quiet sweep. */
	while (drain->rounds < drain->budget) {
		ret = drain->ops->check(drain->context, drain->generation);
		if (ret)
			return ccci_scp_drain_fault(drain, ret);
		ret = ccci_scp_drain_receivers(drain->rx, drain->generation);
		if (ret)
			return ccci_scp_drain_fault(drain, ret);
		busy = false;
		drain->rounds++;
		for (i = 0; i < CCCI_SCP_MAX_MBOXES; i++) {
			ret = ccci_scp_rx_step(drain->rx[i], &result);
			if (ret)
				return ccci_scp_drain_fault(drain, ret);
			busy |= !!result.pending;
		}
		if (busy)
			continue;
		ret = drain->ops->check(drain->context, drain->generation);
		if (ret)
			return ccci_scp_drain_fault(drain, ret);
		ret = ccci_scp_drain_mask(drain, false);
		if (ret) {
			/* One containment attempt; never poll or retry a message. */
			drain->containment_error = ccci_scp_drain_mask(drain, true);
			return ccci_scp_drain_fault(drain, ret);
		}
		drain->state = CCCI_SCP_DRAIN_ACTIVE;
		return 0;
	}
	return ccci_scp_drain_fault(drain, -EOVERFLOW);
}

int ccci_scp_drain_begin_stop(struct ccci_scp_drain *drain)
{
	int ret;

	if (!drain || !drain->ops)
		return -EINVAL;
	if (drain->state == CCCI_SCP_DRAIN_STOPPING)
		return 0;
	drain->state = CCCI_SCP_DRAIN_STOPPING;
	ret = ccci_scp_drain_mask(drain, true);
	ccci_scp_drain_invalidate(drain);
	if (ret) {
		drain->containment_error = ret;
		return ccci_scp_drain_fault(drain, ret);
	}
	return 0;
}

int ccci_scp_drain_finish_stop(struct ccci_scp_drain *drain)
{
	if (!drain || !drain->ops)
		return -EINVAL;
	if (drain->state != CCCI_SCP_DRAIN_STOPPING || !drain->masked)
		return -EBUSY;
	/* Caller has synchronized IRQs and drained all worker/client references. */
	memset(drain->rx, 0, sizeof(drain->rx));
	drain->state = CCCI_SCP_DRAIN_QUIESCED;
	return 0;
}
