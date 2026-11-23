// SPDX-License-Identifier: GPL-2.0-only
/* Framework dependency check only; no modem power or CCIF operations. */

#include <linux/clk.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/platform_device.h>
#include <linux/regulator/consumer.h>
#include <linux/slab.h>

static int ccci_resources_probe(struct platform_device *pdev)
{
	static const char * const supplies[] = {
		"md-vmodem", "md-vnr", "md-vsram", "md-vdigrf",
	};
	struct clk_bulk_data *clocks;
	struct regulator *supply;
	struct device *dev = &pdev->dev;
	char property[32];
	int i, ret;

	clocks = devm_kcalloc(dev, 4, sizeof(*clocks), GFP_KERNEL);
	if (!clocks)
		return -ENOMEM;
	clocks[0].id = "ccif-ap";
	clocks[1].id = "ccif-md";
	clocks[2].id = "ccif1-ap";
	clocks[3].id = "ccif1-md";

	for (i = 0; i < ARRAY_SIZE(supplies); i++) {
		snprintf(property, sizeof(property), "%s-supply", supplies[i]);
		if (!of_property_present(dev->of_node, property))
			return dev_err_probe(dev, -EINVAL, "missing %s\n", property);
		/* Required here; the optional API prevents a dummy fallback. */
		supply = devm_regulator_get_optional(dev, supplies[i]);
		if (IS_ERR(supply))
			return dev_err_probe(dev, PTR_ERR(supply), "unavailable %s\n",
					     supplies[i]);
	}

	ret = devm_clk_bulk_get(dev, 4, clocks);
	if (ret)
		return dev_err_probe(dev, ret, "ordinary CCIF clocks unavailable\n");

	dev_info(dev, "four supply and four clock references acquired; modem/SCP not started\n");
	return 0;
}

static const struct of_device_id ccci_resources_match[] = {
	{ .compatible = "realme,op6893-modem-resources" },
	{}
};
MODULE_DEVICE_TABLE(of, ccci_resources_match);

static struct platform_driver ccci_resources_driver = {
	.probe = ccci_resources_probe,
	.driver = {
		.name = "op6893-ccci-resources",
		.of_match_table = ccci_resources_match,
	},
};
module_platform_driver(ccci_resources_driver);

MODULE_DESCRIPTION("op6893 modem supply and ordinary CCIF clock reference check");
MODULE_LICENSE("GPL");
