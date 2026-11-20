// SPDX-License-Identifier: GPL-2.0-only
/*
 * Bring-up stubs for the vendor SMI entry points the imported mediatek_v2 (and
 * mml) stack calls but that live in the vendor SMI driver
 * (drivers/misc/mediatek/smi/) which this reference clone does not carry yet.
 *
 * The reference tree's own SMI driver does real work here (larb clock/power
 * gating, IOMMU attach, hang detection); until it is imported these are no-ops
 * so the display stack can link and be brought up.  Replace this file once the
 * vendor SMI sources are available.
 */
#include <linux/module.h>
#include <linux/types.h>
#include <linux/device.h>
#include <linux/dma-buf.h>

/* provided by drivers/memory/mtk-smi-downstream.c */

/* provided by drivers/memory/mtk-smi-downstream.c */

/* mainline's drivers/memory/mtk-smi.c is built (CONFIG_MTK_SMI) but does not
 * export these two for modules of this stack; keep them here until the vendor
 * SMI driver is imported. */
/* provided by drivers/memory/mtk-smi-downstream.c */

/* provided by drivers/memory/mtk-smi-downstream.c */

long mtk_dma_buf_set_name(struct dma_buf *dmabuf, const char *buf);

long mtk_dma_buf_set_name(struct dma_buf *dmabuf, const char *buf) { return 0; }
EXPORT_SYMBOL_GPL(mtk_dma_buf_set_name);

/* the v2 sources call this trace helper without a declaration (they rely on the
 * vendor header); an old-style definition keeps any argument list linkable. */
void mtk_disp_trace(void);

void mtk_disp_trace(void) { }
EXPORT_SYMBOL_GPL(mtk_disp_trace);
