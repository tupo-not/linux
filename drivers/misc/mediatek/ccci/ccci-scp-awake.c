// SPDX-License-Identifier: GPL-2.0-only
/* MT6893 RV AP awake handshake; no probe, READY inference or reset fallback. */

#include <linux/bitops.h>
#include <linux/err.h>
#include <linux/errno.h>
#include <linux/limits.h>
#include <linux/regmap.h>

#include "ccci-scp-awake.h"

#define SCP_AWAKE_SET		0xb14
#define SCP_AWAKE_CLEAR		0xb18
#define SCP_AWAKE_MAGIC		0xa0
#define SCP_AWAKE_LOCK		BIT(0)
#define SCP_AWAKE_UNLOCK	BIT(1)

static int ccci_scp_awake_request(struct ccci_scp_awake *awake, u32 bit)
{
	unsigned int value;
	int ret, cleanup;

	/* Do not clear a request left by another owner or an earlier boot. */
	ret = regmap_read(awake->infra, SCP_AWAKE_SET, &value);
	if (ret)
		goto fault;
	if (value & (SCP_AWAKE_LOCK | SCP_AWAKE_UNLOCK)) {
		ret = -EBUSY;
		goto fault;
	}

	/* SET/CLEAR are write-one registers: never use a read-modify-write. */
	ret = regmap_write(awake->infra, SCP_AWAKE_SET, SCP_AWAKE_MAGIC | bit);
	if (!ret) {
		/* The A path reads SET itself, not the adjacent +0xb10 register. */
		ret = regmap_read_poll_timeout(awake->infra, SCP_AWAKE_SET, value,
					       (value & SCP_AWAKE_MAGIC) != SCP_AWAKE_MAGIC ||
					       !(value & bit), 10, 1000000);
		if (!ret && (value & SCP_AWAKE_MAGIC) != SCP_AWAKE_MAGIC)
			ret = -EPROTO;
	}

	/* A failed write can still have reached the bus. Always attempt cleanup. */
	cleanup = regmap_write(awake->infra, SCP_AWAKE_CLEAR, SCP_AWAKE_MAGIC | bit);
	awake->cleanup_error = cleanup;
	if (!ret)
		ret = cleanup;
	if (!ret)
		return 0;
fault:
	awake->fault = true;
	return ret;
}

int ccci_scp_awake_init(struct ccci_scp_awake *awake, struct regmap *infra)
{
	if (!awake)
		return -EINVAL;
	*awake = (struct ccci_scp_awake) { .fault = true };
	if (IS_ERR_OR_NULL(infra))
		return -EINVAL;
	awake->infra = infra;
	awake->fault = false;
	return 0;
}

int ccci_scp_awake_get(struct ccci_scp_awake *awake)
{
	int ret;

	if (!awake || !awake->infra)
		return -EINVAL;
	if (awake->fault)
		return -EIO;
	if (awake->users == U32_MAX)
		return -EOVERFLOW;
	if (!awake->users) {
		ret = ccci_scp_awake_request(awake, SCP_AWAKE_LOCK);
		if (ret)
			return ret;
	}
	awake->users++;
	return 0;
}

int ccci_scp_awake_put(struct ccci_scp_awake *awake)
{
	int ret;

	if (!awake || !awake->infra)
		return -EINVAL;
	if (awake->fault)
		return -EIO;
	if (!awake->users)
		return -EINVAL;
	if (awake->users == 1) {
		ret = ccci_scp_awake_request(awake, SCP_AWAKE_UNLOCK);
		if (ret)
			return ret;
	}
	awake->users--;
	return 0;
}

void ccci_scp_awake_invalidate(struct ccci_scp_awake *awake)
{
	if (awake)
		awake->fault = true;
}
