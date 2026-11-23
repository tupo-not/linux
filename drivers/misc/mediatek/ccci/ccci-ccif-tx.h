/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef __CCCI_CCIF_TX_H
#define __CCCI_CCIF_TX_H

#include <linux/types.h>

struct ccci_ccif_tx_ops {
	int (*runtime_write)(void *context, u32 offset, u32 value);
	int (*read)(void *context, u32 offset, u32 *value);
	int (*write)(void *context, u32 offset, u32 value);
	void (*fence)(void *context);
};

/* Single owner, powered clocks/domain; caller contains failures, never retries. */
int ccci_ccif_runtime_send(const struct ccci_ccif_tx_ops *ops, void *context,
			   const void *runtime, u32 used, const void *upload);

#endif
