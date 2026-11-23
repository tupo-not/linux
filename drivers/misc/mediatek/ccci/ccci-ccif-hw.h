/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef __CCCI_CCIF_HW_H
#define __CCCI_CCIF_HW_H

#include <linux/types.h>

struct ccci_ccif_layout;

/* bank 0: infracfg, bank 1: AP CCIF, bank 2: MD CCIF. */
struct ccci_ccif_hw_ops {
	int (*write)(void *context, u32 bank, u32 offset, u32 value);
	int (*read)(void *context, u32 bank, u32 offset, u32 *value);
	void (*fence)(void *context);
};

/* Caller owns clocks, MD domain, stopped CPU and masked/synchronized IRQs. */
int ccci_ccif_hw_reset(const struct ccci_ccif_hw_ops *ops, void *context,
		       const struct ccci_ccif_layout *layout);

#endif
