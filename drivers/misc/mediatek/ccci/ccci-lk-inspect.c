// SPDX-License-Identifier: GPL-2.0-only
/* LK metadata validation; optional same-board MD platform owner. */
#include <linux/io.h>
#include <linux/ioport.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/of_address.h>
#include <linux/platform_device.h>
#include <linux/slab.h>

#include "ccci-lk.h"
#include "ccci-md-platform.h"
#include "ccci-smem.h"

static int ccci_lk_get_region(struct device_node *node, const char *name,
			      struct ccci_lk_region *region)
{
	struct device_node *memory, *parent;
	struct resource resource;
	int index, ret;

	index = of_property_match_string(node, "memory-region-names", name);
	if (index < 0)
		return index;
	memory = of_parse_phandle(node, "memory-region", index);
	if (!memory)
		return -EINVAL;
	parent = of_get_parent(memory);
	ret = -EINVAL;
	if (!parent || !of_node_name_eq(parent, "reserved-memory") ||
	    of_n_addr_cells(memory) != 2 || of_n_size_cells(memory) != 2 ||
	    !of_property_read_bool(memory, "no-map") ||
	    of_property_read_bool(memory, "reusable") ||
	    of_property_count_u32_elems(memory, "reg") != 4)
		goto out;
	ret = of_address_to_resource(memory, 0, &resource);
	if (ret)
		goto out;
	region->base = resource.start;
	region->size = resource_size(&resource);
	if (!ccci_lk_contains(region, region->base, region->size))
		ret = -ERANGE;
out:
	of_node_put(parent);
	of_node_put(memory);
	return ret;
}

static int ccci_lk_inspect_probe(struct platform_device *pdev)
{
	struct ccci_lk_region tags, noncached, cached;
	struct ccci_lk_header header;
	struct ccci_lk_layout layout;
	struct ccci_smem_layout *smem = NULL;
	struct device *dev = &pdev->dev;
	struct ccci_lk_tag *workspace = NULL;
	struct device_node *chosen;
	const void *property;
	void *mapping, *snapshot = NULL, *second;
	const char *stage = "regions";
	int ret, len;

	ret = ccci_lk_get_region(dev->of_node, "tags", &tags);
	if (ret)
		goto out;
	ret = ccci_lk_get_region(dev->of_node, "noncached", &noncached);
	if (ret)
		goto out;
	ret = ccci_lk_get_region(dev->of_node, "cached", &cached);
	if (ret)
		goto out;
	stage = "header";
	chosen = of_find_node_by_path("/chosen");
	if (!chosen) {
		ret = -ENODATA;
		goto out;
	}
	property = of_get_property(chosen, CCCI_LK_PROPERTY, &len);
	ret = property ? ccci_lk_decode_header(property, len, &header) : -ENODATA;
	of_node_put(chosen);
	if (ret)
		goto out;
	if (!ccci_lk_contains(&tags, header.base, header.size)) {
		ret = -ERANGE;
		goto out;
	}
	stage = "snapshot";
	workspace = kcalloc(CCCI_LK_MAX_TAGS, sizeof(*workspace), GFP_KERNEL);
	snapshot = kmalloc(header.size * 2, GFP_KERNEL);
	if (!workspace || !snapshot) {
		ret = -ENOMEM;
		goto out;
	}
	if (!devm_request_mem_region(dev, header.base, header.size, dev_name(dev))) {
		ret = -EBUSY;
		goto out;
	}
	/* Only the bounded LK tag buffer; never map MD firmware or shared memory. */
	mapping = memremap(header.base, header.size, MEMREMAP_WB);
	if (!mapping) {
		ret = -ENOMEM;
		goto out;
	}
	second = snapshot + header.size;
	memcpy(snapshot, mapping, header.size);
	memcpy(second, mapping, header.size);
	memunmap(mapping);
	/* This detects differing reads, not atomicity or DMA coherency. */
	if (memcmp(snapshot, second, header.size)) {
		ret = -EAGAIN;
		goto out;
	}
	stage = "tags/layout";
	ret = ccci_lk_parse(&header, snapshot, header.size, &noncached, &cached,
			    workspace, &layout);
	if (ret)
		goto out;
	if (IS_ENABLED(CONFIG_OP6893_CCCI_SMEM)) {
		stage = "subregions";
		smem = kzalloc_obj(*smem);
		if (!smem) {
			ret = -ENOMEM;
			goto out;
		}
		ret = ccci_smem_parse(&header, snapshot, header.size, &noncached, &cached,
				      workspace, smem);
		if (ret)
			goto out;
		dev_info(dev, "SMEM geometry valid: users=%u nc_bytes=%llu cache_bytes=%llu cache_gaps=%u; not mapped or cleared\n",
			 smem->count, smem->noncached.size, smem->cached.size,
			 smem->cache_gap_bytes);
	}
	dev_info(dev, "LK metadata valid: version=%u tags=%u bytes=%u ccb=%u; modem not started\n",
		 header.version, header.count, header.size, layout.ccb_present);
	/*
	 * Names and sizes only: payloads stay uninterpreted and are wiped below.
	 * The scalar tags are printed as one u32 because they are counts/sizes the
	 * later stages must compare against, not opaque payload.
	 */
	for (u32 i = 0; i < header.count; i++) {
		const char *tag = (const char *)snapshot + workspace[i].offset;
		const u8 *value = snapshot + workspace[i].data;

		if (workspace[i].size == 4 && ccci_lk_scalar_tag(tag))
			dev_info(dev, "LK tag[%u]=%s size=4 value=%u\n", i, tag,
				 get_unaligned_le32(value));
		else
			dev_info(dev, "LK tag[%u]=%s size=%u\n", i, tag,
				 workspace[i].size);
	}
	if (IS_ENABLED(CONFIG_OP6893_CCCI_MD_PLATFORM) &&
	    (of_device_is_compatible(dev->of_node, "realme,op6893-modem-platform") ||
	     of_device_is_compatible(dev->of_node, "infinix,x6710-modem-platform"))) {
		stage = "platform-owner";
		ret = ccci_md_platform_create(pdev, smem);
	}
out:
	/* Do not retain or expose unknown/private payloads, even on parse failure. */
	kfree_sensitive(snapshot);
	kfree_sensitive(workspace);
	kfree_sensitive(smem);
	if (ret)
		dev_err(dev, "LK metadata inspection failed: stage=%s error=%d\n", stage, ret);
	return ret;
}

static const struct of_device_id ccci_lk_inspect_match[] = {
	{ .compatible = "realme,op6893-modem-handoff" },
#ifdef CONFIG_OP6893_CCCI_MD_PLATFORM
	{ .compatible = "realme,op6893-modem-platform" },
	{ .compatible = "infinix,x6710-modem-platform" },
#endif
	{}
};
MODULE_DEVICE_TABLE(of, ccci_lk_inspect_match);

static void ccci_lk_inspect_shutdown(struct platform_device *pdev)
{
	if (IS_ENABLED(CONFIG_OP6893_CCCI_MD_PLATFORM) &&
	    (of_device_is_compatible(pdev->dev.of_node, "realme,op6893-modem-platform") ||
	     of_device_is_compatible(pdev->dev.of_node, "infinix,x6710-modem-platform")))
		ccci_md_platform_shutdown(pdev);
}

static struct platform_driver ccci_lk_inspect_driver = {
	.probe = ccci_lk_inspect_probe,
	.shutdown = ccci_lk_inspect_shutdown,
	.driver = {
		.name = "op6893-ccci-lk",
		.of_match_table = ccci_lk_inspect_match,
		.suppress_bind_attrs = true,
	},
};
module_platform_driver(ccci_lk_inspect_driver);

MODULE_DESCRIPTION("Read-only op6893 LK modem metadata inspection");
MODULE_LICENSE("GPL");
