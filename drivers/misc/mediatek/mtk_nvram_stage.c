// SPDX-License-Identifier: GPL-2.0
/*
 * Stage WiFi / BT NVRAM blobs in kernel memory.
 *
 * The initramfs mounts nvdata and writes the per-device calibration files
 * here (as raw binary) before switch_root.  Built-in connsys drivers then
 * read them from memory, which works regardless of which root kernel
 * threads see after switch_root.
 */
#include <linux/init.h>
#include <linux/kernel.h>
#include <linux/kobject.h>
#include <linux/mediatek/mtk_nvram_stage.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/sysfs.h>
#include <linux/string.h>

#define WIFI_STAGE_MAX	8192
#define BT_STAGE_MAX	64

static DEFINE_MUTEX(stage_lock);
static u8 wifi_stage[WIFI_STAGE_MAX];
static size_t wifi_len;
static u8 bt_stage[BT_STAGE_MAX];
static size_t bt_len;

static ssize_t stage_write(struct file *file, struct kobject *kobj,
			   const struct bin_attribute *attr, char *buf,
			   loff_t off, size_t count)
{
	u8 *dst;
	size_t max;
	size_t *len;

	if (!strcmp(attr->attr.name, "wifi")) {
		dst = wifi_stage;
		max = sizeof(wifi_stage);
		len = &wifi_len;
	} else if (!strcmp(attr->attr.name, "bt_addr")) {
		dst = bt_stage;
		max = sizeof(bt_stage);
		len = &bt_len;
	} else {
		return -EINVAL;
	}

	/* sysfs bin attributes are written in chunks by e.g. "cat > file";
	 * accept sequential offsets instead of only the first write.
	 */
	if (off > max || count > max - off)
		return -EFBIG;

	mutex_lock(&stage_lock);
	memcpy(dst + off, buf, count);
	*len = off + count;
	mutex_unlock(&stage_lock);

	return count;
}

static ssize_t stage_read(struct file *file, struct kobject *kobj,
			 const struct bin_attribute *attr, char *buf,
			 loff_t off, size_t count)
{
	const u8 *src;
	size_t len;

	if (!strcmp(attr->attr.name, "wifi")) {
		src = wifi_stage;
		len = wifi_len;
	} else if (!strcmp(attr->attr.name, "bt_addr")) {
		src = bt_stage;
		len = bt_len;
	} else {
		return -EINVAL;
	}

	if (off >= len)
		return 0;
	if (count > len - off)
		count = len - off;
	memcpy(buf, src + off, count);
	return count;
}

static BIN_ATTR(wifi, 0600, stage_read, stage_write, WIFI_STAGE_MAX);
static BIN_ATTR(bt_addr, 0600, stage_read, stage_write, BT_STAGE_MAX);

static struct bin_attribute *stage_attrs[] = {
	&bin_attr_wifi,
	&bin_attr_bt_addr,
	NULL,
};

static struct kobject *stage_kobj;

int mtk_nvram_staged_get(const char *name, const u8 **buf, size_t *len)
{
	if (!name || !buf || !len)
		return -EINVAL;

	mutex_lock(&stage_lock);
	if (!strcmp(name, "wifi")) {
		*buf = wifi_stage;
		*len = wifi_len;
	} else if (!strcmp(name, "bt_addr")) {
		*buf = bt_stage;
		*len = bt_len;
	} else {
		mutex_unlock(&stage_lock);
		return -EINVAL;
	}
	if (!*len) {
		mutex_unlock(&stage_lock);
		return -ENOENT;
	}
	mutex_unlock(&stage_lock);
	return 0;
}
EXPORT_SYMBOL_GPL(mtk_nvram_staged_get);

static int __init mtk_nvram_stage_init(void)
{
	int ret;

	stage_kobj = kobject_create_and_add("mtk_nvram", kernel_kobj);
	if (!stage_kobj)
		return -ENOMEM;

	int i;

	for (i = 0; stage_attrs[i]; i++) {
		ret = sysfs_create_bin_file(stage_kobj, stage_attrs[i]);
		if (ret) {
			while (--i >= 0)
				sysfs_remove_bin_file(stage_kobj, stage_attrs[i]);
			kobject_put(stage_kobj);
			stage_kobj = NULL;
			return ret;
		}
	}
	pr_info("mtk_nvram_stage: /sys/kernel/mtk_nvram/{wifi,bt_addr} ready\n");
	return 0;
}

static void __exit mtk_nvram_stage_exit(void)
{
	int i;

	for (i = 0; stage_attrs[i]; i++)
		sysfs_remove_bin_file(stage_kobj, stage_attrs[i]);
	kobject_put(stage_kobj);
}

module_init(mtk_nvram_stage_init);
module_exit(mtk_nvram_stage_exit);

MODULE_DESCRIPTION("Stage MediaTek connsys NVRAM blobs in kernel memory");
MODULE_LICENSE("GPL v2");
