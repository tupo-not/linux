// SPDX-License-Identifier: GPL-2.0-only
#include <linux/errno.h>
#include <linux/printk.h>
#include <linux/string.h>
#include <linux/unaligned.h>

#include "ccci-service-router.h"

static int router_check(struct ccci_service_router *router, u64 generation)
{
	if (!router || !router->ops)
		return -EINVAL;
	if (!generation || generation != router->generation)
		return -ESTALE;
	if (!router->active)
		return -ESHUTDOWN;
	return router->error;
}

static int router_fault(struct ccci_service_router *router, int error)
{
	if (!router->error)
		router->error = error < 0 ? error : -EIO;
	return router->error;
}

int ccci_service_router_init(struct ccci_service_router *router,
			     const struct ccci_service_ops *ops, void *context,
			     const struct ccci_service_storage *storage,
			     void *tx, size_t tx_capacity)
{
	u32 i;

	if (!router)
		return -EINVAL;
	memset(router, 0, sizeof(*router));
	if (!ops || !ops->submit || !ops->send || !storage || !tx ||
	    tx_capacity < CCCI_SERVICE_TX_SIZE)
		return -EINVAL;
	for (i = 0; i < CCCI_SERVICE_SLOTS; i++) {
		if (!storage[i].request || !storage[i].reply ||
		    storage[i].capacity < (i < CCCI_FS_SLOTS ?
					  CCCI_FS_MESSAGE_MAX : CCCI_RPC_MAX_PACKET))
			return -EINVAL;
	}
	for (i = 0; i < CCCI_SERVICE_SLOTS; i++) {
		router->slots[i].storage = storage[i];
		if (i < CCCI_FS_SLOTS)
			ccci_fs_assembly_init(&router->slots[i].assembly,
					      storage[i].request, storage[i].capacity);
	}
	router->ops = ops;
	router->context = context;
	router->tx = tx;
	return 0;
}

int ccci_service_router_begin(struct ccci_service_router *router, u64 generation)
{
	if (!router || !router->ops || !generation)
		return -EINVAL;
	if (router->attempted)
		return -EALREADY;
	router->attempted = true;
	router->active = true;
	router->generation = generation;
	return 0;
}

int ccci_service_router_hs2(struct ccci_service_router *router, u64 generation)
{
	int ret = router_check(router, generation);

	if (ret)
		return ret;
	if (router->hs2_seen)
		return -EALREADY;
	router->hs2_seen = true;
	return 0;
}

int ccci_service_router_receive(struct ccci_service_router *router,
				u64 generation, u32 queue, const void *packet,
				size_t length, u64 now_ms)
{
	struct ccci_rpc_message rpc;
	struct ccci_fs_fragment fs;
	struct ccci_service_slot *slot;
	enum ccci_service_kind kind;
	u32 index;
	int ret;

	ret = router_check(router, generation);
	if (ret) {
		pr_err("ccci: router receive stage=check error=%d\n", ret);
		return ret;
	}
	if (!packet || length < CCCI_FS_HEADER_SIZE) {
		pr_err("ccci: router receive stage=short length=%zu\n", length);
		return router_fault(router, -EMSGSIZE);
	}
	if ((get_unaligned_le32((const u8 *)packet + 8) & 0xffff) == CCCI_FS_RX) {
		kind = CCCI_SERVICE_FS;
		ret = queue == CCCI_SERVICE_FS_QUEUE ?
			ccci_fs_fragment_decode(packet, length, CCCI_FS_RX, &fs) : -EPROTO;
		if (ret) {
			pr_err("ccci: router receive stage=fs-fragment queue=%u error=%d\n", queue, ret);
			return router_fault(router, ret);
		}
		index = fs.slot;
	} else if ((get_unaligned_le32((const u8 *)packet + 8) & 0xffff) == CCCI_RPC_RX) {
		kind = CCCI_SERVICE_RPC;
		ret = queue == CCCI_RPC_QUEUE ?
			ccci_rpc_decode_request(packet, length, &rpc) : -EPROTO;
		if (ret) {
			pr_err("ccci: router receive stage=rpc-decode queue=%u error=%d\n", queue, ret);
			return router_fault(router, ret);
		}
		index = CCCI_FS_SLOTS + rpc.slot;
	} else {
		return -EOPNOTSUPP;
	}
	slot = &router->slots[index];
	if (slot->submitted) {
		pr_err("ccci: router receive stage=submitted index=%u\n", index);
		return router_fault(router, -EBUSY);
	}
	if (!slot->occupied) {
		if (router->next_token == ~(u64)0)
			return router_fault(router, -EOVERFLOW);
		slot->token = ++router->next_token;
		slot->started_ms = now_ms;
		slot->occupied = true;
	}
	if (kind == CCCI_SERVICE_FS) {
		ret = ccci_fs_assembly_feed(&slot->assembly, packet, length);
		if (ret < 0) {
			pr_err("ccci: router receive stage=assembly index=%u error=%d\n", index, ret);
			return router_fault(router, ret);
		}
		if (!ret)
			return 0;
		slot->request_length = slot->assembly.length;
	} else {
		memcpy(slot->storage.request, packet, length);
		slot->request_length = length;
	}
	ret = router->ops->submit(router->context, generation, slot->token,
				  kind, kind == CCCI_SERVICE_FS ? index :
				  index - CCCI_FS_SLOTS, slot->storage.request,
				  slot->request_length);
	if (ret) {
		pr_err("ccci: router receive stage=submit index=%u error=%d\n", index, ret);
		return router_fault(router, ret);
	}
	slot->submitted = true;
	return 0;
}

int ccci_service_router_reply(struct ccci_service_router *router, u64 generation,
			      u64 token, enum ccci_service_kind kind, u32 index,
			      const void *packet, size_t length)
{
	struct ccci_rpc_message request;
	struct ccci_fs_message message;
	struct ccci_service_slot *slot;
	const u8 *bytes = packet;
	int ret;

	ret = router_check(router, generation);
	if (ret)
		return ret;
	if ((kind != CCCI_SERVICE_FS && kind != CCCI_SERVICE_RPC) ||
	    index >= (kind == CCCI_SERVICE_FS ? CCCI_FS_SLOTS : CCCI_SERVICE_RPC_SLOTS))
		return -EINVAL;
	slot = &router->slots[index + (kind == CCCI_SERVICE_RPC ? CCCI_FS_SLOTS : 0)];
	if (!slot->submitted || !token || token != slot->token)
		return -ESTALE;
	if (slot->reply_length)
		return -EALREADY;
	if (kind == CCCI_SERVICE_FS) {
		if (!packet || length < 24 || length > CCCI_FS_MESSAGE_MAX ||
		    get_unaligned_le32(bytes + 4) != length ||
		    (get_unaligned_le32(bytes + 8) & 0xffff) != CCCI_FS_TX ||
		    get_unaligned_le32(bytes + 12) != index ||
		    (get_unaligned_le32(bytes) & CCCI_FS_MORE) ||
		    (get_unaligned_le32(bytes + 16) & 0xffff) !=
			    (slot->assembly.operation & 0xffff))
			return -EPROTO;
		ret = ccci_fs_decode(bytes + 16, length - 16, &message);
	} else {
		ret = ccci_rpc_decode_request(slot->storage.request,
					      slot->request_length, &request);
		if (!ret)
			ret = ccci_rpc_validate_reply(&request, packet, length);
	}
	if (ret)
		return ret;
	memcpy(slot->storage.reply, packet, length);
	slot->reply_length = length;
	slot->cursor = kind == CCCI_SERVICE_FS ? CCCI_FS_FRAGMENT_PREFIX : 0;
	return 0;
}

int ccci_service_router_pump(struct ccci_service_router *router, u64 generation)
{
	struct ccci_service_slot *slot;
	const u8 *reply;
	size_t amount, length;
	bool more;
	u32 i, data0;
	int ret;

	ret = router_check(router, generation);
	if (ret)
		return ret;
	for (i = 0; i < CCCI_SERVICE_SLOTS; i++) {
		slot = &router->slots[i];
		while (slot->reply_length) {
			reply = slot->storage.reply;
			if (i < CCCI_FS_SLOTS) {
				amount = slot->reply_length - slot->cursor;
				if (amount > CCCI_FS_FRAGMENT_DATA)
					amount = CCCI_FS_FRAGMENT_DATA;
				length = CCCI_FS_FRAGMENT_PREFIX + amount;
				more = amount < slot->reply_length - slot->cursor;
				memcpy(router->tx, reply, CCCI_FS_FRAGMENT_PREFIX);
				data0 = get_unaligned_le32(reply);
				put_unaligned_le32(more ? data0 | CCCI_FS_MORE : data0,
						   router->tx);
				put_unaligned_le32(length, router->tx + 4);
				memcpy(router->tx + CCCI_FS_FRAGMENT_PREFIX,
				       reply + slot->cursor, amount);
			} else {
				amount = slot->reply_length;
				length = amount;
				memcpy(router->tx, reply, length);
			}
			ret = router->ops->send(router->context, router->tx,
						length, router->hs2_seen);
			if (ret == -EBUSY || ret == -ENOSPC)
				return ret;
			if (ret)
				return router_fault(router, ret);
			slot->cursor += amount;
			if (slot->cursor < slot->reply_length)
				continue;
			memzero_explicit(slot->storage.request, slot->storage.capacity);
			memzero_explicit(slot->storage.reply, slot->storage.capacity);
			slot->occupied = false;
			slot->submitted = false;
			slot->request_length = 0;
			slot->reply_length = 0;
			slot->cursor = 0;
			if (i < CCCI_FS_SLOTS)
				ccci_fs_assembly_init(&slot->assembly, slot->storage.request,
						      slot->storage.capacity);
		}
	}
	memzero_explicit(router->tx, CCCI_SERVICE_TX_SIZE);
	return 0;
}

int ccci_service_router_tick(struct ccci_service_router *router, u64 generation,
			     u64 now_ms)
{
	u32 i;
	int ret = router_check(router, generation);

	if (ret)
		return ret;
	for (i = 0; i < CCCI_SERVICE_SLOTS; i++) {
		if (router->slots[i].occupied &&
		    now_ms - router->slots[i].started_ms >= CCCI_SERVICE_TIMEOUT_MS)
			return router_fault(router, -ETIMEDOUT);
	}
	return 0;
}

int ccci_service_router_abort(struct ccci_service_router *router, u64 generation,
			      int error)
{
	int ret = router_check(router, generation);

	if (ret)
		return ret;
	return router_fault(router, error);
}

int ccci_service_router_close(struct ccci_service_router *router, u64 generation,
			      bool md_stopped)
{
	u32 i;

	if (!router || !router->ops)
		return -EINVAL;
	if (!generation || generation != router->generation)
		return -ESTALE;
	if (!md_stopped)
		return -EBUSY;
	if (!router->active)
		return -EALREADY;
	router->active = false;
	for (i = 0; i < CCCI_SERVICE_SLOTS; i++) {
		memzero_explicit(router->slots[i].storage.request,
				 router->slots[i].storage.capacity);
		memzero_explicit(router->slots[i].storage.reply,
				 router->slots[i].storage.capacity);
		router->slots[i].occupied = false;
		router->slots[i].submitted = false;
		router->slots[i].reply_length = 0;
		router->slots[i].request_length = 0;
	}
	memzero_explicit(router->tx, CCCI_SERVICE_TX_SIZE);
	return 0;
}
