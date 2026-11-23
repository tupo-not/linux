/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef __CCCI_MD_BOOT_H
#define __CCCI_MD_BOOT_H

#include <linux/errno.h>
#include <linux/kconfig.h>
#include <linux/types.h>

#include "ccci-runtime.h"

struct device;
struct ccci_ccif;
struct ccci_dpmaif;
struct ccci_nvram_cache;
struct ccci_port_dispatch;
struct ccci_smem_layout;
struct ccci_md_boot;

#if IS_ENABLED(CONFIG_OP6893_CCCI_MD_BOOT)
/* Dormant construction; no protocol, SMC, mapping or release. */
struct ccci_md_boot *
ccci_md_boot_create(struct device *dev, struct ccci_ccif *ccif,
		    struct ccci_port_dispatch *dispatch,
		    struct ccci_nvram_cache *cache);
/*
 * Parent lifecycle lock held. This is the ready gate: prepare CCIF rings, map
 * and first-clear the NVRAM cache, build typed runtime offers from the validated
 * layout and reserve the connected FS backend plus SCP lease. One attempt per generation.
 */
int ccci_md_boot_prepare(struct ccci_md_boot *boot, const struct ccci_smem_layout *smem,
			 u64 generation, enum ccci_runtime_crystal crystal);
/* Operator bisection only: reject once the single attempt has started. */
int ccci_md_boot_set_feature_mask(struct ccci_md_boot *boot, u64 mask);
u64 ccci_md_boot_feature_mask(struct ccci_md_boot *boot);
int ccci_md_boot_rearm(struct ccci_md_boot *boot);
/* Optional DPMAIF reset and DL setup precede RX and the MD CPU release. */
int ccci_md_boot_release(struct ccci_md_boot *boot, struct ccci_dpmaif *dpmaif);
/* Hardware stop evidence only (parent domain OFF readback); stops services. */
int ccci_md_boot_stop(struct ccci_md_boot *boot, bool md_stopped);
int ccci_md_boot_offline(struct ccci_md_boot *boot);
bool ccci_md_boot_active(struct ccci_md_boot *boot);
/* Diagnostic SPM PCM REG13 sample taken before the parent powers MD off. */
void ccci_md_boot_note_pcm(struct ccci_md_boot *boot, u32 r13);
ssize_t ccci_md_boot_status(struct ccci_md_boot *boot, char *buf);
#else
static inline struct ccci_md_boot *
ccci_md_boot_create(struct device *dev, struct ccci_ccif *ccif,
		    struct ccci_port_dispatch *dispatch,
		    struct ccci_nvram_cache *cache) { return NULL; }
static inline int ccci_md_boot_prepare(struct ccci_md_boot *boot,
				       const struct ccci_smem_layout *smem,
				       u64 generation,
				       enum ccci_runtime_crystal crystal)
{
	return -EOPNOTSUPP;
}

static inline int ccci_md_boot_set_feature_mask(struct ccci_md_boot *boot, u64 mask)
{
	return -EOPNOTSUPP;
}

static inline u64 ccci_md_boot_feature_mask(struct ccci_md_boot *boot) { return 0; }

static inline int ccci_md_boot_rearm(struct ccci_md_boot *boot) { return -EOPNOTSUPP; }
static inline int ccci_md_boot_release(struct ccci_md_boot *boot,
				       struct ccci_dpmaif *dpmaif)
{
	return -EOPNOTSUPP;
}
static inline int ccci_md_boot_stop(struct ccci_md_boot *boot, bool md_stopped) { return 0; }
static inline int ccci_md_boot_offline(struct ccci_md_boot *boot) { return 0; }
static inline bool ccci_md_boot_active(struct ccci_md_boot *boot) { return false; }
static inline void ccci_md_boot_note_pcm(struct ccci_md_boot *boot, u32 r13) { }
static inline ssize_t ccci_md_boot_status(struct ccci_md_boot *boot, char *buf)
{
	return -EOPNOTSUPP;
}
#endif
#endif
