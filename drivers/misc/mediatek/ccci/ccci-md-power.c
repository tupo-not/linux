// SPDX-License-Identifier: GPL-2.0-only
/* One cold platform power cycle, stopping at uncertain transitions. */
#include <linux/errno.h>

#include "ccci-md-power.h"

int ccci_md_power_init(struct ccci_md_power *power,
		       const struct ccci_md_power_ops *ops, void *context)
{
	if (!power || !ops || !ops->off_check || !ops->rails_check || !ops->clocks ||
	    !ops->srcclk || !ops->srclken_o1 || !ops->flight || !ops->domain ||
	    !ops->handoff_check || !ops->handoff_acquire)
		return -EINVAL;
	*power = (struct ccci_md_power) {
		.ops = ops, .context = context, .state = CCCI_MD_COLD, .step = "cold",
	};
	return 0;
}

static int ccci_md_power_fault(struct ccci_md_power *power, int error)
{
	power->state = CCCI_MD_FAULT;
	power->error = error;
	return error;
}

int ccci_md_power_on(struct ccci_md_power *power)
{
	const struct ccci_md_power_ops *ops = power->ops;
	void *context = power->context;
	int ret;

	if (power->state == CCCI_MD_FAULT)
		return -EIO;
	if (power->state == CCCI_MD_INHERITED)
		return -EAGAIN;
	if (power->attempted)
		return -EALREADY;
	power->error = 0;
	power->step = "off-check";
	ret = ops->off_check(context);
	if (ret) {
		power->error = ret;
		return ret;
	}
	power->step = "cold-rails";
	ret = ops->rails_check(context);
	if (ret) {
		power->error = ret;
		return ret;
	}
	/* Commit only after the read-only gates, before the first possible write. */
	power->attempted = true;
	power->step = "ccif-clocks-on";
	ret = ops->clocks(context, true);
	if (ret)
		goto fault;
	power->clocks = true;
	power->step = "srcclk-on";
	ret = ops->srcclk(context, true);
	if (ret)
		goto fault;
	power->step = "srclken-o1";
	ret = ops->srclken_o1(context);
	if (ret)
		goto fault;
	power->step = "flight-off";
	ret = ops->flight(context, false);
	if (ret)
		goto fault;
	power->step = "domain-on";
	ret = ops->domain(context, true);
	if (ret)
		goto fault;
	power->domain = true;
	power->state = CCCI_MD_POWERED;
	power->step = "powered-cpu-not-released";
	return 0;
fault:
	/* Leave clocks/references held; no guessed rollback after a partial write. */
	return ccci_md_power_fault(power, ret);
}

static int ccci_md_power_stop(struct ccci_md_power *power)
{
	const struct ccci_md_power_ops *ops = power->ops;
	void *context = power->context;
	int ret;

	power->step = "domain-off";
	ret = ops->domain(context, false);
	if (ret)
		goto fault;
	power->domain = false;
	power->step = "off-readback";
	ret = ops->off_check(context);
	if (ret)
		goto fault;
	power->step = "srcclk-off";
	ret = ops->srcclk(context, false);
	if (ret)
		goto fault;
	power->step = "flight-on";
	ret = ops->flight(context, true);
	if (ret)
		goto fault;
	power->step = "ccif-clocks-off";
	if (power->clocks) {
		ret = ops->clocks(context, false);
		if (ret)
			goto fault;
	}
	power->clocks = false;
	power->state = CCCI_MD_STOPPED;
	power->step = "stopped";
	return 0;
fault:
	return ccci_md_power_fault(power, ret);
}

int ccci_md_power_off(struct ccci_md_power *power)
{
	if (power->state == CCCI_MD_FAULT)
		return -EIO;
	if (power->state == CCCI_MD_INHERITED)
		return -EAGAIN;
	if (power->state != CCCI_MD_POWERED)
		return 0;
	return ccci_md_power_stop(power);
}

int ccci_md_power_takeover(struct ccci_md_power *power)
{
	int ret;

	if (power->state == CCCI_MD_FAULT)
		return -EIO;
	if (power->state != CCCI_MD_INHERITED)
		return -EALREADY;
	power->step = "lk-check-done";
	ret = power->ops->handoff_check(power->context);
	if (ret)
		goto gate;
	power->step = "inherited-rails";
	ret = power->ops->rails_check(power->context);
	if (ret)
		goto gate;
	power->attempted = true;
	power->step = "handoff-domain-reference";
	ret = power->ops->handoff_acquire(power->context);
	if (ret)
		return ccci_md_power_fault(power, ret);
	power->domain = true;
	ret = ccci_md_power_stop(power);
	if (ret)
		return ret;
	/* Handoff completed. Allow one fresh cold ON/OFF cycle, no warm restart. */
	power->state = CCCI_MD_COLD;
	power->step = "cold-after-lk";
	power->attempted = false;
	power->error = 0;
	return 0;
gate:
	power->error = ret;
	return ret;
}
