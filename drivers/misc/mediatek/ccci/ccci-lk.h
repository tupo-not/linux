/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef __CCCI_LK_H
#define __CCCI_LK_H

#include <linux/soc/mediatek/ccci-lk.h>

struct ccci_lk_tag {
	u32 offset;
	u32 data;
	u32 size;
};

struct ccci_lk_layout {
	struct ccci_lk_region smem;
	struct ccci_lk_region ap_md1;
	struct ccci_lk_region ccb;
	bool ccb_present;
};

/* Caller owns and clears workspace; no allocation or hardware access here. */
int ccci_lk_parse(const struct ccci_lk_header *header, const void *data,
		  size_t len, const struct ccci_lk_region *noncached,
		  const struct ccci_lk_region *cached,
		  struct ccci_lk_tag workspace[CCCI_LK_MAX_TAGS],
		  struct ccci_lk_layout *layout);

/* True for the allowlisted single-u32 tags; all other payloads stay opaque. */
bool ccci_lk_scalar_tag(const char *name);

#endif
