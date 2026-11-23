// SPDX-License-Identifier: GPL-2.0-only
/* Bounded decoding only. Unknown tag payloads are neither interpreted nor logged. */
#include <linux/kernel.h>

#include "ccci-lk.h"

/*
 * Scalar LK tags: a single u32 count/size that a later stage compares against.
 * Everything else stays an uninterpreted payload.
 */
static const char *const ccci_lk_scalars[] = {
	"md1img", "hdr_count", "retrieve_num", "ccb_gear_id",
	"md1_smem_cahce_offset", "md_mpu_num", "free_in_kernel",
};

bool ccci_lk_scalar_tag(const char *name)
{
	unsigned int i;

	for (i = 0; i < ARRAY_SIZE(ccci_lk_scalars); i++)
		if (!strcmp(name, ccci_lk_scalars[i]))
			return true;
	return false;
}

static bool ccci_lk_overlap(u32 a, u32 size_a, u32 b, u32 size_b)
{
	/* Each interval's end has already been checked to fit in a u32. */
	return size_a && size_b && a < b + size_b && b < a + size_a;
}

static int ccci_lk_smem(const char *raw, u32 size,
			const struct ccci_lk_region *noncached,
			struct ccci_lk_layout *layout)
{
	u32 offset, length, offsets[3], lengths[3];
	unsigned int i, j;

	/* 36 bytes of fields, rounded to 40 by the native u64 alignment. */
	if (size < 40)
		return -EINVAL;
	layout->smem.base = get_unaligned_le64(raw);
	layout->smem.size = get_unaligned_le32(raw + 32);
	if (!ccci_lk_contains(noncached, layout->smem.base, layout->smem.size))
		return -ERANGE;
	for (i = 0; i < 3; i++) {
		offset = get_unaligned_le32(raw + 8 + i * 8);
		length = get_unaligned_le32(raw + 12 + i * 8);
		if (offset > layout->smem.size || length > layout->smem.size - offset)
			return -ERANGE;
		offsets[i] = offset;
		lengths[i] = length;
		for (j = 0; j < i; j++)
			if (ccci_lk_overlap(offset, length, offsets[j], lengths[j]))
				return -EINVAL;
	}
	if (!lengths[0])
		return -ENODATA;
	layout->ap_md1.base = layout->smem.base + offsets[0];
	layout->ap_md1.size = lengths[0];
	return 0;
}

int ccci_lk_parse(const struct ccci_lk_header *header, const void *data,
		  size_t len, const struct ccci_lk_region *noncached,
		  const struct ccci_lk_region *cached,
		  struct ccci_lk_tag workspace[CCCI_LK_MAX_TAGS],
		  struct ccci_lk_layout *layout)
{
	struct ccci_lk_layout value = {};
	const char *raw = data;
	const struct ccci_lk_tag *tag, *old;
	u32 offset = 0, next;
	unsigned int i, j;
	bool smem_found = false;
	int ret;

	memset(layout, 0, sizeof(*layout));
	if (header->version != 2 && header->version != 3)
		return -EPROTONOSUPPORT;
	if (len != header->size || len > CCCI_LK_MAX_SIZE ||
	    len < CCCI_LK_TAG_SIZE || !header->count ||
	    header->count > CCCI_LK_MAX_TAGS || header->count > len / CCCI_LK_TAG_SIZE)
		return -ERANGE;
	for (i = 0; i < header->count; i++) {
		if (offset > len - CCCI_LK_TAG_SIZE)
			return -ERANGE;
		if (!raw[offset] || !memchr(raw + offset, 0, 64))
			return -EINVAL;
		workspace[i].offset = offset;
		workspace[i].data = get_unaligned_le32(raw + offset + 64);
		workspace[i].size = get_unaligned_le32(raw + offset + 68);
		tag = &workspace[i];
		if (tag->data > len || tag->size > len - tag->data)
			return -ERANGE;
		for (j = 0; j < i; j++) {
			old = &workspace[j];
			if (ccci_lk_overlap(offset, CCCI_LK_TAG_SIZE,
					    old->offset, CCCI_LK_TAG_SIZE) ||
			    !strcmp(raw + offset, raw + old->offset) ||
			    ccci_lk_overlap(tag->data, tag->size, old->data, old->size))
				return -EINVAL;
		}
		next = get_unaligned_le32(raw + offset + 72);
		if (i + 1 < header->count) {
			if (!next)
				return -EINVAL;
		} else if (next && next != len) {
			/* Accept only a null or one-past-buffer terminal link. */
			return -EINVAL;
		}
		offset = next;
	}
	/* Check payloads against every header, including later headers. */
	for (i = 0; i < header->count; i++) {
		tag = &workspace[i];
		for (j = 0; j < header->count; j++)
			if (ccci_lk_overlap(tag->data, tag->size,
					    workspace[j].offset, CCCI_LK_TAG_SIZE))
				return -EINVAL;
		if (!strcmp(raw + tag->offset, "smem_layout")) {
			ret = ccci_lk_smem(raw + tag->data, tag->size, noncached, &value);
			if (ret)
				return ret;
			smem_found = true;
		} else if (!strcmp(raw + tag->offset, "ccb_info")) {
			/* 12 bytes of fields and four bytes of native tail padding. */
			if (tag->size < 16)
				return -EINVAL;
			value.ccb.base = get_unaligned_le64(raw + tag->data);
			value.ccb.size = get_unaligned_le32(raw + tag->data + 8);
			if (!ccci_lk_contains(cached, value.ccb.base, value.ccb.size))
				return -ERANGE;
			value.ccb_present = true;
		}
	}
	if (!smem_found)
		return -ENODATA;
	*layout = value;
	return 0;
}
