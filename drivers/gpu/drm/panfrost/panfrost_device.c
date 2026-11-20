// SPDX-License-Identifier: GPL-2.0
/* Copyright 2018 Marty E. Plummer <hanetzer@startmail.com> */
/* Copyright 2019 Linaro, Ltd, Rob Herring <robh@kernel.org> */

#include <linux/clk.h>
#include <linux/io.h>
#include <linux/of.h>
#include <linux/of_address.h>
#include <linux/reset.h>
#include <linux/platform_device.h>
#include <linux/pm_domain.h>
#include <linux/pm_runtime.h>
#include <linux/regulator/consumer.h>

#include "panfrost_device.h"
#include "panfrost_devfreq.h"
#include "panfrost_features.h"
#include "panfrost_gem.h"
#include "panfrost_issues.h"
#include "panfrost_gpu.h"
#include "panfrost_job.h"
#include "panfrost_mmu.h"
#include "panfrost_perfcnt.h"

static int panfrost_reset_init(struct panfrost_device *pfdev)
{
	pfdev->rstc = devm_reset_control_array_get_optional_exclusive(pfdev->base.dev);
	if (IS_ERR(pfdev->rstc)) {
		dev_err(pfdev->base.dev, "get reset failed %ld\n", PTR_ERR(pfdev->rstc));
		return PTR_ERR(pfdev->rstc);
	}

	return reset_control_deassert(pfdev->rstc);
}

static void panfrost_reset_fini(struct panfrost_device *pfdev)
{
	reset_control_assert(pfdev->rstc);
}

/*
 * chopin: retune the GPU clock safely.
 *
 * The GPU takes its clock straight from mfgpll through the mfg_sel mux, so
 * calling clk_set_rate() on the mux makes the clock framework raise the PLL
 * *while the mux still points at it*.  The relock glitch stalls the GPU's AXI
 * bus and the next register access never returns - that is what hung the
 * machine twice (the DT OPP table at probe, and a raw clk_set_rate at runtime).
 *
 * The vendor gpufreq driver avoids this by moving the mux onto a slow parent
 * first.  mfg_sel's parents are clk26m, mfgpll, univpll_d6 and mainpll_d5_d2
 * ("clk_possible_parents" in debugfs), so do the same:
 *
 *   mfg_sel -> clk26m        (GPU drops to 26 MHz, PLL has no consumer)
 *   mfgpll  -> new rate      (safe to relock now)
 *   mfg_sel -> mfgpll        (back at the new rate)
 */
static struct panfrost_device *chopin_pfdev;
static struct clk *chopin_core_pll;
static struct clk *chopin_core_slow;

static int chopin_gpu_set_rate(struct panfrost_device *pfdev, unsigned long hz)
{
	int ret;

	if (!chopin_core_pll || !chopin_core_slow || !pfdev->clock)
		return -ENODEV;

	/* Park the GPU on 26 MHz so the PLL is free. */
	ret = clk_set_parent(pfdev->clock, chopin_core_slow);
	if (ret)
		return ret;

	ret = clk_set_rate(chopin_core_pll, hz);

	/* Switch back to the PLL even if the retune failed, otherwise the GPU
	 * would be left running at 26 MHz. */
	if (clk_set_parent(pfdev->clock, chopin_core_pll))
		return -EIO;

	return ret;
}

static ssize_t core_clk_rate_show(struct device *dev,
				  struct device_attribute *attr, char *buf)
{
	if (!chopin_pfdev || !chopin_pfdev->clock)
		return -ENODEV;
	return sprintf(buf, "%lu\n", clk_get_rate(chopin_pfdev->clock));
}

static ssize_t core_clk_rate_store(struct device *dev,
				   struct device_attribute *attr,
				   const char *buf, size_t count)
{
	unsigned long rate;
	int ret;

	if (!chopin_pfdev)
		return -ENODEV;

	ret = kstrtoul(buf, 0, &rate);
	if (ret)
		return ret;

	/* Below the boot rate is pointless; far above it is certainly unsafe
	 * without GPU voltage control (MT6315 is not reachable from Linux). */
	if (rate < 350000000UL || rate > 900000000UL)
		return -EINVAL;

	dev_info(dev, "chopin: GPU core %lu -> %lu Hz\n",
		 clk_get_rate(chopin_pfdev->clock), rate);

	ret = chopin_gpu_set_rate(chopin_pfdev, rate);
	if (ret)
		return ret;

	dev_info(dev, "chopin: GPU core clock is now %lu Hz\n",
		 clk_get_rate(chopin_pfdev->clock));
	return count;
}

static DEVICE_ATTR_RW(core_clk_rate);

static int panfrost_clk_init(struct panfrost_device *pfdev)
{
	int err;
	unsigned long rate;

	pfdev->clock = devm_clk_get(pfdev->base.dev, NULL);
	if (IS_ERR(pfdev->clock)) {
		dev_err(pfdev->base.dev, "get clock failed %ld\n", PTR_ERR(pfdev->clock));
		return PTR_ERR(pfdev->clock);
	}

	rate = clk_get_rate(pfdev->clock);
	dev_info(pfdev->base.dev, "clock rate = %lu\n", rate);

	err = clk_prepare_enable(pfdev->clock);
	if (err)
		return err;

	chopin_core_pll = devm_clk_get_optional(pfdev->base.dev, "core_pll");
	if (IS_ERR(chopin_core_pll))
		chopin_core_pll = NULL;
	chopin_core_slow = devm_clk_get_optional(pfdev->base.dev, "core_slow");
	if (IS_ERR(chopin_core_slow))
		chopin_core_slow = NULL;
	chopin_pfdev = pfdev;
	if (device_create_file(pfdev->base.dev, &dev_attr_core_clk_rate))
		dev_warn(pfdev->base.dev, "chopin: no core_clk_rate knob\n");

	pfdev->bus_clock = devm_clk_get_optional(pfdev->base.dev, "bus");
	if (IS_ERR(pfdev->bus_clock)) {
		dev_err(pfdev->base.dev, "get bus_clock failed %ld\n",
			PTR_ERR(pfdev->bus_clock));
		err = PTR_ERR(pfdev->bus_clock);
		goto disable_clock;
	}

	if (pfdev->bus_clock) {
		rate = clk_get_rate(pfdev->bus_clock);
		dev_info(pfdev->base.dev, "bus_clock rate = %lu\n", rate);

		err = clk_prepare_enable(pfdev->bus_clock);
		if (err)
			goto disable_clock;
	}

	pfdev->bus_ace_clock = devm_clk_get_optional(pfdev->base.dev, "bus_ace");
	if (IS_ERR(pfdev->bus_ace_clock)) {
		err = PTR_ERR(pfdev->bus_ace_clock);
		dev_err(pfdev->base.dev, "get bus_ace_clock failed %d\n", err);
		goto disable_bus_clock;
	}

	err = clk_prepare_enable(pfdev->bus_ace_clock);
	if (err)
		goto disable_bus_clock;

	return 0;

disable_bus_clock:
	clk_disable_unprepare(pfdev->bus_clock);
disable_clock:
	clk_disable_unprepare(pfdev->clock);

	return err;
}

static void panfrost_clk_fini(struct panfrost_device *pfdev)
{
	clk_disable_unprepare(pfdev->bus_ace_clock);
	clk_disable_unprepare(pfdev->bus_clock);
	clk_disable_unprepare(pfdev->clock);
}

static int panfrost_regulator_init(struct panfrost_device *pfdev)
{
	int ret, i;

	pfdev->regulators = devm_kcalloc(pfdev->base.dev, pfdev->comp->num_supplies,
					 sizeof(*pfdev->regulators),
					 GFP_KERNEL);
	if (!pfdev->regulators)
		return -ENOMEM;

	for (i = 0; i < pfdev->comp->num_supplies; i++)
		pfdev->regulators[i].supply = pfdev->comp->supply_names[i];

	ret = devm_regulator_bulk_get(pfdev->base.dev,
				      pfdev->comp->num_supplies,
				      pfdev->regulators);
	if (ret < 0) {
		if (ret != -EPROBE_DEFER)
			dev_err(pfdev->base.dev, "failed to get regulators: %d\n",
				ret);
		return ret;
	}

	ret = regulator_bulk_enable(pfdev->comp->num_supplies,
				    pfdev->regulators);
	if (ret < 0) {
		dev_err(pfdev->base.dev, "failed to enable regulators: %d\n", ret);
		return ret;
	}

	return 0;
}

static void panfrost_regulator_fini(struct panfrost_device *pfdev)
{
	if (!pfdev->regulators)
		return;

	regulator_bulk_disable(pfdev->comp->num_supplies, pfdev->regulators);
}

static void panfrost_pm_domain_fini(struct panfrost_device *pfdev)
{
	int i;

	for (i = 0; i < ARRAY_SIZE(pfdev->pm_domain_devs); i++) {
		if (!pfdev->pm_domain_devs[i])
			break;

		if (pfdev->pm_domain_links[i])
			device_link_del(pfdev->pm_domain_links[i]);

		dev_pm_domain_detach(pfdev->pm_domain_devs[i], true);
	}
}

static int panfrost_pm_domain_init(struct panfrost_device *pfdev)
{
	int err;
	int i, num_domains;

	num_domains = of_count_phandle_with_args(pfdev->base.dev->of_node,
						 "power-domains",
						 "#power-domain-cells");

	/*
	 * Single domain is handled by the core, and, if only a single power
	 * the power domain is requested, the property is optional.
	 */
	if (num_domains < 2 && pfdev->comp->num_pm_domains < 2)
		return 0;

	if (num_domains != pfdev->comp->num_pm_domains) {
		dev_err(pfdev->base.dev,
			"Incorrect number of power domains: %d provided, %d needed\n",
			num_domains, pfdev->comp->num_pm_domains);
		return -EINVAL;
	}

	if (WARN(num_domains > ARRAY_SIZE(pfdev->pm_domain_devs),
			"Too many supplies in compatible structure.\n"))
		return -EINVAL;

	for (i = 0; i < num_domains; i++) {
		pfdev->pm_domain_devs[i] =
			dev_pm_domain_attach_by_name(pfdev->base.dev,
						     pfdev->comp->pm_domain_names[i]);
		if (IS_ERR_OR_NULL(pfdev->pm_domain_devs[i])) {
			err = PTR_ERR(pfdev->pm_domain_devs[i]) ? : -ENODATA;
			pfdev->pm_domain_devs[i] = NULL;
			dev_err(pfdev->base.dev,
				"failed to get pm-domain %s(%d): %d\n",
				pfdev->comp->pm_domain_names[i], i, err);
			goto err;
		}
		pfdev->pm_domain_links[i] =
			device_link_add(pfdev->base.dev,
					pfdev->pm_domain_devs[i], DL_FLAG_PM_RUNTIME |
					DL_FLAG_STATELESS | DL_FLAG_RPM_ACTIVE);
		if (!pfdev->pm_domain_links[i]) {
			dev_err(pfdev->pm_domain_devs[i],
				"adding device link failed!\n");
			err = -ENODEV;
			goto err;
		}
	}

	return 0;

err:
	panfrost_pm_domain_fini(pfdev);
	return err;
}

static const char * const chopin_mtcmos_names[7] = {
	"mtcmos_mfg_async",
	"mtcmos_mfg",
	"mtcmos_mfg_core0",
	"mtcmos_mfg_core1_2",
	"mtcmos_mfg_core3_4",
	"mtcmos_mfg_core5_6",
	"mtcmos_mfg_core7_8",
};

static int chopin_mtcmos_on(struct panfrost_device *pfdev)
{
	struct device *dev = pfdev->base.dev;
	bool defer = false;
	int i;

	if (!of_machine_is_compatible("mediatek,mt6893"))
		return 0;

	for (i = 0; i < ARRAY_SIZE(chopin_mtcmos_names); i++) {
		pfdev->mtcmos_clks[i] = devm_clk_get_optional(dev,
							      chopin_mtcmos_names[i]);
		if (IS_ERR(pfdev->mtcmos_clks[i])) {
			long ret = PTR_ERR(pfdev->mtcmos_clks[i]);

			dev_warn(dev, "mtcmos %s: %ld\n", chopin_mtcmos_names[i], ret);
			pfdev->mtcmos_clks[i] = NULL;
			/* The MTCMOS clock provider (scpsys) can still be
			 * probing.  Without these clocks the GPU is powered
			 * off and every reset times out, so defer instead of
			 * limping on and failing with -ETIMEDOUT. */
			if (ret == -EPROBE_DEFER)
				defer = true;
			continue;
		}
		if (pfdev->mtcmos_clks[i])
			clk_prepare_enable(pfdev->mtcmos_clks[i]);
	}

	return defer ? -EPROBE_DEFER : 0;
}

static void chopin_gpu_rails_on(struct device *dev)
{
	struct regulator *vsram, *vgpu;
	int ret;

	if (!of_machine_is_compatible("mediatek,mt6893"))
		return;

	/* Same order as the vendor gpufreq driver.  Going through the SPM
	 * domain-supply path instead hard-resets this board. */
	vsram = regulator_get(NULL, "vsram_others");
	if (!IS_ERR_OR_NULL(vsram)) {
		ret = regulator_enable(vsram);
		if (ret)
			dev_err(dev, "failed to enable vsram_others: %d\n", ret);
	}

	vgpu = regulator_get(NULL, "vgpu11");
	if (!IS_ERR_OR_NULL(vgpu)) {
		ret = regulator_enable(vgpu);
		if (ret)
			dev_err(dev, "failed to enable vgpu11: %d\n", ret);
	}
}

/*
 * MediaTek MFG AXI / clock-gating setup.
 *
 * The vendor gpufreq driver runs this right after the MFG MTCMOS islands
 * are powered on (mt_gpufreq_external_cg_control()): it enables the memory
 * port clock gating, stops the free-running clocks and turns on the SOC-in
 * AXI clock and the Q-channel.  Mainline never did this, so the GPU ran
 * with every MFG clock forced on; doing it matches the vendor power
 * sequence and lets the GPU gate its idle clocks.
 *
 * The registers live in g3d_config@13fbf000, which the board DT exposes as
 * "mediatek,mt6893-mfgcfg".  The "bus" clock (the g3d_config gate) is
 * already enabled by panfrost_clk_init() when this runs.
 */
static void chopin_mfg_axi_config(struct device *dev)
{
	struct device_node *np;
	struct resource res;
	void __iomem *mfg;
	u32 val;

	if (!of_machine_is_compatible("mediatek,mt6893"))
		return;

	np = of_find_compatible_node(NULL, NULL, "mediatek,mt6893-mfgcfg");
	if (!np)
		return;
	if (of_address_to_resource(np, 0, &res)) {
		of_node_put(np);
		return;
	}
	mfg = devm_ioremap(dev, res.start, resource_size(&res));
	of_node_put(np);
	if (!mfg) {
		dev_warn(dev, "chopin: cannot map MFG config\n");
		return;
	}

	/* MFG_ASYNC_CON: MEM0/1 master+slave clock gating. */
	val = readl(mfg + 0x0020);
	val |= BIT(22) | BIT(23) | BIT(24) | BIT(25);
	writel(val, mfg + 0x0020);

	/* MFG_ASYNC_CON_3: MEM0_1/MEM1_1 clock gating. */
	val = readl(mfg + 0x002c);
	val |= BIT(12) | BIT(13) | BIT(14) | BIT(15);
	writel(val, mfg + 0x002c);

	/* MFG_GLOBAL_CON: stop GPU/SOC AXI and power-CG free-run clocks. */
	val = readl(mfg + 0x00b0);
	val &= ~(BIT(8) | BIT(9) | BIT(10) | BIT(19));
	writel(val, mfg + 0x00b0);

	/* MFG_QCHANNEL_CON: enable the Q-channel. */
	val = readl(mfg + 0x00b4);
	val |= BIT(4);
	writel(val, mfg + 0x00b4);

	/* MFG_ASYNC_CON_1: enable the SOC-in AXI clock. */
	val = readl(mfg + 0x0024);
	val |= BIT(0);
	writel(val, mfg + 0x0024);

	dev_info(dev, "chopin MFG AXI/CG configured\n");
}


int panfrost_device_init(struct panfrost_device *pfdev)
{
	int err;

	chopin_gpu_rails_on(pfdev->base.dev);
	err = chopin_mtcmos_on(pfdev);
	if (err)
		return err;

	mutex_init(&pfdev->sched_lock);
	INIT_LIST_HEAD(&pfdev->as_lru_list);

	spin_lock_init(&pfdev->as_lock);

	spin_lock_init(&pfdev->cycle_counter.lock);

#ifdef CONFIG_DEBUG_FS
	mutex_init(&pfdev->debugfs.gems_lock);
	INIT_LIST_HEAD(&pfdev->debugfs.gems_list);
#endif

	pfdev->iomem = devm_platform_ioremap_resource(to_platform_device(pfdev->base.dev), 0);
	if (IS_ERR(pfdev->iomem)) {
		err = PTR_ERR(pfdev->iomem);
		goto out_regulator;
	}

	err = panfrost_pm_domain_init(pfdev);
	if (err)
		return err;

	/* Power the attached PM domains before touching the GPU registers.
	 * panfrost_probe() used to enable runtime PM only after this function
	 * returned, so the first reset ran with every MFG domain off. */
	pm_runtime_enable(pfdev->base.dev);
	pm_runtime_get_sync(pfdev->base.dev);

	err = panfrost_reset_init(pfdev);
	if (err) {
		dev_err(pfdev->base.dev, "reset init failed %d\n", err);
		goto out_pm_domain;
	}

	err = panfrost_clk_init(pfdev);
	if (err) {
		dev_err(pfdev->base.dev, "clk init failed %d\n", err);
		goto out_reset;
	}

	chopin_mfg_axi_config(pfdev->base.dev);

	err = panfrost_devfreq_init(pfdev);
	if (err) {
		if (err != -EPROBE_DEFER)
			dev_err(pfdev->base.dev, "devfreq init failed %d\n", err);
		goto out_clk;
	}

	/* OPP will handle regulators */
	if (!pfdev->pfdevfreq.opp_of_table_added) {
		err = panfrost_regulator_init(pfdev);
		if (err)
			goto out_devfreq;
	}

	err = panfrost_gpu_init(pfdev);
	if (err)
		goto out_regulator;

	err = panfrost_mmu_init(pfdev);
	if (err)
		goto out_gpu;

	err = panfrost_jm_init(pfdev);
	if (err)
		goto out_mmu;

	/*
	 * MT6893: the first job submitted after probe is accepted by the JM
	 * but never executed (no interrupt, no fault, slot left idle), while
	 * the identical submission works after the timeout recovery's
	 * panfrost_device_reset().  Repeat that reset here now that the MMU
	 * and job manager are initialised, so the very first userspace job
	 * already runs in the recovered state.
	 */
	panfrost_device_reset(pfdev, true);

	err = panfrost_perfcnt_init(pfdev);
	if (err)
		goto out_job;

	panfrost_gem_init(pfdev);

	return 0;
out_job:
	panfrost_jm_fini(pfdev);
out_mmu:
	panfrost_mmu_fini(pfdev);
out_gpu:
	panfrost_gpu_fini(pfdev);
out_regulator:
	panfrost_regulator_fini(pfdev);
out_devfreq:
	panfrost_devfreq_fini(pfdev);
out_clk:
	panfrost_clk_fini(pfdev);
out_reset:
	panfrost_reset_fini(pfdev);
out_pm_domain:
	panfrost_pm_domain_fini(pfdev);
	return err;
}

void panfrost_device_fini(struct panfrost_device *pfdev)
{
	panfrost_perfcnt_fini(pfdev);
	panfrost_jm_fini(pfdev);
	panfrost_mmu_fini(pfdev);
	panfrost_gpu_fini(pfdev);
	panfrost_devfreq_fini(pfdev);
	panfrost_regulator_fini(pfdev);
	panfrost_clk_fini(pfdev);
	panfrost_reset_fini(pfdev);
	panfrost_pm_domain_fini(pfdev);
}

#define PANFROST_EXCEPTION(id) \
	[DRM_PANFROST_EXCEPTION_ ## id] = { \
		.name = #id, \
	}

struct panfrost_exception_info {
	const char *name;
};

static const struct panfrost_exception_info panfrost_exception_infos[] = {
	PANFROST_EXCEPTION(OK),
	PANFROST_EXCEPTION(DONE),
	PANFROST_EXCEPTION(INTERRUPTED),
	PANFROST_EXCEPTION(STOPPED),
	PANFROST_EXCEPTION(TERMINATED),
	PANFROST_EXCEPTION(KABOOM),
	PANFROST_EXCEPTION(EUREKA),
	PANFROST_EXCEPTION(ACTIVE),
	PANFROST_EXCEPTION(JOB_CONFIG_FAULT),
	PANFROST_EXCEPTION(JOB_POWER_FAULT),
	PANFROST_EXCEPTION(JOB_READ_FAULT),
	PANFROST_EXCEPTION(JOB_WRITE_FAULT),
	PANFROST_EXCEPTION(JOB_AFFINITY_FAULT),
	PANFROST_EXCEPTION(JOB_BUS_FAULT),
	PANFROST_EXCEPTION(INSTR_INVALID_PC),
	PANFROST_EXCEPTION(INSTR_INVALID_ENC),
	PANFROST_EXCEPTION(INSTR_TYPE_MISMATCH),
	PANFROST_EXCEPTION(INSTR_OPERAND_FAULT),
	PANFROST_EXCEPTION(INSTR_TLS_FAULT),
	PANFROST_EXCEPTION(INSTR_BARRIER_FAULT),
	PANFROST_EXCEPTION(INSTR_ALIGN_FAULT),
	PANFROST_EXCEPTION(DATA_INVALID_FAULT),
	PANFROST_EXCEPTION(TILE_RANGE_FAULT),
	PANFROST_EXCEPTION(ADDR_RANGE_FAULT),
	PANFROST_EXCEPTION(IMPRECISE_FAULT),
	PANFROST_EXCEPTION(OOM),
	PANFROST_EXCEPTION(OOM_AFBC),
	PANFROST_EXCEPTION(UNKNOWN),
	PANFROST_EXCEPTION(DELAYED_BUS_FAULT),
	PANFROST_EXCEPTION(GPU_SHAREABILITY_FAULT),
	PANFROST_EXCEPTION(SYS_SHAREABILITY_FAULT),
	PANFROST_EXCEPTION(GPU_CACHEABILITY_FAULT),
	PANFROST_EXCEPTION(TRANSLATION_FAULT_0),
	PANFROST_EXCEPTION(TRANSLATION_FAULT_1),
	PANFROST_EXCEPTION(TRANSLATION_FAULT_2),
	PANFROST_EXCEPTION(TRANSLATION_FAULT_3),
	PANFROST_EXCEPTION(TRANSLATION_FAULT_4),
	PANFROST_EXCEPTION(TRANSLATION_FAULT_IDENTITY),
	PANFROST_EXCEPTION(PERM_FAULT_0),
	PANFROST_EXCEPTION(PERM_FAULT_1),
	PANFROST_EXCEPTION(PERM_FAULT_2),
	PANFROST_EXCEPTION(PERM_FAULT_3),
	PANFROST_EXCEPTION(TRANSTAB_BUS_FAULT_0),
	PANFROST_EXCEPTION(TRANSTAB_BUS_FAULT_1),
	PANFROST_EXCEPTION(TRANSTAB_BUS_FAULT_2),
	PANFROST_EXCEPTION(TRANSTAB_BUS_FAULT_3),
	PANFROST_EXCEPTION(ACCESS_FLAG_0),
	PANFROST_EXCEPTION(ACCESS_FLAG_1),
	PANFROST_EXCEPTION(ACCESS_FLAG_2),
	PANFROST_EXCEPTION(ACCESS_FLAG_3),
	PANFROST_EXCEPTION(ADDR_SIZE_FAULT_IN0),
	PANFROST_EXCEPTION(ADDR_SIZE_FAULT_IN1),
	PANFROST_EXCEPTION(ADDR_SIZE_FAULT_IN2),
	PANFROST_EXCEPTION(ADDR_SIZE_FAULT_IN3),
	PANFROST_EXCEPTION(ADDR_SIZE_FAULT_OUT0),
	PANFROST_EXCEPTION(ADDR_SIZE_FAULT_OUT1),
	PANFROST_EXCEPTION(ADDR_SIZE_FAULT_OUT2),
	PANFROST_EXCEPTION(ADDR_SIZE_FAULT_OUT3),
	PANFROST_EXCEPTION(MEM_ATTR_FAULT_0),
	PANFROST_EXCEPTION(MEM_ATTR_FAULT_1),
	PANFROST_EXCEPTION(MEM_ATTR_FAULT_2),
	PANFROST_EXCEPTION(MEM_ATTR_FAULT_3),
	PANFROST_EXCEPTION(MEM_ATTR_NONCACHE_0),
	PANFROST_EXCEPTION(MEM_ATTR_NONCACHE_1),
	PANFROST_EXCEPTION(MEM_ATTR_NONCACHE_2),
	PANFROST_EXCEPTION(MEM_ATTR_NONCACHE_3),
};

const char *panfrost_exception_name(u32 exception_code)
{
	if (WARN_ON(exception_code >= ARRAY_SIZE(panfrost_exception_infos) ||
		    !panfrost_exception_infos[exception_code].name))
		return "Unknown exception type";

	return panfrost_exception_infos[exception_code].name;
}

bool panfrost_exception_needs_reset(const struct panfrost_device *pfdev,
				    u32 exception_code)
{
	/* If an occlusion query write causes a bus fault on affected GPUs,
	 * future fragment jobs may hang. Reset to workaround.
	 */
	if (exception_code == DRM_PANFROST_EXCEPTION_JOB_BUS_FAULT)
		return panfrost_has_hw_issue(pfdev, HW_ISSUE_TTRX_3076);

	/* No other GPUs we support need a reset */
	return false;
}

void panfrost_device_reset(struct panfrost_device *pfdev, bool enable_job_int)
{
	panfrost_gpu_soft_reset(pfdev);

	panfrost_gpu_power_on(pfdev);
	panfrost_mmu_reset(pfdev);

	panfrost_jm_reset_interrupts(pfdev);
	if (enable_job_int)
		panfrost_jm_enable_interrupts(pfdev);
}

static int panfrost_device_runtime_resume(struct device *dev)
{
	struct panfrost_device *pfdev = dev_get_drvdata(dev);
	int ret;

	if (pfdev->comp->pm_features & BIT(GPU_PM_RT)) {
		ret = reset_control_deassert(pfdev->rstc);
		if (ret)
			return ret;

		ret = clk_enable(pfdev->clock);
		if (ret)
			goto err_clk;

		ret = clk_enable(pfdev->bus_clock);
		if (ret)
			goto err_bus_clk;

		ret = clk_enable(pfdev->bus_ace_clock);
		if (ret)
			goto err_bus_ace_clk;
	}

	panfrost_device_reset(pfdev, true);
	panfrost_devfreq_resume(pfdev);

	return 0;

err_bus_ace_clk:
	if (pfdev->comp->pm_features & BIT(GPU_PM_RT))
		clk_disable(pfdev->bus_clock);
err_bus_clk:
	if (pfdev->comp->pm_features & BIT(GPU_PM_RT))
		clk_disable(pfdev->clock);
err_clk:
	if (pfdev->comp->pm_features & BIT(GPU_PM_RT))
		reset_control_assert(pfdev->rstc);
	return ret;
}

static int panfrost_device_runtime_suspend(struct device *dev)
{
	struct panfrost_device *pfdev = dev_get_drvdata(dev);

	if (!panfrost_jm_is_idle(pfdev))
		return -EBUSY;

	panfrost_devfreq_suspend(pfdev);
	panfrost_jm_suspend_irq(pfdev);
	panfrost_mmu_suspend_irq(pfdev);
	panfrost_gpu_suspend_irq(pfdev);
	panfrost_gpu_power_off(pfdev);

	if (pfdev->comp->pm_features & BIT(GPU_PM_RT)) {
		clk_disable(pfdev->bus_ace_clock);
		clk_disable(pfdev->bus_clock);
		clk_disable(pfdev->clock);
		reset_control_assert(pfdev->rstc);
	}

	return 0;
}

static int panfrost_device_resume(struct device *dev)
{
	struct panfrost_device *pfdev = dev_get_drvdata(dev);
	int ret;

	if (pfdev->comp->pm_features & BIT(GPU_PM_VREG_OFF)) {
		unsigned long freq = pfdev->pfdevfreq.fast_rate;
		struct dev_pm_opp *opp;

		opp = dev_pm_opp_find_freq_ceil(dev, &freq);
		if (IS_ERR(opp))
			return PTR_ERR(opp);
		dev_pm_opp_set_opp(dev, opp);
		dev_pm_opp_put(opp);
	}

	if (pfdev->comp->pm_features & BIT(GPU_PM_CLK_DIS)) {
		ret = clk_enable(pfdev->clock);
		if (ret)
			goto err_clk;

		if (pfdev->bus_clock) {
			ret = clk_enable(pfdev->bus_clock);
			if (ret)
				goto err_bus_clk;
		}
	}

	ret = pm_runtime_force_resume(dev);
	if (ret)
		goto err_resume;

	return 0;

err_resume:
	if (pfdev->comp->pm_features & BIT(GPU_PM_CLK_DIS) && pfdev->bus_clock)
		clk_disable(pfdev->bus_clock);
err_bus_clk:
	if (pfdev->comp->pm_features & BIT(GPU_PM_CLK_DIS))
		clk_disable(pfdev->clock);
err_clk:
	if (pfdev->comp->pm_features & BIT(GPU_PM_VREG_OFF))
		dev_pm_opp_set_opp(dev, NULL);
	return ret;
}

static int panfrost_device_suspend(struct device *dev)
{
	struct panfrost_device *pfdev = dev_get_drvdata(dev);
	int ret;

	ret = pm_runtime_force_suspend(dev);
	if (ret)
		return ret;

	if (pfdev->comp->pm_features & BIT(GPU_PM_CLK_DIS)) {
		if (pfdev->bus_clock)
			clk_disable(pfdev->bus_clock);

		clk_disable(pfdev->clock);
	}

	if (pfdev->comp->pm_features & BIT(GPU_PM_VREG_OFF))
		dev_pm_opp_set_opp(dev, NULL);

	return 0;
}

EXPORT_GPL_DEV_PM_OPS(panfrost_pm_ops) = {
	RUNTIME_PM_OPS(panfrost_device_runtime_suspend, panfrost_device_runtime_resume, NULL)
	SYSTEM_SLEEP_PM_OPS(panfrost_device_suspend, panfrost_device_resume)
};
