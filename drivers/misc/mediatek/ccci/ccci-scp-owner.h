/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef __CCCI_SCP_OWNER_H
#define __CCCI_SCP_OWNER_H

#include <linux/mutex.h>
#include "ccci-scp-reset.h"
#include "ccci-scp-resource.h"
#include "ccci-scp-runtime.h"
#include "ccci-scp-protocol.h"

struct clk;
struct ccci_scp_irq;
struct ccci_scp_mmio;
struct ccci_scp_receive;
struct ccci_scp_inspect;

struct ccci_scp_resource_owner {
	struct clk *clock;
	/* Serialize observations and the one pin with future STOP/reset handling. */
	struct mutex lock;
	struct ccci_scp_resource resource;
	void __iomem *cfg;
	void __iomem *core_tcm[3];
	struct ccci_scp_irq *irq;
	struct ccci_scp_mmio *mmio;
	struct ccci_scp_receive *receive;
	struct ccci_scp_inspect *inspect;
#if IS_ENABLED(CONFIG_OP6893_CCCI_SCP_RESET_ASSERT)
	struct ccci_scp_reset reset;
	bool reset_requested;
#endif
#if IS_ENABLED(CONFIG_OP6893_CCCI_SCP_INITIAL_BOOT)
	struct ccci_scp_layout layout;
	bool boot_requested;
	bool boot_running;
	u32 boot_ready_channel;
	u32 boot_discarded;
	u32 boot_events;
	int boot_error;
	int boot_cleanup_error;
#endif
#if IS_ENABLED(CONFIG_OP6893_CCCI_SCP_RUNTIME)
	struct ccci_scp_runtime runtime;
#endif
	/* Valid only between asserted-core old-message discard and final STOP. */
	u64 reset_generation;
};

#endif
