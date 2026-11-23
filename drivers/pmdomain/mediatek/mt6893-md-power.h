/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef __MT6893_MD_POWER_H
#define __MT6893_MD_POWER_H

#include <linux/types.h>

struct regmap;

enum mt6893_md_power_state {
	MT6893_MD_POWER_UNKNOWN,
	MT6893_MD_POWER_OFF,
	MT6893_MD_POWER_ON,
	MT6893_MD_POWER_FAULT,
};

/* Serialized by genpd. Supplies and firmware release belong to the consumer. */
struct mt6893_md_power {
	struct regmap *spm;
	struct regmap *infra;
	enum mt6893_md_power_state state;
	const char *step;
	bool inherited_on;
};

int mt6893_md_power_init(struct mt6893_md_power *md, struct regmap *spm,
			 struct regmap *infra);
int mt6893_md_power_set(struct mt6893_md_power *md, bool on);
int mt6893_md_power_adopt(struct mt6893_md_power *md);

#endif
