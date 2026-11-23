/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef __CCCI_SCP_RESET_H
#define __CCCI_SCP_RESET_H

#include <linux/io.h>
#include <linux/types.h>

struct ccci_scp_reset {
	u32 set[2];
	u32 raw[2];
	u32 writes;
	u32 reads;
	int error;
	bool attempted;
	bool latched;
};

/*
 * Caller owns both mappings and serializes with observations. Before calling,
 * STOP/synchronize IRQ/work without the owner lock, then reacquire that lock.
 * check validates the held clock/vote/TOPCK. No reset release or mailbox ACK.
 * SW_RSTN bit0 is active low: SET assertion reads 0, CLR release reads 1.
 * Success means reset readback only, not whole-system DMA/message isolation.
 */
int ccci_scp_reset_assert(struct ccci_scp_reset *reset, void __iomem * const cores[2],
			  int (*check)(void *context), void *context);

#endif
