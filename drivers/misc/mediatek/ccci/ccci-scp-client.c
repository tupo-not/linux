// SPDX-License-Identifier: GPL-2.0-only
/* A single leased APCCCI client of the boot-lifetime SCP runtime owner. */
#include <linux/device.h>
#include <linux/err.h>
#include <linux/of.h>
#include <linux/of_platform.h>
#include <linux/platform_device.h>
#include <linux/slab.h>

#include "ccci-scp-inspect.h"
#include "ccci-scp-mmio.h"
#include "ccci-scp-owner.h"

static int scp_client_check(struct ccci_scp_client *client, u64 generation)
{
	struct ccci_scp_resource_owner *owner = client->owner;
	struct ccci_scp_runtime *runtime = &owner->runtime;

	lockdep_assert_held(&owner->lock);
	if (!client->attached || runtime->client != client || !generation ||
	    client->generation != generation || owner->reset_generation != generation)
		return -ESTALE;
	if (client->faulted || runtime->closing || !runtime->active || !runtime->ready ||
	    runtime->error || runtime->cleanup_error)
		return -ESHUTDOWN;
	return 0;
}

int ccci_scp_client_acquire(struct ccci_scp_client *client, u64 *generation)
{
	struct ccci_scp_resource_owner *owner = client->owner;
	struct ccci_scp_runtime *runtime = &owner->runtime;
	int ret = 0;

	if (!generation)
		return -EINVAL;
	*generation = 0;
	mutex_lock(&runtime->control);
	mutex_lock(&owner->lock);
	if (runtime->client) {
		ret = -EBUSY;
	} else if (runtime->client_attempted) {
		ret = -EALREADY;
	} else if (!runtime->active || !runtime->ready || !owner->reset_generation ||
		   runtime->closing || runtime->error || runtime->cleanup_error) {
		ret = -ESHUTDOWN;
	} else if (runtime->apccci_events && runtime->apccci_state == CCCI_SCP_STATE_RBREADY) {
		/* An inherited RBREADY is not a reply to this client's SHM_INIT. */
		ret = -EPROTO;
	} else {
		runtime->client_attempted = true;
		client->generation = owner->reset_generation;
		client->attached = true;
		runtime->client = client;
		*generation = client->generation;
	}
	mutex_unlock(&owner->lock);
	mutex_unlock(&runtime->control);
	return ret;
}

int ccci_scp_client_check(struct ccci_scp_client *client, u64 generation)
{
	struct ccci_scp_resource_owner *owner = client->owner;
	int ret;

	mutex_lock(&owner->runtime.control);
	mutex_lock(&owner->lock);
	ret = scp_client_check(client, generation);
	mutex_unlock(&owner->lock);
	mutex_unlock(&owner->runtime.control);
	return ret;
}

/* Both locks held. Fault work joins RX only after these locks are released. */
static void scp_client_fault(struct ccci_scp_client *client, int error)
{
	struct ccci_scp_resource_owner *owner = client->owner;
	struct ccci_scp_runtime *runtime = &owner->runtime;

	if (!runtime->error)
		runtime->error = error;
	runtime->ready = false;
	owner->boot_error = runtime->error;
	if (!client->faulted) {
		client->faulted = true;
		client->notify(client->context, client->generation, 0, runtime->error);
	}
	complete_all(&runtime->ready_event);
	schedule_work(&runtime->fault_work);
}

int ccci_scp_client_send(struct ccci_scp_client *client, u16 op, u32 data)
{
	struct ccci_scp_resource_owner *owner = client->owner;
	struct ccci_scp_runtime *runtime = &owner->runtime;
	u8 validation[8];
	int ret, put_ret;

	/* Reject bad requests before taking awake or touching any hardware. */
	ret = ccci_scp_encode(validation, sizeof(validation), 2, op, data);
	memzero_explicit(validation, sizeof(validation));
	if (ret != sizeof(validation))
		return ret < 0 ? ret : -EPROTO;
	mutex_lock(&runtime->control);
	mutex_lock(&owner->lock);
	ret = scp_client_check(client, client->generation);
	if (ret)
		goto unlock;
	ret = ccci_scp_inspect_reset_check(owner->inspect);
	if (ret)
		goto fault;
	ret = ccci_scp_awake_get(&runtime->awake);
	if (ret)
		goto fault;
	ret = ccci_scp_mmio_apccci(owner->mmio, &owner->layout, client->generation, op, data);
	put_ret = ccci_scp_awake_put(&runtime->awake);
	if (!put_ret)
		runtime->awake_pairs++;
	if (!ret)
		ret = put_ret;
	if (!ret)
		ret = ccci_scp_inspect_reset_check(owner->inspect);
	if (ret)
		goto fault;
	client->tx_completed++;
	goto unlock;
fault:
	scp_client_fault(client, ret);
unlock:
	mutex_unlock(&owner->lock);
	mutex_unlock(&runtime->control);
	return ret;
}

void ccci_scp_client_release(struct ccci_scp_client *client)
{
	struct ccci_scp_resource_owner *owner = client->owner;

	mutex_lock(&owner->runtime.control);
	mutex_lock(&owner->lock);
	if (owner->runtime.client == client)
		owner->runtime.client = NULL;
	client->attached = false;
	mutex_unlock(&owner->lock);
	mutex_unlock(&owner->runtime.control);
}

static void scp_client_cleanup(void *data)
{
	ccci_scp_client_release(data);
}

static void scp_client_put_provider(void *data)
{
	put_device(data);
}

struct ccci_scp_client *ccci_scp_client_create(struct device *consumer,
					       ccci_scp_state_fn notify, void *context)
{
	struct platform_device *provider;
	struct device_node *node;
	struct ccci_scp_client *client;
	int ret;

	if (!notify)
		return ERR_PTR(-EINVAL);
	node = of_parse_phandle(consumer->of_node, "mediatek,scp", 0);
	if (!node)
		return ERR_PTR(-EINVAL);
	if (!of_device_is_compatible(node, "realme,op6893-scp-resource-hold") ||
	    !of_device_is_available(node)) {
		of_node_put(node);
		return ERR_PTR(-ENODEV);
	}
	provider = of_find_device_by_node(node);
	of_node_put(node);
	if (!provider)
		return ERR_PTR(-EPROBE_DEFER);
	ret = devm_add_action_or_reset(consumer, scp_client_put_provider, &provider->dev);
	if (ret)
		return ERR_PTR(ret);
	/* The provider publishes drvdata only after runtime and cleanup are ready. */
	if (!platform_get_drvdata(provider))
		return ERR_PTR(-EPROBE_DEFER);
	if (!device_link_add(consumer, &provider->dev, DL_FLAG_AUTOREMOVE_CONSUMER))
		return ERR_PTR(-ENOMEM);
	client = devm_kzalloc(consumer, sizeof(*client), GFP_KERNEL);
	if (!client)
		return ERR_PTR(-ENOMEM);
	client->owner = platform_get_drvdata(provider);
	client->notify = notify;
	client->context = context;
	ret = devm_add_action_or_reset(consumer, scp_client_cleanup, client);
	return ret ? ERR_PTR(ret) : client;
}
