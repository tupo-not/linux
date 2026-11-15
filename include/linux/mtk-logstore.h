/* SPDX-License-Identifier: GPL-2.0 */
/*
 * MediaTek log_store — the DRAM log ring that the bootloader (preloader and
 * LK) relays into the "expdb" partition on the next boot.
 *
 * This header only carries the pieces that other translation units need:
 *
 *  - the vendor SRAM header layout.  The header lives at SRAM
 *    0x11df00 ("sram buff header") and is *not* a struct pl_lk_log; the
 *    current-log descriptor sits at +0x34 inside it.  LK prints that address
 *    itself ("sram buff header 0x11df00,current log header 0x11df34"), and
 *    drivers/misc/mediatek/log_store/log_store_kernel.h agrees.  Read every
 *    field through offsetof() so a wrong assumption cannot come back.
 *
 *  - the two entry points the arm64 setup code calls: mtk_logstore_printk()
 *    is the vprintk_emit() mirror, the other three are called from
 *    setup_arch() at well defined points in the mapping life cycle:
 *
 *        early_ioremap_init()  -> mtk_logstore_breadcrumb_early()
 *        paging_init()         -> mtk_logstore_attach_early()
 *        ...                   -> mtk_logstore_breadcrumb()
 *
 * The ring itself is described in drivers/misc/mtk-logstore-console.c; see
 * port/SUMMARY.md for why each address and window is what it is.
 */
#ifndef _LINUX_MTK_LOGSTORE_H
#define _LINUX_MTK_LOGSTORE_H

#include <linux/types.h>

/* --- vendor header layout, log_store_kernel.h ---------------------------- */

/* total 32 bytes, "current log" descriptor for the PL/LK ring */
struct pl_lk_log {
	u32 sig;		/* SRAM_HEADER_SIG */
	u32 buff_size;		/* total buf size */
	u32 off_pl;		/* preloader offset */
	u32 sz_pl;		/* preloader size */
	u32 pl_flag;		/* preloader log flag */
	u32 off_lk;		/* LK offset */
	u32 sz_lk;		/* LK log size */
	u32 lk_flag;		/* LK log flag */
} __packed;

/* total 40 bytes, the DRAM side of the ring */
struct dram_buf_header {
	u32 sig;
	u32 flag;
	u32 buf_addr;
	u32 buf_size;
	u32 buf_offsize;
	u32 buf_point;		/* used end of the buffer */
	u32 klog_addr;
	u32 klog_size;
	u32 atf_log_addr;
	u32 atf_log_len;
} __packed;

/* total 256 bytes, at SRAM 0x11df00 */
struct sram_log_header {
	u32 sig;
	u32 reboot_count;
	u32 save_to_emmc;
	struct dram_buf_header dram_buf;		/* at +0x0c */
	struct pl_lk_log dram_curlog_header;		/* at +0x34 */
	u32 gz_log_addr;
	u32 gz_log_len;
	u32 reserve[41];	/* reserve[0] = log size LK should relay */
} __packed;

/* reserve[] indices, from the vendor header */
#define SRAM_RECORD_LOG_SIZE	0
#define SRAM_BLOCK_SIZE		1
#define SRAM_PMIC_BOOT_PHASE	2
#define SRAM_HISTORY_BOOT_PHASE	3

/* dram_buf_header.flag */
#define DRAM_FLAG_EARLY_PRINTK	0x400

/* --- log_store ring ------------------------------------------------------ */

#define LOGSTORE_PA		0x7ffbf000	/* non-secure reserved DRAM */
#define LOGSTORE_SIZE		0x40000		/* 256 KiB */
#define LOGSTORE_SRAM_HEADER_PA	0x11df00	/* only with the Kconfig below */

#ifdef CONFIG_MTK_LOGSTORE_CONSOLE
void mtk_logstore_printk(const char *s, unsigned int n);
void __init mtk_logstore_breadcrumb(const char *what);
void __init mtk_logstore_breadcrumb_early(const char *what);
void __init mtk_logstore_attach_early(void);
#else
static inline void mtk_logstore_breadcrumb(const char *what) { }
static inline void mtk_logstore_breadcrumb_early(const char *what) { }
static inline void mtk_logstore_attach_early(void) { }
#endif

#endif /* _LINUX_MTK_LOGSTORE_H */
