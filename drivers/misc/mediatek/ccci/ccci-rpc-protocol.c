// SPDX-License-Identifier: GPL-2.0-only
#include <linux/errno.h>
#include <linux/string.h>
#include <linux/unaligned.h>

#include "ccci-rpc-protocol.h"

enum ccci_rpc_owner ccci_rpc_owner(u32 operation)
{
	if (operation == 0x400f || operation == 0x4010)
		return CCCI_RPC_USERSPACE;
	return CCCI_RPC_KERNEL;
}

static int ccci_rpc_decode(const void *packet, size_t length, bool reply,
			   struct ccci_rpc_message *message)
{
	const u8 *bytes = packet;
	size_t offset = 24, padded;
	u32 i, size;

	if (!message)
		return -EINVAL;
	memset(message, 0, sizeof(*message));
	if (!packet || length < offset || length > CCCI_RPC_MAX_PACKET)
		return -EMSGSIZE;
	message->data0 = get_unaligned_le32(bytes);
	message->channel_seq = get_unaligned_le32(bytes + 8);
	message->slot = get_unaligned_le32(bytes + 12);
	message->operation = get_unaligned_le32(bytes + 16);
	message->argc = get_unaligned_le32(bytes + 20);
	if (get_unaligned_le32(bytes + 4) != length ||
	    (message->channel_seq & 0xffff) != (reply ? CCCI_RPC_TX : CCCI_RPC_RX) ||
	    message->slot > 2 || message->argc > CCCI_RPC_MAX_ARGS ||
	    (message->operation & CCCI_RPC_RESPONSE) != (reply ? CCCI_RPC_RESPONSE : 0))
		goto malformed;
	for (i = 0; i < message->argc; i++) {
		if (length - offset < sizeof(u32))
			goto malformed;
		size = get_unaligned_le32(bytes + offset);
		offset += sizeof(u32);
		if (size > length - offset)
			goto malformed;
		padded = ((size_t)size + 3) & ~(size_t)3;
		if (padded > length - offset)
			goto malformed;
		message->args[i].data = bytes + offset;
		message->args[i].size = size;
		offset += padded;
	}
	if (offset != length)
		goto malformed;
	return 0;

malformed:
	memset(message, 0, sizeof(*message));
	return -EPROTO;
}

int ccci_rpc_decode_request(const void *packet, size_t length,
			    struct ccci_rpc_message *request)
{
	return ccci_rpc_decode(packet, length, false, request);
}

int ccci_rpc_encode_reply(const struct ccci_rpc_message *request,
			  const struct ccci_rpc_arg *args, u32 argc,
			  void *packet, size_t capacity)
{
	size_t length = 24, offset = 24, padded;
	u8 *bytes = packet;
	u32 i;

	if (!request || !packet || (argc && !args) || argc > CCCI_RPC_MAX_ARGS ||
	    request->slot > 2 || (request->operation & CCCI_RPC_RESPONSE) ||
	    (request->channel_seq & 0xffff) != CCCI_RPC_RX)
		return -EINVAL;
	for (i = 0; i < argc; i++) {
		if ((args[i].size && !args[i].data) ||
		    args[i].size > CCCI_RPC_MAX_PACKET - 4)
			return -EINVAL;
		padded = ((size_t)args[i].size + 3) & ~(size_t)3;
		if (padded + 4 > CCCI_RPC_MAX_PACKET - length)
			return -EMSGSIZE;
		length += padded + 4;
	}
	if (length > capacity)
		return -ENOSPC;
	memset(packet, 0, length);
	put_unaligned_le32(request->data0, bytes);
	put_unaligned_le32(length, bytes + 4);
	put_unaligned_le32((request->channel_seq & 0xffff0000) | CCCI_RPC_TX, bytes + 8);
	put_unaligned_le32(request->slot, bytes + 12);
	put_unaligned_le32(request->operation | CCCI_RPC_RESPONSE, bytes + 16);
	put_unaligned_le32(argc, bytes + 20);
	for (i = 0; i < argc; i++) {
		put_unaligned_le32(args[i].size, bytes + offset);
		offset += sizeof(u32);
		if (args[i].size)
			memcpy(bytes + offset, args[i].data, args[i].size);
		offset += ((size_t)args[i].size + 3) & ~(size_t)3;
	}
	return length;
}

int ccci_rpc_validate_reply(const struct ccci_rpc_message *request,
			    const void *packet, size_t length)
{
	struct ccci_rpc_message reply;
	int ret;

	if (!request || request->slot > 2 || (request->operation & CCCI_RPC_RESPONSE) ||
	    (request->channel_seq & 0xffff) != CCCI_RPC_RX)
		return -EINVAL;
	ret = ccci_rpc_decode(packet, length, true, &reply);
	if (ret)
		return ret;
	if (reply.operation != (request->operation | CCCI_RPC_RESPONSE) ||
	    reply.slot != request->slot || reply.data0 != request->data0)
		return -EPROTO;
	return 0;
}
