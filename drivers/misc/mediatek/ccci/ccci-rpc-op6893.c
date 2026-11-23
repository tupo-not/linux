// SPDX-License-Identifier: GPL-2.0-only
/* Audited saved op6893 board facts; no hardware access or inferred defaults. */
#include <linux/kernel.h>

#include "ccci-rpc-op6893.h"
#include "ccci-rpc-provider.h"

/*
 * Saved ACK FDT: /gpio_usage_mapping, /md1_sim1_hot_plug_eint, /md_attr_node.
 * SHA256: a71de7467be17736865302711eed5ef72e80ba165b842cee900d758d9b7cacb1
 * Independently matched against the preserved DT in the working 6.6 tree.
 * Detailed source hashes, conversions and limitations are documented in
 * Documentation/arch/arm64/op6893-bringup/MODEM-RPC-BOARD-FACTS.md.
 */
static const struct ccci_rpc_named_fact op6893_gpio[] = {
	{ "GPIO_SIM1_HOT_PLUG", 43 },
	{ "GPIO_SIM2_SCLK", 45 },
	{ "GPIO_SIM2_SRST", 46 },
	{ "GPIO_SIM2_SIO", 47 },
	{ "GPIO_SIM1_SIO", 48 },
	{ "GPIO_SIM1_SRST", 49 },
	{ "GPIO_SIM1_SCLK", 50 },
	{ "GPIO_FDD_BAND_SUPPORT_DETECT_1ST_PIN", 63 },
	{ "GPIO_FDD_BAND_SUPPORT_DETECT_2ND_PIN", 64 },
	{ "GPIO_FDD_BAND_SUPPORT_DETECT_3RD_PIN", 65 },
	{ "GPIO_FDD_BAND_SUPPORT_DETECT_4TH_PIN", 66 },
	/* A's port_rpc.c aliases; values are MD-view cells, never Linux GPIO IDs. */
	{ "GPIO_FDD_Band_Support_Detection_1", 63 },
	{ "GPIO_FDD_Band_Support_Detection_2", 64 },
	{ "GPIO_FDD_Band_Support_Detection_3", 65 },
	{ "GPIO_FDD_Band_Support_Detection_4", 66 },
};

static const struct ccci_rpc_eint_fact op6893_eint[] = {
	/* NUM, debounce milliseconds, polarity, sensitivity, socket, dedicated, src. */
	{ "MD1_SIM1_HOT_PLUG_EINT", { 0, 10, 0, 0, 0, 0, 1 } },
};

static const struct ccci_rpc_named_fact op6893_dtsi[] = {
	{ "mediatek,md_product_name_model_id", 20615 },
	{ "mediatek,md_drdi_rf_set_idx", 0 },
	/* Vendor u32 lookup also accepts the compatible string's first BE cell. */
	{ "compatible", 0x6d656469 },
};

static const struct ccci_rpc_provider op6893_provider = {
	.gpio = op6893_gpio,
	.dtsi = op6893_dtsi,
	.eint = op6893_eint,
	.gpio_count = ARRAY_SIZE(op6893_gpio),
	.dtsi_count = ARRAY_SIZE(op6893_dtsi),
	.eint_count = ARRAY_SIZE(op6893_eint),
	.gpio_present = true,
	.dtsi_present = true,
	.eint_present = true,
	/* ADC's name-independent channel query needs a different typed contract. */
};

const struct ccci_rpc_provider *ccci_rpc_op6893_provider(void)
{
	return &op6893_provider;
}
