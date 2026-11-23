/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef __CCCI_NVRAM_CACHE_H
#define __CCCI_NVRAM_CACHE_H

#include <linux/errno.h>
#include <linux/kconfig.h>
#include <linux/types.h>

struct device;
struct ccci_smem_layout;
struct ccci_nvram_cache;

#if IS_ENABLED(CONFIG_OP6893_CCCI_NVRAM_CACHE)
/* Dormant geometry check only; no mapping or write. */
struct ccci_nvram_cache *
ccci_nvram_cache_create(struct device *dev, const struct ccci_smem_layout *smem);
/*
 * Parent lifecycle lock held. Maps the cached user-32 window once and applies
 * the same-board first-start clear. Never re-clears on a later reconnect.
 */
int ccci_nvram_cache_prepare(struct ccci_nvram_cache *cache);
int ccci_nvram_cache_resume(struct ccci_nvram_cache *cache);
/* Parent lock held, MD domain already proven off; keeps the mapping. */
int ccci_nvram_cache_stop(struct ccci_nvram_cache *cache);
int ccci_nvram_cache_offline(struct ccci_nvram_cache *cache);
bool ccci_nvram_cache_prepared(struct ccci_nvram_cache *cache);
ssize_t ccci_nvram_cache_status(struct ccci_nvram_cache *cache, char *buf);
#else
static inline struct ccci_nvram_cache *
ccci_nvram_cache_create(struct device *dev, const struct ccci_smem_layout *smem)
{
	return NULL;
}

static inline int ccci_nvram_cache_prepare(struct ccci_nvram_cache *cache) { return -EOPNOTSUPP; }
static inline int ccci_nvram_cache_stop(struct ccci_nvram_cache *cache) { return 0; }
static inline int ccci_nvram_cache_offline(struct ccci_nvram_cache *cache) { return 0; }
static inline bool ccci_nvram_cache_prepared(struct ccci_nvram_cache *cache) { return false; }
static inline ssize_t ccci_nvram_cache_status(struct ccci_nvram_cache *cache, char *buf)
{
	return -EOPNOTSUPP;
}
#endif
#endif
