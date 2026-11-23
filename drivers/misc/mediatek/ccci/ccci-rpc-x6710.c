// SPDX-License-Identifier: GPL-2.0-only
/*
 * Audited X6710 board facts from the stock Android FDT (device.dts).
 * No hardware access, no inferred defaults, no dynamic GPIO/IRQ numbering.
 *
 * Provenance (device.dts):
 *   /md_attr_node:
 *     mediatek,md_product_name_model_id = <0x0f0f0f0f>
 *     mediatek,md_drdi_rf_set_idx       = <0x0f0f0f0f>
 *   /soc/gpio (compatible = "mediatek,gpio_usage_mapping"):
 *     every property is <pinctrl phandle, MD-view cell, 0>; the MD value is
 *     cell index 1 (see MODEM-RPC-BOARD-FACTS.md, vendor get_gpio_id_from_dt).
 *   /soc/MD1_SIM1_HOT_PLUG_EINT and /soc/MD1_SIM2_HOT_PLUG_EINT:
 *     NUM=interrupts[0], DEBOUNCE=debounce[1]/1000,
 *     POLA=!!(interrupts[1]&5), SENS=!!(interrupts[1]&3),
 *     SOCKET=sockettype[1], DEDICATED=dedicated[1], SRCPIN=src_pin[1].
 *
 * The X6710 mapping node has only the seven SIM GPIO properties; there are no
 * FDD-band-detect pins, so the four FDD names and their aliases are absent.
 */
#include <linux/kernel.h>

#include "ccci-rpc-provider.h"
#include "ccci-rpc-x6710.h"

static const struct ccci_rpc_named_fact x6710_gpio[] = {
	{ "GPIO_SIM1_HOT_PLUG", 0x2b },
	{ "GPIO_SIM2_SCLK",     0x2d },
	{ "GPIO_SIM2_SRST",     0x2e },
	{ "GPIO_SIM2_SIO",      0x2f },
	{ "GPIO_SIM1_SIO",      0x30 },
	{ "GPIO_SIM1_SRST",     0x31 },
	{ "GPIO_SIM1_SCLK",     0x32 },
};

/* NUM, debounce ms, polarity, sensitivity, socket, dedicated, src. */
static const struct ccci_rpc_eint_fact x6710_eint[] = {
	/* SIM1: interrupts <0 8>, debounce <0 10000>, src_pin <0 1>. */
	{ "MD1_SIM1_HOT_PLUG_EINT", { 0, 10, 0, 0, 0, 0, 1 } },
	/* SIM2: interrupts <1 4>, debounce <1 10000>, src_pin <1 2>. */
	{ "MD1_SIM2_HOT_PLUG_EINT", { 1, 10, 1, 0, 0, 0, 2 } },
};

static const struct ccci_rpc_named_fact x6710_dtsi[] = {
	{ "mediatek,md_product_name_model_id", 0x0f0f0f0f },
	{ "mediatek,md_drdi_rf_set_idx",       0x0f0f0f0f },
	/* Vendor u32 lookup also accepts the compatible string's first BE cell. */
	{ "compatible", 0x6d656469 },
};

static const struct ccci_rpc_provider x6710_provider = {
	.gpio = x6710_gpio,
	.dtsi = x6710_dtsi,
	.eint = x6710_eint,
	.gpio_count = ARRAY_SIZE(x6710_gpio),
	.dtsi_count = ARRAY_SIZE(x6710_dtsi),
	.eint_count = ARRAY_SIZE(x6710_eint),
	.gpio_present = true,
	.dtsi_present = true,
	.eint_present = true,
};

const struct ccci_rpc_provider *ccci_rpc_x6710_provider(void)
{
	return &x6710_provider;
}
