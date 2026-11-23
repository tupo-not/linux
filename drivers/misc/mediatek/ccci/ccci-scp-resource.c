// SPDX-License-Identifier: GPL-2.0-only
/* Same-board ATF resource ABI; no generic SMC interface or release operation. */

#include <linux/arm-smccc.h>
#include <linux/bitops.h>
#include <linux/errno.h>
#include <linux/limits.h>
#include <linux/soc/mediatek/mtk_sip_svc.h>

#include "ccci-scp-resource.h"

#define SCP_RESOURCE_SMC		MTK_SIP_SMC_CMD(0x232)
#define SCP_RESOURCE_REQUEST	1
#define SCP_RESOURCE_REQUEST_26M	BIT(0)
#define SCP_LPM_SMC		MTK_SIP_SMC_CMD(0x507)
#define SCP_LPM_DEBUG_UID		0xda010000
#define SCP_LPM_GET		BIT(2)
#define SCP_RESOURCE_COUNT	6
#define SCP_RESOURCE_USERS_MAX	32
#define SCP_RESOURCE_NAME	0x00504353 /* "SCP" packed little-endian. */

enum scp_resource_query {
	SCP_QUERY_RESOURCE_COUNT = 11,
	SCP_QUERY_USAGE = 13,
	SCP_QUERY_USER_COUNT = 14,
	SCP_QUERY_USER_VALID = 15,
	SCP_QUERY_USER_NAME = 16,
};

static int ccci_scp_resource_query(enum scp_resource_query query, u32 index, u32 *value)
{
	struct arm_smccc_res result;

	arm_smccc_smc(SCP_LPM_SMC, SCP_LPM_DEBUG_UID | query, SCP_LPM_GET,
		      index, 0, 0, 0, 0, &result);
	/* Query data is u32; unsupported SMC returns all bits set in x0. */
	if (result.a0 > U32_MAX)
		return result.a0 == ~0UL ? -EOPNOTSUPP : -EIO;
	*value = result.a0;
	return 0;
}

static int ccci_scp_resource_sample(struct ccci_scp_resource_sample *sample)
{
	u32 valid, users_after, valid_after, name, usage, user_mask;
	unsigned int i;
	int ret;

	*sample = (struct ccci_scp_resource_sample) { .scp_user = U32_MAX };
	ret = ccci_scp_resource_query(SCP_QUERY_RESOURCE_COUNT, 0, &sample->resources);
	if (ret)
		return ret;
	/* Reject a different ABI or an uninstalled debug service returning zero. */
	if (sample->resources != SCP_RESOURCE_COUNT)
		return -EPROTO;
	ret = ccci_scp_resource_query(SCP_QUERY_USER_COUNT, 0, &sample->users);
	if (ret)
		return ret;
	if (sample->users > SCP_RESOURCE_USERS_MAX)
		return -EOVERFLOW;
	user_mask = sample->users ? U32_MAX >> (32 - sample->users) : 0;
	ret = ccci_scp_resource_query(SCP_QUERY_USER_VALID, 0, &valid);
	if (ret)
		return ret;
	if (valid & ~user_mask)
		return -EPROTO;
	for (i = 0; i < sample->users; i++) {
		ret = ccci_scp_resource_query(SCP_QUERY_USER_NAME, i, &name);
		if (ret)
			return ret;
		if (name != SCP_RESOURCE_NAME)
			continue;
		if (sample->scp_present)
			return -EPROTO;
		sample->scp_present = true;
		sample->scp_user = i;
		sample->scp_valid = !!(valid & BIT(i));
	}
	for (i = 0; i < SCP_RESOURCE_COUNT; i++) {
		ret = ccci_scp_resource_query(SCP_QUERY_USAGE, i, &usage);
		if (ret)
			return ret;
		if (usage & ~user_mask)
			return -EAGAIN;
		if (sample->scp_present && (usage & BIT(sample->scp_user)))
			sample->scp_resources |= BIT(i);
	}
	ret = ccci_scp_resource_query(SCP_QUERY_USER_COUNT, 0, &users_after);
	if (ret)
		return ret;
	ret = ccci_scp_resource_query(SCP_QUERY_USER_VALID, 0, &valid_after);
	if (ret)
		return ret;
	if (users_after != sample->users || valid_after != valid)
		return -EAGAIN;
	return 0;
}

int ccci_scp_resource_read(struct ccci_scp_resource_sample *sample)
{
	struct ccci_scp_resource_sample first, second;
	int ret;

	if (!sample)
		return -EINVAL;
	*sample = (struct ccci_scp_resource_sample) {};
	ret = ccci_scp_resource_sample(&first);
	if (ret)
		return ret;
	ret = ccci_scp_resource_sample(&second);
	if (ret)
		return ret;
	if (first.resources != second.resources || first.users != second.users ||
	    first.scp_user != second.scp_user || first.scp_valid != second.scp_valid ||
	    first.scp_resources != second.scp_resources)
		return -EAGAIN;
	*sample = second;
	return 0;
}

int ccci_scp_resource_pin(struct ccci_scp_resource *resource)
{
	struct arm_smccc_res result;
	int ret;

	if (!resource)
		return -EINVAL;
	if (resource->fault)
		return -EIO;
	if (resource->pinned)
		return 0;
	ret = ccci_scp_resource_read(&resource->before);
	if (ret)
		goto fault;
	/* Never override a firmware/debug decision to bypass the SCP user. */
	if (resource->before.scp_present && !resource->before.scp_valid) {
		ret = -EACCES;
		goto fault;
	}
	resource->attempted = true;
	arm_smccc_smc(SCP_RESOURCE_SMC, SCP_RESOURCE_REQUEST, SCP_RESOURCE_REQUEST_26M,
		      0, 0, 0, 0, 0, &result);
	ret = -EIO;
	if (result.a0 || result.a1)
		goto fault;
	resource->accepted = true;
	ret = ccci_scp_resource_read(&resource->after);
	if (ret)
		goto fault;
	/* Registration can be lazy, but an existing SCP identity must persist. */
	if (!resource->after.scp_present || !resource->after.scp_valid ||
	    (resource->before.scp_present &&
	     resource->before.scp_user != resource->after.scp_user) ||
	    resource->after.scp_resources !=
	    (resource->before.scp_resources | CCCI_SCP_RESOURCE_26M)) {
		ret = -EPROTO;
		goto fault;
	}
	resource->pinned = true;
	return 0;
fault:
	resource->fault = true;
	resource->error = ret;
	return ret;
}

void ccci_scp_resource_invalidate(struct ccci_scp_resource *resource)
{
	if (resource)
		resource->fault = true;
}

int ccci_scp_resource_check(struct ccci_scp_resource *resource, unsigned long rate)
{
	struct ccci_scp_resource_sample sample;
	int ret;

	if (!resource)
		return -EINVAL;
	if (resource->fault)
		return -ESHUTDOWN;
	/* Before an explicit pin, do not even query or register a firmware user. */
	if (!resource->pinned)
		return -EAGAIN;
	ret = -EBUSY;
	if (rate != 26000000)
		goto fault;
	ret = ccci_scp_resource_read(&sample);
	if (ret)
		goto fault;
	if (sample.scp_valid && sample.scp_present &&
	    (sample.scp_resources & CCCI_SCP_RESOURCE_26M) &&
	    sample.scp_user == resource->after.scp_user &&
	    sample.scp_resources == resource->after.scp_resources)
		return 0;
	ret = -EPROTO;
fault:
	resource->error = ret;
	ccci_scp_resource_invalidate(resource);
	return ret;
}
