// SPDX-License-Identifier: GPL-2.0-only
/* Bounded CPU queue adapter: reserve full batch before completion and ACK. */
#include <linux/bitops.h>
#include <linux/errno.h>
#include <linux/string.h>

#include "ccci-scp-queue.h"

void ccci_scp_queue_invalidate(struct ccci_scp_queue *queue)
{
	u32 i;

	if (!queue)
		return;
	queue->fault = true;
	for (i = 0; i < CCCI_SCP_MAX_MBOXES; i++) {
		ccci_scp_rx_invalidate(&queue->rx[i]);
		queue->ports[i].pending = 0;
		queue->ports[i].reserved = 0;
	}
	memzero_explicit(queue->events, sizeof(queue->events));
	queue->count = 0;
	queue->head = 0;
}

static int ccci_scp_queue_error(struct ccci_scp_queue *queue, int error)
{
	ccci_scp_queue_invalidate(queue);
	return error;
}

static int ccci_scp_queue_pending(void *context, u32 *mask)
{
	struct ccci_scp_queue_port *port = context;
	struct ccci_scp_queue *queue = port->queue;
	int ret;

	*mask = 0;
	if (queue->fault)
		return -ESHUTDOWN;
	if (port->reserved)
		return ccci_scp_queue_error(queue, -EPROTO);
	port->pending = 0;
	ret = port->hardware->pending(port->context, &port->pending);
	if (ret)
		return ccci_scp_queue_error(queue, ret);
	if (port->pending & ~queue->rx[port->mbox].supported_mask)
		return ccci_scp_queue_error(queue, -EOPNOTSUPP);
	*mask = port->pending;
	return 0;
}

static int ccci_scp_queue_read(void *context, u32 offset, u32 words, void *buffer)
{
	struct ccci_scp_queue_port *port = context;
	int ret;

	if (port->queue->fault)
		return -ESHUTDOWN;
	ret = port->hardware->read_words(port->context, offset, words, buffer);
	if (ret)
		return ccci_scp_queue_error(port->queue, ret);
	return 0;
}

static int ccci_scp_queue_post(void *context)
{
	struct ccci_scp_queue_port *port = context;
	struct ccci_scp_queue *queue = port->queue;
	u32 needed = hweight32(port->pending);
	int ret;

	if (queue->fault)
		return -ESHUTDOWN;
	if (!needed || port->reserved || queue->count > queue->capacity)
		return ccci_scp_queue_error(queue, -EPROTO);
	if (needed > queue->capacity - queue->count)
		return ccci_scp_queue_error(queue, -ENOSPC);
	/* Owner serialization prevents any other producer consuming these slots. */
	port->reserved = needed;
	ret = port->hardware->post_capture(port->context);
	if (ret)
		return ccci_scp_queue_error(queue, ret);
	return 0;
}

static int ccci_scp_queue_ack(void *context, u32 mask)
{
	struct ccci_scp_queue_port *port = context;
	struct ccci_scp_queue *queue = port->queue;
	int ret;

	if (queue->fault)
		return -ESHUTDOWN;
	if (mask != port->pending || !port->reserved)
		return ccci_scp_queue_error(queue, -EPROTO);
	ret = port->hardware->ack(port->context, mask);
	if (ret)
		return ccci_scp_queue_error(queue, ret);
	return 0;
}

static void ccci_scp_queue_deliver(void *context, const struct ccci_scp_pin *pin,
				   const void *buffer, size_t bytes, u64 generation)
{
	struct ccci_scp_queue_port *port = context;
	struct ccci_scp_queue *queue = port->queue;
	struct ccci_scp_event *event;

	/* Exact layout/payload and complete reservation are established by RX. */
	event = &queue->events[(queue->head + queue->count) % queue->capacity];
	event->generation = generation;
	event->pin = *pin;
	event->bytes = bytes;
	memcpy(event->data, buffer, bytes);
	queue->count++;
	port->reserved--;
}

static const struct ccci_scp_rx_ops ccci_scp_queue_ops = {
	.pending = ccci_scp_queue_pending,
	.read_words = ccci_scp_queue_read,
	.post_capture = ccci_scp_queue_post,
	.ack = ccci_scp_queue_ack,
	.deliver = ccci_scp_queue_deliver,
};

int ccci_scp_queue_init(struct ccci_scp_queue *queue,
			const struct ccci_scp_layout *layout, u64 generation,
			u32 capacity,
			const struct ccci_scp_rx_ops * const hardware[CCCI_SCP_MAX_MBOXES],
			void * const contexts[CCCI_SCP_MAX_MBOXES])
{
	u32 i, j, known;
	int ret;

	if (!queue)
		return -EINVAL;
	memset(queue, 0, sizeof(*queue));
	ccci_scp_queue_invalidate(queue);
	if (!layout || !layout->rx_count || layout->rx_count > CCCI_SCP_MAX_CHANNELS ||
	    !generation || !capacity || capacity > CCCI_SCP_QUEUE_CAPACITY ||
	    !hardware || !contexts)
		return -EINVAL;
	for (i = 0; i < CCCI_SCP_MAX_MBOXES; i++) {
		if (!hardware[i] || !hardware[i]->pending || !hardware[i]->read_words ||
		    !hardware[i]->post_capture || !hardware[i]->ack) {
			ret = -EINVAL;
			goto fault;
		}
		known = 0;
		for (j = 0; j < layout->rx_count; j++) {
			if (layout->rx[j].mbox != i)
				continue;
			if (layout->rx[j].irq >= 32) {
				ret = -EINVAL;
				goto fault;
			}
			known |= BIT(layout->rx[j].irq);
		}
		queue->ports[i].queue = queue;
		queue->ports[i].hardware = hardware[i];
		queue->ports[i].context = contexts[i];
		queue->ports[i].mbox = i;
		ret = ccci_scp_rx_init(&queue->rx[i], layout, i, known, generation,
				       &ccci_scp_queue_ops, &queue->ports[i]);
		if (ret)
			goto fault;
	}
	queue->generation = generation;
	queue->capacity = capacity;
	queue->fault = false;
	return 0;
fault:
	ccci_scp_queue_invalidate(queue);
	return ret;
}

int ccci_scp_queue_pop(struct ccci_scp_queue *queue, u64 generation,
		       struct ccci_scp_event *event)
{
	struct ccci_scp_event *oldest;

	if (!queue || !event)
		return -EINVAL;
	memzero_explicit(event, sizeof(*event));
	if (queue->fault)
		return -ESHUTDOWN;
	if (!generation || generation != queue->generation)
		return -ESTALE;
	if (!queue->count)
		return -ENODATA;
	oldest = &queue->events[queue->head];
	*event = *oldest;
	memzero_explicit(oldest, sizeof(*oldest));
	queue->head = (queue->head + 1) % queue->capacity;
	queue->count--;
	return 0;
}
