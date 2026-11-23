/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef __CCCI_SCP_MEMORY_H
#define __CCCI_SCP_MEMORY_H

#include <linux/types.h>

#define CCCI_SCP_REGION_OFFSET	4
#define CCCI_SCP_REGION_LEGACY_BYTES	68
#define CCCI_SCP_REGION_BYTES		72

struct ccci_scp_span {
	u64 base;
	u64 size;
};

/* Only fields needed to bound the vendor recovery mappings are decoded. */
struct ccci_scp_region {
	struct ccci_scp_span loader;
	struct ccci_scp_span firmware;
	struct ccci_scp_span dram;
	struct ccci_scp_span regdump;
	u32 backup_start;
};

/*
 * Decode a CPU snapshot starting at TCM + REGION_OFFSET, never MMIO directly.
 * Accept the observed 68-byte prefix or audited 72-byte A/V structure only.
 * length must equal the header struct_size, including for a larger allocation.
 * Inputs and output must not overlap; output is cleared on failure.
 */
int ccci_scp_decode_region(const void *buffer, size_t length,
			   struct ccci_scp_region *region);

/* Nonempty aligned u32 physical span inside a trusted, nonwrapping window. */
bool ccci_scp_span_allowed(const struct ccci_scp_span *window,
			   const struct ccci_scp_span *span);

/*
 * Bound the A/V mapping: roundup(bytes, 1024) * (2 cores) * (live + backup).
 * This checks mapping size only, not backup_start, layout ownership or READY.
 * A zero DRAM pair is allowed and returns an empty span; mixed pairs fail.
 * Inputs and output must not overlap.
 */
int ccci_scp_dram_span(const struct ccci_scp_region *region,
		       const struct ccci_scp_span *window,
		       struct ccci_scp_span *mapping);

#endif
