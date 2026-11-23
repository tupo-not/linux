// SPDX-License-Identifier: GPL-2.0-only
/* Unique feature-36 NVRAM cache owner: map, first clear, never re-zero. */
#include <linux/device.h>
#include <linux/errno.h>
#include <linux/io.h>
#include <linux/kernel.h>
#include <linux/mutex.h>
#include <linux/slab.h>
#include <linux/sysfs.h>

#include "ccci-nvram-cache.h"
#include "ccci-smem.h"

enum nvram_state {
	NVRAM_IDLE,
	NVRAM_PREPARED,
	NVRAM_STOPPED,
	NVRAM_FAILED,
};

struct ccci_nvram_cache {
	/* Parent lifecycle lock; no callback or worker re-enters. */
	struct mutex lock;
	struct device *dev;
	u64 ap;
	u32 md;
	u32 size;
	void __iomem *map;
	enum nvram_state state;
	int error;
	bool attempted;
	bool cleared;
};

static int nvram_fault(struct ccci_nvram_cache *cache, int error)
{
	cache->state = NVRAM_FAILED;
	cache->error = error;
	return error;
}

static int nvram_geometry(const struct ccci_smem_layout *smem,
			  struct ccci_nvram_cache *cache)
{
	const struct ccci_smem_region *region = &smem->users[CCCI_SMEM_NVRAM];
	const struct ccci_lk_region *bank = &smem->cached;

	if (!region->present || !region->cached || region->no_map ||
	    (region->ap | region->md | region->size) & 3 || !region->size ||
	    !ccci_lk_contains(bank, region->ap, region->size) ||
	    region->ap - bank->base != region->offset ||
	    (u64)region->md + region->size > 0x100000000ULL)
		return -ERANGE;
	cache->ap = region->ap;
	cache->md = region->md;
	cache->size = region->size;
	return 0;
}

int ccci_nvram_cache_prepare(struct ccci_nvram_cache *cache)
{
	int ret;

	if (!cache)
		return -EINVAL;
	mutex_lock(&cache->lock);
	if (cache->state == NVRAM_FAILED) {
		ret = -EIO;
		goto out;
	}
	if (cache->attempted) {
		ret = -EALREADY;
		goto out;
	}
	/* Commit before the first write; a retry must not re-clear a live cache. */
	cache->attempted = true;
	cache->map = devm_ioremap_wc(cache->dev, cache->ap, cache->size);
	if (!cache->map) {
		ret = nvram_fault(cache, -ENOMEM);
		goto out;
	}
	/* Same-board first-start policy: every byte of this region is cleared. */
	memset_io(cache->map, 0, cache->size);
	if (readl(cache->map) ||
	    readl((u8 __iomem *)cache->map + cache->size - sizeof(u32))) {
		ret = nvram_fault(cache, -EIO);
		goto out;
	}
	cache->cleared = true;
	cache->state = NVRAM_PREPARED;
	ret = 0;
out:
	mutex_unlock(&cache->lock);
	return ret;
}

/* Preserve the cache populated by the preceding MD generation. */
int ccci_nvram_cache_resume(struct ccci_nvram_cache *cache)
{
	int ret = -EBUSY;

	mutex_lock(&cache->lock);
	if (cache->state == NVRAM_STOPPED && cache->map && cache->cleared) {
		cache->state = NVRAM_PREPARED;
		ret = 0;
	}
	mutex_unlock(&cache->lock);
	return ret;
}

int ccci_nvram_cache_stop(struct ccci_nvram_cache *cache)
{
	int ret = 0;

	if (!cache)
		return 0;
	mutex_lock(&cache->lock);
	if (cache->state == NVRAM_FAILED)
		ret = -EIO;
	else if (cache->state == NVRAM_PREPARED)
		cache->state = NVRAM_STOPPED;
	mutex_unlock(&cache->lock);
	return ret;
}

int ccci_nvram_cache_offline(struct ccci_nvram_cache *cache)
{
	int ret;

	if (!cache)
		return 0;
	mutex_lock(&cache->lock);
	ret = cache->attempted && cache->state != NVRAM_STOPPED ? -EBUSY : 0;
	mutex_unlock(&cache->lock);
	return ret;
}

bool ccci_nvram_cache_prepared(struct ccci_nvram_cache *cache)
{
	bool ret;

	if (!cache)
		return false;
	mutex_lock(&cache->lock);
	ret = cache->state == NVRAM_PREPARED;
	mutex_unlock(&cache->lock);
	return ret;
}

ssize_t ccci_nvram_cache_status(struct ccci_nvram_cache *cache, char *buf)
{
	static const char * const states[] = { "idle", "prepared", "stopped", "failed" };
	ssize_t ret;

	if (!cache)
		return -EOPNOTSUPP;
	mutex_lock(&cache->lock);
	ret = sysfs_emit(buf,
			 "state=%s attempted=%u cleared=%u mapped=%u size=0x%x ap=0x%llx md=0x%x error=%d\n",
			 states[cache->state], cache->attempted, cache->cleared,
			 !!cache->map, cache->size, cache->ap, cache->md, cache->error);
	mutex_unlock(&cache->lock);
	return ret;
}

static void nvram_cleanup(void *context)
{
	struct ccci_nvram_cache *cache = context;

	/* No callback holds this owner; devres already unmapped the window. */
	kfree(cache);
}

struct ccci_nvram_cache *
ccci_nvram_cache_create(struct device *dev, const struct ccci_smem_layout *smem)
{
	struct ccci_nvram_cache *cache;
	int ret;

	if (!dev || !smem)
		return ERR_PTR(-EINVAL);
	cache = kzalloc_obj(*cache);
	if (!cache)
		return ERR_PTR(-ENOMEM);
	mutex_init(&cache->lock);
	cache->dev = dev;
	ret = nvram_geometry(smem, cache);
	if (ret) {
		kfree(cache);
		return ERR_PTR(ret);
	}
	ret = devm_add_action_or_reset(dev, nvram_cleanup, cache);
	if (ret)
		return ERR_PTR(ret);
	return cache;
}
