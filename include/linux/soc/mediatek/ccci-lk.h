/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef __LINUX_SOC_MEDIATEK_CCCI_LK_H
#define __LINUX_SOC_MEDIATEK_CCCI_LK_H

#include <linux/errno.h>
#include <linux/limits.h>
#include <linux/string.h>
#include <linux/types.h>
#include <linux/unaligned.h>

#define CCCI_LK_HEADER_SIZE	48
#define CCCI_LK_MAX_SIZE		0x10000
#define CCCI_LK_MAX_TAGS		128
#define CCCI_LK_TAG_SIZE		76
#define CCCI_LK_PROPERTY		"ccci,modem_info_v2"

struct ccci_lk_region {
	u64 base;
	u64 size;
};

struct ccci_lk_header {
	u64 base;
	u32 size;
	u32 version;
	u32 count;
};

static inline bool ccci_lk_contains(const struct ccci_lk_region *region,
				    u64 base, u64 size)
{
	/* Reject wrapped intervals, including a wrapped enclosing reservation. */
	return region->size && region->size <= U64_MAX - region->base &&
	       size && base >= region->base && base - region->base < region->size &&
	       size <= region->size - (base - region->base);
}

/* The op6893 LK ABI is little-endian, with four errors and tail padding. */
static inline int ccci_lk_decode_header(const void *data, size_t len,
					struct ccci_lk_header *header)
{
	const u8 *raw = data;
	struct ccci_lk_header value;
	unsigned int i;

	memset(header, 0, sizeof(*header));
	if (len != CCCI_LK_HEADER_SIZE)
		return -EINVAL;
	value.base = get_unaligned_le64(raw);
	value.size = get_unaligned_le32(raw + 8);
	value.version = get_unaligned_le32(raw + 16);
	value.count = get_unaligned_le32(raw + 20);
	if (value.version != 2 && value.version != 3)
		return -EPROTONOSUPPORT;
	/* This board profile supports only a successfully loaded MD1. */
	if (get_unaligned_le32(raw + 12) || get_unaligned_le32(raw + 24) != 1)
		return -ENODATA;
	for (i = 0; i < 4; i++)
		if (get_unaligned_le32(raw + 28 + i * 4))
			return -ENODATA;
	if (!value.base || value.size < CCCI_LK_TAG_SIZE ||
	    value.size > CCCI_LK_MAX_SIZE || !value.count ||
	    value.count > CCCI_LK_MAX_TAGS ||
	    value.count > value.size / CCCI_LK_TAG_SIZE ||
	    value.size > U64_MAX - value.base)
		return -ERANGE;
	*header = value;
	return 0;
}

#endif
