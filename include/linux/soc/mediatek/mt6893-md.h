/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef __SOC_MEDIATEK_MT6893_MD_H
#define __SOC_MEDIATEK_MT6893_MD_H

struct device;

/* Explicit LK handoff only; caller must own a resumed MD1 runtime reference. */
int mt6893_md_domain_allow_off(struct device *dev);

#endif
