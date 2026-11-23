/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef __CCCI_SERVICE_ROUTER_H
#define __CCCI_SERVICE_ROUTER_H

#include "ccci-fs-protocol.h"
#include "ccci-rpc-protocol.h"

#define CCCI_SERVICE_FS_QUEUE	4
#define CCCI_SERVICE_RPC_SLOTS	3
#define CCCI_SERVICE_SLOTS	(CCCI_FS_SLOTS + CCCI_SERVICE_RPC_SLOTS)
#define CCCI_SERVICE_TIMEOUT_MS	60000
#define CCCI_SERVICE_TX_SIZE	(CCCI_FS_FRAGMENT_PREFIX + CCCI_FS_FRAGMENT_DATA)

enum ccci_service_kind {
	CCCI_SERVICE_FS,
	CCCI_SERVICE_RPC,
};

struct ccci_service_storage {
	void *request;
	void *reply;
	size_t capacity;
};

struct ccci_service_ops {
	/* Must copy bytes before returning success. No reentry into this router. */
	int (*submit)(void *context, u64 generation, u64 token,
		      enum ccci_service_kind kind, u32 slot,
		      const void *packet, size_t length);
	/* BUSY/ENOSPC must mean no publication; other failures are terminal. */
	int (*send)(void *context, const void *packet, size_t length,
		    bool hs2_seen);
};

struct ccci_service_slot {
	struct ccci_service_storage storage;
	struct ccci_fs_assembly assembly;
	u64 token;
	u64 started_ms;
	size_t request_length;
	size_t reply_length;
	size_t cursor;
	bool occupied;
	bool submitted;
};

/* Externally serialized. All buffers must be distinct and survive close(). */
struct ccci_service_router {
	const struct ccci_service_ops *ops;
	void *context;
	struct ccci_service_slot slots[CCCI_SERVICE_SLOTS];
	u8 *tx;
	u64 generation;
	u64 next_token;
	int error;
	bool attempted;
	bool active;
	bool hs2_seen;
};

/* FS storage[0..4] >= MESSAGE_MAX; RPC storage[5..7] >= MAX_PACKET. */
int ccci_service_router_init(struct ccci_service_router *router,
			     const struct ccci_service_ops *ops, void *context,
			     const struct ccci_service_storage *storage,
			     void *tx, size_t tx_capacity);
/*
 * One lease per initialized boot-lifetime router; no automatic restart.
 * Begin only after validated HS1/runtime publication enters WAIT_HS2. The
 * hs2_seen=false send mode means WAIT_HS2, never WAIT_HS1 or CPU-stopped.
 * Caller must quiesce/abort this router before leaving that MD lifecycle.
 */
int ccci_service_router_begin(struct ccci_service_router *router, u64 generation);
/* Caller has validated the real HS2 transition; this does not assert readiness. */
int ccci_service_router_hs2(struct ccci_service_router *router, u64 generation);
int ccci_service_router_receive(struct ccci_service_router *router,
				u64 generation, u32 queue, const void *packet,
				size_t length, u64 now_ms);
/*
 * Complete canonical FS message or RPC reply. Input must not alias buffers.
 * FS matches token/slot/opcode, not data0: only MORE is constrained here;
 * the vendor bit30 handling does not establish an echoed data0 contract.
 */
int ccci_service_router_reply(struct ccci_service_router *router, u64 generation,
			      u64 token, enum ccci_service_kind kind, u32 slot,
			      const void *packet, size_t length);
/* Resume after BUSY/ENOSPC using the retained immutable reply and cursor. */
int ccci_service_router_pump(struct ccci_service_router *router, u64 generation);
int ccci_service_router_tick(struct ccci_service_router *router, u64 generation,
			     u64 now_ms);
/* Latch an owner failure without releasing any live MD-owned resources. */
int ccci_service_router_abort(struct ccci_service_router *router, u64 generation,
			      int error);
/* Only after MD CPU/DMA is stopped; erases all private request/reply storage. */
int ccci_service_router_close(struct ccci_service_router *router, u64 generation,
			      bool md_stopped);

#endif
