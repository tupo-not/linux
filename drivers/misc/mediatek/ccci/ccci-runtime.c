// SPDX-License-Identifier: GPL-2.0-only
/* Bounded 6297 feature records and v2.1 SRAM response from the same-board ABI. */
#include <linux/errno.h>
#include <linux/string.h>
#include <linux/unaligned.h>

#include "ccci-runtime.h"
#include "ccci-smem.h"

int ccci_runtime_plan(const struct ccci_smem_layout *smem, struct ccci_runtime_plan *plan)
{
	const struct ccci_smem_region *rt;
	u64 nc_md;

	if (!plan)
		return -EINVAL;
	memset(plan, 0, sizeof(*plan));
	if (!smem)
		return -EINVAL;
	rt = &smem->users[CCCI_SMEM_RUNTIME];
	nc_md = 0x40000000ULL + (smem->noncached.base & 0x1ffffff);
	if (!rt->present || rt->cached || rt->no_map || rt->size != 2 * CCCI_RUNTIME_HALF ||
	    (rt->ap | rt->md) & 3 ||
	    !ccci_lk_contains(&smem->noncached, rt->ap, rt->size) ||
	    (u64)rt->md != nc_md + rt->ap - smem->noncached.base ||
	    nc_md + smem->noncached.size > 0x100000000ULL ||
	    !ccci_lk_contains(&smem->cached, smem->cached.base, smem->cached.size) ||
	    (smem->noncached.base | smem->noncached.size |
	     smem->cached.base | smem->cached.size | smem->cache_md_base) & 3 ||
	    (smem->noncached.base < smem->cached.base + smem->cached.size &&
	     smem->cached.base < smem->noncached.base + smem->noncached.size) ||
	    !smem->cache_md_base ||
	    (u64)smem->cache_md_base + smem->cached.size > 0x100000000ULL ||
	    smem->cached.size > 0xffffffff || smem->noncached.size > 0xffffffff ||
	    (nc_md < (u64)smem->cache_md_base + smem->cached.size &&
	     smem->cache_md_base < nc_md + smem->noncached.size))
		return -ERANGE;
	*plan = (struct ccci_runtime_plan) {
		.ap = rt->ap, .md = rt->md,
		.noncache_md = nc_md, .noncache_size = smem->noncached.size,
		.cache_md = smem->cache_md_base, .cache_size = smem->cached.size,
	};
	return 0;
}

static int runtime_length(u32 id)
{
	switch (id) {
	case 0: case 5: case 6: case 7: case 8: case 9: case 10: case 11:
	case 12: case 15: case 19: case 34:
		return 16;
	case 1: case 2: case 3: case 4: case 13: case 14: case 16: case 17:
	case 18: case 25: case 26: case 28: case 29: case 30: case 31:
	case 33: case 35: case 36: case 37:
		return 8;
	case 20: case 22: case 32:
		return 4;
	case 27:
		/* MULTI_MD_MPU is a payload-free flag in A and V, not a missing value. */
		return 0;
	case 38:
		/* Bank0, noncached bank4 and cached bank4: three 16-byte entries. */
		return 48;
	default:
		return -EOPNOTSUPP;
	}
}

void ccci_runtime_hex(char *out, const u8 masks[CCCI_RUNTIME_FEATURES])
{
	static const char digits[] = "0123456789abcdef";

	for (u32 i = 0; i < CCCI_RUNTIME_FEATURES; i++) {
		out[i * 2] = digits[masks[i] >> 4];
		out[i * 2 + 1] = digits[masks[i] & 0xf];
	}
	out[CCCI_RUNTIME_FEATURES * 2] = '\0';
}

int ccci_runtime_validate(const struct ccci_runtime_offer offers[CCCI_RUNTIME_FEATURES])
{
	if (!offers)
		return -EINVAL;
	for (u32 id = 0; id < CCCI_RUNTIME_FEATURES; id++) {
		if (!offers[id].supported)
			continue;
		/* This codec implements the same-board version-zero payloads only. */
		if (offers[id].version || runtime_length(id) < 0)
			return -EOPNOTSUPP;
		if (offers[id].length != runtime_length(id))
			return -EMSGSIZE;
	}
	return 0;
}

static int runtime_negotiate(u8 query, const struct ccci_runtime_offer *offer, u8 *result)
{
	u8 mask = query & 15, version = query >> 4;

	if (mask > 4)
		return -EPROTO;
	if (!mask) {
		*result = query;
		return 0;
	}
	if (mask == 2 && !offer->supported)
		return -EOPNOTSUPP;
	if (mask == 2 && version != offer->version)
		return -EPROTONOSUPPORT;
	*result = offer->supported ? offer->version << 4 | 1 : 1;
	if (offer->supported && (mask == 2 || (mask == 3 && version == offer->version) ||
				 (mask == 4 && version >= offer->version)))
		*result = offer->version << 4 | 2;
	return 0;
}

int ccci_runtime_encode(const u8 query[CCCI_RUNTIME_FEATURES],
			const struct ccci_runtime_offer offers[CCCI_RUNTIME_FEATURES],
			void *output, u32 capacity, u32 *used,
			u8 negotiated[CCCI_RUNTIME_FEATURES], u32 *rejected)
{
	u8 selected[CCCI_RUNTIME_FEATURES], *bytes = output;
	u32 total = 0, offset = 0;
	int ret;

	if (!output || !used || !negotiated || !rejected || capacity > CCCI_RUNTIME_HALF)
		return -EINVAL;
	*used = 0;
	*rejected = CCCI_RUNTIME_FEATURES;
	memset(output, 0, capacity);
	memset(negotiated, 0, CCCI_RUNTIME_FEATURES);
	if (!query)
		return -EINVAL;
	ret = ccci_runtime_validate(offers);
	if (ret)
		return ret;
	/* Complete negotiation and size checks before emitting any record. */
	for (u32 id = 0; id < CCCI_RUNTIME_FEATURES; id++) {
		*rejected = id;
		ret = runtime_negotiate(query[id], &offers[id], &selected[id]);
		if (ret)
			return ret;
		total += 8 + ((selected[id] & 15) == 2 ? offers[id].length : 0);
		if (total > capacity)
			return -ENOSPC;
	}
	for (u32 id = 0; id < CCCI_RUNTIME_FEATURES; id++) {
		u32 length = (selected[id] & 15) == 2 ? offers[id].length : 0;

		bytes[offset] = id;
		bytes[offset + 1] = selected[id];
		put_unaligned_le32(length, bytes + offset + 4);
		memcpy(bytes + offset + 8, offers[id].data, length);
		offset += 8 + length;
	}
	memcpy(negotiated, selected, sizeof(selected));
	*used = total;
	*rejected = CCCI_RUNTIME_FEATURES;
	return 0;
}

int ccci_runtime_upload(const struct ccci_runtime_plan *plan, void *output, u32 size)
{
	u8 *bytes = output;

	if (!output || size != CCCI_RUNTIME_UPLOAD)
		return -EINVAL;
	memset(output, 0, size);
	if (!plan || !plan->noncache_size || !plan->cache_size ||
	    (plan->md | plan->noncache_md | plan->noncache_size |
	     plan->cache_md | plan->cache_size) & 3 ||
	    (u64)plan->noncache_md + plan->noncache_size > 0x100000000ULL ||
	    (u64)plan->cache_md + plan->cache_size > 0x100000000ULL ||
	    (plan->noncache_md < (u64)plan->cache_md + plan->cache_size &&
	     plan->cache_md < (u64)plan->noncache_md + plan->noncache_size) ||
	    (u64)plan->md + 2 * CCCI_RUNTIME_HALF > 0x100000000ULL ||
	    plan->md < plan->noncache_md ||
	    (u64)plan->md + 2 * CCCI_RUNTIME_HALF >
		(u64)plan->noncache_md + plan->noncache_size)
		return -ERANGE;
	/* CCCI header: data[1] is TX packet size here; channel1, initial sequence0. */
	put_unaligned_le32(CCCI_RUNTIME_UPLOAD, bytes + 4);
	put_unaligned_le32(1, bytes + 8);
	put_unaligned_le32(0x5555ffff, bytes + 12);
	put_unaligned_le32(0x43434349, bytes + 16);
	/* AP query feature1: support NOT_EXIST, version1 (vendor MPU flag). */
	bytes[21] = 0x10;
	put_unaligned_le32(2, bytes + 84);
	put_unaligned_le32(plan->md, bytes + 88);
	put_unaligned_le32(CCCI_RUNTIME_HALF, bytes + 92);
	put_unaligned_le32(plan->md + CCCI_RUNTIME_HALF, bytes + 96);
	put_unaligned_le32(CCCI_RUNTIME_HALF, bytes + 100);
	put_unaligned_le32(plan->noncache_md, bytes + 104);
	put_unaligned_le32(plan->noncache_size, bytes + 108);
	put_unaligned_le32(plan->cache_md, bytes + 112);
	put_unaligned_le32(plan->cache_size, bytes + 116);
	put_unaligned_le32(0x43434349, bytes + 168);
	return 0;
}
