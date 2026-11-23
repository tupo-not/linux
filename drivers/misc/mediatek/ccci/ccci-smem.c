// SPDX-License-Identifier: GPL-2.0-only
/* Same-board 6297 SMEM geometry; never maps or clears shared memory. */
#include <linux/kernel.h>

#include "ccci-smem.h"

struct ccci_smem_default {
	u32 id;
	u32 size;
	bool preserve;
};

/* A's mt6885 table with CCCI_USE_DFD_OFFSET_0, security SMEM disabled. */
static const struct ccci_smem_default ccci_smem_defaults[] = {
	{ 10, 0, false }, { 27, 0, false }, { 31, 0, false },
	{ 29, 0, true }, { 12, 2 * 1024, false }, { 13, 14 * 1024, false },
	{ 18, 42 * 1024, false }, { 14, 4 * 1024, false },
	{ 15, 1024, false }, { 33, 512, false }, { 0, 512, false },
	{ 16, 32 * 1024, false }, { 4, 4 * 1024, true },
	{ 7, 8 * 1024, false }, { 8, 4 * 1024, false },
	{ 9, 52 * 1024, true }, { 19, 721 * 1024, true },
	{ 20, 121 * 1024, true },
};

/* Only use this after the complete LK linked list has been validated. */
static const u8 *ccci_smem_tag(const struct ccci_lk_header *header, const u8 *data,
			       const struct ccci_lk_tag *tags, const char *name,
			       u32 *size)
{
	u32 i;

	for (i = 0; i < header->count; i++) {
		if (!strcmp((const char *)data + tags[i].offset, name)) {
			*size = tags[i].size;
			return data + tags[i].data;
		}
	}
	*size = 0;
	return NULL;
}

static int ccci_smem_add(struct ccci_smem_layout *layout, u32 id, u32 offset,
			 u32 size, bool cached, bool preserve, bool from_lk)
{
	struct ccci_smem_region *region;
	const struct ccci_lk_region *bank = cached ? &layout->cached : &layout->noncached;
	u64 md_base = cached ? layout->cache_md_base :
		0x40000000ULL + (bank->base & 0x1ffffff);

	if (id >= CCCI_SMEM_USERS)
		return -EOPNOTSUPP;
	region = &layout->users[id];
	if (region->present)
		return -EINVAL;
	if ((offset | size) & 3 || offset > bank->size || size > bank->size - offset ||
	    md_base + offset >= 0x100000000ULL ||
	    md_base + offset + size > 0x100000000ULL)
		return -ERANGE;
	region->ap = bank->base + offset;
	region->md = md_base + offset;
	region->offset = offset;
	region->size = size;
	region->cached = cached;
	region->preserve_first = preserve;
	region->no_map = cached && id == 22;
	region->from_lk = from_lk;
	region->present = true;
	layout->count++;
	return 0;
}

/* Distinct nonempty aggregates must not alias, even across AP/MD views. */
static int ccci_smem_gaps(struct ccci_smem_layout *layout)
{
	const struct ccci_smem_region *a, *b;
	u64 used[2] = {};
	u32 i, j;

	for (i = 0; i < CCCI_SMEM_USERS; i++) {
		a = &layout->users[i];
		if (!a->present || !a->size)
			continue;
		used[a->cached] += a->size;
		for (j = 0; j < i; j++) {
			b = &layout->users[j];
			if (!b->present || !b->size)
				continue;
			if ((a->ap < b->ap + b->size && b->ap < a->ap + a->size) ||
			    ((u64)a->md < (u64)b->md + b->size &&
			     (u64)b->md < (u64)a->md + a->size))
				return -EINVAL;
		}
	}
	layout->noncache_gap_bytes = layout->noncached.size - used[0];
	layout->cache_gap_bytes = layout->cached.size - used[1];
	return 0;
}

static int ccci_smem_nc(struct ccci_smem_layout *layout, const u8 *ext, u32 count)
{
	const struct ccci_smem_default *entry;
	u32 i, j, offset = 0, size, ap, md, id;
	bool found;
	int ret;

	for (i = 0; i < count; i++) {
		id = get_unaligned_le32(ext + i * 16 + 12);
		for (j = 0; j < ARRAY_SIZE(ccci_smem_defaults); j++)
			if (ccci_smem_defaults[j].id == id)
				break;
		if (j == ARRAY_SIZE(ccci_smem_defaults))
			return -EOPNOTSUPP;
		for (j = 0; j < i; j++)
			if (get_unaligned_le32(ext + j * 16 + 12) == id)
				return -EINVAL;
	}
	for (i = 0; i < ARRAY_SIZE(ccci_smem_defaults); i++) {
		entry = &ccci_smem_defaults[i];
		size = entry->size;
		found = false;
		for (j = 0; j < count; j++) {
			if (get_unaligned_le32(ext + j * 16 + 12) != entry->id)
				continue;
			ap = get_unaligned_le32(ext + j * 16);
			md = get_unaligned_le32(ext + j * 16 + 4);
			/* This board uses equal AP and MD offsets, not arbitrary remaps. */
			if (ap != md)
				return -EPROTO;
			if (ap || !i)
				offset = ap;
			size = get_unaligned_le32(ext + j * 16 + 8);
			found = true;
			break;
		}
		/* These two anchors cannot be reconstructed from a linear sum. */
		if ((entry->id == 10 || entry->id == 31) && (!found || !size))
			return -ENODATA;
		ret = ccci_smem_add(layout, entry->id, offset, size, false,
				    entry->preserve, found);
		if (ret)
			return ret;
		offset += size;
	}
	return 0;
}

static int ccci_smem_cache(struct ccci_smem_layout *layout, const u8 *items, u32 count)
{
	u32 i, id, offset, size;
	u64 address;
	int ret;

	for (i = 0; i < count; i++) {
		address = get_unaligned_le64(items + i * 24);
		offset = get_unaligned_le32(items + i * 24 + 8);
		size = get_unaligned_le32(items + i * 24 + 12);
		/* item_cnt is a user ID in an item, a count in the bank header. */
		id = get_unaligned_le32(items + i * 24 + 16);
		if (id != 1 && id != 22 && id != 24 && id != 28 && id != 32)
			return -EOPNOTSUPP;
		if (address != layout->cached.base + offset)
			return -EPROTO;
		ret = ccci_smem_add(layout, id, offset, size, true, id == 22 || id == 24,
				    true);
		if (ret)
			return ret;
	}
	if (!layout->users[CCCI_SMEM_NVRAM].size || !layout->users[CCCI_SMEM_CCB].size)
		return -ENODATA;
	return 0;
}

int ccci_smem_parse(const struct ccci_lk_header *header, const void *data, size_t len,
		    const struct ccci_lk_region *noncached,
		    const struct ccci_lk_region *cached,
		    struct ccci_lk_tag workspace[CCCI_LK_MAX_TAGS],
		    struct ccci_smem_layout *layout)
{
	struct ccci_smem_layout result = {};
	struct ccci_lk_layout coarse;
	const u8 *nc_count, *ext, *info, *items, *cache_offset;
	const u8 *bank, *count_tag;
	u32 bytes, count, ext_size, info_size, items_size, offset_size, count_size;
	u64 md_base;
	int ret;

	if (!layout)
		return -EINVAL;
	memset(layout, 0, sizeof(*layout));
	if (!header || !data || !noncached || !cached || !workspace)
		return -EINVAL;
	ret = ccci_lk_parse(header, data, len, noncached, cached, workspace, &coarse);
	if (ret)
		return ret;
	nc_count = ccci_smem_tag(header, data, workspace, "nc_smem_info_ext_num", &bytes);
	ext = ccci_smem_tag(header, data, workspace, "nc_smem_info_ext", &ext_size);
	info = ccci_smem_tag(header, data, workspace, "md1_bank4_cache_info", &info_size);
	items = ccci_smem_tag(header, data, workspace, "md1_bank4_cache_layout", &items_size);
	cache_offset = ccci_smem_tag(header, data, workspace, "md1_smem_cahce_offset",
				     &offset_size);
	if (!nc_count || !ext || !info || !items || !cache_offset)
		return -ENODATA;
	if (bytes != 4 || info_size != 24 || offset_size != 4)
		return -EMSGSIZE;
	result.noncached = coarse.ap_md1;
	result.cached.base = get_unaligned_le64(info);
	result.cached.size = get_unaligned_le32(info + 12);
	if (!ccci_lk_contains(cached, result.cached.base, result.cached.size) ||
	    result.noncached.size > 0xffffffffULL ||
	    ((result.noncached.base | result.cached.base | result.noncached.size |
	      result.cached.size) & 3))
		return -ERANGE;
	/* A uses the separate offset tag; header md_offset is not a physical base. */
	md_base = 0x40000000ULL + get_unaligned_le32(cache_offset) +
		(result.cached.base & 0xfffff);
	if ((md_base & 3) || md_base + result.cached.size > 0x100000000ULL)
		return -ERANGE;
	result.cache_md_base = md_base;
	if (0x40000000ULL + (result.noncached.base & 0x1ffffff) +
	    result.noncached.size > md_base ||
	    (result.noncached.base < result.cached.base + result.cached.size &&
	     result.cached.base < result.noncached.base + result.noncached.size))
		return -ERANGE;
	count = get_unaligned_le32(nc_count);
	if (!count || count > CCCI_SMEM_USERS || ext_size != count * 16)
		return -EMSGSIZE;
	ret = ccci_smem_nc(&result, ext, count);
	if (ret)
		return ret;
	if (result.users[CCCI_SMEM_RUNTIME].size < 4096 ||
	    result.users[CCCI_SMEM_CCISM_SCP].size < 32 * 1024 ||
	    result.users[CCCI_SMEM_CCISM_MCU].size < 721 * 1024 ||
	    result.users[CCCI_SMEM_CCISM_EXP].size < 121 * 1024)
		return -EMSGSIZE;
	count = get_unaligned_le32(info + 16);
	if (!count || count > CCCI_SMEM_USERS || items_size != count * 24)
		return -EMSGSIZE;
	ret = ccci_smem_cache(&result, items, count);
	if (ret)
		return ret;
	if (!coarse.ccb_present || coarse.ccb.base != result.users[CCCI_SMEM_CCB].ap ||
	    coarse.ccb.size != result.users[CCCI_SMEM_CCB].size)
		return -EPROTO;
	ret = ccci_smem_gaps(&result);
	if (ret)
		return ret;
	/* LK hdr_tbl_inf: one 24-byte {base,size,id,errno,type,ver,reserved[2]}. */
	bank = ccci_smem_tag(header, data, workspace, "hdr_tbl_inf", &bytes);
	count_tag = ccci_smem_tag(header, data, workspace, "hdr_count", &count_size);
	if (bank && count_tag && count_size == 4 && bytes == 24 &&
	    get_unaligned_le32(count_tag) == 1) {
		u64 base = get_unaligned_le64(bank);
		u32 size = get_unaligned_le32(bank + 8);

		if (!base || !size || (base | size) & 3 ||
		    base + size > 0x100000000ULL)
			return -ERANGE;
		result.bank0 = (struct ccci_lk_region) { .base = base, .size = size };
	}
	*layout = result;
	return 0;
}
