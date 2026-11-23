/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef __CCCI_RPC_PROVIDER_H
#define __CCCI_RPC_PROVIDER_H

#include "ccci-rpc-protocol.h"

#define CCCI_RPC_FACTS_MAX	32
#define CCCI_RPC_FACT_NAME	64
#define CCCI_RPC_EINT_ATTRS	7

struct ccci_rpc_named_fact {
	char name[CCCI_RPC_FACT_NAME];
	u32 value;
};

struct ccci_rpc_eint_fact {
	char name[CCCI_RPC_FACT_NAME];
	/* MD units/numbering, after vendor polarity/sensitivity conversion. */
	s32 values[CCCI_RPC_EINT_ATTRS];
};

/*
 * Immutable, externally owned audited snapshots, never a DT or GPIO accessor.
 * A present flag asserts a complete namespace, including known absent names;
 * absence of a provider is not evidence that every lookup should fail on wire.
 * Names are nonempty NUL-terminated byte strings, with no duplicate entries.
 * Snapshots and packet/output buffers must be distinct and not change in use.
 */
struct ccci_rpc_provider {
	const struct ccci_rpc_named_fact *gpio;
	const struct ccci_rpc_named_fact *adc;
	const struct ccci_rpc_named_fact *dtsi;
	const struct ccci_rpc_eint_fact *eint;
	u32 gpio_count;
	u32 adc_count;
	u32 dtsi_count;
	u32 eint_count;
	bool gpio_present;
	bool adc_present;
	bool dtsi_present;
	bool eint_present;
};

/* Includes A/V declared IDs, factory range and explicitly reserved 0x400c. */
bool ccci_rpc_provider_known(u32 operation);
/* Validates snapshots; true availability describes only these typed providers. */
int ccci_rpc_provider_available(const struct ccci_rpc_provider *provider, u32 operation);
/*
 * Complete RX32 request -> complete TX33 reply bytes, no allocation or I/O.
 * Known unavailable returns -EOPNOTSUPP, never a fake success or unknown-op
 * reply. Truly unknown IDs return the vendor's single s32 NO_OP=-1 argument.
 * 0x400d is a command, not a fact lookup: it records the requested LHIF queue
 * remap and replies A's two zeros without reporting whether it was applied.
 * Any error leaves output unchanged. NULL provider has no available services.
 */
int ccci_rpc_provider_reply(const struct ccci_rpc_provider *provider,
			    const void *request, size_t length,
			    void *reply, size_t capacity);

#endif
