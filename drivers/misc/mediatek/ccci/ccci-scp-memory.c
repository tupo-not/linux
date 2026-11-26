// SPDX-License-Identifier: GPL-2.0-only
/* Bounded SCP RV metadata decoding; no mapping, loading or recovery actions. */

#include <linux/errno.h>
#include <linux/limits.h>
#include <linux/string.h>
#include <linux/unaligned.h>

#include "ccci-scp-memory.h"

static bool ccci_scp_pair_valid(const struct ccci_scp_span *span)
{
	if (!span->base && !span->size)
		return true;
	if (!span->base || !span->size || ((span->base | span->size) & 3))
		return false;
	return span->base <= U32_MAX && span->size - 1 <= U32_MAX - span->base;
}

int ccci_scp_decode_region(const void *buffer, size_t length,
			   struct ccci_scp_region *region)
{
	struct ccci_scp_region value = {};
	const u8 *raw = buffer;

	if (!region)
		return -EINVAL;
	memset(region, 0, sizeof(*region));
	if (!buffer || (length != CCCI_SCP_REGION_LEGACY_BYTES &&
			length != CCCI_SCP_REGION_BYTES &&
			length != CCCI_SCP_REGION_X6710_BYTES))
		return -EMSGSIZE;
	/* struct_size is the eighth u32, not the first member. */
	if (get_unaligned_le32(raw + 28) != length)
		return -EPROTONOSUPPORT;
	value.loader.base = get_unaligned_le32(raw);
	value.loader.size = get_unaligned_le32(raw + 4);
	value.firmware.base = get_unaligned_le32(raw + 8);
	value.firmware.size = get_unaligned_le32(raw + 12);
	value.dram.base = get_unaligned_le32(raw + 16);
	value.dram.size = get_unaligned_le32(raw + 20);
	value.backup_start = get_unaligned_le32(raw + 24);
	value.regdump.base = get_unaligned_le32(raw + 44);
	value.regdump.size = get_unaligned_le32(raw + 48);
	if (!ccci_scp_pair_valid(&value.loader) ||
	    !ccci_scp_pair_valid(&value.firmware) ||
	    !ccci_scp_pair_valid(&value.dram) ||
	    !ccci_scp_pair_valid(&value.regdump) || (value.backup_start & 3))
		return -ERANGE;
	*region = value;
	return 0;
}

bool ccci_scp_span_allowed(const struct ccci_scp_span *window,
			   const struct ccci_scp_span *span)
{
	u64 offset;

	if (!window || !span || !window->base || !window->size ||
	    window->size - 1 > U64_MAX - window->base || !span->size ||
	    !ccci_scp_pair_valid(span) || span->base < window->base)
		return false;
	offset = span->base - window->base;
	return offset < window->size && span->size <= window->size - offset;
}

int ccci_scp_dram_span(const struct ccci_scp_region *region,
		       const struct ccci_scp_span *window,
		       struct ccci_scp_span *mapping)
{
	struct ccci_scp_span value;

	if (!mapping)
		return -EINVAL;
	memset(mapping, 0, sizeof(*mapping));
	if (!region || !ccci_scp_pair_valid(&region->dram))
		return -EINVAL;
	if (!region->dram.size)
		return 0;
	value.base = region->dram.base;
	/* Widen before rounding and multiplying; metadata is not trusted. */
	value.size = ((region->dram.size + 1023) & ~1023ULL) * 4;
	if (!ccci_scp_span_allowed(window, &value))
		return -ERANGE;
	*mapping = value;
	return 0;
}
