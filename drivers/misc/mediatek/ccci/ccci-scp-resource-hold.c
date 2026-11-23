// SPDX-License-Identifier: GPL-2.0-only
/* Explicit op6893 boot-lifetime SCP resource pin, separate from diagnostics. */

#include <linux/clk.h>
#include <linux/delay.h>
#include <linux/jiffies.h>
#include <linux/io.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/of.h>
#include <linux/platform_device.h>
#include <linux/sysfs.h>

#include "ccci-scp-inspect.h"
#include "ccci-scp-reset.h"
#include "ccci-scp-resource.h"
#include "ccci-scp-irq.h"
#include "ccci-scp-mmio.h"
#include "ccci-scp-receive.h"

#include "ccci-scp-owner.h"

/* The firmware request channel belongs to all SCP clients, not each observer. */
static DEFINE_MUTEX(ccci_scp_resource_owner_lock);
static bool ccci_scp_resource_claimed;

#if IS_ENABLED(CONFIG_OP6893_CCCI_SCP_RECEIVE_RESERVE)
static int ccci_scp_resource_receive_check(void *context, u64 generation)
{
	struct ccci_scp_resource_owner *owner = context;
	int ret;

	lockdep_assert_held(&owner->lock);
	/* Inherited state cannot arm RX; the initial boot owner establishes this. */
	if (!generation || generation != owner->reset_generation)
		return -ESTALE;
	ret = ccci_scp_resource_check(&owner->resource, clk_get_rate(owner->clock));
	if (ret)
		return ret;
	/* Detect a clock change during the bounded firmware resource sample. */
	if (clk_get_rate(owner->clock) != 26000000) {
		owner->resource.error = -EAGAIN;
		ccci_scp_resource_invalidate(&owner->resource);
		return -EAGAIN;
	}
	return 0;
}
#endif

#if IS_ENABLED(CONFIG_OP6893_CCCI_SCP_OWNER_OBSERVE)
static int ccci_scp_resource_inspect_check(void *context)
{
	struct ccci_scp_resource_owner *owner = context;
	int ret;

	lockdep_assert_held(&owner->lock);
	ret = ccci_scp_resource_check(&owner->resource, clk_get_rate(owner->clock));
	if (ret)
		return ret;
	if (clk_get_rate(owner->clock) != 26000000) {
		owner->resource.error = -EAGAIN;
		ccci_scp_resource_invalidate(&owner->resource);
		return -EAGAIN;
	}
	return 0;
}
#endif

static void ccci_scp_resource_unclaim(void *unused)
{
	mutex_lock(&ccci_scp_resource_owner_lock);
	ccci_scp_resource_claimed = false;
	mutex_unlock(&ccci_scp_resource_owner_lock);
}

static ssize_t state_show(struct device *dev, struct device_attribute *attr, char *buf)
{
	struct ccci_scp_resource_owner *owner = dev_get_drvdata(dev);
	struct ccci_scp_resource_sample sample;
	bool verified;
	ssize_t length;
	int ret;

	mutex_lock(&owner->lock);
	ret = ccci_scp_resource_read(&sample);
	if (ret)
		goto out;
	verified = owner->resource.pinned && !owner->resource.fault && sample.scp_valid &&
		   sample.scp_user == owner->resource.after.scp_user &&
		   sample.scp_resources == owner->resource.after.scp_resources &&
		   clk_get_rate(owner->clock) == 26000000;
	if (owner->resource.pinned && !verified) {
		owner->resource.error = -EPROTO;
		ccci_scp_resource_invalidate(&owner->resource);
	}
	length = sysfs_emit(buf,
			    "resources=%u users=%u scp_present=%u scp_user=%u scp_valid=%u scp_resources=0x%02x attempted=%u accepted=%u pin_verified=%u fault=%u error=%d release_supported=0 ready_known=0\n",
			    sample.resources, sample.users, sample.scp_present, sample.scp_user,
			    sample.scp_valid, sample.scp_resources, owner->resource.attempted,
			    owner->resource.accepted, verified, owner->resource.fault,
			    owner->resource.error);
	mutex_unlock(&owner->lock);
	return length;
out:
	if (owner->resource.pinned) {
		owner->resource.error = ret;
		ccci_scp_resource_invalidate(&owner->resource);
	}
	mutex_unlock(&owner->lock);
	return ret;
}
static DEVICE_ATTR_RO(state);

static ssize_t pin_store(struct device *dev, struct device_attribute *attr,
			 const char *buf, size_t count)
{
	struct ccci_scp_resource_owner *owner = dev_get_drvdata(dev);
	int ret;

	if (!sysfs_streq(buf, "26m"))
		return -EINVAL;
	mutex_lock(&owner->lock);
	if (clk_get_rate(owner->clock) != 26000000)
		ret = -EBUSY;
	else
		ret = ccci_scp_resource_pin(&owner->resource);
	if (!ret && clk_get_rate(owner->clock) != 26000000) {
		ret = -EAGAIN;
		owner->resource.error = ret;
		ccci_scp_resource_invalidate(&owner->resource);
	}
	mutex_unlock(&owner->lock);
	if (ret)
		return ret;
	return count;
}
static DEVICE_ATTR_WO(pin);

/* Reserving the window must not consume an inherited notification. */
#if IS_ENABLED(CONFIG_OP6893_CCCI_SCP_CFG_RESERVE) && \
	!IS_ENABLED(CONFIG_OP6893_CCCI_SCP_MMIO_RESERVE)
static int ccci_scp_cfg_reserve(struct platform_device *pdev,
				struct ccci_scp_resource_owner *owner)
{
	struct resource *res;
	void __iomem *cfg;

	res = platform_get_resource_byname(pdev, IORESOURCE_MEM, "scp-cfg");
	if (!res || res->start != 0x10724000 || resource_size(res) != 0x1000)
		return -EINVAL;
	/* These alternate completion protocols have not been established here. */
	if (of_property_present(pdev->dev.of_node, "scp-cfgreg-ap-en") ||
	    of_property_present(pdev->dev.of_node, "scp-ipc-wa"))
		return -EOPNOTSUPP;
	cfg = devm_ioremap_resource(&pdev->dev, res);
	if (IS_ERR(cfg))
		return PTR_ERR(cfg);
	owner->cfg = cfg;
	return 0;
}
#endif

static ssize_t cfg_show(struct device *dev, struct device_attribute *attr, char *buf)
{
	struct ccci_scp_resource_owner *owner = dev_get_drvdata(dev);

#if IS_ENABLED(CONFIG_OP6893_CCCI_SCP_MMIO_RESERVE)
	ssize_t length;

	mutex_lock(&owner->lock);
	length = ccci_scp_mmio_cfg_status(owner->mmio, buf);
	mutex_unlock(&owner->lock);
	return length;
#else
	if (!owner->cfg)
		return -EOPNOTSUPP;
	/* The legacy mapping-only option has no register access caller. */
	return sysfs_emit(buf,
			 "reserved=1 bytes=4096 register_reads=0 register_writes=0 completion_enabled=0 ready_known=0\n");
#endif
}
static DEVICE_ATTR_RO(cfg);

static ssize_t mmio_show(struct device *dev, struct device_attribute *attr, char *buf)
{
#if IS_ENABLED(CONFIG_OP6893_CCCI_SCP_MMIO_RESERVE)
	struct ccci_scp_resource_owner *owner = dev_get_drvdata(dev);
	ssize_t length;

	mutex_lock(&owner->lock);
	length = ccci_scp_mmio_status(owner->mmio, buf);
	mutex_unlock(&owner->lock);
	return length;
#else
	return -EOPNOTSUPP;
#endif
}
static DEVICE_ATTR_RO(mmio);

static ssize_t irq_show(struct device *dev, struct device_attribute *attr, char *buf)
{
#if IS_ENABLED(CONFIG_OP6893_CCCI_SCP_RECEIVE_RESERVE)
	struct ccci_scp_resource_owner *owner = dev_get_drvdata(dev);

	return ccci_scp_receive_status(owner->receive, buf);
#elif IS_ENABLED(CONFIG_OP6893_CCCI_SCP_IRQ_RESERVE)
	struct ccci_scp_resource_owner *owner = dev_get_drvdata(dev);

	return ccci_scp_irq_status(owner->irq, buf);
#else
	return -EOPNOTSUPP;
#endif
}
static DEVICE_ATTR_RO(irq);

/* Only the unique owner may acquire these reset-capable windows. */
#if IS_ENABLED(CONFIG_OP6893_CCCI_SCP_CORE_TCM_RESERVE)
static int ccci_scp_core_tcm_reserve(struct platform_device *pdev,
				     struct ccci_scp_resource_owner *owner)
{
	static const char * const names[] = { "core0", "core1", "tcm" };
	static const resource_size_t bases[] = { 0x10730000, 0x10740000, 0x10500000 };
	static const resource_size_t sizes[] = { 0x1000, 0x1000, 0x180000 };
	struct resource resources[3], *res;
	void __iomem *mappings[3];
	unsigned int i;

	/* Reject every geometry error before reserving the first window. */
	for (i = 0; i < ARRAY_SIZE(resources); i++) {
		res = platform_get_resource_byname(pdev, IORESOURCE_MEM, names[i]);
		if (!res || res->start != bases[i] || resource_size(res) != sizes[i])
			return -EINVAL;
		resources[i] = *res;
		/* Device-nGnRnE, matching cfg/mailbox; never a cached RAM alias. */
		resources[i].flags |= IORESOURCE_MEM_NONPOSTED;
	}
	for (i = 0; i < ARRAY_SIZE(resources); i++) {
		mappings[i] = devm_ioremap_resource(&pdev->dev, &resources[i]);
		if (IS_ERR(mappings[i]))
			return PTR_ERR(mappings[i]);
	}
	/* Publish only a complete set. Probe failure unwinds devres mappings. */
	for (i = 0; i < ARRAY_SIZE(resources); i++)
		owner->core_tcm[i] = mappings[i];
	return 0;
}
#endif

static ssize_t core_tcm_show(struct device *dev, struct device_attribute *attr, char *buf)
{
	struct ccci_scp_resource_owner *owner = dev_get_drvdata(dev);
	ssize_t length;

	mutex_lock(&owner->lock);
	if (!owner->core_tcm[0] || !owner->core_tcm[1] || !owner->core_tcm[2])
		length = -EOPNOTSUPP;
#if IS_ENABLED(CONFIG_OP6893_CCCI_SCP_OWNER_OBSERVE)
	else if (owner->inspect)
		length = ccci_scp_inspect_status(owner->inspect, buf);
#endif
	else
		/* Bookkeeping only: no register or TCM reads, including at probe. */
		length = sysfs_emit(buf,
				    "mapped=3 bytes=1581056 nonposted=1 register_reads=0 register_writes=0 tcm_reads=0 tcm_writes=0 reset_supported=0 ready_known=0\n");
	mutex_unlock(&owner->lock);
	return length;
}
static DEVICE_ATTR_RO(core_tcm);

#if IS_ENABLED(CONFIG_OP6893_CCCI_SCP_OWNER_OBSERVE)
static ssize_t cores_show(struct device *dev, struct device_attribute *attr, char *buf)
{
	struct ccci_scp_resource_owner *owner = dev_get_drvdata(dev);
	ssize_t length;

	mutex_lock(&owner->lock);
	length = ccci_scp_inspect_cores(owner->inspect, buf);
	mutex_unlock(&owner->lock);
	return length;
}
static DEVICE_ATTR_RO(cores);

static ssize_t header_show(struct device *dev, struct device_attribute *attr, char *buf)
{
	struct ccci_scp_resource_owner *owner = dev_get_drvdata(dev);
	ssize_t length;

	mutex_lock(&owner->lock);
	length = ccci_scp_inspect_header(owner->inspect, buf);
	mutex_unlock(&owner->lock);
	return length;
}
static DEVICE_ATTR_RO(header);
#endif

#if IS_ENABLED(CONFIG_OP6893_CCCI_SCP_RESET_ASSERT)
static ssize_t reset_assert_store(struct device *dev, struct device_attribute *attr,
				  const char *buf, size_t count)
{
	struct ccci_scp_resource_owner *owner = dev_get_drvdata(dev);
	int ret;

	if (!sysfs_streq(buf, "assert"))
		return -EINVAL;
	mutex_lock(&owner->lock);
	if (owner->reset_requested) {
		ret = -EALREADY;
	} else {
		ret = ccci_scp_inspect_reset_check(owner->inspect);
		if (!ret)
			owner->reset_requested = true;
	}
	mutex_unlock(&owner->lock);
	if (ret)
		return ret;
	/* STOP waits for IRQ/work which can take owner->lock. Never hold it here. */
	ccci_scp_receive_stop(owner->receive);
	mutex_lock(&owner->lock);
	ret = ccci_scp_reset_assert(&owner->reset, owner->core_tcm,
				    ccci_scp_inspect_reset_check, owner->inspect);
	ccci_scp_inspect_record_reset(owner->inspect, owner->reset.writes);
	owner->reset.error = ret;
	mutex_unlock(&owner->lock);
	if (ret)
		return ret;
	return count;
}
static DEVICE_ATTR_WO(reset_assert);

static ssize_t reset_state_show(struct device *dev, struct device_attribute *attr, char *buf)
{
	struct ccci_scp_resource_owner *owner = dev_get_drvdata(dev);
	struct ccci_scp_reset *reset = &owner->reset;
	ssize_t length;

	mutex_lock(&owner->lock);
	length = sysfs_emit(buf,
			    "requested=%u attempted=%u writes=%u reads=%u core0_set=%u core1_set=%u core0_raw=0x%08x core1_raw=0x%08x latched=%u error=%d release_supported=0 isolation_known=0 ready_known=0\n",
			    owner->reset_requested, reset->attempted, reset->writes, reset->reads,
			    reset->set[0], reset->set[1], reset->raw[0], reset->raw[1],
			    reset->latched, reset->error);
	mutex_unlock(&owner->lock);
	return length;
}
static DEVICE_ATTR_RO(reset_state);
#endif

#if IS_ENABLED(CONFIG_OP6893_CCCI_SCP_INITIAL_BOOT)
/* Owner mutex held; no receive API reentry. */
static int ccci_scp_initial_asserted(void *context)
{
	struct ccci_scp_resource_owner *owner = context;
	int ret;

	ret = ccci_scp_inspect_reset_check(owner->inspect);
	if (ret)
		return ret;
	if ((readl(owner->core_tcm[0] + 4) | readl(owner->core_tcm[1] + 4)) & 1)
		return -EBUSY;
	return 0;
}

static int ccci_scp_initial_layout(struct device *dev, struct ccci_scp_layout *layout)
{
	u32 tx[42], rx[68];
	int ret;

	/* This experiment implements the complete same-board 14 TX / 17 RX table. */
	if (of_property_count_u32_elems(dev->of_node, "mediatek,send-table") !=
	    ARRAY_SIZE(tx) ||
	    of_property_count_u32_elems(dev->of_node, "mediatek,recv-table") !=
	    ARRAY_SIZE(rx))
		return -EINVAL;
	ret = of_property_read_u32_array(dev->of_node, "mediatek,send-table",
					 tx, ARRAY_SIZE(tx));
	if (ret)
		return ret;
	ret = of_property_read_u32_array(dev->of_node, "mediatek,recv-table",
					 rx, ARRAY_SIZE(rx));
	if (ret)
		return ret;
	return ccci_scp_build_layout(tx, ARRAY_SIZE(tx), rx, ARRAY_SIZE(rx),
				     CCCI_SCP_MAX_MBOXES, layout);
}

static ssize_t initial_boot_store(struct device *dev, struct device_attribute *attr,
				  const char *buf, size_t count)
{
	struct ccci_scp_resource_owner *owner = dev_get_drvdata(dev);
	struct ccci_scp_reset cleanup = {};
	struct ccci_scp_event event;
	unsigned long deadline;
	u32 base, size, gpr1 = 0, gpr2 = 0;
	bool saved = false;
	int ret, cleanup_ret;

	if (!sysfs_streq(buf, "test"))
		return -EINVAL;
	mutex_lock(&owner->lock);
	if (owner->reset_requested) {
		ret = -EALREADY;
	} else {
		ret = ccci_scp_inspect_reset_check(owner->inspect);
		if (!ret) {
			/* Shared one-shot gate also excludes concurrent reset_assert. */
			owner->reset_requested = true;
			owner->boot_requested = true;
			owner->boot_running = true;
		}
	}
	mutex_unlock(&owner->lock);
	if (ret)
		return ret;

	ccci_scp_receive_stop(owner->receive);
	mutex_lock(&owner->lock);
	ret = ccci_scp_reset_assert(&owner->reset, owner->core_tcm,
				    ccci_scp_inspect_reset_check, owner->inspect);
	ccci_scp_inspect_record_reset(owner->inspect, owner->reset.writes);
	if (ret)
		goto unlock_cleanup;
	ret = ccci_scp_inspect_boot(owner->inspect, &base, &size);
	if (ret)
		goto unlock_cleanup;
	ret = ccci_scp_mmio_discard(owner->mmio, &owner->layout,
				    ccci_scp_initial_asserted, owner, &owner->boot_discarded);
	if (ret)
		goto unlock_cleanup;
	/* One attempt per Linux boot; no reuse of LK or a previous RX generation. */
	owner->reset_generation = 1;
	mutex_unlock(&owner->lock);
	ret = ccci_scp_receive_start(owner->receive, &owner->layout, 1, 16);
	if (ret)
		goto cleanup;
	mutex_lock(&owner->lock);
	ret = ccci_scp_initial_asserted(owner);
	if (ret)
		goto unlock_cleanup;
	gpr1 = readl(owner->core_tcm[0] + 0x44);
	gpr2 = readl(owner->core_tcm[0] + 0x48);
	saved = true;
	writel(base, owner->core_tcm[0] + 0x44);
	writel(size, owner->core_tcm[0] + 0x48);
	writel(1, owner->core_tcm[0]);
	/* Complete GPR publication and reset release before any READY acceptance. */
	mb();
	ccci_scp_inspect_record_reset(owner->inspect, 3);
	owner->reset.latched = false;
	ret = (readl(owner->core_tcm[0] + 4) & 1) ? 0 : -EIO;
	mutex_unlock(&owner->lock);
	if (ret)
		goto cleanup;

	deadline = jiffies + msecs_to_jiffies(3000);
	ret = -ETIMEDOUT;
	do {
		int popped = ccci_scp_receive_pop(owner->receive, 1, &event);

		if (popped == -ENODATA) {
			usleep_range(10000, 12000);
			continue;
		}
		if (popped) {
			ret = popped;
			break;
		}
		mutex_lock(&owner->lock);
		owner->boot_events++;
		mutex_unlock(&owner->lock);
		if (event.pin.channel == 9 || event.pin.channel == 22) {
			ret = event.generation == 1 ?
				ccci_scp_decode_ready(&event.pin, event.data, event.bytes,
						      0x180000) : -ESTALE;
			if (!ret) {
				mutex_lock(&owner->lock);
				owner->boot_ready_channel = event.pin.channel;
				mutex_unlock(&owner->lock);
			}
			memzero_explicit(&event, sizeof(event));
			break;
		}
		/* Discard logger/client notifications; never follow firmware pointers. */
		memzero_explicit(&event, sizeof(event));
	} while (time_before(jiffies, deadline));
	goto cleanup;

unlock_cleanup:
	mutex_unlock(&owner->lock);
cleanup:
	/* Stop consumption before asserting cores, without the worker's mutex. */
	ccci_scp_receive_stop(owner->receive);
	memzero_explicit(&event, sizeof(event));
	mutex_lock(&owner->lock);
	owner->reset_generation = 0;
	cleanup_ret = ccci_scp_reset_assert(&cleanup, owner->core_tcm,
					    ccci_scp_inspect_reset_check, owner->inspect);
	ccci_scp_inspect_record_reset(owner->inspect, cleanup.writes);
	if (!cleanup_ret && saved) {
		writel(gpr1, owner->core_tcm[0] + 0x44);
		writel(gpr2, owner->core_tcm[0] + 0x48);
		/* Finish restoring AP-owned GPRs while both cores remain asserted. */
		mb();
		ccci_scp_inspect_record_reset(owner->inspect, 2);
	}
	owner->reset = cleanup;
	owner->boot_error = ret;
	owner->boot_cleanup_error = cleanup_ret;
	owner->boot_running = false;
	mutex_unlock(&owner->lock);
	if (ret || cleanup_ret)
		return ret ? ret : cleanup_ret;
	return count;
}
static DEVICE_ATTR_WO(initial_boot);

static ssize_t boot_state_show(struct device *dev, struct device_attribute *attr, char *buf)
{
	struct ccci_scp_resource_owner *owner = dev_get_drvdata(dev);
	ssize_t length;
	bool ready = false;

	mutex_lock(&owner->lock);
#if IS_ENABLED(CONFIG_OP6893_CCCI_SCP_RUNTIME)
	ready = owner->runtime.ready;
#endif
	length = sysfs_emit(buf,
			    "requested=%u running=%u generation=%llu discarded=%u events=%u ready_observed=%u ready_channel=%u error=%d cleanup_error=%d ready_now=%u modem_started=0\n",
			    owner->boot_requested, owner->boot_running, owner->reset_generation,
			    owner->boot_discarded, owner->boot_events, !!owner->boot_ready_channel,
			    owner->boot_ready_channel, owner->boot_error,
			    owner->boot_cleanup_error, ready);
	mutex_unlock(&owner->lock);
	return length;
}
static DEVICE_ATTR_RO(boot_state);
#endif

static struct attribute *ccci_scp_resource_attrs[] = {
#if IS_ENABLED(CONFIG_OP6893_CCCI_SCP_INITIAL_BOOT)
#if IS_ENABLED(CONFIG_OP6893_CCCI_SCP_RUNTIME)
	&dev_attr_runtime.attr,
	&dev_attr_runtime_state.attr,
#endif
	&dev_attr_initial_boot.attr,
	&dev_attr_boot_state.attr,
#endif
	&dev_attr_state.attr,
	&dev_attr_pin.attr,
	&dev_attr_cfg.attr,
	&dev_attr_irq.attr,
	&dev_attr_mmio.attr,
	&dev_attr_core_tcm.attr,
#if IS_ENABLED(CONFIG_OP6893_CCCI_SCP_RESET_ASSERT)
	&dev_attr_reset_assert.attr,
	&dev_attr_reset_state.attr,
#endif
#if IS_ENABLED(CONFIG_OP6893_CCCI_SCP_OWNER_OBSERVE)
	&dev_attr_cores.attr,
	&dev_attr_header.attr,
#endif
	NULL,
};
ATTRIBUTE_GROUPS(ccci_scp_resource);

static int ccci_scp_resource_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct ccci_scp_resource_owner *owner;
	int ret;

	if (!of_machine_is_compatible("realme,op6893"))
		return -ENODEV;
	owner = devm_kzalloc(dev, sizeof(*owner), GFP_KERNEL);
	if (!owner)
		return -ENOMEM;
	owner->clock = devm_clk_get_enabled(dev, "scp");
	if (IS_ERR(owner->clock))
		return dev_err_probe(dev, PTR_ERR(owner->clock), "SCP clock unavailable\n");
	if (clk_get_rate(owner->clock) != 26000000)
		return dev_err_probe(dev, -EBUSY, "SCP bootstrap condition changed\n");
	mutex_init(&owner->lock);
	mutex_lock(&ccci_scp_resource_owner_lock);
	if (ccci_scp_resource_claimed) {
		mutex_unlock(&ccci_scp_resource_owner_lock);
		return -EBUSY;
	}
	ccci_scp_resource_claimed = true;
	mutex_unlock(&ccci_scp_resource_owner_lock);
	ret = devm_add_action_or_reset(dev, ccci_scp_resource_unclaim, NULL);
	if (ret)
		return ret;
#if IS_ENABLED(CONFIG_OP6893_CCCI_SCP_MMIO_RESERVE)
	/* Replace, never alias, the old cfg and observer pending mappings. */
	ret = ccci_scp_mmio_reserve(pdev, &owner->mmio);
	if (ret)
		return dev_err_probe(dev, ret, "SCP MMIO reservation failed\n");
#elif IS_ENABLED(CONFIG_OP6893_CCCI_SCP_CFG_RESERVE)
	ret = ccci_scp_cfg_reserve(pdev, owner);
	if (ret)
		return dev_err_probe(dev, ret, "SCP cfg reservation failed\n");
#endif
#if IS_ENABLED(CONFIG_OP6893_CCCI_SCP_CORE_TCM_RESERVE)
	ret = ccci_scp_core_tcm_reserve(pdev, owner);
	if (ret)
		return dev_err_probe(dev, ret, "SCP core/TCM reservation failed\n");
#endif
#if IS_ENABLED(CONFIG_OP6893_CCCI_SCP_OWNER_OBSERVE)
	ret = ccci_scp_inspect_create(pdev, owner->core_tcm, &owner->lock,
				      ccci_scp_resource_inspect_check, owner, &owner->inspect);
	if (ret)
		return dev_err_probe(dev, ret, "SCP observer setup failed\n");
#endif
#if IS_ENABLED(CONFIG_OP6893_CCCI_SCP_RECEIVE_RESERVE)
	/* Replace the legacy IRQ owner; reserve does not prepare or enable RX. */
	ret = ccci_scp_receive_reserve(pdev, &owner->lock, owner->mmio,
				       ccci_scp_resource_receive_check, owner, &owner->receive);
	if (ret)
		return dev_err_probe(dev, ret, "SCP receive reservation failed\n");
#elif IS_ENABLED(CONFIG_OP6893_CCCI_SCP_IRQ_RESERVE)
	ret = ccci_scp_irq_reserve(pdev, &owner->lock, &owner->irq);
	if (ret)
		return dev_err_probe(dev, ret, "SCP IRQ reservation failed\n");
#endif
#if IS_ENABLED(CONFIG_OP6893_CCCI_SCP_INITIAL_BOOT)
	ret = ccci_scp_initial_layout(dev, &owner->layout);
	if (ret)
		return dev_err_probe(dev, ret, "SCP mailbox layout invalid\n");
#endif
#if IS_ENABLED(CONFIG_OP6893_CCCI_SCP_RUNTIME)
	ret = ccci_scp_runtime_create(pdev, owner);
	if (ret)
		return dev_err_probe(dev, ret, "SCP runtime setup failed\n");
#endif
	platform_set_drvdata(pdev, owner);
	dev_info(dev, "explicit resource query/pin only; no SMC at probe or SCP start\n");
	return 0;
}

static const struct of_device_id ccci_scp_resource_match[] = {
	{ .compatible = "realme,op6893-scp-resource-hold" },
	{}
};

/* Boot-lifetime owner: consumer unbind must never release all SCP resources. */
static struct platform_driver ccci_scp_resource_driver = {
	.probe = ccci_scp_resource_probe,
	.driver = {
		.name = "op6893-scp-resource-hold",
		.of_match_table = ccci_scp_resource_match,
		.dev_groups = ccci_scp_resource_groups,
		.suppress_bind_attrs = true,
	},
};
builtin_platform_driver(ccci_scp_resource_driver);

MODULE_DESCRIPTION("op6893 explicit boot-lifetime SCP 26 MHz resource pin");
MODULE_LICENSE("GPL");
