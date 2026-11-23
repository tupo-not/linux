// SPDX-License-Identifier: GPL-2.0-only
/* Explicit bounded SCP observations, not READY or a remoteproc driver. */
#include <linux/clk.h>
#include <linux/io.h>
#include <linux/mfd/syscon.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/of.h>
#include <linux/of_irq.h>
#include <linux/of_reserved_mem.h>
#include <linux/platform_device.h>
#include <linux/regmap.h>
#include <linux/sysfs.h>
#include <linux/unaligned.h>

#include "ccci-scp-memory.h"
#include "ccci-scp-protocol.h"

struct ccci_scp_observer {
	struct regmap *top;
	struct clk *bootstrap;
	void __iomem *core0;
	void __iomem *header;
	void __iomem *pending[CCCI_SCP_MAX_MBOXES];
	u32 known_rx[CCCI_SCP_MAX_MBOXES];
	/* Serialize explicit SCP observation samples; no other SCP actor is enabled. */
	struct mutex lock;
	phys_addr_t firmware_base;
	phys_addr_t firmware_size;
	phys_addr_t share_base;
	phys_addr_t share_size;
};

static int ccci_scp_reservation(struct device_node *np, unsigned int index,
				phys_addr_t *base, phys_addr_t *size)
{
	struct device_node *node, *parent;
	struct reserved_mem *rmem;
	int ret = -EINVAL;

	node = of_parse_phandle(np, "memory-region", index);
	if (!node)
		return -EINVAL;
	parent = of_get_parent(node);
	/* Accept only this board's local reserved-memory phandles. */
	if (!parent || !of_node_name_eq(parent, "reserved-memory") ||
	    !of_device_is_available(node) ||
	    !of_property_present(node, "no-map") ||
	    of_property_present(node, "reusable"))
		goto out;
	rmem = of_reserved_mem_lookup(node);
	if (!rmem || !rmem->base || !rmem->size ||
	    rmem->base > U32_MAX || rmem->size - 1 > U32_MAX - rmem->base)
		goto out;
	*base = rmem->base;
	*size = rmem->size;
	ret = 0;
out:
	of_node_put(parent);
	of_node_put(node);
	return ret;
}

static ssize_t state_show(struct device *dev, struct device_attribute *attr,
			  char *buf)
{
	struct ccci_scp_observer *observer = dev_get_drvdata(dev);
	unsigned int before, after;
	int ret;

	/* CLK_CFG_0 read only: scp_sel bits 16..18, active-high gate bit 23. */
	ret = regmap_read(observer->top, 0x10, &before);
	if (ret)
		return ret;
	ret = regmap_read(observer->top, 0x10, &after);
	if (ret)
		return ret;
	return sysfs_emit(buf,
		"selector=%u gated=%u samples_equal=%u firmware_base=%pa firmware_size=%pa share_base=%pa share_size=%pa scp_access=0 ready_known=0\n",
		(after >> 16) & 7, !!(after & BIT(23)),
		!((before ^ after) & (GENMASK(18, 16) | BIT(23))),
		&observer->firmware_base, &observer->firmware_size,
		&observer->share_base, &observer->share_size);
}
static DEVICE_ATTR_RO(state);

#if IS_ENABLED(CONFIG_OP6893_CCCI_SCP_BOOTSTRAP_HOLD)
static ssize_t bootstrap_show(struct device *dev, struct device_attribute *attr,
			      char *buf)
{
	struct ccci_scp_observer *observer = dev_get_drvdata(dev);
	unsigned long rate;

	mutex_lock(&observer->lock);
	rate = clk_get_rate(observer->bootstrap);
	mutex_unlock(&observer->lock);
	if (rate != 26000000)
		return -EAGAIN;
	return sysfs_emit(buf,
		"consumer_hold=1 provider_critical=1 rate=%lu spm_vote_owned=0 ready_known=0\n",
		rate);
}
static DEVICE_ATTR_RO(bootstrap);
#endif

/* Same-board vendor RV definitions; no reset writes or debug-PC reads. */
struct ccci_scp_core0_sample {
	u32 clr;
	u32 set;
	u32 status;
};

static int ccci_scp_core0_read(struct ccci_scp_observer *observer,
			       struct ccci_scp_core0_sample *sample)
{
	struct ccci_scp_core0_sample first, second;
	unsigned int before, after;
	int ret;

	if (!observer->core0)
		return -EOPNOTSUPP;
	ret = regmap_read(observer->top, 0x10, &before);
	if (ret)
		return ret;
	/* Only the inherited parent0 (26M), ungated bootstrap condition. */
	if (before & (GENMASK(18, 16) | BIT(23)))
		return -EAGAIN;
	first.clr = readl(observer->core0) & BIT(0);
	first.set = readl(observer->core0 + 4) & BIT(0);
	first.status = readl(observer->core0 + 0x70) & GENMASK(1, 0);
	second.clr = readl(observer->core0) & BIT(0);
	second.set = readl(observer->core0 + 4) & BIT(0);
	second.status = readl(observer->core0 + 0x70) & GENMASK(1, 0);
	ret = regmap_read(observer->top, 0x10, &after);
	if (ret)
		return ret;
	if (after & (GENMASK(18, 16) | BIT(23)))
		return -EAGAIN;
	if (first.clr != second.clr || first.set != second.set ||
	    first.status != second.status)
		return -EAGAIN;
	*sample = second;
	return 0;
}

static ssize_t core0_show(struct device *dev, struct device_attribute *attr,
			  char *buf)
{
	struct ccci_scp_observer *observer = dev_get_drvdata(dev);
	struct ccci_scp_core0_sample sample;
	int ret;

	mutex_lock(&observer->lock);
	ret = ccci_scp_core0_read(observer, &sample);
	mutex_unlock(&observer->lock);
	if (ret)
		return ret;
	/* SET/CLR aliases are readbacks, not an inferred reset/READY state. */
	return sysfs_emit(buf,
		"reset_clr_bit=%u reset_set_bit=%u core_gated=%u core_halt=%u samples_equal=1 ready_known=0\n",
		sample.clr, sample.set, !!(sample.status & BIT(0)),
		!!(sample.status & BIT(1)));
}
static DEVICE_ATTR_RO(core0);

#if IS_ENABLED(CONFIG_OP6893_CCCI_SCP_TCM_OBSERVE)
/* Read only aligned words within the exact accepted header, never TCM code. */
static int ccci_scp_header_copy(void __iomem *header, u8 *buffer, size_t *length)
{
	u8 first[CCCI_SCP_REGION_BYTES], second[CCCI_SCP_REGION_BYTES];
	u32 bytes;
	unsigned int offset;

	*length = 0;
	memset(buffer, 0, CCCI_SCP_REGION_BYTES);
	bytes = readl(header + 28);
	if (bytes != CCCI_SCP_REGION_LEGACY_BYTES && bytes != CCCI_SCP_REGION_BYTES)
		return -EPROTONOSUPPORT;
	for (offset = 0; offset < bytes; offset += sizeof(u32))
		put_unaligned_le32(readl(header + offset), first + offset);
	for (offset = 0; offset < bytes; offset += sizeof(u32))
		put_unaligned_le32(readl(header + offset), second + offset);
	if (readl(header + 28) != bytes ||
	    get_unaligned_le32(first + 28) != bytes ||
	    get_unaligned_le32(second + 28) != bytes ||
	    memcmp(first, second, bytes))
		return -EAGAIN;
	memcpy(buffer, first, bytes);
	*length = bytes;
	return 0;
}

static int ccci_scp_header_read(struct ccci_scp_observer *observer,
				struct ccci_scp_region *region, size_t *length)
{
	struct ccci_scp_core0_sample before, after;
	u8 buffer[CCCI_SCP_REGION_BYTES];
	int ret, post_ret;

	memset(region, 0, sizeof(*region));
	*length = 0;
	if (!observer->header)
		return -EOPNOTSUPP;
	ret = ccci_scp_core0_read(observer, &before);
	if (ret)
		return ret;
	ret = ccci_scp_header_copy(observer->header, buffer, length);
	post_ret = ccci_scp_core0_read(observer, &after);
	if (post_ret || before.clr != after.clr || before.set != after.set ||
	    before.status != after.status) {
		*length = 0;
		return post_ret ? post_ret : -EAGAIN;
	}
	if (ret)
		return ret;
	ret = ccci_scp_decode_region(buffer, *length, region);
	if (ret)
		*length = 0;
	return ret;
}

static bool ccci_scp_spans_overlap(const struct ccci_scp_span *a,
				   const struct ccci_scp_span *b)
{
	return a->size && b->size && a->base < b->base + b->size &&
		b->base < a->base + a->size;
}

static ssize_t header_show(struct device *dev, struct device_attribute *attr,
			   char *buf)
{
	struct ccci_scp_observer *observer = dev_get_drvdata(dev);
	struct ccci_scp_span firmware = {
		.base = observer->firmware_base, .size = observer->firmware_size,
	};
	struct ccci_scp_span share = {
		.base = observer->share_base, .size = observer->share_size,
	};
	struct ccci_scp_span mapping;
	struct ccci_scp_region region;
	size_t length;
	bool mapping_allowed, disjoint, backup_matches;
	int ret;

	mutex_lock(&observer->lock);
	ret = ccci_scp_header_read(observer, &region, &length);
	mutex_unlock(&observer->lock);
	if (ret)
		return ret;
	mapping_allowed = !ccci_scp_dram_span(&region, &firmware, &mapping);
	disjoint = mapping_allowed &&
		!ccci_scp_spans_overlap(&region.loader, &region.firmware) &&
		!ccci_scp_spans_overlap(&region.loader, &mapping) &&
		!ccci_scp_spans_overlap(&region.firmware, &mapping) &&
		!ccci_scp_spans_overlap(&region.loader, &share) &&
		!ccci_scp_spans_overlap(&region.firmware, &share) &&
		!ccci_scp_spans_overlap(&mapping, &share);
	backup_matches = region.dram.size &&
		region.backup_start == region.dram.base +
		(((region.dram.size + 1023) & ~1023ULL) * 2);
	return sysfs_emit(buf,
		"header_bytes=%zu samples_equal=1 loader_bytes=%llu loader_allowed=%u firmware_bytes=%llu firmware_allowed=%u dram_bytes=%llu dram_allowed=%u dram_mapping_bytes=%llu dram_mapping_allowed=%u payloads_disjoint=%u backup_matches=%u regdump_bytes=%llu regdump_in_firmware=%u regdump_in_share=%u ready_known=0\n",
		length, region.loader.size,
		ccci_scp_span_allowed(&firmware, &region.loader),
		region.firmware.size,
		ccci_scp_span_allowed(&firmware, &region.firmware),
		region.dram.size, ccci_scp_span_allowed(&firmware, &region.dram),
		mapping.size, mapping_allowed, disjoint, backup_matches,
		region.regdump.size,
		ccci_scp_span_allowed(&firmware, &region.regdump),
		ccci_scp_span_allowed(&share, &region.regdump));
}
static DEVICE_ATTR_RO(header);
#endif

#if IS_ENABLED(CONFIG_OP6893_CCCI_SCP_PENDING_OBSERVE)
static int ccci_scp_irq_spec(struct device_node *node, const char *name, u32 box)
{
	struct of_phandle_args spec;
	int index, ret;

	index = of_property_match_string(node, "interrupt-names", name);
	if (index < 0)
		return index;
	/* Parse only: no Linux IRQ mapping, request, wake or unmask. */
	ret = of_irq_parse_one(node, index, &spec);
	if (ret)
		return ret;
	ret = -EINVAL;
	if (of_device_is_compatible(spec.np, "arm,gic-v3") &&
	    of_device_is_available(spec.np) && spec.args_count == 3 &&
	    spec.args[0] == 0 && spec.args[1] == 436 + box && spec.args[2] == 4)
		ret = 0;
	of_node_put(spec.np);
	return ret;
}

static int ccci_scp_table(struct device *dev, const char *name, u32 stride,
			  u32 **cells)
{
	int count, ret;

	count = of_property_count_u32_elems(dev->of_node, name);
	if (count <= 0 || count % stride || count > stride * CCCI_SCP_MAX_CHANNELS)
		return -EINVAL;
	*cells = devm_kcalloc(dev, count, sizeof(u32), GFP_KERNEL);
	if (!*cells)
		return -ENOMEM;
	ret = of_property_read_u32_array(dev->of_node, name, *cells, count);
	return ret ? ret : count;
}

static int ccci_scp_pending_setup(struct platform_device *pdev,
				  struct ccci_scp_observer *observer)
{
	struct device *dev = &pdev->dev;
	struct ccci_scp_layout *layout;
	struct resource *res;
	u32 *tx, *rx, i;
	char name[16];
	int tx_count, rx_count, ret;

	layout = devm_kzalloc(dev, sizeof(*layout), GFP_KERNEL);
	if (!layout)
		return -ENOMEM;
	tx_count = ccci_scp_table(dev, "mediatek,send-table", 3, &tx);
	if (tx_count < 0)
		return tx_count;
	rx_count = ccci_scp_table(dev, "mediatek,recv-table", 4, &rx);
	if (rx_count < 0)
		return rx_count;
	ret = ccci_scp_build_layout(tx, tx_count, rx, rx_count,
				    CCCI_SCP_MAX_MBOXES, layout);
	if (ret)
		return ret;
	for (i = 0; i < layout->rx_count; i++)
		observer->known_rx[layout->rx[i].mbox] |= BIT(layout->rx[i].irq);
	for (i = 0; i < CCCI_SCP_MAX_MBOXES; i++) {
		snprintf(name, sizeof(name), "mbox%u", i);
		ret = ccci_scp_irq_spec(dev->of_node, name, i);
		if (ret)
			return ret;
		snprintf(name, sizeof(name), "mbox%u-clr", i);
		res = platform_get_resource_byname(pdev, IORESOURCE_MEM, name);
		if (!res || res->start != 0x107fb10c + i * 0x1000 ||
		    resource_size(res) != sizeof(u32))
			return -EINVAL;
		observer->pending[i] = devm_ioremap_resource(dev, res);
		if (IS_ERR(observer->pending[i]))
			return PTR_ERR(observer->pending[i]);
	}
	return 0;
}

static int ccci_scp_pending_read(struct ccci_scp_observer *observer, u32 *pending)
{
	struct ccci_scp_core0_sample before, after;
	u32 first[CCCI_SCP_MAX_MBOXES], second[CCCI_SCP_MAX_MBOXES];
	unsigned int i;
	int ret;

	memset(pending, 0, sizeof(first));
	for (i = 0; i < CCCI_SCP_MAX_MBOXES; i++)
		if (!observer->pending[i])
			return -EOPNOTSUPP;
	ret = ccci_scp_core0_read(observer, &before);
	if (ret)
		return ret;
	for (i = 0; i < CCCI_SCP_MAX_MBOXES; i++)
		first[i] = readl(observer->pending[i]);
	for (i = 0; i < CCCI_SCP_MAX_MBOXES; i++)
		second[i] = readl(observer->pending[i]);
	ret = ccci_scp_core0_read(observer, &after);
	if (ret)
		return ret;
	if (before.clr != after.clr || before.set != after.set ||
	    before.status != after.status || memcmp(first, second, sizeof(first)))
		return -EAGAIN;
	memcpy(pending, second, sizeof(second));
	return 0;
}

static ssize_t pending_show(struct device *dev, struct device_attribute *attr,
			    char *buf)
{
	struct ccci_scp_observer *observer = dev_get_drvdata(dev);
	u32 pending[CCCI_SCP_MAX_MBOXES];
	unsigned int i;
	ssize_t length = 0;
	int ret;

	mutex_lock(&observer->lock);
	ret = ccci_scp_pending_read(observer, pending);
	mutex_unlock(&observer->lock);
	if (ret)
		return ret;
	for (i = 0; i < CCCI_SCP_MAX_MBOXES; i++)
		length += sysfs_emit_at(buf, length,
			"mbox%u=0x%08x known%u=0x%08x unknown%u=0x%08x ",
			i, pending[i], i, observer->known_rx[i],
			i, pending[i] & ~observer->known_rx[i]);
	length += sysfs_emit_at(buf, length,
		"samples_equal=1 core0_stable=1 irq_specs=5 irq_requested=0 ack_writes=0 ready_known=0\n");
	return length;
}
static DEVICE_ATTR_RO(pending);
#endif

static struct attribute *ccci_scp_observer_attrs[] = {
	&dev_attr_state.attr,
	&dev_attr_core0.attr,
#if IS_ENABLED(CONFIG_OP6893_CCCI_SCP_BOOTSTRAP_HOLD)
	&dev_attr_bootstrap.attr,
#endif
#if IS_ENABLED(CONFIG_OP6893_CCCI_SCP_PENDING_OBSERVE)
	&dev_attr_pending.attr,
#endif
#if IS_ENABLED(CONFIG_OP6893_CCCI_SCP_TCM_OBSERVE)
	&dev_attr_header.attr,
#endif
	NULL,
};
ATTRIBUTE_GROUPS(ccci_scp_observer);

static int ccci_scp_observer_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct ccci_scp_observer *observer;
	struct resource *res, header;
	int ret;

	if (!of_machine_is_compatible("realme,op6893"))
		return -ENODEV;
	observer = devm_kzalloc(dev, sizeof(*observer), GFP_KERNEL);
	if (!observer)
		return -ENOMEM;
	ret = ccci_scp_reservation(dev->of_node, 0, &observer->firmware_base,
				   &observer->firmware_size);
	if (ret)
		return dev_err_probe(dev, ret, "firmware reservation unavailable\n");
	ret = ccci_scp_reservation(dev->of_node, 1, &observer->share_base,
				   &observer->share_size);
	if (ret)
		return dev_err_probe(dev, ret, "share reservation unavailable\n");
	if (observer->firmware_base < observer->share_base + observer->share_size &&
	    observer->share_base < observer->firmware_base + observer->firmware_size)
		return dev_err_probe(dev, -EINVAL, "SCP reservations overlap\n");
	/* Existing shared syscon; no second TOPCK platform owner. */
	observer->top =
		syscon_regmap_lookup_by_phandle(dev->of_node, "mediatek,topckgen");
	if (IS_ERR(observer->top))
		return dev_err_probe(dev, PTR_ERR(observer->top), "TOPCK unavailable\n");
	mutex_init(&observer->lock);
	if (IS_ENABLED(CONFIG_OP6893_CCCI_SCP_CORE0_OBSERVE)) {
		res = platform_get_resource(pdev, IORESOURCE_MEM, 0);
		if (!res || res->start != 0x10730000 ||
		    resource_size(res) != 0x1000)
			return dev_err_probe(dev, -EINVAL, "invalid core0 resource\n");
		observer->core0 = devm_ioremap_resource(dev, res);
		if (IS_ERR(observer->core0))
			return PTR_ERR(observer->core0);
	}
	if (IS_ENABLED(CONFIG_OP6893_CCCI_SCP_TCM_OBSERVE)) {
		res = platform_get_resource(pdev, IORESOURCE_MEM, 1);
		if (!res || res->start != 0x10500000 ||
		    resource_size(res) != 0x180000)
			return dev_err_probe(dev, -EINVAL, "invalid TCM resource\n");
		/* Claim/map only the 72-byte header window, not the whole TCM. */
		header = *res;
		header.start += CCCI_SCP_REGION_OFFSET;
		header.end = header.start + CCCI_SCP_REGION_BYTES - 1;
		observer->header = devm_ioremap_resource(dev, &header);
		if (IS_ERR(observer->header))
			return PTR_ERR(observer->header);
	}
#if IS_ENABLED(CONFIG_OP6893_CCCI_SCP_PENDING_OBSERVE)
	ret = ccci_scp_pending_setup(pdev, observer);
	if (ret)
		return dev_err_probe(dev, ret, "mailbox observation unavailable\n");
#endif
	if (IS_ENABLED(CONFIG_OP6893_CCCI_SCP_BOOTSTRAP_HOLD)) {
		observer->bootstrap = devm_clk_get_enabled(dev, "scp");
		if (IS_ERR(observer->bootstrap))
			return dev_err_probe(dev, PTR_ERR(observer->bootstrap),
					     "SCP bootstrap clock unavailable\n");
		if (clk_get_rate(observer->bootstrap) != 26000000)
			return dev_err_probe(dev, -EBUSY, "SCP bootstrap changed\n");
	}
	platform_set_drvdata(pdev, observer);
	dev_info(dev, "explicit read-only observation; no SCP start or READY\n");
	return 0;
}

static const struct of_device_id ccci_scp_observer_match[] = {
	{ .compatible = "realme,op6893-scp-observe" },
	{}
};
MODULE_DEVICE_TABLE(of, ccci_scp_observer_match);

static struct platform_driver ccci_scp_observer_driver = {
	.probe = ccci_scp_observer_probe,
	.driver = {
		.name = "op6893-scp-observe",
		.of_match_table = ccci_scp_observer_match,
		.dev_groups = ccci_scp_observer_groups,
	},
};
module_platform_driver(ccci_scp_observer_driver);

MODULE_DESCRIPTION("op6893 AP-side SCP clock and reservation observer");
MODULE_LICENSE("GPL");
