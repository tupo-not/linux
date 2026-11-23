// SPDX-License-Identifier: GPL-2.0-only
/* Typed bootstrap payloads from the validated same-board SMEM layout. */
#include <linux/errno.h>
#include <linux/kernel.h>
#include <linux/random.h>
#include <linux/string.h>
#include <linux/time.h>
#include <linux/timekeeping.h>
#include <linux/unaligned.h>

#include "ccci-runtime.h"
#include "ccci-smem.h"

#define EXCEPTION_BYTES (64 * 1024)
/* Board scalars (wmid / c2k-flags) are supplied by struct ccci_runtime_boot,
 * sourced from DT properties or the same-SoC defaults in ccci-md-boot.c. */
/* NET_RX_BUF is SKB_4K = CCCI_MTU + 128; the MD gets it minus the header. */
#define CCCI_AP_CCMNI_MTU	(3584 - 16)
/* The LK CCB aggregate is split at 2 MiB into CCB and raw downlink buffers. */
#define CCCI_CCB_MIN_SIZE	(2 * 1024 * 1024)

static int profile_region(const struct ccci_smem_layout *smem, u32 id, bool cached)
{
	const struct ccci_smem_region *region = &smem->users[id];
	const struct ccci_lk_region *bank = cached ? &smem->cached : &smem->noncached;
	u64 md_base = cached ? smem->cache_md_base :
		0x40000000ULL + (bank->base & 0x1ffffff);

	if (!region->present || region->cached != cached || region->no_map ||
	    (region->ap | region->md | region->size) & 3 ||
	    !ccci_lk_contains(bank, region->ap, region->size) ||
	    region->ap - bank->base != region->offset ||
	    md_base + region->offset != region->md ||
	    (u64)region->md + region->size > 0x100000000ULL)
		return -ERANGE;
	return 0;
}

/*
 * Same geometry test for regions the AP never maps itself. LK marks some of
 * them no-map, which only forbids an AP mapping, not reporting them to the MD.
 */
static int profile_reportable(const struct ccci_smem_layout *smem, u32 id, bool cached)
{
	const struct ccci_smem_region *region = &smem->users[id];
	const struct ccci_lk_region *bank = cached ? &smem->cached : &smem->noncached;
	u64 md_base = cached ? smem->cache_md_base :
		0x40000000ULL + (bank->base & 0x1ffffff);

	if (!region->present || region->cached != cached || !region->size ||
	    (region->ap | region->md | region->size) & 3 ||
	    !ccci_lk_contains(bank, region->ap, region->size) ||
	    region->ap - bank->base != region->offset ||
	    md_base + region->offset != region->md ||
	    (u64)region->md + region->size > 0x100000000ULL)
		return -ERANGE;
	return 0;
}

static void profile_offer(struct ccci_runtime_offer *offer, u16 length, u32 first, u32 second)
{
	offer->supported = true;
	offer->length = length;
	put_unaligned_le32(first, offer->data);
	put_unaligned_le32(second, offer->data + 4);
}

static void profile_empty(struct ccci_runtime_offer *offer, u16 length)
{
	offer->supported = true;
	offer->length = length;
	memset(offer->data, 0, length);
}

static void profile_words(struct ccci_runtime_offer *offer, u16 length,
			  const u32 *words, u32 count)
{
	profile_empty(offer, length);
	for (u32 i = 0; i < count; i++)
		put_unaligned_le32(words[i], offer->data + i * 4);
}

/*
 * The remaining capabilities the same-board vendor stack grants and mainline
 * did not: board scalars, the data-path SMEM users, and the CCB split. Any
 * region that fails its geometry test is simply left unsupported, so a bad
 * table can never publish a wrong address.
 */
static int profile_aux(const struct ccci_smem_layout *smem,
		       const struct ccci_runtime_boot *boot,
		       struct ccci_runtime_offer offers[CCCI_RUNTIME_FEATURES])
{
	static const struct {
		u32 feature;
		u32 user;
		bool cached;
	} regions[] = {
		{ 17, 7, false },	/* DT_NETD */
		{ 18, 8, false },	/* DT_USB */
		{ 26, 9, false },	/* AUDIO_RAW */
		{ 31, 24, true },	/* MD_USIP */
		{ 35, 31, false },	/* MD_WIFI_PROXY */
		{ 30, 22, true },	/* MD_CONSYS */
	};
	const struct ccci_smem_region *ccb = &smem->users[CCCI_SMEM_CCB];
	struct timespec64 ts;
	u32 words[4];

	/* Zero-flag misc features: HIF DMA remap, GPS coclock, EE after EPOF. */
	profile_empty(&offers[5], 16);
	profile_empty(&offers[8], 16);
	profile_empty(&offers[19], 16);
	/*
	 * LWA is MUST on the same-board stack even though this layout has no
	 * LWA user, so the vendor sends an explicit empty region instead.
	 */
	profile_offer(&offers[25], 8, 0, 0);
	words[0] = get_random_u32();
	profile_words(&offers[7], 16, words, 1);
	words[0] = 0;
	words[1] = boot->wmid;
	profile_words(&offers[9], 16, words, 2);
	words[0] = 3;	/* sequence check and MD status polling */
	profile_words(&offers[10], 16, words, 1);
	ktime_get_real_ts64(&ts);
	words[0] = lower_32_bits(ts.tv_sec);
	words[1] = upper_32_bits(ts.tv_sec);
	words[2] = sys_tz.tz_minuteswest;
	words[3] = sys_tz.tz_dsttime;
	profile_words(&offers[11], 16, words, 4);
	words[0] = boot->c2k_flags;
	profile_words(&offers[12], 16, words, 1);
	words[0] = CCCI_AP_CCMNI_MTU;
	profile_words(&offers[20], 4, words, 1);
	words[0] = 0;	/* no MTEE on this board */
	profile_words(&offers[32], 4, words, 1);
	for (u32 i = 0; i < ARRAY_SIZE(regions); i++) {
		const struct ccci_smem_region *region = &smem->users[regions[i].user];

		if (profile_reportable(smem, regions[i].user, regions[i].cached))
			continue;
		profile_offer(&offers[regions[i].feature], 8, region->md, region->size);
	}
	/*
	 * CCB_SHARE_MEMORY is deliberately not offered: the MD asserts in
	 * ccci_shm_bm during boot when it is told about the CCB window while no
	 * CCB control/port subsystem exists on the AP side. It is only needed
	 * once real data channels are implemented.
	 */
	/* DHL_RAW_SHARE_MEMORY: the CCB aggregate past the 2 MiB CCB window. */
	if (!profile_reportable(smem, CCCI_SMEM_CCB, true) &&
	    ccb->size > CCCI_CCB_MIN_SIZE &&
	    (u64)ccb->md + ccb->size <= 0x100000000ULL) {
		profile_offer(&offers[16], 8, ccb->md + CCCI_CCB_MIN_SIZE,
			      ccb->size - CCCI_CCB_MIN_SIZE);
	}
	return 0;
}

int ccci_runtime_bootstrap(const struct ccci_smem_layout *smem,
			   const struct ccci_runtime_boot *boot,
			   struct ccci_runtime_offer offers[CCCI_RUNTIME_FEATURES])
{
	static const u32 users[] = { 12, CCCI_SMEM_CCISM_MCU, CCCI_SMEM_CCISM_SCP,
				    CCCI_SMEM_CCISM_EXP, CCCI_SMEM_NVRAM };
	static const u32 features[] = { 1, 2, 14, 28, 36 };
	const struct ccci_smem_region *region, *other;
	struct ccci_runtime_plan plan;
	int ret;
	u32 i, j;

	if (!offers)
		return -EINVAL;
	memset(offers, 0, sizeof(*offers) * CCCI_RUNTIME_FEATURES);
	if (!smem || !boot)
		return -EINVAL;
	if (boot->crystal != CCCI_RUNTIME_CRYSTAL_PRESENT &&
	    boot->crystal != CCCI_RUNTIME_CRYSTAL_ABSENT)
		return -ENODATA;
	ret = ccci_runtime_plan(smem, &plan);
	if (ret)
		return ret;
	/* Validate all sources before emitting even the first capability. */
	for (i = 0; i < ARRAY_SIZE(users); i++) {
		ret = profile_region(smem, users[i], users[i] == CCCI_SMEM_NVRAM);
		if (ret)
			return ret;
		region = &smem->users[users[i]];
		if (users[i] == CCCI_SMEM_CCISM_SCP && region->size < 32768)
			return -ERANGE;
		for (j = 0; j < i; j++) {
			other = &smem->users[users[j]];
			if (region->ap < other->ap + other->size &&
			    other->ap < region->ap + region->size)
				return -EINVAL;
		}
	}
	region = &smem->users[12];
	/* Vendor exception feature spans 64 KiB, not just the 2 KiB debug user. */
	if (!ccci_lk_contains(&smem->noncached, region->ap, EXCEPTION_BYTES) ||
	    (u64)region->md + EXCEPTION_BYTES > 0x100000000ULL)
		return -ERANGE;
	for (i = 1; i < ARRAY_SIZE(users); i++) {
		other = &smem->users[users[i]];
		if (region->ap < other->ap + other->size &&
		    other->ap < region->ap + EXCEPTION_BYTES)
			return -EINVAL;
	}
	profile_offer(&offers[0], 16, 0, boot->booting_start_id);
	for (i = 0; i < ARRAY_SIZE(users); i++) {
		region = &smem->users[users[i]];
		profile_offer(&offers[features[i]], 8, region->md,
			      i ? region->size : EXCEPTION_BYTES);
	}
	/* ENABLE_32K_CLK_LESS: support means no external 32 kHz crystal. */
	if (boot->crystal == CCCI_RUNTIME_CRYSTAL_ABSENT)
		profile_offer(&offers[6], 16, 0, 0);
	offers[27].supported = true;
	ret = profile_aux(smem, boot, offers);
	if (ret)
		return ret;
	/* MD_MEM_AP_VIEW_INF: bank0, noncached bank4, cached bank4 as 16B entries. */
	if (smem->bank0.size) {
		u8 *view = offers[38].data;

		put_unaligned_le32(0, view);
		put_unaligned_le32(smem->bank0.size, view + 4);
		put_unaligned_le32(smem->bank0.base, view + 8);
		put_unaligned_le32(smem->bank0.base >> 32, view + 12);
		put_unaligned_le32(plan.noncache_md, view + 16);
		put_unaligned_le32(plan.noncache_size, view + 20);
		put_unaligned_le32(smem->noncached.base, view + 24);
		put_unaligned_le32(smem->noncached.base >> 32, view + 28);
		put_unaligned_le32(plan.cache_md, view + 32);
		put_unaligned_le32(plan.cache_size, view + 36);
		put_unaligned_le32(smem->cached.base, view + 40);
		put_unaligned_le32(smem->cached.base >> 32, view + 44);
		offers[38].supported = true;
		offers[38].length = 48;
	}
	return 0;
}
