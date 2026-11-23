/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef __CCCI_FS_PROTOCOL_H
#define __CCCI_FS_PROTOCOL_H

#include <linux/types.h>

#define CCCI_FS_RX		14
#define CCCI_FS_TX		15
#define CCCI_FS_SLOTS		5
#define CCCI_FS_ARGS		6
#define CCCI_FS_HEADER_SIZE	16
#define CCCI_FS_FRAGMENT_PREFIX	20
#define CCCI_FS_FRAGMENT_DATA	0xd80
#define CCCI_FS_MESSAGE_MAX	0x40c8
#define CCCI_FS_BODY_MAX		(CCCI_FS_MESSAGE_MAX - CCCI_FS_HEADER_SIZE)
#define CCCI_FS_MORE		0x80000000U
/* The MD strips this prefix on TX but requires it on every reply. */
#define CCCI_FS_RESPONSE	0xffff0000U

struct ccci_fs_arg {
	const void *data;
	u32 length;
};

struct ccci_fs_message {
	u32 operation;
	u32 argc;
	struct ccci_fs_arg args[CCCI_FS_ARGS];
};

struct ccci_fs_fragment {
	u32 data0;
	u32 channel_sequence;
	u32 slot;
	u32 operation;
	const u8 *data;
	u32 length;
	bool more;
};

/* Caller owns serialization and storage; no allocation or file operations. */
struct ccci_fs_assembly {
	u8 *buffer;
	size_t capacity;
	size_t length;
	u32 slot;
	u32 operation;
	bool active;
	int error;
};

/* Decode a complete body, not a fragment; args borrow the input storage. */
int ccci_fs_decode(const void *body, size_t length,
		   struct ccci_fs_message *message);
/* Returns body bytes; errors leave the destination unchanged. No aliasing. */
int ccci_fs_encode(void *body, size_t capacity, u32 operation,
		   const struct ccci_fs_arg *args, u32 argc);
/* The transport sequence is opaque; direction must be RX or TX. */
int ccci_fs_fragment_decode(const void *packet, size_t length, u32 direction,
			    struct ccci_fs_fragment *fragment);
/* Initialize/reset only when the transport generation is quiescent. */
int ccci_fs_assembly_init(struct ccci_fs_assembly *assembly,
			  void *buffer, size_t capacity);
/*
 * Returns 0 for MORE, 1 for a validated complete message, negative on error.
 * An error is sticky until init. Feed must not overlap assembly storage.
 * Final fragment supplies the canonical header; no fragment ordinal is on wire.
 * The completed buffer contains a canonical header and full body, not a wire
 * fragment. A new transaction requires init, preventing accidental duplicates.
 */
int ccci_fs_assembly_feed(struct ccci_fs_assembly *assembly,
			  const void *packet, size_t length);

#endif
