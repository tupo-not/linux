/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef __CCCI_RPC_PROTOCOL_H
#define __CCCI_RPC_PROTOCOL_H

#include <linux/types.h>

#define CCCI_RPC_MAX_ARGS	6
#define CCCI_RPC_MAX_PACKET	2048
#define CCCI_RPC_QUEUE		1
#define CCCI_RPC_RX		32
#define CCCI_RPC_TX		33
#define CCCI_RPC_RESPONSE	0xffff0000U

enum ccci_rpc_owner {
	CCCI_RPC_KERNEL,
	CCCI_RPC_USERSPACE,
};

struct ccci_rpc_arg {
	const u8 *data;
	u32 size;
};

/* Argument slices borrow the complete, immutable packet until dispatch ends. */
struct ccci_rpc_message {
	u32 data0;
	u32 channel_seq;
	u32 slot;
	u32 operation;
	u32 argc;
	struct ccci_rpc_arg args[CCCI_RPC_MAX_ARGS];
};

/* Same-board 4.19 ownership; newer platforms route additional operations. */
enum ccci_rpc_owner ccci_rpc_owner(u32 operation);
/* Complete normal-ring packet only; output is cleared on failure. */
int ccci_rpc_decode_request(const void *packet, size_t length,
			    struct ccci_rpc_message *request);
/* Use an unmodified successful decode result. Buffers must not overlap. */
int ccci_rpc_encode_reply(const struct ccci_rpc_message *request,
			  const struct ccci_rpc_arg *args, u32 argc,
			  void *packet, size_t capacity);
/* Match a userspace reply to its leased request before ordinary-ring TX. */
int ccci_rpc_validate_reply(const struct ccci_rpc_message *request,
			    const void *packet, size_t length);

#endif
