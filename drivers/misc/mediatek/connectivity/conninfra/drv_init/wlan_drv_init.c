// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (c) 2019 MediaTek Inc.
 */

#include <linux/kernel.h>
#include <linux/errno.h>

#include "wlan_drv_init.h"
#include "wifi_pwr_on.h"

int __attribute__((weak)) mtk_wcn_wlan_gen4_init(void)
{
	pr_info("no impl. mtk_wcn_wlan_gen4_init\n");
	return 0;
}

int __attribute__((weak)) mtk_wcn_wmt_wifi_init(void)
{
	pr_info("no impl. mtk_wcn_wmt_wifi_init");
	return 0;
}

int do_wlan_drv_init(int chip_id)
{
	pr_info("Start to do wlan module init 0x%x\n", chip_id);

	/* Chopin bring-up: the WiFi NVRAM blob is not shipped yet, so skip the
	 * WLAN driver entirely instead of retrying request_firmware() forever.
	 */
	pr_info("WLAN disabled for chopin bring-up\n");

	return 0;
}
