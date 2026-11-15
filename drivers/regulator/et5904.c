// SPDX-License-Identifier: GPL-2.0-only
/*
 * ET5904 four-channel LDO regulator
 *
 * Register layout and voltage ranges were recovered from the X6710 stock
 * et5904.ko.  The chip is register-compatible with the common four-output
 * camera LDOs: voltage selectors live at 0x03..0x06 and enable bits at 0x0e.
 */

#include <linux/bitops.h>
#include <linux/i2c.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/regmap.h>
#include <linux/regulator/driver.h>
#include <linux/regulator/machine.h>
#include <linux/regulator/of_regulator.h>

#define ET5904_REG_PRODUCT_ID	0x00
#define ET5904_REG_VSEL_BASE	0x03
#define ET5904_REG_ENABLE	0x0e
#define ET5904_MAX_REGISTER	0x0f
#define ET5904_NUM_LDOS		4

static const struct linear_range et5904_dvdd_ranges[] = {
	REGULATOR_LINEAR_RANGE(600000, 0, 255, 6000),
};

static const struct linear_range et5904_avdd_ranges[] = {
	REGULATOR_LINEAR_RANGE(1200000, 0, 255, 12500),
};

static const struct regulator_ops et5904_ops = {
	.list_voltage = regulator_list_voltage_linear_range,
	.map_voltage = regulator_map_voltage_linear_range,
	.get_voltage_sel = regulator_get_voltage_sel_regmap,
	.set_voltage_sel = regulator_set_voltage_sel_regmap,
	.enable = regulator_enable_regmap,
	.disable = regulator_disable_regmap,
	.is_enabled = regulator_is_enabled_regmap,
};

static int et5904_channel_from_name(const char *name)
{
	const char *ldo = strstr(name, "LDO");

	if (!ldo || ldo[3] < '1' || ldo[3] > '4')
		return -EINVAL;

	return ldo[3] - '1';
}

static int et5904_register_ldo(struct device *dev, struct regmap *regmap,
				struct device_node *node, unsigned long *registered)
{
	struct regulator_config config = { };
	struct regulator_init_data *init_data;
	struct regulator_desc *desc;
	struct regulator_dev *rdev;
	const char *name;
	int id, ret;

	ret = of_property_read_string(node, "regulator-name", &name);
	if (ret)
		return dev_err_probe(dev, ret, "%pOF: missing regulator-name\n",
				     node);

	id = et5904_channel_from_name(name);
	if (id < 0)
		return dev_err_probe(dev, id, "%s: cannot determine LDO channel\n",
				     name);
	if (test_and_set_bit(id, registered))
		return dev_err_probe(dev, -EINVAL, "duplicate LDO channel %d\n", id + 1);

	desc = devm_kzalloc(dev, sizeof(*desc), GFP_KERNEL);
	if (!desc)
		return -ENOMEM;

	desc->name = devm_kstrdup(dev, name, GFP_KERNEL);
	if (!desc->name)
		return -ENOMEM;
	desc->of_match = node->name;
	desc->id = id;
	desc->owner = THIS_MODULE;
	desc->type = REGULATOR_VOLTAGE;
	desc->ops = &et5904_ops;
	desc->n_voltages = 256;
	desc->vsel_reg = ET5904_REG_VSEL_BASE + id;
	desc->vsel_mask = 0xff;
	desc->enable_reg = ET5904_REG_ENABLE;
	desc->enable_mask = BIT(id);
	if (id < 2) {
		desc->linear_ranges = et5904_dvdd_ranges;
		desc->n_linear_ranges = ARRAY_SIZE(et5904_dvdd_ranges);
	} else {
		desc->linear_ranges = et5904_avdd_ranges;
		desc->n_linear_ranges = ARRAY_SIZE(et5904_avdd_ranges);
	}

	init_data = of_get_regulator_init_data(dev, node, desc);
	if (!init_data)
		return dev_err_probe(dev, -EINVAL, "%s: missing regulator data\n",
				     name);
	init_data->constraints.valid_ops_mask |= REGULATOR_CHANGE_STATUS |
						 REGULATOR_CHANGE_VOLTAGE;

	config.dev = dev;
	config.regmap = regmap;
	config.of_node = node;
	config.init_data = init_data;

	rdev = devm_regulator_register(dev, desc, &config);
	if (IS_ERR(rdev))
		return dev_err_probe(dev, PTR_ERR(rdev),
				     "failed to register %s\n", name);

	return 0;
}

static int et5904_probe(struct i2c_client *client)
{
	static const struct regmap_config regmap_config = {
		.reg_bits = 8,
		.val_bits = 8,
		.max_register = ET5904_MAX_REGISTER,
	};
	struct device *dev = &client->dev;
	struct device_node *regulators, *child;
	unsigned long registered = 0;
	struct regmap *regmap;
	unsigned int product_id;
	int ret;

	regmap = devm_regmap_init_i2c(client, &regmap_config);
	if (IS_ERR(regmap))
		return dev_err_probe(dev, PTR_ERR(regmap), "failed to create regmap\n");

	ret = regmap_read(regmap, ET5904_REG_PRODUCT_ID, &product_id);
	if (ret)
		return dev_err_probe(dev, ret, "failed to read product ID\n");

	regulators = of_get_child_by_name(dev->of_node, "regulators");
	if (!regulators)
		return dev_err_probe(dev, -ENODEV, "missing regulators node\n");

	for_each_available_child_of_node(regulators, child) {
		ret = et5904_register_ldo(dev, regmap, child, &registered);
		if (ret) {
			of_node_put(child);
			of_node_put(regulators);
			return ret;
		}
	}
	of_node_put(regulators);

	dev_info(dev, "ET5904 product ID 0x%02x, %lu regulators registered\n",
		 product_id, hweight_long(registered));
	return 0;
}

static const struct of_device_id et5904_of_match[] = {
	{ .compatible = "tran,et5904" },
	{ }
};
MODULE_DEVICE_TABLE(of, et5904_of_match);

static const struct i2c_device_id et5904_i2c_ids[] = {
	{ "et5904" },
	{ }
};
MODULE_DEVICE_TABLE(i2c, et5904_i2c_ids);

static struct i2c_driver et5904_driver = {
	.driver = {
		.name = "et5904-regulator",
		.of_match_table = et5904_of_match,
	},
	.probe = et5904_probe,
	.id_table = et5904_i2c_ids,
};
module_i2c_driver(et5904_driver);

MODULE_AUTHOR("OpenCode X6710 port");
MODULE_DESCRIPTION("ET5904 four-channel LDO regulator");
MODULE_LICENSE("GPL");
