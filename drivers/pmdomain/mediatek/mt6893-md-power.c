// SPDX-License-Identifier: GPL-2.0-only
/* MT6893 AP-controlled MD1 MTCMOS; no ATF modem boot command is issued here. */
#include <linux/bitops.h>
#include <linux/errno.h>
#include <linux/iopoll.h>
#include <linux/kernel.h>
#include <linux/regmap.h>

#include "mt6893-md-power.h"

#define MD_SPM_CONFIG		0x000
#define MD_SPM_KEY		((0xb16 << 16) | BIT(0))
#define MD_PWR_STATUS		0x16c
#define MD_PWR_CON		0x300
#define MD_BUCK_ISO		0x398
#define MD_RESET_B		BIT(0)
#define MD_PWR_ON		BIT(2)
#define MD_PWR_ACK		BIT(0)
#define MD_PROTECT1		BIT(7)
#define MD_PROTECT2		(BIT(10) | BIT(21) | BIT(29))
#define MD_PROTECT3		(BIT(6) | BIT(7))
#define MD_POLL_US		10
#define MD_TIMEOUT_US		1000000

static int mt6893_md_wait(struct regmap *map, unsigned int reg,
			  unsigned int mask, bool set)
{
	u32 value;

	return regmap_read_poll_timeout(map, reg, value,
					(value & mask) == (set ? mask : 0),
					MD_POLL_US, MD_TIMEOUT_US);
}

/* Recognize exact OFF or inherited ON without changing hardware. */
int mt6893_md_power_init(struct mt6893_md_power *md, struct regmap *spm,
			 struct regmap *infra)
{
	u32 status, control, iso, protect1, protect2, protect3;
	int ret;

	md->state = MT6893_MD_POWER_UNKNOWN;
	md->inherited_on = false;
	md->step = "initial-state";
	if (!spm || !infra)
		return -EINVAL;
	ret = regmap_read(spm, MD_PWR_STATUS, &status);
	if (ret)
		return ret;
	ret = regmap_read(spm, MD_PWR_CON, &control);
	if (ret)
		return ret;
	ret = regmap_read(spm, MD_BUCK_ISO, &iso);
	if (ret)
		return ret;
	ret = regmap_read(infra, 0x228, &protect1);
	if (ret)
		return ret;
	ret = regmap_read(infra, 0xb90, &protect2);
	if (ret)
		return ret;
	ret = regmap_read(infra, 0xbc4, &protect3);
	if (ret)
		return ret;
	if ((status & MD_PWR_ACK) &&
	    (control & (MD_PWR_ON | MD_RESET_B)) == (MD_PWR_ON | MD_RESET_B) &&
	    !(iso & GENMASK(1, 0)) && !(protect1 & MD_PROTECT1) &&
	    !(protect2 & MD_PROTECT2) && !(protect3 & MD_PROTECT3)) {
		md->state = MT6893_MD_POWER_ON;
		md->inherited_on = true;
	} else if ((status & MD_PWR_ACK) || (control & (MD_PWR_ON | MD_RESET_B)) ||
	    (iso & GENMASK(1, 0)) != GENMASK(1, 0) ||
	    (protect1 & MD_PROTECT1) != MD_PROTECT1 ||
	    (protect2 & MD_PROTECT2) != MD_PROTECT2 ||
	    (protect3 & MD_PROTECT3) != MD_PROTECT3)
		return -EBUSY;

	md->spm = spm;
	md->infra = infra;
	if (!md->inherited_on)
		md->state = MT6893_MD_POWER_OFF;
	md->step = md->inherited_on ? "lk-on-held" : "off";
	return 0;
}

/* Caller holds an active genpd runtime reference and has checked LK completion. */
int mt6893_md_power_adopt(struct mt6893_md_power *md)
{
	if (md->state != MT6893_MD_POWER_ON || !READ_ONCE(md->inherited_on))
		return -EINVAL;
	WRITE_ONCE(md->inherited_on, false);
	return 0;
}

enum md_operation {
	MD_WRITE,
	MD_SET,
	MD_CLEAR,
	MD_WAIT_SET,
	MD_WAIT_CLEAR,
};

struct md_step {
	bool infra;
	enum md_operation operation;
	unsigned int reg;
	u32 mask;
	const char *name;
};

static const struct md_step md_on_steps[] = {
	{ false, MD_WRITE, MD_SPM_CONFIG, MD_SPM_KEY, "unlock" },
	{ false, MD_CLEAR, MD_BUCK_ISO, BIT(0), "deisolate-0" },
	{ false, MD_CLEAR, MD_BUCK_ISO, BIT(1), "deisolate-1" },
	{ false, MD_SET, MD_PWR_CON, MD_RESET_B, "reset-release" },
	{ false, MD_SET, MD_PWR_CON, MD_PWR_ON, "power-on" },
	{ false, MD_WAIT_SET, MD_PWR_STATUS, MD_PWR_ACK, "power-on-ack" },
	/* Vendor order is 2_0, 2_1, 1, not reverse protection order. */
	{ true, MD_WRITE, 0xb88, MD_PROTECT2, "unprotect-2" },
	{ true, MD_WRITE, 0xbbc, MD_PROTECT3, "unprotect-3" },
	{ true, MD_WRITE, 0x2a4, MD_PROTECT1, "unprotect-1" },
};

static const struct md_step md_off_steps[] = {
	{ false, MD_WRITE, MD_SPM_CONFIG, MD_SPM_KEY, "unlock" },
	{ true, MD_WRITE, 0x2a0, MD_PROTECT1, "protect-1" },
	{ true, MD_WAIT_SET, 0x228, MD_PROTECT1, "protect-1-ack" },
	{ true, MD_WRITE, 0xb84, MD_PROTECT2, "protect-2" },
	{ true, MD_WAIT_SET, 0xb90, MD_PROTECT2, "protect-2-ack" },
	{ true, MD_WRITE, 0xbb8, MD_PROTECT3, "protect-3" },
	{ true, MD_WAIT_SET, 0xbc4, MD_PROTECT3, "protect-3-ack" },
	{ false, MD_CLEAR, MD_PWR_CON, MD_PWR_ON, "power-off" },
	{ false, MD_WAIT_CLEAR, MD_PWR_STATUS, MD_PWR_ACK, "power-off-ack" },
	{ false, MD_SET, MD_BUCK_ISO, BIT(0), "isolate-0" },
	{ false, MD_SET, MD_BUCK_ISO, BIT(1), "isolate-1" },
	{ false, MD_CLEAR, MD_PWR_CON, MD_RESET_B, "reset-assert" },
};

/* Stop at the first error. A partial transition requires reboot/recovery. */
int mt6893_md_power_set(struct mt6893_md_power *md, bool on)
{
	const struct md_step *steps = on ? md_on_steps : md_off_steps;
	unsigned int count = on ? ARRAY_SIZE(md_on_steps) : ARRAY_SIZE(md_off_steps);
	int ret;

	if (md->state == MT6893_MD_POWER_FAULT)
		return -EIO;
	if (md->state == MT6893_MD_POWER_UNKNOWN)
		return -EINVAL;
	/* Never let unused-domain work power off LK before the explicit handoff. */
	if (!on && READ_ONCE(md->inherited_on))
		return -EBUSY;
	if (md->state == (on ? MT6893_MD_POWER_ON : MT6893_MD_POWER_OFF))
		return 0;

	for (unsigned int i = 0; i < count; i++) {
		const struct md_step *step = &steps[i];
		struct regmap *map = step->infra ? md->infra : md->spm;

		md->step = step->name;
		switch (step->operation) {
		case MD_WRITE:
			ret = regmap_write(map, step->reg, step->mask);
			break;
		case MD_SET:
			ret = regmap_set_bits(map, step->reg, step->mask);
			break;
		case MD_CLEAR:
			ret = regmap_clear_bits(map, step->reg, step->mask);
			break;
		case MD_WAIT_SET:
		case MD_WAIT_CLEAR:
			ret = mt6893_md_wait(map, step->reg, step->mask,
					     step->operation == MD_WAIT_SET);
			break;
		default:
			ret = -EINVAL;
			break;
		}
		if (ret) {
			md->state = MT6893_MD_POWER_FAULT;
			return ret;
		}
	}
	md->state = on ? MT6893_MD_POWER_ON : MT6893_MD_POWER_OFF;
	md->step = on ? "on" : "off";
	return 0;
}
