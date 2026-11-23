// SPDX-License-Identifier: GPL-2.0-only
/* Unique post-HS2 CCIF2 owner. No MD CPU release or implicit start at probe. */
#include <linux/arm-smccc.h>
#include <linux/clk.h>
#include <linux/err.h>
#include <linux/io.h>
#include <linux/ioport.h>
#include <linux/mutex.h>
#include <linux/platform_device.h>
#include <linux/slab.h>
#include <linux/soc/mediatek/mtk_sip_svc.h>
#include <linux/sysfs.h>

#include "ccci-ccif.h"
#include "ccci-ccism.h"
#include "ccci-ccism-bridge.h"
#include "ccci-ccism-session.h"
#include "ccci-scp-runtime.h"
#include "ccci-smem.h"

struct ccci_ccism_bridge {
	/* Lifecycle only; callbacks must not take this or the parent MD mutex. */
	struct mutex lifecycle;
	struct ccci_ccif *ccif;
	struct ccci_scp_client *client;
	struct ccci_ccism_session *session;
	struct ccci_ccism_plan plan;
	void __iomem *ap;
	void __iomem *md;
	void __iomem *shared;
	struct clk_bulk_data clocks[2];
	bool attempted;
	bool leased;
	/* Session serializes these fields; no hardware access outside its ops. */
	bool clocks_held;
	bool smc_attempted;
};

static int bridge_check(void *context, u64 generation)
{
	struct ccci_ccism_bridge *bridge = context;

	return ccci_scp_client_check(bridge->client, generation);
}

static int bridge_on(void *context)
{
	struct ccci_ccism_bridge *bridge = context;
	struct arm_smccc_res result;
	int ret;

	if (bridge->clocks_held || bridge->smc_attempted)
		return -EALREADY;
	ret = clk_bulk_prepare_enable(ARRAY_SIZE(bridge->clocks), bridge->clocks);
	if (ret)
		return ret;
	bridge->clocks_held = true;
	bridge->smc_attempted = true;
	/* Same-board SCP_CLK_SET_DONE. No address or alternative ABI is passed. */
	arm_smccc_smc(MTK_SIP_KERNEL_CCCI_CONTROL, 13, 0, 0, 0, 0, 0, 0, &result);
	if (result.a0 == (unsigned long)SMCCC_RET_NOT_SUPPORTED)
		return -EOPNOTSUPP;
	/* Conservative bring-up policy: vendor logs a0 but does not define it. */
	return result.a0 ? -EIO : 0;
}

static int bridge_md_send(void *context, u32 message)
{
	struct ccci_ccism_bridge *bridge = context;

	return ccci_ccif_system_send(bridge->ccif, message, 0);
}

static int bridge_clear(void *context, u32 ap, u32 size)
{
	struct ccci_ccism_bridge *bridge = context;

	if (!bridge->clocks_held || ap != bridge->plan.ap || size != bridge->plan.size)
		return -ERANGE;
	memset_io(bridge->shared, 0, size);
	/* ID16 must be globally initialized before SHM_INIT exposes it to SCP. */
	mb();
	return 0;
}

static int bridge_scp_send(void *context, u16 op, u32 data)
{
	struct ccci_ccism_bridge *bridge = context;

	return ccci_scp_client_send(bridge->client, op, data);
}

static int bridge_off(void *context)
{
	struct ccci_ccism_bridge *bridge = context;

	if (!bridge->clocks_held)
		return -EHOSTDOWN;
	/* Only called after stopped MD and a matching-generation SCP STOP. */
	writel(0xffff, bridge->ap + 0x14);
	writel(0xffff, bridge->md + 0x14);
	/* Complete both ACK writes before releasing the bridge references. */
	mb();
	clk_bulk_disable_unprepare(ARRAY_SIZE(bridge->clocks), bridge->clocks);
	bridge->clocks_held = false;
	return 0;
}

static const struct ccci_ccism_ops bridge_ops = {
	.check = bridge_check,
	.bridge_on = bridge_on,
	.md_send = bridge_md_send,
	.clear = bridge_clear,
	.scp_send = bridge_scp_send,
	.bridge_off = bridge_off,
};

static void bridge_fault(void *context, int error)
{
	struct ccci_ccism_bridge *bridge = context;

	/* Contain ordinary TX/RX without reentering the controller or MD lock. */
	ccci_ccif_abort(bridge->ccif, error);
}

static void bridge_notify(void *context, u64 generation, u32 state, int error)
{
	struct ccci_ccism_bridge *bridge = context;

	/* Provider owner mutex held: the session copies scalars only. */
	ccci_ccism_session_scp_notify(bridge->session, generation, state, error);
}

int ccci_ccism_bridge_arm(struct ccci_ccism_bridge *bridge)
{
	u64 generation;
	int ret;

	mutex_lock(&bridge->lifecycle);
	if (bridge->attempted) {
		ret = -EALREADY;
		goto out;
	}
	bridge->attempted = true;
	ret = ccci_scp_client_acquire(bridge->client, &generation);
	if (ret)
		goto out;
	bridge->leased = true;
	ret = ccci_ccism_session_arm(bridge->session, generation);
	/* Retain a failed lease until explicit stopped-MD cleanup. */
out:
	mutex_unlock(&bridge->lifecycle);
	return ret;
}

int ccci_ccism_bridge_port(void *context, const struct ccci_ccif_event *event)
{
	struct ccci_ccism_bridge *bridge = context;
	struct ccci_ccif_header header;
	int ret;

	if (!event || event->type != CCCI_CCIF_EVENT_RING || event->queue ||
	    event->size != CCCI_CCIF_HEADER_SIZE)
		return -EOPNOTSUPP;
	ret = ccci_ccif_header_decode(event->data, event->size, &header);
	if (ret)
		return ret;
	if (!header.channel && ccci_ccif_control_decode(&header) == CCCI_CCIF_CONTROL_HS2)
		return ccci_ccism_session_md_ready(bridge->session);
	if (header.channel != 2 ||
	    (header.data[1] != CCCI_CCISM_ACK && header.data[1] != CCCI_CCISM_DONE))
		return -EOPNOTSUPP;
	return ccci_ccism_session_md_event(bridge->session, event);
}

int ccci_ccism_bridge_stop(struct ccci_ccism_bridge *bridge, bool md_stopped)
{
	int ret;

	if (!md_stopped)
		return -EBUSY;
	mutex_lock(&bridge->lifecycle);
	ret = ccci_ccism_session_stop(bridge->session, true);
	mutex_unlock(&bridge->lifecycle);
	return ret;
}

int ccci_ccism_bridge_offline(struct ccci_ccism_bridge *bridge)
{
	struct ccci_ccism_session_status status;
	int ret;

	if (!bridge)
		return 0;
	mutex_lock(&bridge->lifecycle);
	/* A never-armed dormant constructor cannot have touched the bridge. */
	if (!bridge->attempted) {
		ret = 0;
		goto out;
	}
	ccci_ccism_session_status(bridge->session, &status);
	if (status.armed && status.state != CCCI_CCISM_STOPPED) {
		ret = -EBUSY;
		goto out;
	}
	ret = ccci_ccism_session_quiesce(bridge->session);
	if (!ret && bridge->leased) {
		ccci_scp_client_release(bridge->client);
		bridge->leased = false;
	}
out:
	mutex_unlock(&bridge->lifecycle);
	return ret;
}

int ccci_ccism_bridge_rearm(struct ccci_ccism_bridge *bridge, bool commit)
{
	int ret;

	mutex_lock(&bridge->lifecycle);
	if (bridge->leased) {
		ret = -EBUSY;
	} else {
		ret = ccci_ccism_session_rearm(bridge->session, commit);
		if (!ret && commit) {
			bridge->attempted = false;
			bridge->smc_attempted = false;
		}
	}
	mutex_unlock(&bridge->lifecycle);
	return ret;
}

ssize_t ccci_ccism_bridge_status(struct ccci_ccism_bridge *bridge, char *buf)
{
	struct ccci_ccism_session_status status;

	ccci_ccism_session_status(bridge->session, &status);
	return sysfs_emit(buf,
			 "state=%u generation=%llu error=%d cleanup_error=%d queue_error=%d queued=%u armed=%u md_open=%u scp_open=%u bridge_touched=%u cpu_release_issued=0\n",
			 status.state, status.generation, status.error, status.cleanup_error,
			 status.queue_error, status.queued, status.armed, status.md_open,
			 status.scp_open, status.bridge_touched);
}

static void bridge_cleanup(void *context)
{
	struct ccci_ccism_bridge *bridge = context;

	/* Built-in boot-lifetime owner; only a failed dormant probe unwinds. */
	WARN_ON(ccci_ccism_session_destroy(bridge->session));
}

static void __iomem *bridge_window(struct platform_device *pdev, const char *name, u64 base)
{
	struct resource *resource = platform_get_resource_byname(pdev, IORESOURCE_MEM, name);

	if (!resource || resource->start != base || resource_size(resource) != 0x1000)
		return ERR_PTR(-EINVAL);
	return devm_ioremap_resource(&pdev->dev, resource);
}

struct ccci_ccism_bridge *
ccci_ccism_bridge_create(struct platform_device *pdev, struct ccci_ccif *ccif,
			 const struct ccci_smem_layout *smem)
{
	struct device *dev = &pdev->dev;
	struct ccci_ccism_bridge *bridge;
	int ret;

	if (!ccif)
		return ERR_PTR(-EINVAL);
	bridge = devm_kzalloc(dev, sizeof(*bridge), GFP_KERNEL);
	if (!bridge)
		return ERR_PTR(-ENOMEM);
	mutex_init(&bridge->lifecycle);
	bridge->ccif = ccif;
	ret = ccci_ccism_plan(smem, &bridge->plan);
	if (ret)
		return ERR_PTR(ret);
	/* Reject enclosing-page aliases into runtime or adjacent SMEM users. */
	if ((bridge->plan.ap | bridge->plan.size) & 0xfff)
		return ERR_PTR(-ERANGE);
	if (!devm_request_mem_region(dev, bridge->plan.ap, bridge->plan.size, dev_name(dev)))
		return ERR_PTR(-EBUSY);
	bridge->shared = devm_ioremap_wc(dev, bridge->plan.ap, bridge->plan.size);
	if (!bridge->shared)
		return ERR_PTR(-ENOMEM);
	bridge->ap = bridge_window(pdev, "ccif2-ap", 0x1023c000);
	if (IS_ERR(bridge->ap))
		return ERR_CAST(bridge->ap);
	bridge->md = bridge_window(pdev, "ccif2-md", 0x1023d000);
	if (IS_ERR(bridge->md))
		return ERR_CAST(bridge->md);
	bridge->clocks[0].id = "ccif2-ap";
	bridge->clocks[1].id = "ccif2-md";
	ret = devm_clk_bulk_get(dev, ARRAY_SIZE(bridge->clocks), bridge->clocks);
	if (ret)
		return ERR_PTR(ret);
	bridge->session = ccci_ccism_session_create(dev, &bridge_ops, bridge,
						    &bridge->plan, bridge_fault);
	if (IS_ERR(bridge->session))
		return ERR_CAST(bridge->session);
	ret = devm_add_action_or_reset(dev, bridge_cleanup, bridge);
	if (ret)
		return ERR_PTR(ret);
	bridge->client = ccci_scp_client_create(dev, bridge_notify, bridge);
	if (IS_ERR(bridge->client))
		return ERR_CAST(bridge->client);
	return bridge;
}
