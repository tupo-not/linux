/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef __CCCI_SCP_IRQ_H
#define __CCCI_SCP_IRQ_H

#include <linux/types.h>

struct platform_device;
struct mutex;
struct ccci_scp_irq;

/* Shared board validation. Resolve only after validating all five specifiers. */
int ccci_scp_irq_validate_specs(struct platform_device *pdev);
int ccci_scp_irq_resolve(struct platform_device *pdev, u32 mbox);

/*
 * Same built-in cfg/vote owner; allocate five NO_AUTOEN IRQ actions and faulted
 * CPU queue storage. No enable/arm path, mailbox MMIO or reset generation yet.
 * Probe/unwind is process context and must not hold the shared owner mutex.
 */
int ccci_scp_irq_reserve(struct platform_device *pdev, struct mutex *owner_lock,
			 struct ccci_scp_irq **result);
ssize_t ccci_scp_irq_status(struct ccci_scp_irq *irq, char *buf);

#endif
