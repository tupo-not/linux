// SPDX-License-Identifier: GPL-2.0-only
/* Explicit SCP service using LK's initial image and clean-stop recovery. */
#include <linux/bitops.h>
#include <linux/device.h>
#include <linux/mfd/syscon.h>
#include <linux/platform_device.h>
#include <linux/sysfs.h>

#include "ccci-scp-inspect.h"
#include "ccci-scp-mmio.h"
#include "ccci-scp-owner.h"
#include "ccci-scp-receive.h"

/* owner->lock held; callback may only enqueue scalar events, never reenter. */
static void ccci_scp_runtime_notify(struct ccci_scp_resource_owner *owner, u32 state, int error)
{
	struct ccci_scp_client *client = owner->runtime.client;

	lockdep_assert_held(&owner->lock);
	if (!client || client->faulted)
		return;
	if (error)
		client->faulted = true;
	client->notify(client->context, client->generation, state, error);
}

/* control held; receive worker must be joined without owner->lock. */
static int ccci_scp_runtime_stop(struct ccci_scp_resource_owner *owner)
{
	struct ccci_scp_runtime *runtime = &owner->runtime;
	struct ccci_scp_reset reset = {};
	bool clean;
	int ret;

	lockdep_assert_held(&runtime->control);
	ccci_scp_receive_stop(owner->receive);
	ccci_scp_receive_consumer(owner->receive, NULL, NULL);
	mutex_lock(&owner->lock);
	if (!runtime->active) {
		mutex_unlock(&owner->lock);
		return runtime->cleanup_error;
	}
	clean = runtime->ready && !runtime->client && !runtime->error &&
		!runtime->awake.users && !runtime->awake.fault;
	runtime->ready = false;
	runtime->released = false;
	ccci_scp_runtime_notify(owner, 0, runtime->error ? runtime->error : -ESHUTDOWN);
	ccci_scp_awake_invalidate(&runtime->awake);
	owner->reset_generation = 0;
	ret = ccci_scp_reset_assert(&reset, owner->core_tcm,
				    ccci_scp_inspect_reset_check, owner->inspect);
	ccci_scp_inspect_record_reset(owner->inspect, reset.writes);
	if (!ret && runtime->saved) {
		writel(runtime->gpr[0], owner->core_tcm[0] + 0x44);
		writel(runtime->gpr[1], owner->core_tcm[0] + 0x48);
		/* Restore AP GPRs only after both cores' asserted readbacks. */
		mb();
		ccci_scp_inspect_record_reset(owner->inspect, 2);
	}
	memzero_explicit(runtime->gpr, sizeof(runtime->gpr));
	runtime->saved = false;
	runtime->cleanup_error = ret;
	runtime->clean_stop = clean && !ret;
	runtime->active = false;
	owner->boot_running = false;
	owner->reset = reset;
	owner->boot_cleanup_error = ret;
	mutex_unlock(&owner->lock);
	return ret;
}

static void ccci_scp_runtime_fault_work(struct work_struct *work)
{
	struct ccci_scp_runtime *runtime = container_of(work, struct ccci_scp_runtime,
						      fault_work);

	mutex_lock(&runtime->control);
	ccci_scp_runtime_stop(runtime->owner);
	mutex_unlock(&runtime->control);
}

/* Receive callback already owns owner->lock; never reenter receive/STOP. */
static int ccci_scp_runtime_consume(void *context, const struct ccci_scp_event *event,
				    int error)
{
	struct ccci_scp_resource_owner *owner = context;
	struct ccci_scp_runtime *runtime = &owner->runtime;
	u32 state;
	int ret = error;

	lockdep_assert_held(&owner->lock);
	if (!ret && (!runtime->active || !runtime->released || !event || !owner->reset_generation ||
		     event->generation != owner->reset_generation))
		ret = -ESTALE;
	if (ret)
		goto fault;
	owner->boot_events++;
	switch (event->pin.channel) {
	case 9:
	case 22:
		ret = ccci_scp_decode_ready(&event->pin, event->data, event->bytes, 0x180000);
		if (ret)
			goto fault;
		owner->boot_ready_channel = event->pin.channel;
		runtime->ready = true;
		complete_all(&runtime->ready_event);
		break;
	case CCCI_SCP_APCCCI_RX:
		ret = ccci_scp_decode_state(event->data, event->bytes, &state);
		if (ret || event->pin.response) {
			ret = ret ? ret : -EPROTO;
			goto fault;
		}
		runtime->apccci_events++;
		runtime->apccci_state = state;
		ccci_scp_runtime_notify(owner, state, 0);
		break;
	case 21:
		runtime->logger_events++;
		break;
	default:
		runtime->other_events++;
		break;
	}
	return 0;
fault:
	if (!runtime->error)
		runtime->error = ret;
	runtime->ready = false;
	owner->boot_error = runtime->error;
	ccci_scp_runtime_notify(owner, 0, runtime->error);
	complete_all(&runtime->ready_event);
	/* A separate work item may join the receive worker after this returns. */
	schedule_work(&runtime->fault_work);
	return ret;
}

/* Asserted-core callback used while rejecting inherited notifications. */
static int ccci_scp_runtime_asserted(void *context)
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

/* control and owner mutex held; hardware acknowledgment needs no AP RX work. */
static int ccci_scp_runtime_awake(struct ccci_scp_resource_owner *owner,
				   bool send, bool sleep_off)
{
	struct ccci_scp_runtime *runtime = &owner->runtime;
	int ret, put_ret;

	if (!runtime->active || !runtime->ready || runtime->error ||
	    !owner->reset_generation)
		return -ESHUTDOWN;
	ret = ccci_scp_inspect_reset_check(owner->inspect);
	if (ret)
		return ret;
	ret = ccci_scp_awake_get(&runtime->awake);
	if (ret)
		return ret;
	if (send)
		ret = ccci_scp_mmio_md_invalid(owner->mmio, &owner->layout,
					      owner->reset_generation);
	else if (sleep_off)
		ret = ccci_scp_mmio_sleep_off(owner->mmio, &owner->layout,
					      owner->reset_generation);
	put_ret = ccci_scp_awake_put(&runtime->awake);
	if (!put_ret)
		runtime->awake_pairs++;
	if (!ret && !put_ret && send)
		runtime->tx_completed++;
	if (!ret)
		ret = put_ret;
	if (!ret)
		ret = ccci_scp_inspect_reset_check(owner->inspect);
	return ret;
}

static int ccci_scp_runtime_start(struct ccci_scp_resource_owner *owner,
				  bool check_restore, bool restore)
{
	struct ccci_scp_runtime *runtime = &owner->runtime;
	u32 base, size;
	u64 generation;
	int ret;

	lockdep_assert_held(&runtime->control);
	mutex_lock(&owner->lock);
	if (runtime->closing || (!restore && owner->reset_requested)) {
		ret = -EALREADY;
		goto unlock;
	}
	if (restore && (!runtime->clean_stop || runtime->active || runtime->client ||
			!owner->reset.latched || runtime->error || runtime->cleanup_error)) {
		ret = -EBUSY;
		goto unlock;
	}
	generation = runtime->last_generation + 1;
	if (!generation) {
		ret = -EOVERFLOW;
		goto unlock;
	}
	ret = ccci_scp_inspect_reset_check(owner->inspect);
	if (ret)
		goto unlock;
	owner->reset_requested = true;
	owner->boot_requested = true;
	owner->boot_running = true;
	runtime->active = true;
	runtime->clean_stop = false;
	runtime->last_generation = generation;
	runtime->step = "receive-stop";
	reinit_completion(&runtime->ready_event);
	owner->boot_events = 0;
	owner->boot_ready_channel = 0;
	runtime->logger_events = 0;
	runtime->other_events = 0;
	runtime->apccci_events = 0;
	runtime->apccci_state = 0;
	runtime->awake_pairs = 0;
	runtime->tx_completed = 0;
	mutex_unlock(&owner->lock);

	ccci_scp_receive_stop(owner->receive);
	ret = ccci_scp_receive_consumer(owner->receive, ccci_scp_runtime_consume, owner);
	if (ret)
		goto fail;
	mutex_lock(&owner->lock);
	if (restore) {
		runtime->step = "reset-readback";
		ret = ccci_scp_runtime_asserted(owner);
		if (!ret) {
			runtime->step = "tx-idle";
			ret = ccci_scp_mmio_tx_idle(owner->mmio);
		}
		if (!ret) {
			runtime->step = "restore-memory";
			ret = ccci_scp_inspect_restore(owner->inspect, &base, &size);
		}
		if (!ret)
			ret = ccci_scp_awake_init(&runtime->awake, runtime->awake.infra);
		if (ret)
			goto unlock_fail;
		goto discard;
	}
	ret = ccci_scp_reset_assert(&owner->reset, owner->core_tcm,
				    ccci_scp_inspect_reset_check, owner->inspect);
	ccci_scp_inspect_record_reset(owner->inspect, owner->reset.writes);
	if (ret)
		goto unlock_fail;
	ret = ccci_scp_inspect_boot(owner->inspect, &base, &size);
	if (ret)
		goto unlock_fail;
	if (check_restore) {
		ret = ccci_scp_runtime_asserted(owner);
		if (!ret)
			ret = ccci_scp_inspect_restore_check(owner->inspect, true);
		if (ret)
			goto unlock_fail;
	}
discard:
	runtime->step = "discard";
	ret = ccci_scp_mmio_discard(owner->mmio, &owner->layout,
				    ccci_scp_runtime_asserted, owner, &owner->boot_discarded);
	if (ret)
		goto unlock_fail;
	runtime->client_attempted = false;
	owner->reset_generation = generation;
	runtime->step = "receive-start";
	mutex_unlock(&owner->lock);
	ret = ccci_scp_receive_start(owner->receive, &owner->layout, generation, 16);
	if (ret)
		goto fail;
	mutex_lock(&owner->lock);
	runtime->step = "release";
	ret = runtime->error ?: ccci_scp_runtime_asserted(owner);
	if (ret)
		goto unlock_fail;
	runtime->gpr[0] = readl(owner->core_tcm[0] + 0x44);
	runtime->gpr[1] = readl(owner->core_tcm[0] + 0x48);
	runtime->saved = true;
	writel(base, owner->core_tcm[0] + 0x44);
	writel(size, owner->core_tcm[0] + 0x48);
	writel(1, owner->core_tcm[0]);
	/* Publish the share span and release before the worker can accept READY. */
	mb();
	ccci_scp_inspect_record_reset(owner->inspect, 3);
	owner->reset.latched = false;
	ret = (readl(owner->core_tcm[0] + 4) & 1) ? 0 : -EIO;
	runtime->released = !ret;
	if (!ret)
		runtime->step = "wait-ready";
	mutex_unlock(&owner->lock);
	if (ret)
		goto fail;
	if (!wait_for_completion_timeout(&runtime->ready_event, msecs_to_jiffies(3000))) {
		ret = -ETIMEDOUT;
		goto fail;
	}
	mutex_lock(&owner->lock);
	ret = runtime->error ? runtime->error : ccci_scp_runtime_awake(owner, false, false);
	if (!ret)
		runtime->step = "ready";
	mutex_unlock(&owner->lock);
	if (!ret)
		return 0;
	goto fail;
unlock_fail:
	mutex_unlock(&owner->lock);
fail:
	mutex_lock(&owner->lock);
	runtime->error = ret;
	owner->boot_error = ret;
	mutex_unlock(&owner->lock);
	ccci_scp_runtime_stop(owner);
	return ret;
unlock:
	mutex_unlock(&owner->lock);
	return ret;
}

static ssize_t runtime_store(struct device *dev, struct device_attribute *attr,
			     const char *buf, size_t count)
{
	struct ccci_scp_resource_owner *owner = dev_get_drvdata(dev);
	struct ccci_scp_runtime *runtime = &owner->runtime;
	bool send = sysfs_streq(buf, "md-invalid");
	bool sleep_off = sysfs_streq(buf, "sleep-off");
	int ret;

	mutex_lock(&runtime->control);
	if (sysfs_streq(buf, "start") || sysfs_streq(buf, "start-check") ||
	    sysfs_streq(buf, "restart")) {
		ret = ccci_scp_runtime_start(owner, sysfs_streq(buf, "start-check"),
					     sysfs_streq(buf, "restart"));
	} else if (sysfs_streq(buf, "stop")) {
		ret = runtime->client ? -EBUSY : ccci_scp_runtime_stop(owner);
	} else if (sysfs_streq(buf, "restore-check")) {
		mutex_lock(&owner->lock);
		if (runtime->client || runtime->active || !owner->reset.latched ||
		    runtime->error || runtime->cleanup_error) {
			ret = -EBUSY;
		} else {
			ret = ccci_scp_runtime_asserted(owner);
			if (!ret)
				ret = ccci_scp_inspect_restore_check(owner->inspect, false);
		}
		mutex_unlock(&owner->lock);
	} else if (send || sleep_off || sysfs_streq(buf, "awake")) {
		mutex_lock(&owner->lock);
		if ((send || sleep_off) && runtime->client) {
			ret = -EBUSY;
			mutex_unlock(&owner->lock);
			goto unlock;
		}
		ret = send && runtime->tx_completed ? -EALREADY :
			ccci_scp_runtime_awake(owner, send, sleep_off);
		if (ret && ret != -EALREADY) {
			runtime->error = ret;
			owner->boot_error = ret;
		}
		mutex_unlock(&owner->lock);
		if (ret && ret != -EALREADY)
			ccci_scp_runtime_stop(owner);
	} else {
		ret = -EINVAL;
	}
unlock:
	mutex_unlock(&runtime->control);
	return ret ? ret : (ssize_t)count;
}
DEVICE_ATTR_WO(runtime);

static ssize_t runtime_state_show(struct device *dev, struct device_attribute *attr,
				  char *buf)
{
	struct ccci_scp_resource_owner *owner = dev_get_drvdata(dev);
	struct ccci_scp_runtime *runtime = &owner->runtime;
	ssize_t length;

	mutex_lock(&owner->lock);
	length = sysfs_emit(buf,
			    "active=%u ready=%u generation=%llu events=%u logger=%u other=%u apccci_events=%u apccci_state=%u awake_pairs=%u awake_users=%u awake_fault=%u awake_cleanup_error=%d tx_completed=%u error=%d cleanup_error=%d modem_started=0 step=%s last_generation=%llu\n",
			    runtime->active, runtime->ready, owner->reset_generation,
			    owner->boot_events,
			    runtime->logger_events, runtime->other_events, runtime->apccci_events,
			    runtime->apccci_state, runtime->awake_pairs, runtime->awake.users,
			    runtime->awake.fault, runtime->awake.cleanup_error,
			    runtime->tx_completed,
			    runtime->error, runtime->cleanup_error,
			    runtime->step ?: "idle", runtime->last_generation);
	mutex_unlock(&owner->lock);
	return length;
}
DEVICE_ATTR_RO(runtime_state);

static void ccci_scp_runtime_release(void *data)
{
	struct ccci_scp_resource_owner *owner = data;
	struct ccci_scp_runtime *runtime = &owner->runtime;

	mutex_lock(&runtime->control);
	runtime->closing = true;
	ccci_scp_runtime_stop(owner);
	mutex_unlock(&runtime->control);
	cancel_work_sync(&runtime->fault_work);
}

int ccci_scp_runtime_create(struct platform_device *pdev,
			    struct ccci_scp_resource_owner *owner)
{
	struct ccci_scp_runtime *runtime = &owner->runtime;
	struct regmap *infra;
	int ret;

	infra = syscon_regmap_lookup_by_phandle(pdev->dev.of_node, "mediatek,infracfg");
	if (IS_ERR(infra))
		return PTR_ERR(infra);
	ret = ccci_scp_awake_init(&runtime->awake, infra);
	if (ret)
		return ret;
	mutex_init(&runtime->control);
	init_completion(&runtime->ready_event);
	INIT_WORK(&runtime->fault_work, ccci_scp_runtime_fault_work);
	runtime->owner = owner;
	return devm_add_action_or_reset(&pdev->dev, ccci_scp_runtime_release, owner);
}
