/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef __CCCI_CCIF_RING_H
#define __CCCI_CCIF_RING_H

#include <linux/types.h>

#define CCCI_CCIF_QUEUES	16
#define CCCI_CCIF_BANKS	2
#define CCCI_CCIF_NORMAL	0
#define CCCI_CCIF_EXCEPTION	1
#define CCCI_CCIF_RING_HEADER	0xee0000ee
#define CCCI_CCIF_RING_FOOTER	0xff0000ff

struct ccci_smem_layout;

struct ccci_ccif_slot {
	u32 offset;
	u32 rx;
	u32 tx;
	u32 span;
};

struct ccci_ccif_layout {
	struct ccci_ccif_slot slots[CCCI_CCIF_BANKS][CCCI_CCIF_QUEUES];
	u32 used[CCCI_CCIF_BANKS];
	u32 debug_md;
	u32 debug_size;
};

/* Byte offsets within one exclusively mapped CCISM bank. */
struct ccci_ccif_ring_ops {
	int (*read32)(void *context, u32 offset, u32 *value);
	int (*write32)(void *context, u32 offset, u32 value);
	int (*copy_from)(void *context, u32 offset, void *to, u32 size);
	int (*copy_to)(void *context, u32 offset, const void *from, u32 size);
	int (*zero)(void *context, u32 offset, u32 size);
	void (*fence)(void *context, bool publish);
	void (*bad_control)(void *context, bool tx, u32 offset, u32 value, u32 expected);
	void (*bad_tx_read)(void *context, u32 offset, u32 read, u32 write,
			    u32 length, int sample_error, u32 sample, unsigned int retries);
	void (*wait_tx_read)(void *context);
};

struct ccci_ccif_ring {
	const struct ccci_ccif_ring_ops *ops;
	void *context;
	struct ccci_ccif_slot slot;
};

int ccci_ccif_plan(const struct ccci_smem_layout *smem, struct ccci_ccif_layout *layout);
int ccci_ccif_ring_init(struct ccci_ccif_ring *ring, const struct ccci_ccif_ring_ops *ops,
			void *context, const struct ccci_ccif_slot *slot, u32 bank_size);
/* Caller must have stopped MD and disabled/synchronized all ring users. */
int ccci_ccif_ring_format(struct ccci_ccif_ring *ring);
int ccci_ccif_ring_verify_empty(struct ccci_ccif_ring *ring);
int ccci_ccif_ring_tx_positions(struct ccci_ccif_ring *ring, u32 *read, u32 *write);
/* Serialized SPSC operations. Caller contains non-transient failures. */
int ccci_ccif_ring_receive(struct ccci_ccif_ring *ring, void *data, u32 capacity, u32 *size);
int ccci_ccif_ring_transmit(struct ccci_ccif_ring *ring, const void *data, u32 size);

#endif
