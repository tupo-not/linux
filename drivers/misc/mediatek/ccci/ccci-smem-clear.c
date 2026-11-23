// SPDX-License-Identifier: GPL-2.0-only
/*
 * First-start clearing of the shared-memory users that the same-board vendor
 * also clears on the first MD boot. Two exclusions are deliberate:
 *
 * - CCISM_SCP (16) is shared with an SCP that our runtime owner has already
 *   started, so it is left to the CCISM session owner.
 * - MD_NVRAM_CACHE (32) has its own owner, which already clears it once.
 */
#include <linux/device.h>
#include <linux/errno.h>
#include <linux/io.h>
#include <linux/sizes.h>
#include <linux/string.h>
#include <linux/types.h>

#include "ccci-smem-clear.h"
#include "ccci-smem.h"

/* Same-board 6297 table entries whose vendor flag clears on the first boot. */
static const u8 clear_noncached[] = { 0, 7, 8, 10, 12, 13, 14, 15, 18, 27, 31, 33 };
static const u8 clear_cached[] = { CCCI_SMEM_CCB };

static bool cleared;
/*
 * Operator-selectable policy, defaulting to the same-board behaviour. A caller
 * that wants to keep whatever the vendor stack left behind (for example a NVRAM
 * cache populated during an Android boot) sets this to false before prepare.
 */
static bool clear_enabled = true;

void ccci_smem_clear_policy(bool clear)
{
	clear_enabled = clear;
}

bool ccci_smem_clear_policy_enabled(void)
{
	return clear_enabled;
}

static int clear_region(const struct ccci_smem_region *region)
{
	u64 page = region->ap & ~0xfffULL;
	u64 end = (region->ap + region->size + 0xfff) & ~0xfffULL;
	void *map, *start;
	int ret = 0;

	map = memremap(page, end - page, MEMREMAP_WB);
	if (!map)
		return -ENOMEM;
	start = map + (region->ap - page);
	memset(start, 0, region->size);
	/* Read back both ends: a silent mapping failure must not look cleared. */
	if (readl(start) || readl(start + region->size - 4))
		ret = -EIO;
	memunmap(map);
	return ret;
}

static int clear_list(struct device *dev, const struct ccci_smem_layout *smem,
		      const u8 *ids, size_t count, u64 *bytes)
{
	for (size_t i = 0; i < count; i++) {
		const struct ccci_smem_region *region = &smem->users[ids[i]];
		int ret;

		if (!region->present || !region->size)
			continue;
		ret = clear_region(region);
		if (ret) {
			dev_err(dev, "SMEM clear failed for user %u: %d\n", ids[i], ret);
			return ret;
		}
		*bytes += region->size;
	}
	return 0;
}

int ccci_smem_clear_first(struct device *dev, const struct ccci_smem_layout *smem)
{
	u64 bytes = 0;
	int ret;

	if (!smem)
		return -EINVAL;
	if (cleared)
		return 0;
	if (!clear_enabled) {
		cleared = true;
		dev_info(dev, "SMEM first-start clear: skipped by policy\n");
		return 0;
	}
	ret = clear_list(dev, smem, clear_noncached, ARRAY_SIZE(clear_noncached), &bytes);
	if (!ret)
		ret = clear_list(dev, smem, clear_cached, ARRAY_SIZE(clear_cached), &bytes);
	if (ret)
		return ret;
	cleared = true;
	dev_info(dev, "SMEM first-start clear: %llu bytes\n", bytes);
	return 0;
}

int ccci_smem_clear_restart(const struct ccci_smem_layout *smem)
{
	struct ccci_smem_region dhl = smem->users[CCCI_SMEM_CCB];

	if (!clear_enabled || dhl.size <= SZ_2M)
		return 0;

	/*
	 * The LK parser validated this aggregate and checked that its extent
	 * does not overlap any other user. Same-board 6297 splits it into
	 * the first 2 MiB of CCB DHL and the remainder of RAW DHL.
	 */
	dhl.size = SZ_2M + min_t(u32, dhl.size - SZ_2M, SZ_1K);
	return clear_region(&dhl);
}
