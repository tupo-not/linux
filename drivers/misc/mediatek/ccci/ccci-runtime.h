/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef __CCCI_RUNTIME_H
#define __CCCI_RUNTIME_H

#include <linux/types.h>

#define CCCI_RUNTIME_FEATURES	64
#define CCCI_RUNTIME_HALF	0x800
#define CCCI_RUNTIME_PAYLOAD	64
#define CCCI_RUNTIME_UPLOAD	0xac

struct ccci_smem_layout;

/* Caller declares implemented capabilities, not just reserved memory. */
struct ccci_runtime_offer {
	bool supported;
	u8 version;
	u16 length;
	u8 data[CCCI_RUNTIME_PAYLOAD];
};

struct ccci_runtime_plan {
	u64 ap;
	u32 md;
	u32 noncache_md;
	u32 noncache_size;
	u32 cache_md;
	u32 cache_size;
};

enum ccci_runtime_crystal {
	CCCI_RUNTIME_CRYSTAL_UNKNOWN,
	CCCI_RUNTIME_CRYSTAL_PRESENT,
	CCCI_RUNTIME_CRYSTAL_ABSENT,
};

struct ccci_runtime_boot {
	/* Vendor logging mode/wait and normal/meta/factory boot encoding. */
	u32 booting_start_id;
	enum ccci_runtime_crystal crystal;
	/* Board scalars from DT (mediatek,wmid / mediatek,c2k-flags), or the
	 * same-SoC defaults when the properties are absent. */
	u32 wmid;
	u32 c2k_flags;
};

/*
 * Metadata only, not a service-ready assertion: the owner must prepare all
 * advertised windows and providers before using these offers to boot MD.
 * Requires an unmodified validated SMEM layout and explicit 32 kHz policy.
 * Builds only 0/1/2/6/14/27/28/36; all other offers remain unsupported.
 * Input/output must not overlap. Failure clears the complete offers array.
 */
int ccci_runtime_bootstrap(const struct ccci_smem_layout *smem,
			   const struct ccci_runtime_boot *boot,
			   struct ccci_runtime_offer offers[CCCI_RUNTIME_FEATURES]);

int ccci_runtime_plan(const struct ccci_smem_layout *smem, struct ccci_runtime_plan *plan);
int ccci_runtime_validate(const struct ccci_runtime_offer offers[CCCI_RUNTIME_FEATURES]);
void ccci_runtime_hex(char *out, const u8 masks[CCCI_RUNTIME_FEATURES]);
/* Separate input/output storage; valid output spans are cleared before negotiation. */
int ccci_runtime_encode(const u8 query[CCCI_RUNTIME_FEATURES],
			const struct ccci_runtime_offer offers[CCCI_RUNTIME_FEATURES],
			void *output, u32 capacity, u32 *used,
			u8 negotiated[CCCI_RUNTIME_FEATURES], u32 *rejected);
int ccci_runtime_upload(const struct ccci_runtime_plan *plan, void *output, u32 size);

#endif
