/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef __CCCI_CCISM_BRIDGE_H
#define __CCCI_CCISM_BRIDGE_H

#include <linux/errno.h>
#include <linux/kconfig.h>
#include "ccci-ccif-rx.h"

struct platform_device;
struct ccci_ccif;
struct ccci_smem_layout;
struct ccci_ccism_bridge;

#if IS_ENABLED(CONFIG_OP6893_CCCI_CCISM)
/* Dormant reservation only. No clock enable, SCP lease, SMC or memory clear. */
struct ccci_ccism_bridge *
ccci_ccism_bridge_create(struct platform_device *pdev, struct ccci_ccif *ccif,
			 const struct ccci_smem_layout *smem);
/* Parent lifecycle lock held, powered MD resources, stopped CPU/DMA, SCP READY. */
int ccci_ccism_bridge_arm(struct ccci_ccism_bridge *bridge);
/* Handshake port callback: accepted HS2 or CCISM system replies only. */
int ccci_ccism_bridge_port(void *context, const struct ccci_ccif_event *event);
/* Parent lock held; md_stopped from CPU/DMA ownership, never RX/fault state. */
int ccci_ccism_bridge_stop(struct ccci_ccism_bridge *bridge, bool md_stopped);
/* Parent lock held: gate CCIF reset/MD power-off, no I/O or fabricated STOP. */
int ccci_ccism_bridge_offline(struct ccci_ccism_bridge *bridge);
/* After offline has joined callbacks and released the SCP lease. */
int ccci_ccism_bridge_rearm(struct ccci_ccism_bridge *bridge, bool commit);
ssize_t ccci_ccism_bridge_status(struct ccci_ccism_bridge *bridge, char *buf);
#else
static inline struct ccci_ccism_bridge *
ccci_ccism_bridge_create(struct platform_device *pdev, struct ccci_ccif *ccif,
			 const struct ccci_smem_layout *smem)
{
	return NULL;
}

static inline int ccci_ccism_bridge_offline(struct ccci_ccism_bridge *bridge) { return 0; }

static inline ssize_t ccci_ccism_bridge_status(struct ccci_ccism_bridge *bridge, char *buf)
{
	return -EOPNOTSUPP;
}
#endif
#endif
