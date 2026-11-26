// SPDX-License-Identifier: GPL-2.0-only
/* Explicit, bounded observations through the unique SCP resource owner. */
#include <linux/bitops.h>
#include <linux/crc32.h>
#include <linux/mfd/syscon.h>
#include <linux/mutex.h>
#include <linux/of_reserved_mem.h>
#include <linux/platform_device.h>
#include <linux/regmap.h>
#include <linux/slab.h>
#include <linux/sysfs.h>
#include <linux/unaligned.h>
#include <linux/vmalloc.h>

#include "ccci-scp-inspect.h"
#include "ccci-scp-restore.h"

struct ccci_scp_inspect {
	struct device *dev;
	void __iomem *core[2];
	void __iomem *header;
	void __iomem *tcm;
	struct regmap *top;
	/* Shared with the resource, receive and future reset owner. */
	struct mutex *lock;
	int (*check)(void *context);
	void *context;
	struct ccci_scp_span firmware;
	struct ccci_scp_span share;
	struct ccci_scp_restore_plan saved_plan;
	u8 saved_header[CCCI_SCP_REGION_BYTES];
	u32 restore_crc[2];
	bool restore_captured;
	u64 core_reads;
	u64 core_writes;
	u64 header_reads;
};

struct ccci_scp_inspect_sample {
	u32 clr;
	u32 set;
	u32 status;
};

/* Observations require an explicit, still-valid 26M pin, not a reset generation. */
static int ccci_scp_inspect_check(struct ccci_scp_inspect *inspect)
{
	unsigned int mux;
	int ret;

	lockdep_assert_held(inspect->lock);
	ret = inspect->check(inspect->context);
	if (ret)
		return ret;
	ret = regmap_read(inspect->top, 0x10, &mux);
	if (ret)
		return ret;
	return mux & (GENMASK(18, 16) | BIT(23)) ? -EAGAIN : 0;
}

#if IS_ENABLED(CONFIG_OP6893_CCCI_SCP_RESET_ASSERT)
int ccci_scp_inspect_reset_check(void *context)
{
	return ccci_scp_inspect_check(context);
}

void ccci_scp_inspect_record_reset(struct ccci_scp_inspect *inspect, u32 writes)
{
	lockdep_assert_held(inspect->lock);
	inspect->core_writes += writes;
}
#endif

static void ccci_scp_inspect_pair(struct ccci_scp_inspect *inspect,
				  struct ccci_scp_inspect_sample samples[2])
{
	unsigned int i;

	/* Same-board A/V core0 and core1: only reset aliases and gated/halt bits. */
	for (i = 0; i < 2; i++) {
		samples[i].clr = readl(inspect->core[i]) & BIT(0);
		samples[i].set = readl(inspect->core[i] + 4) & BIT(0);
		samples[i].status = readl(inspect->core[i] + 0x70) & GENMASK(1, 0);
		inspect->core_reads += 3;
	}
}

static int ccci_scp_inspect_sample(struct ccci_scp_inspect *inspect,
				   struct ccci_scp_inspect_sample samples[2])
{
	struct ccci_scp_inspect_sample first[2], second[2];
	int ret;

	memset(samples, 0, sizeof(first));
	ret = ccci_scp_inspect_check(inspect);
	if (ret)
		return ret;
	ccci_scp_inspect_pair(inspect, first);
	ccci_scp_inspect_pair(inspect, second);
	ret = ccci_scp_inspect_check(inspect);
	if (ret)
		return ret;
	if (memcmp(first, second, sizeof(first)))
		return -EAGAIN;
	memcpy(samples, second, sizeof(second));
	return 0;
}

/* Exact 68/72-byte header only; never probe TCM code or unknown tail bytes. */
static int ccci_scp_inspect_copy(struct ccci_scp_inspect *inspect, u8 *buffer,
				 size_t *length)
{
	u8 second[CCCI_SCP_REGION_BYTES] = {};
	u32 bytes;
	unsigned int offset;
	int ret = -EAGAIN;

	*length = 0;
	memset(buffer, 0, CCCI_SCP_REGION_BYTES);
	bytes = readl(inspect->header + 28);
	inspect->header_reads++;
	if (bytes != CCCI_SCP_REGION_LEGACY_BYTES &&
	    bytes != CCCI_SCP_REGION_BYTES &&
	    bytes != CCCI_SCP_REGION_X6710_BYTES) {
		pr_info("ccci scp: unexpected region header size %u\n", bytes);
		return -EPROTONOSUPPORT;
	}
	for (offset = 0; offset < bytes; offset += sizeof(u32)) {
		put_unaligned_le32(readl(inspect->header + offset), buffer + offset);
		inspect->header_reads++;
	}
	for (offset = 0; offset < bytes; offset += sizeof(u32)) {
		put_unaligned_le32(readl(inspect->header + offset), second + offset);
		inspect->header_reads++;
	}
	inspect->header_reads++;
	if (readl(inspect->header + 28) == bytes &&
	    get_unaligned_le32(buffer + 28) == bytes &&
	    get_unaligned_le32(second + 28) == bytes && !memcmp(buffer, second, bytes)) {
		*length = bytes;
		ret = 0;
	} else {
		memset(buffer, 0, CCCI_SCP_REGION_BYTES);
	}
	memzero_explicit(second, sizeof(second));
	return ret;
}

ssize_t ccci_scp_inspect_status(struct ccci_scp_inspect *inspect, char *buf)
{
	lockdep_assert_held(inspect->lock);
	/* Counters include failed observations; this attribute performs no MMIO. */
	return sysfs_emit(buf,
		"mapped=3 bytes=1581056 nonposted=1 register_reads=%llu register_writes=%llu tcm_reads=%llu tcm_writes=0 reset_supported=0 ready_known=0\n",
		inspect->core_reads, inspect->core_writes, inspect->header_reads);
}

ssize_t ccci_scp_inspect_cores(struct ccci_scp_inspect *inspect, char *buf)
{
	struct ccci_scp_inspect_sample samples[2];
	int ret;

	ret = ccci_scp_inspect_sample(inspect, samples);
	if (ret)
		return ret;
	/* Alias readbacks and WFI bits never prove reset completion or CPU/DMA stop. */
	return sysfs_emit(buf,
		"core0_clr=%u core0_set=%u core0_gated=%u core0_halt=%u core1_clr=%u core1_set=%u core1_gated=%u core1_halt=%u samples_equal=1 stopped_known=0 ready_known=0\n",
		samples[0].clr, samples[0].set, samples[0].status & 1,
		(samples[0].status >> 1) & 1, samples[1].clr, samples[1].set,
		samples[1].status & 1, (samples[1].status >> 1) & 1);
}

ssize_t ccci_scp_inspect_header(struct ccci_scp_inspect *inspect, char *buf)
{
	const struct ccci_scp_span tcm = { .base = 0x10500000, .size = 0x180000 };
	struct ccci_scp_inspect_sample before[2], after[2];
	struct ccci_scp_restore_plan plan;
	struct ccci_scp_region region;
	u8 snapshot[CCCI_SCP_REGION_BYTES] = {};
	size_t length;
	int ret, post_ret, geometry;

	ret = ccci_scp_inspect_sample(inspect, before);
	if (ret)
		return ret;
	ret = ccci_scp_inspect_copy(inspect, snapshot, &length);
	/* Recheck both cores/prerequisites even after an unsupported/unstable copy. */
	post_ret = ccci_scp_inspect_sample(inspect, after);
	if (post_ret || memcmp(before, after, sizeof(before))) {
		ret = post_ret ? post_ret : -EAGAIN;
		goto out;
	}
	if (ret)
		goto out;
	ret = ccci_scp_decode_region(snapshot, length, &region);
	if (ret)
		goto out;
	geometry = ccci_scp_plan_restore(snapshot, length, 2, &inspect->firmware,
					 &inspect->share, &tcm, &plan);
	/* Only CPU geometry is inspected, never backup contents or restore actions. */
	ret = sysfs_emit(buf,
			 "header_bytes=%zu samples_equal=1 loader_bytes=%llu firmware_bytes=%llu dram_bytes=%llu regdump_bytes=%llu restore_geometry_valid=%u geometry_error=%d source_valid_known=0 stopped_known=0 ready_known=0\n",
			 length, region.loader.size, region.firmware.size, region.dram.size,
			 region.regdump.size, !geometry, geometry);
out:
	memzero_explicit(snapshot, sizeof(snapshot));
	return ret;
}

int ccci_scp_inspect_boot(struct ccci_scp_inspect *inspect, u32 *base, u32 *size)
{
	const struct ccci_scp_span tcm = { .base = 0x10500000, .size = 0x180000 };
	struct ccci_scp_inspect_sample samples[2];
	struct ccci_scp_restore_plan plan;
	u8 snapshot[CCCI_SCP_REGION_BYTES] = {};
	size_t length;
	int ret;

	ret = ccci_scp_inspect_sample(inspect, samples);
	if (ret)
		return ret;
	if (samples[0].clr || samples[0].set || samples[1].clr || samples[1].set)
		return -EBUSY;
	ret = ccci_scp_inspect_copy(inspect, snapshot, &length);
	if (!ret)
		ret = ccci_scp_plan_restore(snapshot, length, 2, &inspect->firmware,
					    &inspect->share, &tcm, &plan);
	memzero_explicit(snapshot, sizeof(snapshot));
	if (!ret)
		ret = ccci_scp_inspect_check(inspect);
	if (!ret) {
		*base = inspect->share.base;
		*size = inspect->share.size;
	}
	return ret;
}

#if IS_ENABLED(CONFIG_OP6893_CCCI_SCP_RUNTIME)
static int scp_restore_source_crc(const struct ccci_scp_span *span, u32 *result)
{
	u8 buffer[128], any = 0, all = 0xff;
	void __iomem *source;
	u32 crc = ~0U;
	size_t offset = 0;

	source = ioremap_wc(span->base, span->size);
	if (!source)
		return -ENOMEM;
	while (offset < span->size) {
		size_t bytes = min_t(u64, sizeof(buffer), span->size - offset);

		memcpy_fromio(buffer, source + offset, bytes);
		crc = crc32_le(crc, buffer, bytes);
		for (size_t i = 0; i < bytes; i++) {
			any |= buffer[i];
			all &= buffer[i];
		}
		offset += bytes;
	}
	memzero_explicit(buffer, sizeof(buffer));
	iounmap(source);
	if (!any || all == 0xff)
		return -ENODATA;
	*result = crc;
	return 0;
}

/* Both cores held in reset by the runtime owner; never write firmware memory. */
int ccci_scp_inspect_restore_check(struct ccci_scp_inspect *inspect, bool capture)
{
	const struct ccci_scp_span tcm = { .base = 0x10500000, .size = 0x180000 };
	struct ccci_scp_restore_plan plan;
	u8 snapshot[CCCI_SCP_REGION_BYTES];
	u32 crc[2];
	size_t length;
	int ret;

	if (!capture && !inspect->restore_captured)
		return -ENODATA;
	ret = ccci_scp_inspect_check(inspect);
	if (ret)
		return ret;
	if (capture) {
		ret = ccci_scp_inspect_copy(inspect, snapshot, &length);
		if (!ret)
			ret = ccci_scp_plan_restore(snapshot, length, 2, &inspect->firmware,
						    &inspect->share, &tcm, &plan);
		if (ret)
			goto out;
	} else {
		plan = inspect->saved_plan;
	}
	ret = scp_restore_source_crc(&plan.loader_source, &crc[0]);
	if (!ret)
		ret = scp_restore_source_crc(&plan.dram_source, &crc[1]);
	if (!ret)
		ret = ccci_scp_inspect_check(inspect);
	if (ret)
		goto out;
	if (capture) {
		inspect->saved_plan = plan;
		memcpy(inspect->saved_header, snapshot, sizeof(snapshot));
		memcpy(inspect->restore_crc, crc, sizeof(crc));
		inspect->restore_captured = true;
	} else if (memcmp(inspect->restore_crc, crc, sizeof(crc))) {
		ret = -ESTALE;
	}
out:
	memzero_explicit(snapshot, sizeof(snapshot));
	return ret;
}

static int scp_restore_verify(struct device *dev, const char *region,
			      void __iomem *target, const u8 *expected, size_t bytes)
{
	u8 buffer[128];
	size_t offset = 0;
	int ret = 0;

	while (bytes) {
		size_t chunk = min(bytes, sizeof(buffer));

		size_t bad;

		memcpy_fromio(buffer, target, chunk);
		for (bad = 0; bad < chunk; bad++)
			if (buffer[bad] != (expected ? expected[bad] : 0))
				break;
		if (bad < chunk) {
			dev_err(dev, "SCP restore %s mismatch at %#zx (sample_zero=%u sample_ff=%u)\n",
				region, offset + bad, !memchr_inv(buffer, 0, chunk),
				!memchr_inv(buffer, 0xff, chunk));
			ret = -EIO;
			break;
		}
		target += chunk;
		if (expected)
			expected += chunk;
		bytes -= chunk;
		offset += chunk;
	}
	memzero_explicit(buffer, sizeof(buffer));
	return ret;
}

/* Only after clean runtime stop and reset readback, with the owner lock held. */
int ccci_scp_inspect_restore(struct ccci_scp_inspect *inspect, u32 *base, u32 *size)
{
	const struct ccci_scp_restore_plan *plan = &inspect->saved_plan;
	size_t loader_bytes = plan->loader_source.size, dram_bytes = plan->dram_source.size;
	size_t header_offset = plan->header_target.base - plan->tcm_clear.base;
	void __iomem *firmware, *dram, *tcm = inspect->tcm;
	u8 *image, *backup;
	int ret;

	if (!inspect->restore_captured)
		return -ENODATA;
	ret = ccci_scp_inspect_check(inspect);
	if (ret)
		return ret;
	image = kvmalloc(loader_bytes + dram_bytes, GFP_KERNEL);
	if (!image)
		return -ENOMEM;
	backup = image + loader_bytes;
	firmware = ioremap_wc(inspect->firmware.base, inspect->firmware.size);
	if (!firmware) {
		ret = -ENOMEM;
		goto free;
	}
	dram = firmware + (plan->dram_target.base - inspect->firmware.base);
	/* Restore exactly the bytes checked, even if a source changes later. */
	memcpy_fromio(image, firmware + (plan->loader_source.base - inspect->firmware.base),
		      loader_bytes);
	memcpy_fromio(backup, firmware + (plan->dram_source.base - inspect->firmware.base),
		      dram_bytes);
	if (crc32_le(~0U, image, loader_bytes) != inspect->restore_crc[0] ||
	    crc32_le(~0U, backup, dram_bytes) != inspect->restore_crc[1]) {
		ret = -ESTALE;
		goto unmap;
	}
	ret = ccci_scp_inspect_check(inspect);
	if (ret)
		goto unmap;
	memcpy(image + header_offset, inspect->saved_header, plan->header_target.size);
	memset_io(tcm, 0, plan->tcm_clear.size);
	memcpy_toio(tcm, image, loader_bytes);
	/* Finish TCM stores and prove the loader before changing live DRAM. */
	mb();
	ret = scp_restore_verify(inspect->dev, "TCM loader", tcm, image, loader_bytes);
	if (!ret)
		ret = scp_restore_verify(inspect->dev, "TCM tail", tcm + loader_bytes, NULL,
					  plan->tcm_clear.size - loader_bytes);
	if (ret)
		goto unmap;
	memset_io(dram, 0, plan->dram_clear.size);
	memcpy_toio(dram, backup, dram_bytes);
	/* Complete DRAM stores before readback, including the TCM recheck. */
	mb();
	ret = scp_restore_verify(inspect->dev, "DRAM image", dram, backup, dram_bytes);
	if (!ret)
		ret = scp_restore_verify(inspect->dev, "DRAM tail", dram + dram_bytes, NULL,
					  plan->dram_clear.size - dram_bytes);
	if (!ret)
		ret = scp_restore_verify(inspect->dev, "TCM loader after DRAM", tcm, image,
					  loader_bytes);
	if (!ret)
		ret = ccci_scp_inspect_check(inspect);
	if (!ret) {
		*base = inspect->share.base;
		*size = inspect->share.size;
	}
unmap:
	iounmap(firmware);
free:
	kvfree_sensitive(image, loader_bytes + dram_bytes);
	return ret;
}
#endif

static int ccci_scp_inspect_reservation(struct device_node *np, unsigned int index,
					struct ccci_scp_span *span)
{
	struct device_node *node, *parent;
	struct reserved_mem *rmem;
	int ret = -EINVAL;

	node = of_parse_phandle(np, "memory-region", index);
	if (!node)
		return -EINVAL;
	parent = of_get_parent(node);
	if (!parent || !of_node_name_eq(parent, "reserved-memory") ||
	    !of_device_is_available(node) || !of_property_present(node, "no-map") ||
	    of_property_present(node, "reusable"))
		goto out;
	rmem = of_reserved_mem_lookup(node);
	if (!rmem || !rmem->base || !rmem->size || rmem->base > U32_MAX ||
	    rmem->size - 1 > U32_MAX - rmem->base || ((rmem->base | rmem->size) & 3))
		goto out;
	span->base = rmem->base;
	span->size = rmem->size;
	ret = 0;
out:
	of_node_put(parent);
	of_node_put(node);
	return ret;
}

int ccci_scp_inspect_create(struct platform_device *pdev, void __iomem * const maps[3],
			    struct mutex *lock, int (*check)(void *), void *context,
			    struct ccci_scp_inspect **result)
{
	const struct ccci_scp_span tcm = { .base = 0x10500000, .size = 0x180000 };
	struct ccci_scp_inspect *inspect;
	struct device_node *np;
	int ret;

	if (!result)
		return -EINVAL;
	*result = NULL;
	if (!pdev || !maps || !maps[0] || !maps[1] || !maps[2] || !lock || !check)
		return -EINVAL;
	np = pdev->dev.of_node;
	if (of_property_match_string(np, "memory-region-names", "firmware") != 0 ||
	    of_property_match_string(np, "memory-region-names", "share") != 1)
		return -EINVAL;
	inspect = devm_kzalloc(&pdev->dev, sizeof(*inspect), GFP_KERNEL);
	if (!inspect)
		return -ENOMEM;
	ret = ccci_scp_inspect_reservation(np, 0, &inspect->firmware);
	if (ret)
		return ret;
	ret = ccci_scp_inspect_reservation(np, 1, &inspect->share);
	if (ret)
		return ret;
	if ((inspect->firmware.base < inspect->share.base + inspect->share.size &&
	     inspect->share.base < inspect->firmware.base + inspect->firmware.size) ||
	    (inspect->firmware.base < tcm.base + tcm.size &&
	     tcm.base < inspect->firmware.base + inspect->firmware.size) ||
	    (inspect->share.base < tcm.base + tcm.size &&
	     tcm.base < inspect->share.base + inspect->share.size))
		return -ERANGE;
	inspect->top = syscon_regmap_lookup_by_phandle(np, "mediatek,topckgen");
	if (IS_ERR(inspect->top))
		return PTR_ERR(inspect->top);
	inspect->core[0] = maps[0];
	inspect->dev = &pdev->dev;
	inspect->core[1] = maps[1];
	inspect->header = maps[2] + CCCI_SCP_REGION_OFFSET;
	inspect->tcm = maps[2];
	inspect->lock = lock;
	inspect->check = check;
	inspect->context = context;
	/* No callback, TOPCK read, SCP/TCM read, SMC or pin in this constructor. */
	*result = inspect;
	return 0;
}
