/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef __CCCI_MD_POWER_H
#define __CCCI_MD_POWER_H

#include <linux/types.h>

enum ccci_md_power_state {
	CCCI_MD_COLD,
	CCCI_MD_POWERED,
	CCCI_MD_STOPPED,
	CCCI_MD_INHERITED,
	CCCI_MD_FAULT,
};

/* Serialized by the platform owner; no operation releases the MD CPU. */
struct ccci_md_power_ops {
	int (*off_check)(void *context);
	int (*rails_check)(void *context);
	int (*clocks)(void *context, bool on);
	int (*srcclk)(void *context, bool on);
	int (*srclken_o1)(void *context);
	int (*flight)(void *context, bool flight);
	int (*domain)(void *context, bool on);
	int (*handoff_check)(void *context);
	int (*handoff_acquire)(void *context);
};

struct ccci_md_power {
	const struct ccci_md_power_ops *ops;
	void *context;
	enum ccci_md_power_state state;
	const char *step;
	int error;
	bool clocks;
	bool domain;
	bool attempted;
};

int ccci_md_power_init(struct ccci_md_power *power,
		       const struct ccci_md_power_ops *ops, void *context);
int ccci_md_power_on(struct ccci_md_power *power);
int ccci_md_power_off(struct ccci_md_power *power);
int ccci_md_power_takeover(struct ccci_md_power *power);

#endif
