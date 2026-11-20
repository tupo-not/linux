// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (c) 2021 MediaTek Inc.
 */

#include <linux/mutex.h>
#include "mtk_drm_arr.h"
#include "mtk_log.h"


static DEFINE_MUTEX(cb_table_lock);
#define DISP_MAX_FPSCHG_CALLBACK 5
static FPS_CHG_CALLBACK fps_chg_callback_table[DISP_MAX_FPSCHG_CALLBACK];
static bool fisrt_invoke;

/****************ARR function start************************/
int drm_register_fps_chg_callback(FPS_CHG_CALLBACK fps_chg_cb)
{
	int ret = 0;
	int i = 0;
	int j = 0;

	mutex_lock(&cb_table_lock);

	for (i = 0; i < DISP_MAX_FPSCHG_CALLBACK; i++) {
		if (fps_chg_callback_table[i] == fps_chg_cb) {
			DDPMSG(":%s re-register cb",
				__func__);
			mutex_unlock(&cb_table_lock);
			return ret;
		}
	}
	for (j = 0; j < DISP_MAX_FPSCHG_CALLBACK; j++) {
		if (fps_chg_callback_table[j] == NULL) {
			fps_chg_callback_table[j] = fps_chg_cb;
			fisrt_invoke = true;
			DDPMSG(": %s, entry[%d] done!", __func__, j);
			break;
		}
	}
	if (j == DISP_MAX_FPSCHG_CALLBACK) {
		DDPPR_ERR(": %s no entries left for new cb!", __func__);
		ret = -1;
	}
	mutex_unlock(&cb_table_lock);
	return ret;
}
EXPORT_SYMBOL(drm_register_fps_chg_callback);

int drm_unregister_fps_chg_callback(FPS_CHG_CALLBACK fps_chg_cb)
{
	int ret = 0;
	int i = 0;

	mutex_lock(&cb_table_lock);
	for (i = 0; i < DISP_MAX_FPSCHG_CALLBACK; i++) {
		if (fps_chg_callback_table[i] == fps_chg_cb) {
			fps_chg_callback_table[i] = NULL;
			DDPMSG(": %s, register cb %p",
				__func__, fps_chg_cb);
		}
	}
	if (i == DISP_MAX_FPSCHG_CALLBACK) {
		DDPPR_ERR(": %s, haven't registered cb %p",
			__func__, fps_chg_cb);
		ret = -1;
	}
	mutex_unlock(&cb_table_lock);
	return ret;
}
EXPORT_SYMBOL(drm_unregister_fps_chg_callback);

void drm_invoke_fps_chg_callbacks(unsigned int new_fps)
{
	unsigned int i = 0;

	DDPMSG(": %s,new_fps =%d", __func__, new_fps);
	mutex_lock(&cb_table_lock);
	for (i = 0; i < DISP_MAX_FPSCHG_CALLBACK; i++) {
		if (fps_chg_callback_table[i]) {
			fps_chg_callback_table[i](new_fps);
			DDPINFO("%s callback %u", __func__, i);
		}
	}
	mutex_unlock(&cb_table_lock);
}

bool drm_need_fisrt_invoke_fps_callbacks(void)
{
	if (fisrt_invoke) {
		fisrt_invoke = false;
		return true;
	}

	return false;
}
