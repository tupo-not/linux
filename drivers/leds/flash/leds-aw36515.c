// SPDX-License-Identifier: GPL-2.0
/*
 * WIP: flashlight is NOT functional yet — the register sequence and HWEN
 * polarity were reverse-engineered from the stock vendor module and have
 * not been confirmed to light the LED on hardware.  Do not depend on this.
 *
 * Minimal driver for the Awinic AW36515 camera/torch flash controller.
 *
 * The stock vendor driver (flashlights-aw36515.ko) registers an I2C client
 * "mediatek,strobe_main" at 0x63 and toggles HWEN on GPIO12.  This driver
 * replicates only the observed init and on/off register accesses, with the
 * current level and enable bits exposed as module parameters so they can be
 * tuned safely without rebuilding.
 */
#include <linux/i2c.h>
#include <linux/leds.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/gpio/consumer.h>

#define AW36515_REG_ENABLE	0x01
#define AW36515_REG_CH1_LEVEL	0x03
#define AW36515_REG_CH2_LEVEL	0x04
#define AW36515_REG_INIT8	0x08

static u8 init8_val = 0x0f;
module_param(init8_val, byte, 0644);
MODULE_PARM_DESC(init8_val, "value written to register 0x08 at probe (stock: 0x0f)");

static u8 enable_ch1 = 0x01;
module_param(enable_ch1, byte, 0644);
MODULE_PARM_DESC(enable_ch1, "enable bit for channel 1 in register 0x01");

static u8 enable_ch2 = 0x02;
module_param(enable_ch2, byte, 0644);
MODULE_PARM_DESC(enable_ch2, "enable bit for channel 2 in register 0x01");

/* Stock torch sequence (aw36515_set_level): reg 0x05 and 0x03 are the
 * shared torch current settings; bit0/bit1 of reg 0x01 select the channel.
 */
static u8 torch_reg5 = 0x7f;
module_param(torch_reg5, byte, 0644);
MODULE_PARM_DESC(torch_reg5, "value for register 0x05 (stock maximum: 0x7f)");

static u8 torch_reg3 = 0x4c;
module_param(torch_reg3, byte, 0644);
MODULE_PARM_DESC(torch_reg3, "value for register 0x03 (stock maximum: 0x4c)");

struct aw36515 {
	struct i2c_client *client;
	struct gpio_desc *hwen;
	struct mutex lock;
	struct led_classdev led[2];
	u8 enable;
};

static void aw36515_set(struct aw36515 *aw, int ch, int level)
{
	u8 bit = ch ? enable_ch2 : enable_ch1;
	int val;

	mutex_lock(&aw->lock);

	if (level) {
		/* Stock torch setup: shared current regs, then channel enable. */
		i2c_smbus_write_byte_data(aw->client, 0x05, torch_reg5);
		i2c_smbus_write_byte_data(aw->client, 0x03, torch_reg3);
	}

	val = i2c_smbus_read_byte_data(aw->client, AW36515_REG_ENABLE);
	if (val >= 0) {
		u8 next = level ? (val | bit) : (val & ~bit);

		if (next != val)
			i2c_smbus_write_byte_data(aw->client,
						  AW36515_REG_ENABLE, next);
		aw->enable = next;
	}

	if (aw->hwen)
		gpiod_set_value_cansleep(aw->hwen, aw->enable ? 1 : 0);

	mutex_unlock(&aw->lock);
}

static int aw36515_set_ch1(struct led_classdev *led,
			   enum led_brightness value)
{
	struct aw36515 *aw = container_of(led, struct aw36515, led[0]);

	aw36515_set(aw, 0, value);
	return 0;
}

static int aw36515_set_ch2(struct led_classdev *led,
			   enum led_brightness value)
{
	struct aw36515 *aw = container_of(led, struct aw36515, led[1]);

	aw36515_set(aw, 1, value);
	return 0;
}

static int aw36515_probe(struct i2c_client *client)
{
	struct aw36515 *aw;
	int ret;

	if (!i2c_check_functionality(client->adapter, I2C_FUNC_SMBUS_BYTE_DATA))
		return -ENODEV;

	aw = devm_kzalloc(&client->dev, sizeof(*aw), GFP_KERNEL);
	if (!aw)
		return -ENOMEM;

	aw->client = client;
	mutex_init(&aw->lock);
	i2c_set_clientdata(client, aw);

	aw->hwen = devm_gpiod_get_optional(&client->dev, "hwen", GPIOD_OUT_LOW);
	if (IS_ERR(aw->hwen))
		return PTR_ERR(aw->hwen);

	/* Stock init: reg 0x08 = 0x0f, then all outputs off. */
	i2c_smbus_write_byte_data(client, AW36515_REG_INIT8, init8_val);
	i2c_smbus_write_byte_data(client, AW36515_REG_ENABLE, 0);

	aw->led[0].name = "white:flash-ch1";
	aw->led[1].name = "white:flash-ch2";
	aw->led[0].max_brightness = 1;
	aw->led[1].max_brightness = 1;
	aw->led[0].brightness_set_blocking = aw36515_set_ch1;
	aw->led[1].brightness_set_blocking = aw36515_set_ch2;

	ret = devm_led_classdev_register(&client->dev, &aw->led[0]);
	if (ret)
		return ret;
	ret = devm_led_classdev_register(&client->dev, &aw->led[1]);
	if (ret)
		return ret;

	dev_info(&client->dev, "AW36515 ready (init8=0x%02x, bits=%02x/%02x, torch=%02x/%02x)\n",
		 init8_val, enable_ch1, enable_ch2, torch_reg5, torch_reg3);
	return 0;
}

static const struct of_device_id aw36515_of_match[] = {
	{ .compatible = "mediatek,strobe_main" },
	{ }
};
MODULE_DEVICE_TABLE(of, aw36515_of_match);

static const struct i2c_device_id aw36515_id[] = {
	{ "strobe_main", 0 },
	{ }
};
MODULE_DEVICE_TABLE(i2c, aw36515_id);

static struct i2c_driver aw36515_driver = {
	.driver = {
		.name = "aw36515",
		.of_match_table = aw36515_of_match,
	},
	.probe = aw36515_probe,
	.id_table = aw36515_id,
};
module_i2c_driver(aw36515_driver);

MODULE_DESCRIPTION("Awinic AW36515 flashlight driver");
MODULE_LICENSE("GPL v2");
