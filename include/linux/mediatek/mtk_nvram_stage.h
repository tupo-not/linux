/* SPDX-License-Identifier: GPL-2.0 */
/*
 * In-memory staging for connsys NVRAM blobs.
 *
 * Kernel threads keep the initramfs root after switch_root, so they cannot
 * open /data/nvram/... even though userspace can.  The initramfs writes each
 * blob to /sys/kernel/mtk_nvram/<name> before switch_root; drivers fetch the
 * staged copy from memory instead of opening a path.
 */
#ifndef _MTK_NVRAM_STAGE_H
#define _MTK_NVRAM_STAGE_H

/**
 * mtk_nvram_staged_get() - fetch a staged blob
 * @name: "wifi" or "bt_addr"
 * @buf: returned pointer to the staged data (valid until next write)
 * @len: returned length in bytes
 *
 * Returns 0 on success, -ENOENT if not staged yet, -EINVAL for bad args.
 */
int mtk_nvram_staged_get(const char *name, const u8 **buf, size_t *len);

#endif /* _MTK_NVRAM_STAGE_H */
