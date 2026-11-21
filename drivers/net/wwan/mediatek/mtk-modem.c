// SPDX-License-Identifier: GPL-2.0-only
/* Mainline driver for the MediaTek MT6893 integrated LTE modem (MD1).
 *
 * Phase 2: CCIF transport + WWAN control ports.
 *
 * Remaining work before the modem is fully usable:
 *   - power-on / reset sequencing via the MT6315 supplies and SPM
 *   - md1img firmware download into the modem's memory
 *   - HS1/HS2 + challenge/response boot handshake
 *   - DPMAIF data path for the network interface
 *
 * The reserved memory regions and all clock/IRQ/supply wiring are already
 * described by the DT.  This driver deliberately only brings up the control
 * path; it does not yet start the modem or load firmware.
 */

#include <linux/clk.h>
#include <linux/interrupt.h>
#include <linux/io.h>
#include <linux/mod_devicetable.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/of_address.h>
#include <linux/of_irq.h>
#include <linux/of_reserved_mem.h>
#include <linux/platform_device.h>
#include <linux/slab.h>

#include "ccif.h"
#include "port_proxy.h"
#include "mtk_modem.h"

/* Default CCIF queue geometry for MD1 generation (>= 6295).
 * Values are from the vendor driver (rx_queue_buffer_size /
 * tx_queue_buffer_size in ccci_hif_ccif.c). */
static const u32 md1_rx_sizes[16] = {
	80 * 1024, 80 * 1024, 40 * 1024, 80 * 1024,
	20 * 1024, 20 * 1024, 64 * 1024, 0,
	8 * 1024, 0, 0, 0, 0, 0, 0, 0,
};

static const u32 md1_tx_sizes[16] = {
	128 * 1024, 40 * 1024, 8 * 1024, 40 * 1024,
	20 * 1024, 20 * 1024, 64 * 1024, 0,
	8 * 1024, 0, 0, 0, 0, 0, 0, 0,
};

/* AT command port: channel 12 (TX) / 10 (RX), queue 5.
 * Filesystem port: channel 15 (TX) / 14 (RX), queue 4 (raw). */
static const struct mtk_modem_port_desc md1_ports[] = {
	{
		.type = WWAN_PORT_AT,
		.tx_queue = 5, .rx_queue = 5,
		.tx_channel = CCCI_UART2_TX, .rx_channel = CCCI_UART2_RX,
		.preserve_header = false,
		.name = "at",
	},
	{
		.type = WWAN_PORT_MTKFS,
		.tx_queue = 4, .rx_queue = 4,
		.tx_channel = CCCI_FS_TX, .rx_channel = CCCI_FS_RX,
		.preserve_header = true,
		.name = "fs",
	},
};

static const struct mtk_modem_soc_data md1_data = {
	.ports = md1_ports,
	.n_ports = ARRAY_SIZE(md1_ports),
	.nr_queues = 16,
	.sram_size = 0x5560000, /* ap_md_c_smem reserved region */
};

/* Only the CCIF clocks we need; DPMAIF clocks are intentionally skipped. */
static const char *const ccif_clocks[] = {
	"infra-ccif-ap", "infra-ccif-md",
	"infra-ccif1-ap", "infra-ccif1-md",
	"infra-ccif2-ap", "infra-ccif2-md",
	"infra-ccif4-md", "infra-ccif5-md",
};

struct mtk_modem {
	struct device *dev;
	const struct mtk_modem_soc_data *data;
	struct ccif ccif;
	struct port_proxy proxy;
	struct clk *clks[ARRAY_SIZE(ccif_clocks)];
	int nclks;
	int irq;
	void __iomem *ccif_ap;
	void __iomem *ccif_md;
	void __iomem *sram;
};

static irqreturn_t mtk_modem_irq(int irq, void *arg)
{
	struct mtk_modem *modem = arg;
	u8 queue;

	ccif_handle_irq(&modem->ccif);
	port_proxy_poll(&modem->proxy, &queue);
	return IRQ_HANDLED;
}

/* Read the modem boot status words from the MD PLL/status registers.
 * Returns 0 if the modem looks alive, -ENODATA otherwise. */
static int mtk_modem_check_alive(struct mtk_modem *modem)
{
	/* Placeholder: the vendor driver reads md_boot_stats via the MD PLL
	 * register window.  We do not map that window yet, so we report
	 * "unknown" and rely on firmware loading later. */
	dev_info(modem->dev, "modem boot status check not implemented yet\n");
	return -ENODATA;
}

static int mtk_modem_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct mtk_modem *modem;
	struct resource *res;
	struct resource sram_res;
	int error;
	int i;

	modem = devm_kzalloc(dev, sizeof(*modem), GFP_KERNEL);
	if (!modem)
		return -ENOMEM;
	modem->dev = dev;
	modem->data = of_device_get_match_data(dev);
	if (!modem->data)
		return -EINVAL;

	/* reg[0] = AP CCIF, reg[1] = MD CCIF (per vendor FDT). */
	res = platform_get_resource(pdev, IORESOURCE_MEM, 0);
	modem->ccif_ap = devm_ioremap_resource(dev, res);
	if (IS_ERR(modem->ccif_ap))
		return PTR_ERR(modem->ccif_ap);

	res = platform_get_resource(pdev, IORESOURCE_MEM, 1);
	modem->ccif_md = devm_ioremap_resource(dev, res);
	if (IS_ERR(modem->ccif_md))
		return PTR_ERR(modem->ccif_md);

	/* Shared memory region "ap_md_c_smem" from /reserved-memory. */
	error = of_reserved_mem_region_to_resource_byname(dev->of_node,
					"ap_md_c_smem", &sram_res);
	if (error) {
		dev_err(dev, "no ap_md_c_smem reserved region: %d\n", error);
		return error;
	}
	modem->sram = devm_ioremap_resource_wc(dev, &sram_res);
	if (IS_ERR(modem->sram))
		return PTR_ERR(modem->sram);

	/* Clocks: the mddriver node lists DPMAIF clocks that the mainline
	 * clock driver does not provide.  Claim only the CCIF variants that
	 * actually exist; with clk_ignore_unused the rest stay on from the
	 * bootloader. */
	for (i = 0; i < ARRAY_SIZE(ccif_clocks); i++) {
		struct clk *clk = devm_clk_get_optional(dev, ccif_clocks[i]);

		if (IS_ERR(clk)) {
			dev_info(dev, "clock %s unavailable: %ld\n",
				 ccif_clocks[i], PTR_ERR(clk));
			continue;
		}
		if (clk) {
			error = clk_prepare_enable(clk);
			if (error) {
				dev_warn(dev, "failed to enable %s: %d\n",
					 ccif_clocks[i], error);
				continue;
			}
			modem->clks[modem->nclks++] = clk;
		}
	}

	error = ccif_init(&modem->ccif, dev, modem->ccif_ap, modem->ccif_md,
			  modem->sram, modem->data->sram_size,
			  modem->data->nr_queues, md1_rx_sizes, md1_tx_sizes);
	if (error)
		goto err_clks;

	/* IRQ 0 is the MD watchdog; CCIF RX events arrive on IRQ 1 (AP CCIF). */
	modem->irq = platform_get_irq(pdev, 1);
	if (modem->irq < 0)
		modem->irq = platform_get_irq(pdev, 0);
	if (modem->irq < 0) {
		error = modem->irq;
		goto err_ccif;
	}

	error = port_proxy_init(&modem->proxy, &modem->ccif,
				modem->data->ports, modem->data->n_ports);
	if (error)
		goto err_ccif;

	error = request_irq(modem->irq, mtk_modem_irq, IRQF_SHARED,
			    "mtk-modem", modem);
	if (error)
		goto err_proxy;

	platform_set_drvdata(pdev, modem);
	mtk_modem_check_alive(modem);

	dev_info(dev, "MT6893 modem CCIF control path up\n");
	return 0;

err_proxy:
	port_proxy_deinit(&modem->proxy);
err_ccif:
	ccif_deinit(&modem->ccif);
err_clks:
	while (modem->nclks > 0)
		clk_disable_unprepare(modem->clks[--modem->nclks]);
	return error;
}

static void mtk_modem_remove(struct platform_device *pdev)
{
	struct mtk_modem *modem = platform_get_drvdata(pdev);

	free_irq(modem->irq, modem);
	port_proxy_deinit(&modem->proxy);
	ccif_deinit(&modem->ccif);
	while (modem->nclks > 0)
		clk_disable_unprepare(modem->clks[--modem->nclks]);
}

static const struct of_device_id mtk_modem_of_match[] = {
	{ .compatible = "mediatek,mddriver", .data = &md1_data },
	{ }
};
MODULE_DEVICE_TABLE(of, mtk_modem_of_match);

static struct platform_driver mtk_modem_driver = {
	.driver = {
		.name = "mtk-soc-modem",
		.of_match_table = mtk_modem_of_match,
	},
	.probe = mtk_modem_probe,
	.remove = mtk_modem_remove,
};
module_platform_driver(mtk_modem_driver);

MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("MediaTek MT6893 integrated LTE modem (CCIF control path)");
