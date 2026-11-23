/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef __CCCI_RTC32K_H
#define __CCCI_RTC32K_H

#include <linux/errno.h>
#include <linux/kconfig.h>

#include "ccci-runtime.h"

struct device;

enum ccci_rtc32k_source {
	CCCI_RTC32K_SOURCE_NONE,
	CCCI_RTC32K_SOURCE_NVMEM,
	CCCI_RTC32K_SOURCE_DT,
};

#if IS_ENABLED(CONFIG_OP6893_CCCI_MD_BOOT)
/*
 * Same-board 4.19 ccci_rtc.c reads the first byte of the "external-32k" nvmem
 * cell: zero means no crystal. A board that has no cell and no explicit DT
 * fact is UNKNOWN and must not be guessed from the CONN 26 MHz strap.
 */
int ccci_rtc32k_read(struct device *dev, enum ccci_runtime_crystal *crystal,
		     enum ccci_rtc32k_source *source);
#else
static inline int ccci_rtc32k_read(struct device *dev,
				   enum ccci_runtime_crystal *crystal,
				   enum ccci_rtc32k_source *source)
{
	return -EOPNOTSUPP;
}
#endif
#endif
