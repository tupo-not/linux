/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef __CCCI_CCIF_CONTROL_H
#define __CCCI_CCIF_CONTROL_H

#include <linux/kconfig.h>
#include <linux/errno.h>

#include "ccci-ccif-rx.h"

struct ccci_ccif_controller;
struct ccci_smem_layout;
struct ccci_runtime_offer;

/* Under the control mutex: enqueue only, never parent/control lifecycle calls. */
typedef void (*ccci_ccif_control_fault_fn)(void *context, int error);

#if IS_ENABLED(CONFIG_OP6893_CCCI_HANDSHAKE)
struct ccci_ccif_controller *ccci_ccif_control_create(struct device *dev, void __iomem *ap,
						      struct ccci_ccif_rx *rx,
						      const struct ccci_smem_layout *smem);
/*
 * Optional before start, immutable through quiesce plus RX STOP. Callback
 * context must survive both joins; timer polls RX faults even after HS2.
 */
int ccci_ccif_control_set_fault(struct ccci_ccif_controller *control,
				ccci_ccif_control_fault_fn fault, void *context);
/* Caller holds the power/lifecycle lock; MD CPU stopped, ring prepare complete. */
int ccci_ccif_control_start(struct ccci_ccif_controller *control,
			    const struct ccci_runtime_offer *offers,
			    ccci_ccif_consume_fn port, void *context);
/* Close FSM/timer admission; caller must then stop/synchronize RX before power-off. */
void ccci_ccif_control_quiesce(struct ccci_ccif_controller *control);
int ccci_ccif_control_error(struct ccci_ccif_controller *control);
/* Called after both the timer and RX worker have been joined. */
int ccci_ccif_control_rearm(struct ccci_ccif_controller *control, bool commit);
ssize_t ccci_ccif_control_status(struct ccci_ccif_controller *control, char *buf);
#else
static inline struct ccci_ccif_controller *
ccci_ccif_control_create(struct device *dev, void __iomem *ap,
			 struct ccci_ccif_rx *rx, const struct ccci_smem_layout *smem)
{
	return NULL;
}

static inline int ccci_ccif_control_set_fault(struct ccci_ccif_controller *control,
					      ccci_ccif_control_fault_fn fault, void *context)
{
	return -EOPNOTSUPP;
}

static inline int ccci_ccif_control_start(struct ccci_ccif_controller *control,
					  const struct ccci_runtime_offer *offers,
					  ccci_ccif_consume_fn port, void *context)
{
	return -EOPNOTSUPP;
}

static inline void ccci_ccif_control_quiesce(struct ccci_ccif_controller *control) { }
static inline int ccci_ccif_control_error(struct ccci_ccif_controller *control) { return 0; }
static inline int ccci_ccif_control_rearm(struct ccci_ccif_controller *control, bool commit)
{
	return 0;
}
static inline ssize_t ccci_ccif_control_status(struct ccci_ccif_controller *control, char *buf)
{
	return -EOPNOTSUPP;
}
#endif
#endif
