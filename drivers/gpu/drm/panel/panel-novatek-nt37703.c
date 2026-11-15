// SPDX-License-Identifier: GPL-2.0-only
/*
 * Novatek NT37703 AMOLED panels
 *
 * The command, timing, DSC and power data in this driver was recovered from
 * the stock Android 12 module
 * nt37703_fhdp_dsi_vdo_tm_panda_120hz_x6710.ko (build 241029V638).
 */

#include <linux/backlight.h>
#include <linux/delay.h>
#include <linux/fb.h>
#include <linux/gpio/consumer.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/regulator/consumer.h>

#include <drm/display/drm_dsc.h>
#include <drm/drm_connector.h>
#include <drm/drm_mipi_dsi.h>
#include <drm/drm_modes.h>
#include <drm/drm_panel.h>

#include <video/mipi_display.h>

#include "../mediatek/mediatek_v2/mtk_panel_ext.h"
#include "../mediatek/mediatek_v2/mtk_drm_graphics_base.h"

#define NT37703_WIDTH		1080
#define NT37703_HEIGHT		2400
#define NT37703_WIDTH_MM	69
#define NT37703_HEIGHT_MM	154
#define NT37703_DATA_RATE	900
#define NT37703_PLL_CLK		450
#define NT37703_CMD_DELAY	0xfffc
#define NT37703_MAX_PARAMS	21

enum nt37703_supply_id {
	NT37703_DVDD,
	NT37703_DVDD2,
	NT37703_VCI,
	NT37703_NUM_SUPPLIES,
};

struct nt37703_cmd {
	u16 cmd;
	u8 len;
	u8 data[NT37703_MAX_PARAMS];
};

struct nt37703 {
	struct drm_panel panel;
	struct mipi_dsi_device *dsi;
	struct gpio_desc *reset_gpio;
	struct regulator_bulk_data supplies[NT37703_NUM_SUPPLIES];
	struct backlight_device *backlight;
	struct drm_dsc_config dsc;
	bool rail_enabled[NT37703_NUM_SUPPLIES];
	bool prepared;
	bool enabled;
	int error;
};

#define NT37703_CMD(_cmd, ...) { \
	.cmd = (_cmd), \
	.len = sizeof((u8[]){ __VA_ARGS__ }), \
	.data = { __VA_ARGS__ }, \
}
#define NT37703_CMD0(_cmd) { .cmd = (_cmd) }
#define NT37703_DELAY(_ms) { .cmd = NT37703_CMD_DELAY, .len = (_ms) }

static const struct nt37703_cmd nt37703_init_prefix[] = {
	NT37703_CMD(0xf0, 0x55, 0xaa, 0x52, 0x08, 0x00),
	NT37703_CMD(0x6f, 0x06),
	NT37703_CMD(0xb5, 0x29),
	NT37703_CMD(0x6f, 0x07),
	NT37703_CMD(0xb5, 0x2b, 0x23, 0x00, 0x32),
	NT37703_CMD(0x6f, 0x0c),
	NT37703_CMD(0xb5, 0x2e, 0x1e, 0x00, 0x33),
	NT37703_CMD(0x6f, 0x11),
	NT37703_CMD(0xb5, 0x23, 0x23, 0x23),
	NT37703_CMD(0x6f, 0x11),
	NT37703_CMD(0xb2, 0x0f, 0x14, 0x5f, 0x29, 0x03, 0x05),
	NT37703_CMD(0xe4, 0x01, 0x3f, 0x03, 0xff, 0x00, 0x67),
	NT37703_CMD(0x6f, 0x0d),
	NT37703_CMD(0xc0, 0x05, 0x10, 0x00, 0x1a, 0x00, 0x00, 0x00, 0x98),
	NT37703_CMD(0x6f, 0x67),
	NT37703_CMD(0xc0, 0x09, 0x80, 0x0c, 0x78, 0x13, 0x00, 0x00, 0x18, 0xf0),
	NT37703_CMD(0xb2, 0x09),
	NT37703_CMD(0x6f, 0x05),
	NT37703_CMD(0xb2, 0x10, 0x10),
	NT37703_CMD(0xff, 0xaa, 0x55, 0xa5, 0x80),
	NT37703_CMD(0x6f, 0x15),
	NT37703_CMD(0xf8, 0x01, 0xc1),
	NT37703_CMD(0xf0, 0x55, 0xaa, 0x52, 0x08, 0x01),
	NT37703_CMD(0xcc, 0x00),
	NT37703_CMD(0x6f, 0x17),
	NT37703_CMD(0xea, 0x02),
	NT37703_CMD(0xf0, 0x55, 0xaa, 0x52, 0x08, 0x04),
	NT37703_CMD(0x6f, 0x08),
	NT37703_CMD(0xb5, 0x04),
	NT37703_CMD(0x8d, 0x00, 0x00, 0x04, 0x37, 0x00, 0x00, 0x05, 0x9f),
	NT37703_CMD(0xff, 0xaa, 0x55, 0xa5, 0x81),
	NT37703_CMD(0x6f, 0x3c),
	NT37703_CMD(0xf5, 0x81),
	NT37703_CMD(0xff, 0xaa, 0x55, 0xa5, 0x80),
	NT37703_CMD(0x6f, 0x14),
	NT37703_CMD(0xf4, 0x40),
	NT37703_CMD(0xf0, 0x55, 0xaa, 0x52, 0x08, 0x00),
	NT37703_CMD(0xb9, 0x00, 0x04, 0x38),
	NT37703_CMD(0xbd, 0x09, 0x60),
	NT37703_CMD(0x2a, 0x00, 0x00, 0x04, 0x37),
	NT37703_CMD(0x2b, 0x00, 0x00, 0x09, 0x5f),
	NT37703_CMD(0x90, 0x03, 0x43),
	NT37703_CMD(0x91, 0x89, 0x28, 0x00, 0x14, 0xc2, 0x00, 0x02, 0x0e,
		      0x01, 0xe8, 0x00, 0x07, 0x05, 0x0e, 0x05, 0x16, 0x10, 0xf0),
	NT37703_CMD(0x93, 0x89, 0x28, 0x00, 0x14, 0xc2, 0x00, 0x02, 0x0e,
		      0x01, 0xe8, 0x00, 0x07, 0x05, 0x0e, 0x05, 0x16, 0x10, 0xf0),
	NT37703_CMD(0x95, 0x89, 0x28, 0x00, 0x14, 0xc2, 0x00, 0x02, 0x0e,
		      0x01, 0xe8, 0x00, 0x07, 0x05, 0x0e, 0x05, 0x16, 0x10, 0xf0),
	NT37703_CMD(0x97, 0x89, 0x28, 0x00, 0x14, 0xc2, 0x00, 0x02, 0x0e,
		      0x01, 0xe8, 0x00, 0x07, 0x05, 0x0e, 0x05, 0x16, 0x10, 0xf0),
	NT37703_CMD(0x6f, 0x0a),
	NT37703_CMD(0x51, 0xff),
	NT37703_CMD(0x6f, 0x00),
	NT37703_CMD(0x87, 0x20),
	NT37703_CMD(0x6f, 0x00),
	NT37703_CMD(0x88, 0x01),
	NT37703_CMD(0x6f, 0x01),
	NT37703_CMD(0x88, 0x02, 0x1c),
	NT37703_CMD(0x6f, 0x03),
	NT37703_CMD(0x88, 0x08, 0x7b),
	NT37703_CMD(0xf0, 0x55, 0xaa, 0x52, 0x08, 0x00),
	NT37703_CMD(0x6f, 0x00),
	NT37703_CMD(0xdf, 0x08),
	NT37703_CMD(0x6f, 0x01),
	NT37703_CMD(0xdf, 0x04),
	NT37703_CMD(0x6f, 0x13),
	NT37703_CMD(0xdf, 0x02, 0x1b, 0x08, 0x7a, 0x00, 0x00, 0x00, 0x00,
		      0x01, 0xbe, 0x08, 0x1d, 0x02, 0x79, 0x08, 0xd8),
	NT37703_CMD(0x6f, 0x32),
	NT37703_CMD(0xdf, 0x1a),
	NT37703_CMD(0x6f, 0x36),
	NT37703_CMD(0xdf, 0x10),
	NT37703_CMD(0x6f, 0x37),
	NT37703_CMD(0xdf, 0x00),
	NT37703_CMD(0x6f, 0x38),
	NT37703_CMD(0xdf, 0x00, 0x12),
	NT37703_CMD(0x6f, 0x34),
	NT37703_CMD(0xdf, 0x23),
	NT37703_CMD(0xf0, 0x55, 0xaa, 0x52, 0x08, 0x01),
	NT37703_CMD(0x6f, 0x01),
	NT37703_CMD(0xde, 0x26),
	NT37703_CMD(0x6f, 0x06),
	NT37703_CMD(0xde, 0x00, 0x26),
	NT37703_CMD(0xf0, 0x55, 0xaa, 0x52, 0x08, 0x02),
	NT37703_CMD(0x6f, 0x00),
	NT37703_CMD(0xd1, 0x0a, 0xc2),
	NT37703_CMD(0x6f, 0x02),
	NT37703_CMD(0xd1, 0x09, 0x60),
	NT37703_CMD(0x6f, 0x04),
	NT37703_CMD(0xd1, 0x0c, 0x44),
	NT37703_CMD(0xf0, 0x55, 0xaa, 0x52, 0x08, 0x05),
	NT37703_CMD(0xcb, 0x33, 0x33, 0x33, 0x33, 0x33, 0x33, 0x33),
	NT37703_CMD(0xf0, 0x55, 0xaa, 0x52, 0x08, 0x07),
	NT37703_CMD(0xc0, 0x87, 0x01, 0x08),
	NT37703_CMD(0x6f, 0x03),
	NT37703_CMD(0xc0, 0x00, 0x00, 0x00, 0x00, 0x00, 0x50, 0x05, 0x00,
		      0x00, 0x00, 0x00, 0x00, 0x00, 0x00),
	NT37703_CMD(0x6f, 0x00),
	NT37703_CMD(0xd7, 0x21, 0x00, 0x21, 0xc9, 0x21, 0xc9, 0x00, 0x00,
		      0x3f, 0xe7, 0x73, 0xf6, 0x3f, 0xe7, 0x73, 0xf6, 0x80,
		      0x04, 0x75, 0x6f, 0xd1),
	NT37703_CMD(0x6f, 0x15),
	NT37703_CMD(0xd7, 0x1b, 0x12, 0xbe, 0x1b, 0x88, 0x1d, 0x7a, 0x33,
		      0xf0, 0xa3, 0xa3, 0x21, 0xc9, 0x00, 0x00, 0x00),
	NT37703_CMD(0x6f, 0x25),
	NT37703_CMD(0xd7, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
		      0x00, 0x1b, 0x40, 0x40, 0x40, 0x40, 0x40, 0x40),
	NT37703_CMD(0x6f, 0x00),
	NT37703_CMD(0xd8, 0x2d, 0x00, 0x21, 0xc9, 0x21, 0xc9, 0x00, 0x00,
		      0x3f, 0xe7, 0x73, 0xf6, 0x00, 0x00, 0x00, 0x00, 0x00,
		      0x00, 0x00, 0x00, 0x00),
	NT37703_CMD(0x6f, 0x15),
	NT37703_CMD(0xd8, 0x41, 0x12, 0xbe, 0x1b, 0x88, 0x7b, 0xd8, 0x33,
		      0xc0, 0xa3, 0x5d, 0x00, 0x00, 0x00, 0x00, 0x00),
	NT37703_CMD(0x6f, 0x25),
	NT37703_CMD(0xd8, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
		      0x00, 0x41, 0x40, 0x40, 0x40, 0x40, 0x40, 0x40),
	NT37703_CMD(0x6f, 0x00),
	NT37703_CMD(0xd9, 0x27, 0x00, 0x21, 0xc9, 0x21, 0xc9, 0x00, 0x00,
		      0x00, 0x00, 0x00, 0x00, 0x3f, 0xe7, 0x73, 0xf6, 0x80,
		      0x00, 0x00, 0x00, 0x00),
	NT37703_CMD(0x6f, 0x15),
	NT37703_CMD(0xd9, 0x1b, 0x22, 0x1c, 0x79, 0x88, 0x1d, 0x7a, 0x33,
		      0x30, 0x5d, 0xa3, 0x00, 0x00, 0x00, 0x00, 0x00),
	NT37703_CMD(0x6f, 0x25),
	NT37703_CMD(0xd9, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
		      0x00, 0x1b, 0x40, 0x40, 0x40, 0x40, 0x40, 0x40),
	NT37703_CMD(0x6f, 0x00),
	NT37703_CMD(0xea, 0x2b, 0x00, 0x21, 0xc9, 0x21, 0xc9, 0x00, 0x00,
		      0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x7f,
		      0xfb, 0x8a, 0x90, 0x2f),
	NT37703_CMD(0x6f, 0x15),
	NT37703_CMD(0xea, 0x41, 0x22, 0x1c, 0x79, 0x88, 0x7b, 0xd8, 0x33,
		      0x0f, 0x5d, 0x5d, 0xde, 0x37, 0x00, 0x00, 0x00),
	NT37703_CMD(0x6f, 0x25),
	NT37703_CMD(0xea, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
		      0x00, 0x41, 0x40, 0x40, 0x40, 0x40, 0x40, 0x40),
};

static const u16 nt37703_gamma[] = {
	0x0009, 0x001d, 0x0023, 0x0033, 0x003e, 0x0049, 0x0057, 0x0065,
	0x0073, 0x0081, 0x008f, 0x009c, 0x0104, 0x013d, 0x017d, 0x023d,
	0x02ed, 0x039d, 0x044d, 0x04fd, 0x060f, 0x0721, 0x0833, 0x0945,
	0x0a57, 0x0b69, 0x0c7d, 0x0cfd, 0x0d7d, 0x0dfd, 0x0e7d, 0x0efd,
	0x0f7d, 0x0fff, 0x0000, 0x0100, 0x0188, 0x01a0, 0x01de, 0x020c,
	0x023c, 0x0278, 0x029c, 0x02c4, 0x02ea, 0x0310, 0x0334, 0x0404,
	0x0464, 0x04d4, 0x05c8, 0x066c, 0x0728, 0x07ea, 0x08b8, 0x0944,
	0x09e4, 0x0a80, 0x0b20, 0x0bc4, 0x0c66, 0x0d14, 0x0d80, 0x0dec,
	0x0e58, 0x0ec0, 0x0f2e, 0x0f98, 0x0fff,
};

static const struct nt37703_cmd nt37703_init_tail[] = {
	NT37703_CMD(0x53, 0x28),
	NT37703_CMD(0x8b, 0x80),
	NT37703_CMD(0x3b, 0x00, 0x0c, 0x00, 0x14),
	NT37703_CMD(0x35, 0x00),
	NT37703_CMD(0x51, 0x00, 0x00),
	NT37703_CMD(0x2f, 0x00),
	NT37703_CMD(0x26, 0x00),
	NT37703_CMD0(0x11),
	NT37703_DELAY(120),
	NT37703_CMD0(0x29),
	NT37703_DELAY(20),
};

static const struct nt37703_cmd nt37703_suspend[] = {
	NT37703_CMD0(0x28),
	NT37703_DELAY(10),
	NT37703_CMD0(0x10),
	NT37703_DELAY(100),
};

static inline struct nt37703 *panel_to_nt37703(struct drm_panel *panel)
{
	return container_of(panel, struct nt37703, panel);
}

static int nt37703_write(struct nt37703 *ctx, const void *data, size_t len)
{
	const u8 *buf = data;
	ssize_t ret;

	if (ctx->error < 0)
		return ctx->error;

	if (buf[0] < 0xb0)
		ret = mipi_dsi_dcs_write_buffer(ctx->dsi, data, len);
	else
		ret = mipi_dsi_generic_write(ctx->dsi, data, len);
	if (ret < 0) {
		ctx->error = ret;
		dev_err(&ctx->dsi->dev, "DSI write failed for command 0x%02x: %zd\n",
			buf[0], ret);
		return ret;
	}

	return 0;
}

static void nt37703_push_table(struct nt37703 *ctx,
				const struct nt37703_cmd *table, size_t count)
{
	u8 tx[NT37703_MAX_PARAMS + 1];
	size_t i;

	for (i = 0; i < count && ctx->error >= 0; i++) {
		if (table[i].cmd == NT37703_CMD_DELAY) {
			msleep(table[i].len);
			continue;
		}

		tx[0] = table[i].cmd;
		memcpy(&tx[1], table[i].data, table[i].len);
		nt37703_write(ctx, tx, table[i].len + 1);
	}
}

static void nt37703_panel_init(struct nt37703 *ctx)
{
	u8 page[] = { 0xf0, 0x55, 0xaa, 0x52, 0x08, 0x08 };
	u8 index[] = { 0x6f, 0x00 };
	u8 value[] = { 0xb0, 0x00, 0x00 };
	unsigned int i;

	nt37703_push_table(ctx, nt37703_init_prefix,
			     ARRAY_SIZE(nt37703_init_prefix));
	nt37703_write(ctx, page, sizeof(page));

	for (i = 0; i < ARRAY_SIZE(nt37703_gamma) && ctx->error >= 0; i++) {
		index[1] = i * 2;
		value[1] = nt37703_gamma[i] >> 8;
		value[2] = nt37703_gamma[i];
		nt37703_write(ctx, index, sizeof(index));
		nt37703_write(ctx, value, sizeof(value));
	}

	nt37703_push_table(ctx, nt37703_init_tail,
			     ARRAY_SIZE(nt37703_init_tail));
}

static int nt37703_enable_rail(struct nt37703 *ctx, unsigned int id, int uv)
{
	struct regulator *reg = ctx->supplies[id].consumer;
	int ret;

	if (ctx->rail_enabled[id])
		return 0;
	if (uv) {
		ret = regulator_set_voltage(reg, uv, uv);
		if (ret)
			return ret;
	}
	ret = regulator_enable(reg);
	if (!ret)
		ctx->rail_enabled[id] = true;
	return ret;
}

static void nt37703_disable_rail(struct nt37703 *ctx, unsigned int id)
{
	if (!ctx->rail_enabled[id])
		return;
	if (!regulator_disable(ctx->supplies[id].consumer))
		ctx->rail_enabled[id] = false;
}

static int nt37703_power_on(struct nt37703 *ctx)
{
	int ret;

	ret = nt37703_enable_rail(ctx, NT37703_DVDD, 1800000);
	if (ret)
		return ret;
	usleep_range(2000, 2500);

	ret = nt37703_enable_rail(ctx, NT37703_DVDD2, 1350000);
	if (ret)
		return ret;
	usleep_range(2000, 2500);

	ret = nt37703_enable_rail(ctx, NT37703_VCI, 0);
	if (ret)
		return ret;
	msleep(12);
	return 0;
}

static void nt37703_power_off(struct nt37703 *ctx)
{
	gpiod_set_value_cansleep(ctx->reset_gpio, 0);
	usleep_range(2000, 2500);
	nt37703_disable_rail(ctx, NT37703_VCI);
	usleep_range(2000, 2500);
	nt37703_disable_rail(ctx, NT37703_DVDD2);
	/* The stock driver intentionally keeps DVDD enabled. */
}

static void nt37703_reset(struct nt37703 *ctx)
{
	gpiod_set_value_cansleep(ctx->reset_gpio, 1);
	usleep_range(5000, 5500);
	gpiod_set_value_cansleep(ctx->reset_gpio, 0);
	usleep_range(5000, 5500);
	gpiod_set_value_cansleep(ctx->reset_gpio, 1);
	msleep(15);
}

static int nt37703_prepare(struct drm_panel *panel)
{
	struct nt37703 *ctx = panel_to_nt37703(panel);
	int ret;

	if (ctx->prepared)
		return 0;

	ret = nt37703_power_on(ctx);
	if (ret)
		return ret;

	ctx->error = 0;
	nt37703_reset(ctx);
	nt37703_panel_init(ctx);
	ret = ctx->error;
	if (ret < 0) {
		nt37703_power_off(ctx);
		return ret;
	}

	ctx->prepared = true;
	return 0;
}

static int nt37703_unprepare(struct drm_panel *panel)
{
	struct nt37703 *ctx = panel_to_nt37703(panel);

	if (!ctx->prepared)
		return 0;

	ctx->error = 0;
	nt37703_push_table(ctx, nt37703_suspend, ARRAY_SIZE(nt37703_suspend));
	nt37703_power_off(ctx);
	ctx->prepared = false;
	ctx->error = 0;
	return 0;
}

static int nt37703_enable(struct drm_panel *panel)
{
	struct nt37703 *ctx = panel_to_nt37703(panel);

	if (ctx->enabled)
		return 0;
	ctx->backlight->props.power = FB_BLANK_UNBLANK;
	backlight_update_status(ctx->backlight);
	ctx->enabled = true;
	return 0;
}

static int nt37703_disable(struct drm_panel *panel)
{
	struct nt37703 *ctx = panel_to_nt37703(panel);

	if (!ctx->enabled)
		return 0;
	ctx->backlight->props.power = FB_BLANK_POWERDOWN;
	backlight_update_status(ctx->backlight);
	ctx->enabled = false;
	return 0;
}

static const struct drm_display_mode nt37703_modes[] = {
	{
		.clock = 339118,
		.hdisplay = 1080,
		.hsync_start = 1104,
		.hsync_end = 1112,
		.htotal = 1162,
		.vdisplay = 2400,
		.vsync_start = 2420,
		.vsync_end = 2422,
		.vtotal = 2432,
		.type = DRM_MODE_TYPE_DRIVER | DRM_MODE_TYPE_PREFERRED,
	}, {
		.clock = 333819,
		.hdisplay = 1080,
		.hsync_start = 1104,
		.hsync_end = 1112,
		.htotal = 1162,
		.vdisplay = 2400,
		.vsync_start = 3180,
		.vsync_end = 3182,
		.vtotal = 3192,
		.type = DRM_MODE_TYPE_DRIVER,
	}, {
		.clock = 339118,
		.hdisplay = 1080,
		.hsync_start = 1104,
		.hsync_end = 1112,
		.htotal = 1162,
		.vdisplay = 2400,
		.vsync_start = 4852,
		.vsync_end = 4854,
		.vtotal = 4864,
		.type = DRM_MODE_TYPE_DRIVER,
	},
};

static int nt37703_get_modes(struct drm_panel *panel,
			      struct drm_connector *connector)
{
	unsigned int i;

	for (i = 0; i < ARRAY_SIZE(nt37703_modes); i++) {
		struct drm_display_mode *mode;

		mode = drm_mode_duplicate(connector->dev, &nt37703_modes[i]);
		if (!mode)
			return -ENOMEM;
		drm_mode_set_name(mode);
		drm_mode_probed_add(connector, mode);
	}

	connector->display_info.width_mm = NT37703_WIDTH_MM;
	connector->display_info.height_mm = NT37703_HEIGHT_MM;
	return ARRAY_SIZE(nt37703_modes);
}

static const struct drm_panel_funcs nt37703_panel_funcs = {
	.prepare = nt37703_prepare,
	.unprepare = nt37703_unprepare,
	.enable = nt37703_enable,
	.disable = nt37703_disable,
	.get_modes = nt37703_get_modes,
};

static unsigned int nt37703_map_brightness(unsigned int level)
{
	level = min(level, 511U);
	if (level <= 255)
		return 3197 * level / 255;
	return (599 * level - 152745) / 255 + 3197;
}

static int nt37703_bl_update_status(struct backlight_device *bl)
{
	struct nt37703 *ctx = bl_get_data(bl);
	unsigned int brightness = backlight_get_brightness(bl);
	unsigned int level = nt37703_map_brightness(brightness);
	u8 tx[] = { 0x51, (level >> 8) & 0x0f, level & 0xff };

	if (!ctx->prepared)
		return 0;
	return nt37703_write(ctx, tx, sizeof(tx));
}

static const struct backlight_ops nt37703_bl_ops = {
	.update_status = nt37703_bl_update_status,
};

static int nt37703_set_backlight_cmdq(void *dsi, dcs_write_gce cb,
				      void *handle, unsigned int brightness)
{
	unsigned int level = nt37703_map_brightness(brightness);
	u8 tx[] = { 0x51, (level >> 8) & 0x0f, level & 0xff };

	if (!cb)
		return -EINVAL;
	cb(dsi, handle, tx, sizeof(tx));
	return 0;
}

#define NT37703_DSC_PARAMS { \
	.enable = 1, .ver = 17, .slice_mode = 1, .rgb_swap = 0, \
	.dsc_cfg = 34, .rct_on = 1, .bit_per_channel = 8, \
	.dsc_line_buf_depth = 9, .bp_enable = 1, .bit_per_pixel = 128, \
	.pic_height = NT37703_HEIGHT, .pic_width = NT37703_WIDTH, \
	.slice_height = 20, .slice_width = 540, .chunk_size = 540, \
	.xmit_delay = 512, .dec_delay = 526, .scale_value = 32, \
	.increment_interval = 488, .decrement_interval = 7, \
	.line_bpg_offset = 12, .nfl_bpg_offset = 1294, \
	.slice_bpg_offset = 1302, .initial_offset = 6144, \
	.final_offset = 4336, .flatness_minqp = 3, .flatness_maxqp = 12, \
	.rc_model_size = 8192, .rc_edge_factor = 6, \
	.rc_quant_incr_limit0 = 11, .rc_quant_incr_limit1 = 11, \
	.rc_tgt_offset_hi = 3, .rc_tgt_offset_lo = 3, \
}

#define NT37703_EXT_PARAMS(_fps, _value) { \
	.pll_clk = NT37703_PLL_CLK, \
	.data_rate = NT37703_DATA_RATE, \
	.cust_esd_check = 1, \
	.esd_check_enable = 1, \
	.lcm_esd_check_table[0] = { \
		.cmd = 0x0a, .count = 1, .para_list = { 0x9c }, \
		.mask_list = { 0xff }, \
	}, \
	.lcm_color_mode = MTK_DRM_COLOR_MODE_DISPLAY_P3, \
	.physical_width_um = 69552, \
	.physical_height_um = 154560, \
	.is_cphy = 0, \
	.dsc_params = NT37703_DSC_PARAMS, \
	.output_mode = MTK_PANEL_DSC_SINGLE_PORT, \
	.dyn_fps = { \
		.switch_en = 1, \
		.vact_timing_fps = (_fps), \
		.dfps_cmd_table[0] = { \
			.src_fps = 0, .cmd_num = 2, \
			.para_list = { 0x2f, (_value) }, \
		}, \
	}, \
}

static struct mtk_panel_params nt37703_ext_120 = NT37703_EXT_PARAMS(120, 0x00);
static struct mtk_panel_params nt37703_ext_90 = NT37703_EXT_PARAMS(90, 0x01);
static struct mtk_panel_params nt37703_ext_60 = NT37703_EXT_PARAMS(60, 0x02);

static struct drm_display_mode *nt37703_get_mode_by_id(
		struct drm_connector *connector, unsigned int id)
{
	struct drm_display_mode *mode;
	unsigned int i = 0;

	list_for_each_entry(mode, &connector->modes, head) {
		if (i++ == id)
			return mode;
	}
	return NULL;
}

static int nt37703_ext_param_set(struct drm_panel *panel,
				 struct drm_connector *connector,
				 unsigned int mode_id)
{
	struct mtk_panel_ext *ext = find_panel_ext(panel);
	struct drm_display_mode *mode = nt37703_get_mode_by_id(connector, mode_id);
	int refresh;

	if (!ext || !mode)
		return -EINVAL;

	refresh = drm_mode_vrefresh(mode);
	if (refresh == 120)
		ext->params = &nt37703_ext_120;
	else if (refresh == 90)
		ext->params = &nt37703_ext_90;
	else if (refresh == 60)
		ext->params = &nt37703_ext_60;
	else
		return -EINVAL;
	return 0;
}

static int nt37703_ext_reset(struct drm_panel *panel, int on)
{
	struct nt37703 *ctx = panel_to_nt37703(panel);

	gpiod_set_value_cansleep(ctx->reset_gpio, on);
	return 0;
}

static struct mtk_panel_funcs nt37703_ext_funcs = {
	.set_backlight_cmdq = nt37703_set_backlight_cmdq,
	.reset = nt37703_ext_reset,
	.ext_param_set = nt37703_ext_param_set,
};

static int nt37703_claim_bootloader_supplies(struct nt37703 *ctx)
{
	unsigned int i;
	int ret;

	for (i = 0; i < NT37703_NUM_SUPPLIES; i++) {
		ret = regulator_enable(ctx->supplies[i].consumer);
		if (ret)
			return ret;
		ctx->rail_enabled[i] = true;
	}
	return 0;
}

static int nt37703_probe(struct mipi_dsi_device *dsi)
{
	struct device *dev = &dsi->dev;
	struct backlight_properties props = {
		.type = BACKLIGHT_RAW,
		.max_brightness = 511,
		.brightness = 511,
	};
	struct nt37703 *ctx;
	bool boot_on;
	int ret;

	ctx = devm_drm_panel_alloc(dev, struct nt37703, panel,
				   &nt37703_panel_funcs,
				   DRM_MODE_CONNECTOR_DSI);
	if (IS_ERR(ctx))
		return PTR_ERR(ctx);

	ctx->dsi = dsi;
	ctx->supplies[NT37703_DVDD].supply = "dvdd";
	ctx->supplies[NT37703_DVDD2].supply = "dvdd2";
	ctx->supplies[NT37703_VCI].supply = "vci";
	ret = devm_regulator_bulk_get(dev, NT37703_NUM_SUPPLIES, ctx->supplies);
	if (ret)
		return dev_err_probe(dev, ret, "failed to get panel supplies\n");

	ctx->reset_gpio = devm_gpiod_get(dev, "reset", GPIOD_ASIS);
	if (IS_ERR(ctx->reset_gpio))
		return dev_err_probe(dev, PTR_ERR(ctx->reset_gpio),
				     "failed to get reset GPIO\n");

	boot_on = of_property_read_bool(dev->of_node, "panel-boot-on");
	if (boot_on) {
		ret = nt37703_claim_bootloader_supplies(ctx);
		if (ret)
			return dev_err_probe(dev, ret,
					     "failed to claim bootloader supplies\n");
		ctx->prepared = true;
		ctx->enabled = true;
	}

	dsi->lanes = 4;
	dsi->format = MIPI_DSI_FMT_RGB888;
	dsi->mode_flags = MIPI_DSI_MODE_VIDEO |
			  MIPI_DSI_MODE_VIDEO_SYNC_PULSE |
			  MIPI_DSI_MODE_NO_EOT_PACKET |
			  MIPI_DSI_CLOCK_NON_CONTINUOUS |
			  MIPI_DSI_MODE_LPM;
	mipi_dsi_set_drvdata(dsi, ctx);

	ctx->panel.prepare_prev_first = true;

	ctx->backlight = devm_backlight_device_register(dev, dev_name(dev), dev,
							ctx, &nt37703_bl_ops,
							&props);
	if (IS_ERR(ctx->backlight))
		return dev_err_probe(dev, PTR_ERR(ctx->backlight),
				     "failed to register backlight\n");
	ctx->panel.backlight = ctx->backlight;
	drm_panel_add(&ctx->panel);

	ret = mipi_dsi_attach(dsi);
	if (ret) {
		drm_panel_remove(&ctx->panel);
		return dev_err_probe(dev, ret, "failed to attach DSI device\n");
	}

	ret = mtk_panel_ext_create(dev, &nt37703_ext_120, &nt37703_ext_funcs,
				   &ctx->panel);
	if (ret) {
		mipi_dsi_detach(dsi);
		drm_panel_remove(&ctx->panel);
		return dev_err_probe(dev, ret, "failed to create MTK panel extension\n");
	}

	ctx->dsc.dsc_version_major = 1;
	ctx->dsc.dsc_version_minor = 1;
	ctx->dsc.slice_height = 20;
	ctx->dsc.slice_width = 540;
	ctx->dsc.slice_count = 2;
	ctx->dsc.bits_per_component = 8;
	ctx->dsc.bits_per_pixel = 8 << 4;
	ctx->dsc.block_pred_enable = true;
	ctx->dsc.pic_width = NT37703_WIDTH;
	ctx->dsc.pic_height = NT37703_HEIGHT;
	ctx->dsc.line_buf_depth = 9;
	dsi->dsc = &ctx->dsc;

	dev_info(dev, "NT37703 panel registered (bootloader handoff: %d)\n",
		 boot_on);
	return 0;
}

static void nt37703_remove(struct mipi_dsi_device *dsi)
{
	struct nt37703 *ctx = mipi_dsi_get_drvdata(dsi);
	int i;

	mipi_dsi_detach(dsi);
	drm_panel_remove(&ctx->panel);
	for (i = NT37703_NUM_SUPPLIES - 1; i >= 0; i--)
		nt37703_disable_rail(ctx, i);
}

static const struct of_device_id nt37703_of_match[] = {
	{ .compatible = "nt37703,fhdp,dsi,vdo,tm,panda,x6710" },
	{ }
};
MODULE_DEVICE_TABLE(of, nt37703_of_match);

static struct mipi_dsi_driver nt37703_driver = {
	.probe = nt37703_probe,
	.remove = nt37703_remove,
	.driver = {
		.name = "panel-novatek-nt37703",
		.of_match_table = nt37703_of_match,
	},
};
module_mipi_dsi_driver(nt37703_driver);

MODULE_AUTHOR("OpenCode NT37703 port");
MODULE_DESCRIPTION("Novatek NT37703 AMOLED panel");
MODULE_LICENSE("GPL");
