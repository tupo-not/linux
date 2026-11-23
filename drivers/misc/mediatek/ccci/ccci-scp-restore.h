/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef __CCCI_SCP_RESTORE_H
#define __CCCI_SCP_RESTORE_H

#include "ccci-scp-memory.h"

/* Geometry only: this object neither authorizes nor performs any operation. */
struct ccci_scp_restore_plan {
	struct ccci_scp_span loader_source;
	struct ccci_scp_span firmware;
	struct ccci_scp_span tcm_clear;
	struct ccci_scp_span loader_target;
	struct ccci_scp_span header_target;
	struct ccci_scp_span dram_mapping;
	struct ccci_scp_span dram_clear;
	struct ccci_scp_span dram_source;
	struct ccci_scp_span dram_target;
};

/*
 * Plan the same-board, non-secure, two-core vendor recovery layout only.
 * Snapshot is an exact 68/72-byte CPU copy; windows must come independently
 * from this boot's trusted reservations/resources. All inputs and output must
 * be disjoint. Failure clears output. No pointers to source data are retained.
 *
 * Success proves bounds only, NOT valid loader/firmware/backups, coherency,
 * ownership, halted CPUs/DMA, reset completion or mailbox generation isolation.
 * There is no mapping, copying, reset action, IRQ operation or device I/O.
 */
int ccci_scp_plan_restore(const void *snapshot, size_t length, u32 cores,
			  const struct ccci_scp_span *firmware_window,
			  const struct ccci_scp_span *share_window,
			  const struct ccci_scp_span *tcm_window,
			  struct ccci_scp_restore_plan *plan);

#endif
