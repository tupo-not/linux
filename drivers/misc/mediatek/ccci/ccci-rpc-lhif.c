// SPDX-License-Identifier: GPL-2.0-only
/*
 * LHIF queue remap requests from the MD (RPC 0x400d). Same-board A keeps this
 * per CCMNI port as txq_index/txq_exp_index; here it is only the CCIF queue a
 * CCMNI port must transmit on, and every request is retained for inspection
 * because the MD reply cannot report a rejection.
 */
#include <linux/errno.h>
#include <linux/spinlock.h>
#include <linux/string.h>

#include "ccci-rpc-lhif.h"

static DEFINE_SPINLOCK(lhif_lock);
static struct ccci_rpc_lhif_state lhif;

/* A maps the two AP uplink queues to CLDMA q0 and q3; nothing else is valid. */
static bool lhif_queue(u32 lhif_q, u32 *queue)
{
	if (lhif_q == CCCI_RPC_LHIF_AP_UL_Q0) {
		*queue = 0;
		return true;
	}
	if (lhif_q == CCCI_RPC_LHIF_AP_UL_Q1) {
		*queue = 3;
		return true;
	}
	return false;
}

void ccci_rpc_lhif_report(u32 net_if, u32 lhif_q)
{
	struct ccci_rpc_lhif_port *port = NULL;
	unsigned long flags;
	u32 queue = 0;
	bool resolved;
	u32 i;

	resolved = lhif_queue(lhif_q, &queue);
	spin_lock_irqsave(&lhif_lock, flags);
	lhif.requests++;
	if (!resolved) {
		lhif.unknown++;
	} else {
		for (i = 0; i < lhif.ports; i++)
			if (lhif.port[i].net_if == net_if) {
				port = &lhif.port[i];
				break;
			}
		if (!port && lhif.ports < CCCI_RPC_LHIF_PORTS)
			port = &lhif.port[lhif.ports++];
		if (port) {
			port->net_if = net_if;
			port->lhif_q = lhif_q;
			port->queue = queue;
			port->requests++;
		} else {
			lhif.dropped++;
		}
	}
	spin_unlock_irqrestore(&lhif_lock, flags);
}

void ccci_rpc_lhif_malformed(void)
{
	unsigned long flags;

	spin_lock_irqsave(&lhif_lock, flags);
	lhif.malformed++;
	spin_unlock_irqrestore(&lhif_lock, flags);
}

bool ccci_rpc_lhif_queue(u32 net_if, u32 *queue)
{
	unsigned long flags;
	bool found = false;
	u32 i;

	spin_lock_irqsave(&lhif_lock, flags);
	for (i = 0; i < lhif.ports; i++)
		if (lhif.port[i].net_if == net_if) {
			if (queue)
				*queue = lhif.port[i].queue;
			found = true;
			break;
		}
	spin_unlock_irqrestore(&lhif_lock, flags);
	return found;
}

void ccci_rpc_lhif_state(struct ccci_rpc_lhif_state *state)
{
	unsigned long flags;

	if (!state)
		return;
	spin_lock_irqsave(&lhif_lock, flags);
	*state = lhif;
	spin_unlock_irqrestore(&lhif_lock, flags);
}

void ccci_rpc_lhif_reset(void)
{
	unsigned long flags;

	spin_lock_irqsave(&lhif_lock, flags);
	memset(&lhif, 0, sizeof(lhif));
	spin_unlock_irqrestore(&lhif_lock, flags);
}
