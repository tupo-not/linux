// SPDX-License-Identifier: GPL-2.0-only
#include <linux/errno.h>
#include <linux/string.h>
#include <linux/unaligned.h>

#include "ccci-rpc-lhif.h"
#include "ccci-rpc-provider.h"

#define RPC_PARAM_ERROR	(-2)
#define RPC_FUNC_FAIL	(-5)
#define RPC_EINT_TYPE	(-12)
#define RPC_EINT_NAME	(-11)

bool ccci_rpc_provider_known(u32 operation)
{
	return operation == 0x2001 || operation == 0x2002 ||
		(operation >= 0x3001 && operation <= 0x3012) ||
		(operation >= 0x4001 && operation <= 0x4017) || operation == 0x4321;
}

static size_t fact_name_length(const char *name)
{
	size_t i;

	for (i = 0; i < CCCI_RPC_FACT_NAME; i++)
		if (!name[i])
			return i;
	return CCCI_RPC_FACT_NAME;
}

static int facts_valid(const void *table, u32 count, size_t stride, bool present,
		       bool signed_values)
{
	const u8 *bytes = table;
	size_t length;
	u32 i, j;

	if (count > CCCI_RPC_FACTS_MAX || (count && !table) ||
	    (!present && (count || table)))
		return -EINVAL;
	for (i = 0; i < count; i++) {
		const char *name = (const char *)bytes + i * stride;

		length = fact_name_length(name);
		if (!length || length == CCCI_RPC_FACT_NAME)
			return -EINVAL;
		if (signed_values &&
		    ((const struct ccci_rpc_named_fact *)name)->value > 0x7fffffff)
			return -EINVAL;
		for (j = 0; j < i; j++)
			if (!strcmp(name, (const char *)bytes + j * stride))
				return -EINVAL;
	}
	return 0;
}

static int provider_valid(const struct ccci_rpc_provider *provider)
{
	int ret;

	if (!provider)
		return 0;
	ret = facts_valid(provider->gpio, provider->gpio_count, sizeof(*provider->gpio),
			  provider->gpio_present, true);
	if (!ret)
		ret = facts_valid(provider->adc, provider->adc_count, sizeof(*provider->adc),
				  provider->adc_present, true);
	if (!ret)
		ret = facts_valid(provider->dtsi, provider->dtsi_count, sizeof(*provider->dtsi),
				  provider->dtsi_present, false);
	if (!ret)
		ret = facts_valid(provider->eint, provider->eint_count, sizeof(*provider->eint),
				  provider->eint_present, false);
	return ret;
}

int ccci_rpc_provider_available(const struct ccci_rpc_provider *provider, u32 operation)
{
	int ret = provider_valid(provider);

	if (ret || !provider)
		return ret;
	switch (operation) {
	case 0x4002:
		return provider->gpio_present;
	case 0x4003:
		return provider->adc_present;
	case 0x4005:
		return provider->eint_present;
	case 0x400e:
		return provider->dtsi_present;
	default:
		return 0;
	}
}

/* Admit exact names only; a final NUL is optional, interior NUL is malformed. */
static int request_name(const struct ccci_rpc_arg *arg, size_t *length)
{
	size_t n = arg->size, i;

	if (!n || n > CCCI_RPC_FACT_NAME)
		return -EPROTO;
	/* Same-board A compares with strncmp(); a fixed field may pad zeros. */
	for (i = 0; i < n; i++)
		if (!arg->data[i])
			break;
	if (!i)
		return -EPROTO;
	*length = i;
	return 0;
}

static const void *lookup(const void *table, u32 count, size_t stride,
			  const u8 *name, size_t length)
{
	const u8 *bytes = table;
	u32 i;

	for (i = 0; i < count; i++) {
		const char *candidate = (const char *)bytes + i * stride;

		if (fact_name_length(candidate) == length && !memcmp(candidate, name, length))
			return candidate;
	}
	return NULL;
}

int ccci_rpc_provider_reply(const struct ccci_rpc_provider *provider,
			    const void *packet, size_t length,
			    void *reply, size_t capacity)
{
	struct ccci_rpc_message request;
	struct ccci_rpc_arg args[2];
	const struct ccci_rpc_named_fact *fact;
	const struct ccci_rpc_eint_fact *eint;
	struct ccci_rpc_arg name;
	u8 scalar[4], value[68];
	size_t name_length;
	u32 operation, type;
	int ret, result;

	ret = ccci_rpc_decode_request(packet, length, &request);
	if (ret)
		return ret;
	operation = request.operation;
	ret = ccci_rpc_provider_available(provider, operation);
	if (ret < 0)
		return ret;
	result = RPC_PARAM_ERROR;
	args[0] = (struct ccci_rpc_arg){scalar, sizeof(scalar)};
	args[1] = args[0];
	if (operation == 0x4007) {
		/*
		 * Same-board A get_md_adc_val() returns -1 when no ADC driver is
		 * bound; the 0x4007 case then serializes that value twice. This
		 * provider performs no ADC conversion, so it emits that exact
		 * unavailable-value reply instead of a fabricated measurement.
		 */
		if (request.argc != 1 || request.args[0].size != sizeof(u32))
			goto scalar_reply;
		put_unaligned_le32(0xffffffff, scalar);
		return ccci_rpc_encode_reply(&request, args, 2, reply, capacity);
	}
	if (operation == 0x400d) {
		/*
		 * A requires exactly one argument and reads it as one packed
		 * {net_if, lhif_q} pair; a bad shape replies -2 twice, and a
		 * rejected remap is still reported to the MD as two zeros.
		 */
		if (request.argc != 1 || request.args[0].size != 8) {
			ccci_rpc_lhif_malformed();
			put_unaligned_le32(RPC_PARAM_ERROR, scalar);
			args[0] = (struct ccci_rpc_arg){scalar, sizeof(scalar)};
			args[1] = args[0];
			return ccci_rpc_encode_reply(&request, args, 2, reply, capacity);
		}
		ccci_rpc_lhif_report(get_unaligned_le32(request.args[0].data),
				     get_unaligned_le32(request.args[0].data + 4));
		put_unaligned_le32(0, scalar);
		args[0] = (struct ccci_rpc_arg){scalar, sizeof(scalar)};
		args[1] = args[0];
		return ccci_rpc_encode_reply(&request, args, 2, reply, capacity);
	}
	if (!ret) {
		if (ccci_rpc_provider_known(operation))
			return -EOPNOTSUPP;
		put_unaligned_le32(-1, scalar);
		args[0] = (struct ccci_rpc_arg){scalar, sizeof(scalar)};
		return ccci_rpc_encode_reply(&request, args, 1, reply, capacity);
	}
	if (operation == 0x400e) {
		if (request.argc != 1)
			goto scalar_reply;
		if (request.args[0].size != 66)
			return -EPROTO;
		name = (struct ccci_rpc_arg){request.args[0].data + 2, 64};
		/* The packed DTSI name is a fixed-size NUL-terminated field. */
		for (name.size = 0; name.size < 64; name.size++)
			if (!name.data[name.size])
				break;
		if (!name.size || name.size == 64)
			return -EPROTO;
		memset(value, 0x0f, sizeof(value));
		if (request.args[0].data[0] == 1) {
			fact = lookup(provider->dtsi, provider->dtsi_count, sizeof(*fact),
				      name.data, name.size);
			if (fact)
				put_unaligned_le32(fact->value, value);
		}
		put_unaligned_le32(0, scalar);
		args[1] = (struct ccci_rpc_arg){value, sizeof(value)};
		return ccci_rpc_encode_reply(&request, args, 2, reply, capacity);
	}
	if (request.argc < (operation == 0x4005 ? 3 : 2))
		goto scalar_reply;
	ret = request_name(&request.args[0], &name_length);
	if (ret)
		return ret;
	if (operation == 0x4005) {
		/* Same-board A reads one u32 and does not require an exact arg length. */
		if (request.args[2].size < sizeof(u32))
			return -EPROTO;
		type = get_unaligned_le32(request.args[2].data);
		result = RPC_EINT_TYPE;
		if (type >= CCCI_RPC_EINT_ATTRS)
			goto scalar_reply;
		eint = lookup(provider->eint, provider->eint_count, sizeof(*eint),
			      request.args[0].data, name_length);
		result = RPC_EINT_NAME;
		if (!eint || eint->values[type] < 0)
			goto scalar_reply;
		put_unaligned_le32(0, scalar);
		put_unaligned_le32(eint->values[type], value);
		args[1] = (struct ccci_rpc_arg){value, sizeof(u32)};
		return ccci_rpc_encode_reply(&request, args, 2, reply, capacity);
	}
	if (operation == 0x4002)
		fact = lookup(provider->gpio, provider->gpio_count, sizeof(*fact),
			      request.args[0].data, name_length);
	else
		fact = lookup(provider->adc, provider->adc_count, sizeof(*fact),
			      request.args[0].data, name_length);
	result = fact ? (int)fact->value : RPC_FUNC_FAIL;
scalar_reply:
	put_unaligned_le32(result, scalar);
	return ccci_rpc_encode_reply(&request, args, 2, reply, capacity);
}
