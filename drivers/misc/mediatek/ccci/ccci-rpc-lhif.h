/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef __CCCI_RPC_LHIF_H
#define __CCCI_RPC_LHIF_H

#include <linux/types.h>

/*
 * Same-board A port_rpc.c IPC_RPC_CCCI_LHIF_MAPPING (0x400d): the MD asks the
 * AP to move one CCMNI port's uplink onto a named LHIF hardware queue. The
 * request is a {net_if, lhif_q} pair and the reply is two zero words, so the
 * MD never learns whether the port existed or the queue was valid; the
 * resolution has to stay on the AP side. This file is that single record.
 */
#define CCCI_RPC_LHIF_PORTS	4

/* A port_rpc.h enum: the two AP uplink queues, not the SW queues. */
enum ccci_rpc_lhif_q {
	CCCI_RPC_LHIF_AP_UL_Q0 = 2,
	CCCI_RPC_LHIF_AP_UL_Q1 = 3,
};

struct ccci_rpc_lhif_port {
	u32 net_if;
	u32 lhif_q;
	u32 queue;
	u32 requests;
};

struct ccci_rpc_lhif_state {
	u32 requests;
	u32 malformed;
	u32 unknown;
	u32 dropped;
	u32 ports;
	struct ccci_rpc_lhif_port port[CCCI_RPC_LHIF_PORTS];
};

/* Records one well-formed request; never blocks and never fails. */
void ccci_rpc_lhif_report(u32 net_if, u32 lhif_q);
void ccci_rpc_lhif_malformed(void);
/* True and the CCIF queue when this CCMNI port has a resolved mapping. */
bool ccci_rpc_lhif_queue(u32 net_if, u32 *queue);
void ccci_rpc_lhif_state(struct ccci_rpc_lhif_state *state);
/* Mappings belong to one MD generation. */
void ccci_rpc_lhif_reset(void);

#endif
