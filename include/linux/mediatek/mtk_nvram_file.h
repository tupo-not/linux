/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Helper for MediaTek connsys drivers that need NVRAM blobs which live on
 * the mounted nvdata partition (APCFG/APRDEB/...).  Reading them straight
 * from nvdata removes the dependency on the initramfs copying them into
 * /lib/firmware before the driver probes.
 */
#ifndef _MTK_NVRAM_FILE_H
#define _MTK_NVRAM_FILE_H

#include <linux/err.h>
#include <linux/fcntl.h>
#include <linux/file.h>
#include <linux/fs.h>

/**
 * mtk_nvram_read_file() - read a blob from nvdata into a caller buffer
 * @path: absolute path on the mounted nvdata partition
 * @buf: destination buffer
 * @size: size of @buf
 *
 * Returns the number of bytes read (0..size) or a negative errno.
 * Must be called from process context; the existing deferred-probe retry
 * logic in the callers covers the window before /data is mounted.
 */
static inline ssize_t mtk_nvram_read_file(const char *path, void *buf,
					  size_t size)
{
	struct file *file;
	loff_t pos = 0;
	ssize_t ret;

	file = filp_open(path, O_RDONLY, 0);
	if (IS_ERR(file))
		return PTR_ERR(file);

	ret = kernel_read(file, buf, size, &pos);
	fput(file);
	return ret;
}

/**
 * mtk_nvram_read_any() - try a list of candidate paths in order
 * @paths: array of absolute paths
 * @npaths: number of entries in @paths
 * @buf: destination buffer
 * @size: size of @buf
 *
 * Returns the first successful read (>= 0), or the last negative errno.
 * Useful when the blob may live under /data/nvram in the final root or at
 * its original mount point (which stays visible to kernel threads after
 * switch_root).
 */
static inline ssize_t mtk_nvram_read_any(const char *const *paths,
					 size_t npaths, void *buf, size_t size)
{
	ssize_t ret = -ENOENT;
	size_t i;

	for (i = 0; i < npaths; i++) {
		ret = mtk_nvram_read_file(paths[i], buf, size);
		if (ret >= 0)
			return ret;
	}
	return ret;
}

#endif /* _MTK_NVRAM_FILE_H */
