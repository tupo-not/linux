/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef __CCCI_SCP_QUEUE_H
#define __CCCI_SCP_QUEUE_H

#include "ccci-scp-rx.h"

#define CCCI_SCP_QUEUE_CAPACITY 64

struct ccci_scp_event {
	u64 generation;
	struct ccci_scp_pin pin;
	u32 bytes;
	u8 data[CCCI_SCP_MBOX_WORDS * sizeof(u32)];
};

struct ccci_scp_queue;

struct ccci_scp_queue_port {
	struct ccci_scp_queue *queue;
	const struct ccci_scp_rx_ops *hardware;
	void *context;
	u32 pending;
	u32 reserved;
	u32 mbox;
};

/* Allocate outside the stack; all calls use the shared owner's serialization. */
struct ccci_scp_queue {
	struct ccci_scp_event events[CCCI_SCP_QUEUE_CAPACITY];
	struct ccci_scp_rx rx[CCCI_SCP_MAX_MBOXES];
	struct ccci_scp_queue_port ports[CCCI_SCP_MAX_MBOXES];
	u64 generation;
	u32 capacity;
	u32 head;
	u32 count;
	bool fault;
};

/*
 * No I/O. All five hardware ops require pending/read_words/post_capture/ack;
 * their deliver is ignored. No callback may reenter queue/drain operations.
 * Layout must be an unmodified successful build; input arrays cannot alias queue.
 * Queue delivers every pin, including responses and unclaimed client channels,
 * as opaque CPU events; no READY or protocol state is accepted here.
 * Call once per owned reset generation, only after IRQ/worker/client quiescence.
 * Reinitialization cannot substitute for hardware reset or retry uncertain ACK.
 */
int ccci_scp_queue_init(struct ccci_scp_queue *queue,
			const struct ccci_scp_layout *layout, u64 generation,
			u32 capacity,
			const struct ccci_scp_rx_ops * const hardware[CCCI_SCP_MAX_MBOXES],
			void * const contexts[CCCI_SCP_MAX_MBOXES]);

/* Copy oldest CPU event; output cannot alias queue. Scrub consumed storage. */
int ccci_scp_queue_pop(struct ccci_scp_queue *queue, u64 generation,
		       struct ccci_scp_event *event);

/*
 * No I/O: invalidate all RXs and scrub all CPU payloads. Owner must still mask
 * and synchronize IRQ/work/client users before freeing/reinitializing storage.
 * A drain failure must call this even if it originated outside queue callbacks.
 */
void ccci_scp_queue_invalidate(struct ccci_scp_queue *queue);

#endif
