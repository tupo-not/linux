// SPDX-License-Identifier: GPL-2.0-only
/* One-shot CCIF receive session, sharing the existing owner's IRQs and maps. */
#include <linux/bitops.h>
#include <linux/device.h>
#include <linux/err.h>
#include <linux/interrupt.h>
#include <linux/io.h>
#include <linux/slab.h>
#include <linux/spinlock.h>
#include <linux/unaligned.h>
#include <linux/workqueue.h>

#include "ccci-ccif-ring.h"
#include "ccci-ccif-rx.h"

#define CCIF_RCHNUM	0x10
#define CCIF_ACK	0x14
#define CCIF_SRAM	0x100
#define CCIF_DATA_ACK	0x0000ffff
/* IRQ1's vendor mask is 0x7fff8000; bit15 is captured/ACKed once with SRAM. */
#define CCIF_EX_ACK	0x7fff0000
#define CCIF_VALID	0x00ffffff
#define CCIF_CCB	BIT(7)
#define CCIF_HS1	BIT(15)
#define CCIF_BUDGET	32
#define CCIF_BUFFER	(80 * 1024)

struct ccci_ccif_rx {
	void __iomem *ap;
	struct ccci_ccif_ring *rings;
	int irq[2];
	/* Serializes mask depth and the final rearm gate against IRQ and STOP. */
	spinlock_t gate;
	struct work_struct work;
	ccci_ccif_consume_fn consume;
	void *context;
	u8 *packet;
	u8 sram[CCCI_CCIF_HS1_SIZE];
	u32 next_queue;
	int error;
	bool attempted;
	bool active;
	bool armed;
	bool scheduled;
};

static void ccif_rx_mask_locked(struct ccci_ccif_rx *rx)
{
	if (!rx->armed)
		return;
	rx->armed = false;
	for (u32 i = 0; i < 2; i++)
		disable_irq_nosync(rx->irq[i]);
}

static void ccif_rx_fault(struct ccci_ccif_rx *rx, int error)
{
	unsigned long flags;

	spin_lock_irqsave(&rx->gate, flags);
	if (!rx->error)
		rx->error = error;
	rx->active = false;
	ccif_rx_mask_locked(rx);
	spin_unlock_irqrestore(&rx->gate, flags);
}

static bool ccif_rx_active(struct ccci_ccif_rx *rx)
{
	unsigned long flags;
	bool active;

	spin_lock_irqsave(&rx->gate, flags);
	active = rx->active;
	spin_unlock_irqrestore(&rx->gate, flags);
	return active;
}

static int ccif_rx_notifications(struct ccci_ccif_rx *rx)
{
	struct ccci_ccif_event event = { .type = CCCI_CCIF_EVENT_SRAM };
	u32 pending = readl(rx->ap + CCIF_RCHNUM);
	u8 features[CCCI_CCIF_FEATURES];
	int ret;

	if (pending & ~CCIF_VALID)
		return -EPROTO;
	if (pending & CCIF_HS1) {
		/* Save and validate the complete download before either ACK mask. */
		for (u32 i = 0; i < sizeof(rx->sram); i += 4)
			put_unaligned_le32(readl(rx->ap + CCIF_SRAM + i), rx->sram + i);
		ret = ccci_ccif_hs1_decode(rx->sram, sizeof(rx->sram), features);
		if (ret)
			return ret;
		event.data = rx->sram;
		event.size = sizeof(rx->sram);
		event.notifications = pending;
		ccci_ccif_header_decode(event.data, event.size, &event.header);
	}
	/* Snapshot bits only: never ACK a second, newly sampled RCHNUM value. */
	if (pending & CCIF_DATA_ACK)
		writel(pending & CCIF_DATA_ACK, rx->ap + CCIF_ACK);
	if (pending & CCIF_EX_ACK)
		writel(pending & CCIF_EX_ACK, rx->ap + CCIF_ACK);
	/* Flush the posted ACKs before a possible unmask. */
	readl(rx->ap + CCIF_RCHNUM);
	if (pending & (CCIF_CCB | CCIF_EX_ACK)) {
		struct ccci_ccif_event notify = {
			.type = CCCI_CCIF_EVENT_NOTIFY,
			.notifications = pending & (CCIF_CCB | CCIF_EX_ACK),
		};

		/* Give exception handling priority over any simultaneous HS1. */
		ret = rx->consume(rx->context, &notify);
		if (ret)
			return ret < 0 ? ret : -EPROTO;
	}
	/* ACK before callbacks can send a reply; the SRAM copy is already safe. */
	if (event.size) {
		ret = rx->consume(rx->context, &event);
		return ret > 0 ? -EPROTO : ret;
	}
	return 0;
}

static int ccif_rx_packet(struct ccci_ccif_rx *rx, u32 queue)
{
	struct ccci_ccif_event event = {
		.type = CCCI_CCIF_EVENT_RING,
		.data = rx->packet,
		.queue = queue,
	};
	int ret;

	ret = ccci_ccif_ring_receive(&rx->rings[queue], rx->packet, CCIF_BUFFER, &event.size);
	if (ret)
		return ret == -ENODATA ? 0 : ret;
	ret = ccci_ccif_header_decode(event.data, event.size, &event.header);
	if (ret)
		return ret;
	/* The AP ring cursor moved only after this receiver acquired CPU storage. */
	ret = rx->consume(rx->context, &event);
	return ret ? (ret < 0 ? ret : -EPROTO) : 1;
}

static void ccif_rx_work(struct work_struct *work)
{
	struct ccci_ccif_rx *rx = container_of(work, struct ccci_ccif_rx, work);
	u32 empty = 0, packets = 0;
	unsigned long flags;
	int ret;

	if (!ccif_rx_active(rx))
		return;
	ret = ccif_rx_notifications(rx);
	if (ret)
		goto fault;
	/* Scan all rings: coalesced notifications are hints, not packet counts. */
	while (empty < CCCI_CCIF_QUEUES && packets < CCIF_BUDGET) {
		u32 queue = rx->next_queue;

		if (!ccif_rx_active(rx))
			return;
		rx->next_queue = (queue + 1) % CCCI_CCIF_QUEUES;
		if (!rx->rings[queue].slot.rx) {
			empty++;
			continue;
		}
		ret = ccif_rx_packet(rx, queue);
		if (!ret) {
			empty++;
			continue;
		}
		/* A published producer pointer must describe complete packets. */
		if (ret < 0)
			goto fault;
		empty = 0;
		packets++;
	}
	spin_lock_irqsave(&rx->gate, flags);
	if (rx->active) {
		if (packets == CCIF_BUDGET) {
			/* Keep both lines masked while continuation owns the drain. */
			if (!queue_work(system_dfl_wq, &rx->work)) {
				rx->error = -EIO;
				rx->active = false;
			}
		} else {
			rx->scheduled = false;
			rx->armed = true;
			for (u32 i = 0; i < 2; i++)
				enable_irq(rx->irq[i]);
		}
	}
	spin_unlock_irqrestore(&rx->gate, flags);
	return;
fault:
	ccif_rx_fault(rx, ret);
}

void ccci_ccif_rx_interrupt(struct ccci_ccif_rx *rx, int irq)
{
	unsigned long flags;

	spin_lock_irqsave(&rx->gate, flags);
	ccif_rx_mask_locked(rx);
	if (irq != rx->irq[0] && irq != rx->irq[1]) {
		rx->error = -EINVAL;
		rx->active = false;
	} else if (rx->active && !rx->scheduled) {
		rx->scheduled = true;
		if (!queue_work(system_dfl_wq, &rx->work)) {
			rx->error = -EIO;
			rx->active = false;
		}
	} else if (!rx->attempted && !rx->error) {
		rx->error = -EIO;
	}
	spin_unlock_irqrestore(&rx->gate, flags);
}

int ccci_ccif_rx_start(struct ccci_ccif_rx *rx, ccci_ccif_consume_fn consume, void *context)
{
	unsigned long flags;
	int ret = 0;

	if (!consume)
		return -EINVAL;
	spin_lock_irqsave(&rx->gate, flags);
	if (rx->error) {
		ret = -EIO;
	} else if (rx->attempted) {
		ret = -EALREADY;
	} else {
		rx->attempted = true;
		rx->consume = consume;
		rx->context = context;
		rx->active = true;
		rx->scheduled = true;
		/* Start masked and drain first; the worker alone opens the gate. */
		if (!queue_work(system_dfl_wq, &rx->work)) {
			rx->active = false;
			rx->error = -EIO;
			ret = -EIO;
		}
	}
	spin_unlock_irqrestore(&rx->gate, flags);
	return ret;
}

void ccci_ccif_rx_stop(struct ccci_ccif_rx *rx)
{
	unsigned long flags;

	spin_lock_irqsave(&rx->gate, flags);
	rx->active = false;
	ccif_rx_mask_locked(rx);
	spin_unlock_irqrestore(&rx->gate, flags);
	for (u32 i = 0; i < 2; i++)
		synchronize_irq(rx->irq[i]);
	cancel_work_sync(&rx->work);
	rx->scheduled = false;
	/* No callback can still refer to the consumer after this returns. */
	rx->consume = NULL;
	rx->context = NULL;
}

void ccci_ccif_rx_abort(struct ccci_ccif_rx *rx, int error)
{
	ccif_rx_fault(rx, error < 0 ? error : -EPROTO);
}

int ccci_ccif_rx_rearm(struct ccci_ccif_rx *rx, bool commit)
{
	unsigned long flags;
	int ret = 0;

	spin_lock_irqsave(&rx->gate, flags);
	if (rx->active || rx->armed || rx->scheduled || rx->consume || rx->error) {
		ret = -EBUSY;
	} else if (commit) {
		rx->attempted = false;
		rx->next_queue = 0;
	}
	spin_unlock_irqrestore(&rx->gate, flags);
	return ret;
}

bool ccci_ccif_rx_started(struct ccci_ccif_rx *rx)
{
	unsigned long flags;
	bool started;

	spin_lock_irqsave(&rx->gate, flags);
	started = rx->attempted;
	spin_unlock_irqrestore(&rx->gate, flags);
	return started;
}

int ccci_ccif_rx_error(struct ccci_ccif_rx *rx)
{
	unsigned long flags;
	int ret;

	spin_lock_irqsave(&rx->gate, flags);
	ret = rx->error;
	spin_unlock_irqrestore(&rx->gate, flags);
	return ret;
}

bool ccci_ccif_rx_armed(struct ccci_ccif_rx *rx)
{
	unsigned long flags;
	bool armed;

	spin_lock_irqsave(&rx->gate, flags);
	armed = rx->armed;
	spin_unlock_irqrestore(&rx->gate, flags);
	return armed;
}

struct ccci_ccif_rx *ccci_ccif_rx_create(struct device *dev, void __iomem *ap,
					 struct ccci_ccif_ring *rings, const int irq[2])
{
	struct ccci_ccif_rx *rx;

	rx = devm_kzalloc(dev, sizeof(*rx), GFP_KERNEL);
	if (!rx)
		return ERR_PTR(-ENOMEM);
	rx->packet = devm_kmalloc(dev, CCIF_BUFFER, GFP_KERNEL);
	if (!rx->packet)
		return ERR_PTR(-ENOMEM);
	rx->ap = ap;
	rx->rings = rings;
	for (u32 i = 0; i < 2; i++)
		rx->irq[i] = irq[i];
	spin_lock_init(&rx->gate);
	INIT_WORK(&rx->work, ccif_rx_work);
	return rx;
}
