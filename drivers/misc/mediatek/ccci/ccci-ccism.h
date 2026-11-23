/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef __CCCI_CCISM_H
#define __CCCI_CCISM_H

#include "ccci-ccif-rx.h"
#include "ccci-scp-protocol.h"

#define CCCI_CCISM_INIT		0x119
#define CCCI_CCISM_ACK		0x11a
#define CCCI_CCISM_DONE		0x11b
#define CCCI_CCISM_TIMEOUT_MS	60000
#define CCCI_CCISM_STOP_MS	3000

struct ccci_smem_layout;

struct ccci_ccism_plan {
	u32 ap;
	u32 md;
	u32 size;
};

enum ccci_ccism_state {
	CCCI_CCISM_IDLE,
	CCCI_CCISM_ARMED,
	CCCI_CCISM_WAIT_MD_ACK,
	CCCI_CCISM_WAIT_RBREADY,
	CCCI_CCISM_WAIT_MD_DONE,
	CCCI_CCISM_RUNNING,
	CCCI_CCISM_WAIT_STOP,
	CCCI_CCISM_STOPPED,
	CCCI_CCISM_FAILED,
};

/*
 * Serialized, sleepable operations. No reentry, parent MD lock, or direct SCP
 * RX callback use: queue scalar events from that callback to an independent
 * consumer. check validates this leased SCP generation and real READY.
 * bridge_on holds CCIF2 clocks and invokes CCCI SMC13 before the first INIT.
 * On any partial failure retain resources. bridge_off must ACK both CCIF2
 * views before releasing clocks, only after MD stopped and this SCP STOP.
 */
struct ccci_ccism_ops {
	int (*check)(void *context, u64 generation);
	int (*bridge_on)(void *context);
	int (*md_send)(void *context, u32 message);
	int (*clear)(void *context, u32 ap, u32 size);
	int (*scp_send)(void *context, u16 op, u32 data);
	int (*bridge_off)(void *context);
};

struct ccci_ccism {
	const struct ccci_ccism_ops *ops;
	void *context;
	struct ccci_ccism_plan plan;
	u64 generation;
	u64 last_ms;
	u64 deadline_ms;
	enum ccci_ccism_state state;
	int error;
	int cleanup_error;
	bool bridge_touched;
	bool rbready_seen;
	bool md_done_seen;
	bool stop_seen;
	bool stop_attempted;
};

int ccci_ccism_plan(const struct ccci_smem_layout *smem, struct ccci_ccism_plan *plan);
/* Use an unmodified plan() result; arm before MD release, without sending INIT. */
int ccci_ccism_arm(struct ccci_ccism *ccism, const struct ccci_ccism_ops *ops,
		   void *context, const struct ccci_ccism_plan *plan, u64 generation, u64 now_ms);
/* Called by the MD owner only after accepting HS2 and enabling system ports. */
int ccci_ccism_md_ready(struct ccci_ccism *ccism, u64 now_ms);
int ccci_ccism_md_event(struct ccci_ccism *ccism, const struct ccci_ccif_event *event, u64 now_ms);
int ccci_ccism_scp_state(struct ccci_ccism *ccism, u64 generation, u32 state, u64 now_ms);
int ccci_ccism_tick(struct ccci_ccism *ccism, u64 now_ms);
int ccci_ccism_fault(struct ccci_ccism *ccism, int error);
/* Caller must establish MD CPU/DMA stopped; false performs no cleanup I/O. */
int ccci_ccism_stop(struct ccci_ccism *ccism, bool md_stopped, u64 now_ms);

#endif
