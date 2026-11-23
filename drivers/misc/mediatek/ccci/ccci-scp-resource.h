/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef __CCCI_SCP_RESOURCE_H
#define __CCCI_SCP_RESOURCE_H

#include <linux/bits.h>
#include <linux/types.h>

/* MT6893 ATF resource-manager order, not the SCP request-mask numbering. */
#define CCCI_SCP_RESOURCE_26M	BIT(1)

struct ccci_scp_resource_sample {
	u32 resources;
	u32 users;
	u32 scp_user;
	u32 scp_resources;
	bool scp_present;
	bool scp_valid;
};

struct ccci_scp_resource {
	struct ccci_scp_resource_sample before;
	struct ccci_scp_resource_sample after;
	int error;
	bool attempted;
	bool accepted;
	bool pinned;
	bool fault;
};

/* Explicit read-only SMC queries; no registration, MMIO or lazy SCP request. */
int ccci_scp_resource_read(struct ccci_scp_resource_sample *sample);

/* Serialized, read-only prerequisite: no pin/retry/release or READY dependency. */
int ccci_scp_resource_check(struct ccci_scp_resource *resource, unsigned long rate);

/*
 * One built-in, boot-lifetime owner serializes this with clock/STOP/reset.
 * Initialize only once, before first use. The zero-initialized state is idle.
 * Pin requires a held, verified 26 MHz TOPCK output. It sends at most one
 * resource request and validates SCP's resource membership, not just a0.
 * Any failed transaction is sticky; never retry or issue a rollback release.
 * There is deliberately no put: release would clear inherited SCP votes too.
 */
int ccci_scp_resource_pin(struct ccci_scp_resource *resource);

/* No I/O or release; a future STOP owner must preserve this boot's pin. */
void ccci_scp_resource_invalidate(struct ccci_scp_resource *resource);

#endif
