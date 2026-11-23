/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef __CCCI_SCP_INSPECT_H
#define __CCCI_SCP_INSPECT_H

#include <linux/io.h>
#include <linux/types.h>

struct ccci_scp_inspect;
struct mutex;
struct platform_device;

/* Constructor only: caller already exclusively owns all three mappings. */
int ccci_scp_inspect_create(struct platform_device *pdev, void __iomem * const maps[3],
			    struct mutex *lock, int (*check)(void *), void *context,
			    struct ccci_scp_inspect **result);

/* Caller holds the same owner mutex for all observations and lifetime changes. */
ssize_t ccci_scp_inspect_status(struct ccci_scp_inspect *inspect, char *buf);
ssize_t ccci_scp_inspect_cores(struct ccci_scp_inspect *inspect, char *buf);
ssize_t ccci_scp_inspect_header(struct ccci_scp_inspect *inspect, char *buf);

/* Same owner lock; no generation/READY requirement for forced assertion. */
int ccci_scp_inspect_reset_check(void *context);
void ccci_scp_inspect_record_reset(struct ccci_scp_inspect *inspect, u32 writes);
/* Asserted cores and valid LK header; returns this boot's trusted share span. */
int ccci_scp_inspect_boot(struct ccci_scp_inspect *inspect, u32 *base, u32 *size);
int ccci_scp_inspect_restore_check(struct ccci_scp_inspect *inspect, bool capture);
int ccci_scp_inspect_restore(struct ccci_scp_inspect *inspect, u32 *base, u32 *size);

#endif
