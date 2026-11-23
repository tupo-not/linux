// SPDX-License-Identifier: GPL-2.0-only
/* MT6893 ccci_mdinit FS envelope; operation semantics belong to the server. */
#include <linux/errno.h>
#include <linux/string.h>
#include <linux/unaligned.h>

#include "ccci-fs-protocol.h"

static bool fs_operation(u32 operation)
{
	u32 op = operation & 0xffff;

	/* Requests carry the bare opcode; replies OR it with 0xffff0000. */
	return op >= 0x1001 && op <= 0x1025 &&
	       (operation == op || operation == (0xffff0000U | op));
}

int ccci_fs_decode(const void *body, size_t length,
		   struct ccci_fs_message *message)
{
	struct ccci_fs_message result = {};
	const u8 *bytes = body;
	size_t offset = 8, padded;
	u32 i, size;

	if (!message)
		return -EINVAL;
	memset(message, 0, sizeof(*message));
	if (!body || length < 8 || length > CCCI_FS_BODY_MAX)
		return -EMSGSIZE;
	result.operation = get_unaligned_le32(bytes);
	result.argc = get_unaligned_le32(bytes + 4);
	if (!fs_operation(result.operation) || result.argc > CCCI_FS_ARGS)
		return -EPROTO;
	for (i = 0; i < result.argc; i++) {
		if (length - offset < 4)
			return -EBADMSG;
		size = get_unaligned_le32(bytes + offset);
		offset += 4;
		/* Bound before rounding, including on 32-bit hosts. */
		if (size > length - offset)
			return -EBADMSG;
		padded = ((size_t)size + 3) & ~(size_t)3;
		if (padded > length - offset)
			return -EBADMSG;
		result.args[i].data = bytes + offset;
		result.args[i].length = size;
		offset += padded;
	}
	if (offset != length)
		return -EBADMSG;
	*message = result;
	return 0;
}

int ccci_fs_encode(void *body, size_t capacity, u32 operation,
		   const struct ccci_fs_arg *args, u32 argc)
{
	u8 *bytes = body;
	size_t length = 8, padded, offset;
	u32 i;

	if (!body || !fs_operation(operation) || argc > CCCI_FS_ARGS ||
	    (argc && !args))
		return -EINVAL;
	for (i = 0; i < argc; i++) {
		if ((args[i].length && !args[i].data) ||
		    args[i].length > CCCI_FS_BODY_MAX)
			return -EINVAL;
		padded = ((size_t)args[i].length + 3) & ~(size_t)3;
		if (length > CCCI_FS_BODY_MAX - 4 ||
		    padded > CCCI_FS_BODY_MAX - length - 4)
			return -EMSGSIZE;
		length += 4 + padded;
	}
	if (length > capacity)
		return -ENOSPC;
	memset(bytes, 0, length);
	put_unaligned_le32(operation, bytes);
	put_unaligned_le32(argc, bytes + 4);
	offset = 8;
	for (i = 0; i < argc; i++) {
		put_unaligned_le32(args[i].length, bytes + offset);
		offset += 4;
		if (args[i].length)
			memcpy(bytes + offset, args[i].data, args[i].length);
		offset += ((size_t)args[i].length + 3) & ~(size_t)3;
	}
	return length;
}

int ccci_fs_fragment_decode(const void *packet, size_t length, u32 direction,
			    struct ccci_fs_fragment *fragment)
{
	struct ccci_fs_fragment result = {};
	const u8 *bytes = packet;

	if (!fragment)
		return -EINVAL;
	memset(fragment, 0, sizeof(*fragment));
	if (direction != CCCI_FS_RX && direction != CCCI_FS_TX)
		return -EINVAL;
	if (!packet || length <= CCCI_FS_FRAGMENT_PREFIX ||
	    length > CCCI_FS_FRAGMENT_PREFIX + CCCI_FS_FRAGMENT_DATA)
		return -EMSGSIZE;
	if (get_unaligned_le32(bytes + 4) != length)
		return -EBADMSG;
	result.data0 = get_unaligned_le32(bytes);
	result.channel_sequence = get_unaligned_le32(bytes + 8);
	result.slot = get_unaligned_le32(bytes + 12);
	result.operation = get_unaligned_le32(bytes + 16);
	if ((result.channel_sequence & 0xffff) != direction ||
	    result.slot >= CCCI_FS_SLOTS || !fs_operation(result.operation))
		return -EPROTO;
	result.data = bytes + CCCI_FS_FRAGMENT_PREFIX;
	result.length = length - CCCI_FS_FRAGMENT_PREFIX;
	result.more = !!(result.data0 & CCCI_FS_MORE);
	*fragment = result;
	return 0;
}

int ccci_fs_assembly_init(struct ccci_fs_assembly *assembly,
			  void *buffer, size_t capacity)
{
	if (!assembly)
		return -EINVAL;
	memset(assembly, 0, sizeof(*assembly));
	if (!buffer || capacity < CCCI_FS_HEADER_SIZE + 8)
		return -EINVAL;
	assembly->buffer = buffer;
	assembly->capacity = capacity;
	return 0;
}

int ccci_fs_assembly_feed(struct ccci_fs_assembly *assembly,
			  const void *packet, size_t length)
{
	struct ccci_fs_fragment fragment;
	struct ccci_fs_message message;
	size_t offset;
	int ret;

	if (!assembly || !assembly->buffer)
		return -EINVAL;
	if (assembly->error)
		return assembly->error;
	if (assembly->length && !assembly->active) {
		ret = -EALREADY;
		goto fail;
	}
	ret = ccci_fs_fragment_decode(packet, length, CCCI_FS_RX, &fragment);
	if (ret)
		goto fail;
	if (assembly->active && (assembly->slot != fragment.slot ||
				 assembly->operation != fragment.operation)) {
		ret = -EPROTO;
		goto fail;
	}
	offset = assembly->active ? assembly->length : CCCI_FS_FRAGMENT_PREFIX;
	if (offset > CCCI_FS_MESSAGE_MAX ||
	    fragment.length > CCCI_FS_MESSAGE_MAX - offset ||
	    offset > assembly->capacity ||
	    fragment.length > assembly->capacity - offset) {
		ret = -EMSGSIZE;
		goto fail;
	}
	if (!assembly->active) {
		assembly->slot = fragment.slot;
		assembly->operation = fragment.operation;
		put_unaligned_le32(fragment.operation, assembly->buffer + 16);
	}
	memcpy(assembly->buffer + offset, fragment.data, fragment.length);
	assembly->length = offset + fragment.length;
	assembly->active = fragment.more;
	if (fragment.more)
		return 0;
	ret = ccci_fs_decode(assembly->buffer + 16, assembly->length - 16,
			     &message);
	if (ret)
		goto fail;
	memcpy(assembly->buffer, packet, CCCI_FS_HEADER_SIZE);
	put_unaligned_le32(fragment.data0 & ~CCCI_FS_MORE, assembly->buffer);
	put_unaligned_le32(assembly->length, assembly->buffer + 4);
	return 1;
fail:
	assembly->error = ret;
	assembly->active = false;
	return ret;
}
