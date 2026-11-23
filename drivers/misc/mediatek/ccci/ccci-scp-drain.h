/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef __CCCI_SCP_DRAIN_H
#define __CCCI_SCP_DRAIN_H

#include "ccci-scp-rx.h"

#define CCCI_SCP_DRAIN_MAX_ROUNDS 32

enum ccci_scp_drain_state {
	CCCI_SCP_DRAIN_QUIESCED,
	CCCI_SCP_DRAIN_PREPARED,
	CCCI_SCP_DRAIN_ACTIVE,
	CCCI_SCP_DRAIN_FAULT,
	CCCI_SCP_DRAIN_STOPPING,
};

struct ccci_scp_drain_ops {
	/*
	 * Idempotently mask/unmask ALL five dedicated mailbox IRQs. A Linux
	 * adapter must balance its own disable depth, not blindly disable again.
	 * Mask is non-synchronizing: never wait for this worker from its lock.
	 * An error leaves the hardware mask state unknown.
	 */
	int (*set_masked)(void *context, bool masked);
	/* Check the shared owner's clock/vote/reset/lifetime prerequisites. */
	int (*check)(void *context, u64 generation);
};

struct ccci_scp_drain {
	struct ccci_scp_rx *rx[CCCI_SCP_MAX_MBOXES];
	const struct ccci_scp_drain_ops *ops;
	void *context;
	u64 generation;
	u64 last_generation;
	u32 budget;
	u32 rounds;
	int error;
	int containment_error;
	enum ccci_scp_drain_state state;
	bool masked;
};

/*
 * Process-context backend only: no IRQ registration, worker or hardware caller.
 * Owner serializes ALL calls and RX/client lifetime without callback reentry.
 * The hard IRQ adapter must mask sources and queue work without taking this
 * serialization lock. This backend is not that adapter and is not IRQ-safe.
 * Initialize once with IRQs masked and synchronized and no queued worker.
 */
int ccci_scp_drain_init(struct ccci_scp_drain *drain,
			const struct ccci_scp_drain_ops *ops, void *context);

/*
 * From QUIESCED only, after the owner has established a new reset generation
 * and rejected inherited messages. All five independently initialized RXs must
 * share the complete immutable layout and have destinations for EVERY RX pin.
 * A new generation is not permission to retry an uncertain ACK without reset.
 * Prepare verifies masking/prerequisites but never unmasks or polls a mailbox.
 */
int ccci_scp_drain_prepare(struct ccci_scp_drain *drain,
			   struct ccci_scp_rx * const rx[CCCI_SCP_MAX_MBOXES],
			   u64 generation, u32 budget);

/*
 * Worker entry with the generation captured WHEN QUEUED, never the current one.
 * Mask all, check prerequisites, run complete five-mailbox sweeps. Only a full
 * empty sweep may unmask. Exhaustion or any error invalidates ALL RX instances
 * and retains masking; no reschedule/retry/clear-all is performed here.
 * Earlier successful batches may already have been delivered before a fault.
 */
int ccci_scp_drain_run(struct ccci_scp_drain *drain, u64 queued_generation);

/*
 * Under owner serialization: reject work, mask and invalidate all RXs. Then
 * DROP the owner lock, synchronize IRQs and cancel/drain work and client events.
 * Only after that quiescence, reacquire the lock and call finish_stop().
 * Reset/generation preparation comes after this barrier. No pin release here.
 */
int ccci_scp_drain_begin_stop(struct ccci_scp_drain *drain);
int ccci_scp_drain_finish_stop(struct ccci_scp_drain *drain);

#endif
