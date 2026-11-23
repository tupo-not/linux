/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef __CCCI_SMEM_H
#define __CCCI_SMEM_H

#include "ccci-lk.h"

/* Same-board 4.19 wire IDs; CCB is one aggregate until port setup. */
#define CCCI_SMEM_USERS		42
#define CCCI_SMEM_DBM		0
#define CCCI_SMEM_CCB		1
#define CCCI_SMEM_DFD		10
#define CCCI_SMEM_RUNTIME		14
#define CCCI_SMEM_MDSS_DBG	13
#define CCCI_SMEM_CCISM_SCP	16
#define CCCI_SMEM_CCISM_MCU	19
#define CCCI_SMEM_CCISM_EXP	20
#define CCCI_SMEM_NVRAM		32

struct ccci_smem_region {
	u64 ap;
	u32 md;
	u32 offset;
	u32 size;
	bool present;
	bool cached;
	bool preserve_first;
	bool no_map;
	bool from_lk;
};

struct ccci_smem_layout {
	struct ccci_lk_region noncached;
	struct ccci_lk_region cached;
	/* MD RO+RW image region from LK hdr_tbl_inf; MD view base is 0. */
	struct ccci_lk_region bank0;
	struct ccci_smem_region users[CCCI_SMEM_USERS];
	u32 cache_md_base;
	u32 cache_gap_bytes;
	u32 noncache_gap_bytes;
	u32 count;
};

/* CPU metadata only. Output/workspace must not overlap any input. */
int ccci_smem_parse(const struct ccci_lk_header *header, const void *data, size_t len,
		    const struct ccci_lk_region *noncached,
		    const struct ccci_lk_region *cached,
		    struct ccci_lk_tag workspace[CCCI_LK_MAX_TAGS],
		    struct ccci_smem_layout *layout);

#endif
