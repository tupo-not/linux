// SPDX-License-Identifier: GPL-2.0-only
/* Same-board vendor non-secure forced SET sequence, without restore/release. */
#include <linux/bitops.h>
#include <linux/errno.h>

#include "ccci-scp-reset.h"

int ccci_scp_reset_assert(struct ccci_scp_reset *reset, void __iomem * const cores[2],
			  int (*check)(void *context), void *context)
{
	int ret;

	if (!reset || !cores || !cores[0] || !cores[1] || !check)
		return -EINVAL;
	if (reset->attempted)
		return -EALREADY;
	ret = check(context);
	if (ret)
		return ret;
	reset->attempted = true;
	/* V's TIMEOUT/AWAKE recovery path deliberately does not wait for WFI. */
	writel(1, cores[0] + 4);
	writel(1, cores[1] + 4);
	reset->writes = 2;
	/* ARM64 mb() is DSB SY, matching vendor dsb(SY) after both SET writes. */
	mb();
	reset->raw[0] = readl(cores[0] + 4);
	reset->raw[1] = readl(cores[1] + 4);
	reset->set[0] = reset->raw[0] & BIT(0);
	reset->set[1] = reset->raw[1] & BIT(0);
	reset->reads = 2;
	ret = check(context);
	/* Same-board initial CLR/SET experiment: released=1, asserted=0. */
	if (!ret && (reset->set[0] || reset->set[1]))
		ret = -EIO;
	reset->error = ret;
	reset->latched = !ret;
	/* Failure leaves both SET writes in place; no retry, CLR or fallback SMC. */
	return ret;
}
