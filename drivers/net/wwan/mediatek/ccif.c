// SPDX-License-Identifier: GPL-2.0-only
/* CCIF transport for the MediaTek SoC modem.
 *
 * CCIF is a set of mailboxes plus ring buffers in shared SRAM.  Each logical
 * queue has one ring buffer; the AP writes TX and the modem writes RX (and
 * vice versa).  A channel is signalled by setting BUSY and writing TCHNUM;
 * the modem signals reception via RCHNUM which generates our IRQ.
 */

#include <linux/io.h>
#include <linux/slab.h>
#include <linux/spinlock.h>
#include <linux/atomic.h>
#include "ccif.h"

#define RB_CTL_LEN (sizeof(u32) * 2 + sizeof(struct ccci_ring_ctl) * 2 + \
		    sizeof(u32) * 2)

static u32 ccif_readl(void __iomem *base, u32 reg)
{
	return readl(base + reg);
}

static void ccif_writel(void __iomem *base, u32 reg, u32 val)
{
	writel(val, base + reg);
}

/* Bytes available to read from a ring. */
static u32 ring_readable(struct ccci_ring_ctl *ctl)
{
	u32 r = READ_ONCE(ctl->read);
	u32 w = READ_ONCE(ctl->write);

	if (w >= r)
		return w - r;
	return ctl->length - r + w;
}

/* Free bytes for writing into a ring (leave one byte to avoid full==empty). */
static u32 ring_writeable(struct ccci_ring_ctl *ctl)
{
	u32 r = READ_ONCE(ctl->read);
	u32 w = READ_ONCE(ctl->write);
	u32 used;

	if (w >= r)
		used = w - r;
	else
		used = ctl->length - r + w;

	return used < ctl->length ? ctl->length - used - 1 : 0;
}

static void ring_write(struct ccci_ring_ctl *ctl, u8 *base,
		       const u8 *data, u32 len)
{
	u32 w = READ_ONCE(ctl->write);
	u32 n = min_t(u32, len, ctl->length - w);

	memcpy(base + w, data, n);
	if (n < len)
		memcpy(base, data + n, len - n);
	WRITE_ONCE(ctl->write, (w + len) % ctl->length);
}

static void ring_read(struct ccci_ring_ctl *ctl, u8 *base,
		      u8 *data, u32 len)
{
	u32 r = READ_ONCE(ctl->read);
	u32 n = min_t(u32, len, ctl->length - r);

	memcpy(data, base + r, n);
	if (n < len)
		memcpy(data + n, base, len - n);
	WRITE_ONCE(ctl->read, (r + len) % ctl->length);
}

/* Set up a CCCI ring buffer: guard words, control blocks, zeroed data. */
static void ccif_create_ring(struct ccci_ringbuf *rb, u32 rx_size, u32 tx_size)
{
	u32 total = RB_CTL_LEN + rx_size + tx_size;
	u8 *p = (u8 *)rb;

	memset(p, 0, total);
	rb->head[0] = CCCI_RBF_HEADER;
	rb->head[1] = CCCI_RBF_HEADER;
	*(u32 *)(p + total - sizeof(u32)) = CCCI_RBF_FOOTER;
	*(u32 *)(p + total - 2 * sizeof(u32)) = CCCI_RBF_FOOTER;

	rb->rx_control.length = rx_size;
	rb->tx_control.length = tx_size;
}

/* Called after the modem has been released from BROM, when the shared SRAM
 * is accessible.  Sets up the ring buffers and brings CCIF out of reset. */
/* The CCISM_MCU region (where the modem expects CCCI ring buffers) starts
 * at 164KB into the non-cacheable AP/MD shared memory on MD1 generation
 * (vendor: md1_6297_noncacheable_fat[].offset for SMEM_USER_CCISM_MCU). */
#define CCISM_MCU_OFFSET (164 * 1024)

int ccif_hw_start(struct ccif *ccif, const u32 *rx_sizes, const u32 *tx_sizes)
{
	u8 *p = (u8 *)ccif->sram + CCISM_MCU_OFFSET;
	u32 i;

	if (ccif->started)
		return 0;

	dev_info(ccif->dev, "CCIF regs: CON=%#x BUSY=%#x RCHNUM=%#x\n",
		 ccif_readl(ccif->ap_base, APCCIF_CON),
		 ccif_readl(ccif->ap_base, APCCIF_BUSY),
		 ccif_readl(ccif->ap_base, APCCIF_RCHNUM));

	for (i = 0; i < ccif->nr_queues; i++) {
		u32 size = RB_CTL_LEN + rx_sizes[i] + tx_sizes[i];

		if ((size_t)(p - (u8 *)ccif->sram) + size > ccif->sram_size) {
			dev_err(ccif->dev, "CCIF SRAM too small for queue %u\n", i);
			return -ENOMEM;
		}

		ccif_create_ring((struct ccci_ringbuf *)p,
				 rx_sizes[i], tx_sizes[i]);

		ccif->rxq[i].rb = (struct ccci_ringbuf *)p;
		ccif->rxq[i].ccif_ch = i; /* RINGQ_BASE = 0 for MD1 */
		ccif->txq[i].rb = (struct ccci_ringbuf *)p;
		ccif->txq[i].ccif_ch = i;

		p += size;
	}

	ccif_writel(ccif->ap_base, APCCIF_BUSY, 0);
	ccif_writel(ccif->ap_base, APCCIF_ACK, 0xFFFFFFFF);
	atomic_set(&ccif->pending_rx, 0);
	ccif->started = true;

	dev_info(ccif->dev, "CCIF hardware started (%u queues)\n",
		 ccif->nr_queues);
	return 0;
}

int ccif_init(struct ccif *ccif, struct device *dev,
	      void __iomem *ap_base, void __iomem *md_base,
	      void __iomem *sram, size_t sram_size, u32 nr_queues,
	      const u32 *rx_sizes, const u32 *tx_sizes)
{
	memset(ccif, 0, sizeof(*ccif));
	ccif->dev = dev;
	ccif->ap_base = ap_base;
	ccif->md_base = md_base;
	ccif->sram = sram;
	ccif->sram_size = sram_size;
	ccif->nr_queues = nr_queues;

	/* NOTE: we deliberately do NOT touch the shared SRAM or the CCIF
	 * registers here.  The ap_md_c_smem region is only accessible once
	 * the modem's power domain / SPM state has been brought up, which
	 * this phase-2 scaffolding does not do yet.  Writing to it now
	 * causes an asynchronous SError (bus abort).  Hardware setup will
	 * move to a ccif_hw_start() called after modem power-on in a later
	 * phase.  For now we only initialise the software queue objects so
	 * the WWAN ports can be created. */
	ccif->txq = kcalloc(nr_queues, sizeof(*ccif->txq), GFP_KERNEL);
	ccif->rxq = kcalloc(nr_queues, sizeof(*ccif->rxq), GFP_KERNEL);
	if (!ccif->txq || !ccif->rxq)
		goto err_free;

	atomic_set(&ccif->pending_rx, 0);
	return 0;

err_free:
	kfree(ccif->txq);
	kfree(ccif->rxq);
	return -ENOMEM;
}

void ccif_deinit(struct ccif *ccif)
{
	kfree(ccif->txq);
	kfree(ccif->rxq);
}

static void ccif_trigger(struct ccif *ccif, u8 channel)
{
	if (ccif_readl(ccif->ap_base, APCCIF_BUSY) & BIT(channel))
		return;
	ccif_writel(ccif->ap_base, APCCIF_BUSY, BIT(channel));
	ccif_writel(ccif->ap_base, APCCIF_TCHNUM, channel);
}

void ccif_send_skb(struct ccif *ccif, u8 queue, struct sk_buff *skb)
{
	struct ccif_queue *q;
	struct ccci_ringbuf *rb;
	unsigned long flags;
	u32 space;

	if (!ccif->started) {
		dev_dbg_ratelimited(ccif->dev, "CCIF not started, dropping TX\n");
		return;
	}

	if (queue >= ccif->nr_queues || !ccif->txq) {
		dev_err(ccif->dev, "invalid TX queue %u\n", queue);
		return;
	}

	q = &ccif->txq[queue];
	rb = q->rb;
	spin_lock_irqsave(&q->lock, flags);

	space = ring_writeable(&rb->tx_control);
	if (space < skb->len) {
		dev_err_ratelimited(ccif->dev,
				    "TX queue %u full (%u < %u)\n",
				    queue, space, skb->len);
		spin_unlock_irqrestore(&q->lock, flags);
		return;
	}

	ring_write(&rb->tx_control, rb->data + rb->rx_control.length,
		   skb->data, skb->len);
	spin_unlock_irqrestore(&q->lock, flags);

	dev_info_ratelimited(ccif->dev, "CCIF TX: queue=%u ch=%u len=%u\n",
			     queue, q->ccif_ch, skb->len);
	ccif_trigger(ccif, q->ccif_ch);
}

void ccif_handle_irq(struct ccif *ccif)
{
	u32 rchnum;
	u32 i;

	if (!ccif->started)
		return;

	rchnum = ccif_readl(ccif->ap_base, APCCIF_RCHNUM);
	dev_info_ratelimited(ccif->dev, "CCIF IRQ: RCHNUM=0x%08x\n", rchnum);

	for (i = 0; i < ccif->nr_queues; i++)
		if (rchnum & BIT(i))
			atomic_or(BIT(i), &ccif->pending_rx);

	ccif_writel(ccif->ap_base, APCCIF_ACK, rchnum & GENMASK(15, 0));
}

struct sk_buff *ccif_poll(struct ccif *ccif, u8 *out_queue)
{
	u32 pending;
	u8 q;

	if (!ccif->started)
		return NULL;
	unsigned long flags;
	struct ccif_queue *rxq;
	struct ccci_ringbuf *rb;
	u32 len;
	struct sk_buff *skb;

	pending = atomic_read(&ccif->pending_rx);
	while (pending) {
		q = __ffs(pending);
		if (q >= ccif->nr_queues) {
			atomic_andnot(BIT(q), &ccif->pending_rx);
			pending = atomic_read(&ccif->pending_rx);
			continue;
		}

		rxq = &ccif->rxq[q];
		rb = rxq->rb;
		spin_lock_irqsave(&rxq->lock, flags);

		if (ring_readable(&rb->rx_control) < sizeof(struct ccci_header)) {
			spin_unlock_irqrestore(&rxq->lock, flags);
			atomic_andnot(BIT(q), &ccif->pending_rx);
			pending = atomic_read(&ccif->pending_rx);
			continue;
		}

		/* Peek header to learn the total packet length. */
		{
			struct ccci_header hdr;
			ring_read(&rb->rx_control,
				  rb->data, (u8 *)&hdr, sizeof(hdr));
			len = le32_to_cpu(hdr.packet_len);
		}

		if (len < sizeof(struct ccci_header) ||
		    len > ring_readable(&rb->rx_control) ||
		    len > 16384) {
			dev_err_ratelimited(ccif->dev,
					    "bogus RX len %u on queue %u\n",
					    len, q);
			spin_unlock_irqrestore(&rxq->lock, flags);
			atomic_andnot(BIT(q), &ccif->pending_rx);
			pending = atomic_read(&ccif->pending_rx);
			continue;
		}

		skb = alloc_skb(len, GFP_ATOMIC);
		if (!skb) {
			spin_unlock_irqrestore(&rxq->lock, flags);
			pending = atomic_read(&ccif->pending_rx);
			continue;
		}

		ring_read(&rb->rx_control, rb->data, skb->data, len);
		skb_put(skb, len);
		spin_unlock_irqrestore(&rxq->lock, flags);

		atomic_andnot(BIT(q), &ccif->pending_rx);
		dev_info_ratelimited(ccif->dev,
				     "CCIF RX: queue=%u len=%u\n", q, len);
		*out_queue = q;
		return skb;
	}

	return NULL;
}
