// SPDX-License-Identifier: GPL-2.0-only
/*
 * DPMAIF for the op6893 CCMNI data path.
 *
 * The same-board ABI carries CCMNI over DPMAIF on MD generation 6297, not over
 * CCIF, so the data path has to be brought up on this block. Probe only maps
 * the regions and reads the clocks: every register write happens in one of the
 * explicitly operator-driven uplink or downlink windows below, and each of
 * them can be dismantled again without leaving a hardware pointer behind.
 */
#include <linux/if_arp.h>
#include <linux/ip.h>
#include <linux/ipv6.h>
#include <linux/iopoll.h>
#include <linux/netdevice.h>
#include <linux/skbuff.h>
#include <linux/bits.h>
#include <linux/clk.h>
#include <linux/device.h>
#include <linux/dma-mapping.h>
#include <linux/dmapool.h>
#include <linux/interrupt.h>
#include <linux/io.h>
#include <linux/kernel.h>
#include <linux/mfd/syscon.h>
#include <linux/mm.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/of.h>
#include <linux/of_irq.h>
#include <linux/of_platform.h>
#include <linux/platform_device.h>
#include <linux/regmap.h>
#include <linux/spinlock.h>
#include <linux/string.h>
#include <linux/sysfs.h>
#include <linux/vmalloc.h>

#include "ccci-dpmaif.h"

#define CCCI_DPMAIF_BLOCKS	5


struct ccci_dpmaif_ul {
	void *drb;
	dma_addr_t drb_dma;
	void *payload;
	void *data;
	dma_addr_t data_dma;
	u32 entries;
	u32 payload_len;
	u32 queue;
	u32 wr;
	u32 rd;
	u32 sent;
	u32 done;
	u32 irqs;
	u32 err;
	u32 len_err;
	u32 md_not_ready;
	u32 md_power_not_ready;
	u32 err_status;
	u32 not_ready;
	u32 last_status;
	/* Message-descriptor metadata the modem reads for this packet. */
	u32 network_type;
	bool started;
};

/* One packet buffer behind a BAT entry, kept separately so each has its own address. */
struct ccci_dpmaif_bat_buf {
	void *cpu;
	dma_addr_t dma;
};

/* One BAT ring and the packet buffers its entries point at. */
struct ccci_dpmaif_bat_ring {
	void *ring;
	dma_addr_t ring_dma;
	struct dma_pool *pool;
	struct ccci_dpmaif_bat_buf *buf;
	u32 entries;
	u32 buf_sz;
	u32 wr;
};

struct ccci_dpmaif_dl {
	struct ccci_dpmaif_bat_ring bat;
	struct ccci_dpmaif_bat_ring frg;
	void *pit;
	dma_addr_t pit_dma;
	u32 pit_entries;
	u32 pit_rd;
	u32 irqs;
	u32 pkts;
	u32 bytes;
	u32 msg_pits;
	u32 frag_pits;
	u32 err;
	u32 mtu_err;
	u32 overrun;
	u32 last_status;
	u32 last_len;
	u32 last_channel;
	u32 last_bid;
	u32 snap_len;
	u32 rx_channel;
	bool rx_head;
	u8 snap[CCCI_DPMAIF_DL_SNAP_LEN];
	/* Offset of the deferred register that last refused to settle, if any. */
	u32 fail_reg;
	/* The PIT ready handshake did not clear, as the vendor tolerates. */
	u32 pit_init_stuck;
	bool started;
	/* Keeps a drain from racing the teardown that frees the rings. */
	spinlock_t lock;
};

struct ccci_dpmaif {
	struct device *dev;
	void __iomem *block[CCCI_DPMAIF_BLOCKS];
	resource_size_t base[CCCI_DPMAIF_BLOCKS];
	struct regmap *infra;
	struct regmap *topckgen;
	struct clk *clk_main;
	struct clk *clk_bus;
	int irq;
	unsigned int irq_refs;
	/* Serializes the clock-on/sample/clock-off window and both rings. */
	struct mutex lock;
	struct net_device *net;
	/* Protects the TX ring and IRQ wakeups against atomic transmit. */
	spinlock_t tx_lock;
	bool net_tx;
	bool net_rx;
	bool power_blocked;
	bool reset_pending;
	u8 net_channel;
	struct ccci_dpmaif_ul ul;
	struct ccci_dpmaif_dl dl;
	/* Last result of the bounded register window. */
	u32 peek_block;
	u32 peek_offset;
	u32 peek_value;
};

/*
 * Register windows. Only two regions are described by the device tree; the
 * downlink blocks are derived from them the same way the vendor driver does.
 */
static inline void __iomem *dpmaif_ao_ul(struct ccci_dpmaif *dpmaif)
{
	return dpmaif->block[0];
}

static inline void __iomem *dpmaif_pd_ul(struct ccci_dpmaif *dpmaif)
{
	return dpmaif->block[1];
}

static inline void __iomem *dpmaif_ap_misc(struct ccci_dpmaif *dpmaif)
{
	return dpmaif->block[1] + CCCI_DPMAIF_PD_MISC_BASE;
}

static inline void __iomem *dpmaif_ao_dl(struct ccci_dpmaif *dpmaif)
{
	return dpmaif->block[0] + CCCI_DPMAIF_AO_DL_BASE;
}

static inline void __iomem *dpmaif_pd_dl(struct ccci_dpmaif *dpmaif)
{
	return dpmaif->block[1] + CCCI_DPMAIF_PD_DL_BASE;
}

static inline void __iomem *dpmaif_infra_ao_mem(struct ccci_dpmaif *dpmaif)
{
	return dpmaif->block[4];
}

static irqreturn_t dpmaif_irq(int irq, void *data);

static void dpmaif_ul_reset_counters(struct ccci_dpmaif *dpmaif)
{
	struct ccci_dpmaif_ul *ul = &dpmaif->ul;

	ul->wr = 0;
	ul->rd = 0;
	ul->sent = 0;
	ul->done = 0;
	ul->irqs = 0;
	ul->err = 0;
	ul->len_err = 0;
	ul->md_not_ready = 0;
	ul->md_power_not_ready = 0;
	ul->err_status = 0;
	ul->not_ready = 0;
	ul->last_status = 0;
}

/* Ring programming order is the same-board v2 driver's: size, base, rdy, arb. */
static void dpmaif_ul_program(struct ccci_dpmaif *dpmaif)
{
	struct ccci_dpmaif_ul *ul = &dpmaif->ul;
	void __iomem *ao = dpmaif_ao_ul(dpmaif);
	u64 base = (u64)ul->drb_dma;
	u32 value;

	value = readl(ao + CCCI_DPMAIF_UL_DRBSIZE(ul->queue));
	value &= ~CCCI_DPMAIF_DRB_SIZE_MSK;
	value |= ((ul->entries * sizeof(struct ccci_dpmaif_drb_pd)) / 4) &
		 CCCI_DPMAIF_DRB_SIZE_MSK;
	writel(value, ao + CCCI_DPMAIF_UL_DRBSIZE(ul->queue));
	writel(lower_32_bits(base), ao + CCCI_DPMAIF_UL_QSAR(ul->queue));
	value = readl(ao + CCCI_DPMAIF_UL_DRBSIZE(ul->queue));
	value &= ~CCCI_DPMAIF_DRB_ADDRH_MSK;
	value |= (upper_32_bits(base) << 24) & CCCI_DPMAIF_DRB_ADDRH_MSK;
	writel(value, ao + CCCI_DPMAIF_UL_DRBSIZE(ul->queue));

	value = readl(ao + CCCI_DPMAIF_AO_UL_CHNL_ARB0);
	value |= BIT(ul->queue);
	value |= BIT(ul->queue + 8);
	writel(value, ao + CCCI_DPMAIF_AO_UL_CHNL_ARB0);
}

static int dpmaif_ul_unprogram(struct ccci_dpmaif *dpmaif)
{
	struct ccci_dpmaif_ul *ul = &dpmaif->ul;
	void __iomem *ao = dpmaif_ao_ul(dpmaif);
	u32 value;
	int ret;

	value = readl(ao + CCCI_DPMAIF_AO_UL_CHNL_ARB0);
	value &= ~BIT(ul->queue);
	value &= ~BIT(ul->queue + 8);
	writel(value, ao + CCCI_DPMAIF_AO_UL_CHNL_ARB0);
	/* Mask every uplink event again; only this driver unmasked them. */
	writel(CCCI_DPMAIF_UL_INT_EN, ao + CCCI_DPMAIF_AO_UL_AP_L2TIMSR0);
	/*
	 * Wait for the engine to go idle before the ring is taken away. Tearing it
	 * down while the modem still holds descriptors faults the modem itself,
	 * which is why the same-board driver polls its queues before stopping.
	 */
	ret = readl_poll_timeout(dpmaif_pd_ul(dpmaif) + CCCI_DPMAIF_PD_UL_DBG_STA2,
				 value, ((value >> CCCI_DPMAIF_UL_STS_CUR_SHIFT) &
				 CCCI_DPMAIF_UL_IDLE_STS_MSK) == CCCI_DPMAIF_UL_IDLE_STS,
				 1, 10000);
	if (ret)
		return ret;
	/*
	 * Drop the ring base and size before the memory is freed: the hardware
	 * would otherwise keep DMAing from a buffer that no longer belongs to us.
	 */
	writel(0, ao + CCCI_DPMAIF_UL_QSAR(ul->queue));
	value = readl(ao + CCCI_DPMAIF_UL_DRBSIZE(ul->queue));
	value &= ~CCCI_DPMAIF_DRB_SIZE_MSK;
	value &= ~CCCI_DPMAIF_DRB_ADDRH_MSK;
	writel(value, ao + CCCI_DPMAIF_UL_DRBSIZE(ul->queue));
	/* Leave no latched uplink event behind for the next owner. */
	writel(0xffffffff, dpmaif_ap_misc(dpmaif) + CCCI_DPMAIF_PD_AP_UL_L2TISAR0);
	return 0;
}

/*
 * Clear the uplink status latches, then enable the wanted events. A set bit in
 * the mask register means masked, so enabling is clearing the mask bits.
 */
static int dpmaif_ul_irq_enable(struct ccci_dpmaif *dpmaif)
{
	void __iomem *ao = dpmaif_ao_ul(dpmaif);
	u32 count = 0;

	writel(0xffffffff, dpmaif_ap_misc(dpmaif) + CCCI_DPMAIF_PD_AP_UL_L2TISAR0);
	writel(CCCI_DPMAIF_UL_INT_EN, ao + CCCI_DPMAIF_AO_UL_AP_L2TIMCR0);
	writel(~CCCI_DPMAIF_UL_INT_EN, ao + CCCI_DPMAIF_AO_UL_AP_L2TIMSR0);
	while ((readl(ao + CCCI_DPMAIF_AO_UL_AP_L2TIMR0) &
		CCCI_DPMAIF_UL_INT_EN) == CCCI_DPMAIF_UL_INT_EN) {
		if (++count >= CCCI_DPMAIF_UL_POLL_LIMIT)
			return -ETIMEDOUT;
	}
	return 0;
}

static unsigned int dpmaif_ul_space(struct ccci_dpmaif *dpmaif)
{
	struct ccci_dpmaif_ul *ul = &dpmaif->ul;
	u32 index = readl(dpmaif_ao_ul(dpmaif) + CCCI_DPMAIF_UL_STA0(ul->queue));

	ul->rd = (index >> 16) / CCCI_DPMAIF_UL_DRB_WORDS;
	if (ul->rd >= ul->entries)
		return 0;
	return (ul->rd + ul->entries - ul->wr - 1) % ul->entries;
}

/* Returns true when this call consumed at least one uplink event. */
static bool dpmaif_ul_irq(struct ccci_dpmaif *dpmaif)
{
	struct ccci_dpmaif_ul *ul = &dpmaif->ul;
	void __iomem *misc = dpmaif_ap_misc(dpmaif);
	u32 status = 0, masked, errors;

	spin_lock(&dpmaif->tx_lock);
	if (!ul->started)
		goto out;
	status = readl(misc + CCCI_DPMAIF_PD_AP_UL_L2TISAR0);
	masked = readl(dpmaif_ao_ul(dpmaif) + CCCI_DPMAIF_AO_UL_AP_L2TIMR0);
	status &= ~masked;
	if (!status)
		goto out;
	writel(status, misc + CCCI_DPMAIF_PD_AP_UL_L2TISAR0);
	ul->irqs++;
	ul->last_status = status;
	if (status & CCCI_DPMAIF_UL_INT_DONE(ul->queue)) {
		ul->done++;
		if (dpmaif->net_tx && dpmaif_ul_space(dpmaif) >= 2)
			netif_wake_queue(dpmaif->net);
	}
	errors = status & (CCCI_DPMAIF_UL_INT_ERR_MSK | CCCI_DPMAIF_UL_INT_MD_NOTREADY_MSK |
			   CCCI_DPMAIF_UL_INT_MD_PWR_NOTREADY_MSK);
	if (errors) {
		ul->err++;
		ul->err_status |= errors;
		if (errors & CCCI_DPMAIF_UL_INT_ERR_MSK)
			ul->len_err++;
		if (errors & CCCI_DPMAIF_UL_INT_MD_NOTREADY_MSK)
			ul->md_not_ready++;
		if (errors & CCCI_DPMAIF_UL_INT_MD_PWR_NOTREADY_MSK)
			ul->md_power_not_ready++;
	}
out:
	spin_unlock(&dpmaif->tx_lock);
	return status != 0;
}

/* Caller holds tx_lock; each pair owns a distinct DMA payload until consumed. */
static int dpmaif_ul_send(struct ccci_dpmaif *dpmaif, u32 channel_id,
			  const struct sk_buff *skb)
{
	struct ccci_dpmaif_ul *ul = &dpmaif->ul;
	void __iomem *doorbell = dpmaif_pd_ul(dpmaif) + CCCI_DPMAIF_UL_ADD_DESC(ul->queue);
	struct ccci_dpmaif_drb_msg *msg;
	struct ccci_dpmaif_drb_pd *pd;
	u32 offset, size, val;
	dma_addr_t dma;

	if (!ul->started)
		return -ENOTCONN;
	if (dpmaif_ul_space(dpmaif) < 2)
		return -ENOSPC;
	if (readl_poll_timeout_atomic(doorbell, val, !(val & CCCI_DPMAIF_UL_ADD_NOT_READY),
				      0, 100)) {
		ul->not_ready++;
		return -EBUSY;
	}
	size = skb ? skb->len : ul->payload_len;
	offset = ul->wr / 2 * CCCI_DPMAIF_UL_PAYLOAD_MAX;
	dma = ul->data_dma + offset;
	if (skb) {
		if (skb_copy_bits(skb, 0, ul->data + offset, size))
			return -EINVAL;
	} else {
		memcpy(ul->data + offset, ul->payload, size);
	}

	msg = (struct ccci_dpmaif_drb_msg *)ul->drb + ul->wr;
	memset(msg, 0, sizeof(*msg));
	msg->dtyp = CCCI_DPMAIF_DES_MSG;
	msg->c_bit = 1;
	msg->packet_len = size;
	msg->channel_id = channel_id;
	msg->network_type = ul->network_type & 0x7;

	pd = (struct ccci_dpmaif_drb_pd *)ul->drb + ul->wr + 1;
	memset(pd, 0, sizeof(*pd));
	pd->dtyp = CCCI_DPMAIF_DES_PD;
	pd->data_len = size;
	pd->p_data_addr = lower_32_bits(dma);
	pd->data_addr_ext = upper_32_bits(dma) & 0xff;
	ul->wr = (ul->wr + 2) % ul->entries;

	/* writel orders the payload and descriptors before the doorbell. */
	writel((CCCI_DPMAIF_UL_DRB_WORDS * 2) | CCCI_DPMAIF_UL_ADD_UPDATE, doorbell);
	ul->sent++;
	return 0;
}

static int dpmaif_ul_send_probe(struct ccci_dpmaif *dpmaif, u32 channel)
{
	int ret;

	spin_lock_irq(&dpmaif->tx_lock);
	ret = dpmaif_ul_send(dpmaif, channel, NULL);
	spin_unlock_irq(&dpmaif->tx_lock);
	return ret;
}

/* The interrupt line is shared between the two rings and requested once. */
static int dpmaif_irq_get(struct ccci_dpmaif *dpmaif)
{
	int ret;

	if (dpmaif->irq_refs) {
		dpmaif->irq_refs++;
		return 0;
	}
	ret = request_irq(dpmaif->irq, dpmaif_irq, 0, "op6893-dpmaif", dpmaif);
	if (ret)
		return ret;
	dpmaif->irq_refs = 1;
	return 0;
}

static void dpmaif_irq_put(struct ccci_dpmaif *dpmaif)
{
	if (!dpmaif->irq_refs)
		return;
	if (--dpmaif->irq_refs)
		return;
	free_irq(dpmaif->irq, dpmaif);
}

static void dpmaif_ul_free(struct ccci_dpmaif *dpmaif)
{
	struct ccci_dpmaif_ul *ul = &dpmaif->ul;

	dma_free_coherent(dpmaif->dev, ul->entries / 2 * CCCI_DPMAIF_UL_PAYLOAD_MAX,
			  ul->data, ul->data_dma);
	ul->data = NULL;
	kfree(ul->payload);
	ul->payload = NULL;
	dma_free_coherent(dpmaif->dev, ul->entries * sizeof(struct ccci_dpmaif_drb_pd),
			  ul->drb, ul->drb_dma);
	ul->drb = NULL;
	clk_disable_unprepare(dpmaif->clk_bus);
	clk_disable_unprepare(dpmaif->clk_main);
}

static int dpmaif_ul_start(struct ccci_dpmaif *dpmaif, u32 queue)
{
	struct ccci_dpmaif_ul *ul = &dpmaif->ul;
	int ret, stop_ret;

	if (dpmaif->power_blocked)
		return -ESHUTDOWN;
	if (ul->drb)
		return -EBUSY;
	if (queue > 3)
		return -EINVAL;
	ret = clk_prepare_enable(dpmaif->clk_main);
	if (ret)
		return ret;
	ret = clk_prepare_enable(dpmaif->clk_bus);
	if (ret)
		goto out_main;
	ul->drb = dma_alloc_coherent(dpmaif->dev, ul->entries * sizeof(struct ccci_dpmaif_drb_pd),
				     &ul->drb_dma, GFP_KERNEL);
	if (!ul->drb) {
		ret = -ENOMEM;
		goto out_bus;
	}
	ul->payload = kmalloc(CCCI_DPMAIF_UL_PAYLOAD_MAX, GFP_KERNEL);
	if (!ul->payload) {
		ret = -ENOMEM;
		goto out_drb;
	}
	ul->data = dma_alloc_coherent(dpmaif->dev, ul->entries / 2 * CCCI_DPMAIF_UL_PAYLOAD_MAX,
				      &ul->data_dma, GFP_KERNEL);
	if (!ul->data) {
		ret = -ENOMEM;
		goto out_payload;
	}
	ul->queue = queue;
	dpmaif_ul_reset_counters(dpmaif);
	dpmaif->reset_pending = true;
	writel(readl(dpmaif_infra_ao_mem(dpmaif)) | CCCI_DPMAIF_INFRA_PROT,
	       dpmaif_infra_ao_mem(dpmaif));
	dpmaif_ul_program(dpmaif);
	ret = dpmaif_ul_irq_enable(dpmaif);
	if (ret)
		goto out_unprogram;
	ret = dpmaif_irq_get(dpmaif);
	if (ret)
		goto out_unprogram;
	spin_lock_irq(&dpmaif->tx_lock);
	ul->started = true;
	spin_unlock_irq(&dpmaif->tx_lock);
	return 0;

out_unprogram:
	stop_ret = dpmaif_ul_unprogram(dpmaif);
	if (stop_ret) {
		dev_err(dpmaif->dev, "UL start error %d; stop error %d, retaining DMA buffers\n",
			ret, stop_ret);
		return stop_ret;
	}
	dpmaif_ul_free(dpmaif);
	return ret;
out_payload:
	kfree(ul->payload);
	ul->payload = NULL;
out_drb:
	dma_free_coherent(dpmaif->dev, ul->entries * sizeof(struct ccci_dpmaif_drb_pd),
			  ul->drb, ul->drb_dma);
	ul->drb = NULL;
out_bus:
	clk_disable_unprepare(dpmaif->clk_bus);
out_main:
	clk_disable_unprepare(dpmaif->clk_main);
	return ret;
}

static int dpmaif_ul_stop(struct ccci_dpmaif *dpmaif)
{
	struct ccci_dpmaif_ul *ul = &dpmaif->ul;
	bool started = ul->started;
	int ret;

	if (!ul->drb)
		return 0;
	spin_lock_irq(&dpmaif->tx_lock);
	ul->started = false;
	spin_unlock_irq(&dpmaif->tx_lock);
	ret = dpmaif_ul_unprogram(dpmaif);
	if (started)
		dpmaif_irq_put(dpmaif);
	if (!ret)
		dpmaif_ul_free(dpmaif);
	return ret;
}

/*
 * Downlink. The modem fills PIT entries that name a BAT slot and a length; the
 * payload lands in the buffer that BAT slot points at. Both rings live in
 * coherent memory, so the only per-packet work is walking the PIT and handing
 * the buffers back.
 */

/* The first register that refuses to settle is the one worth reporting. */
static int dpmaif_dl_timeout(struct ccci_dpmaif *dpmaif, u32 reg)
{
	if (!dpmaif->dl.fail_reg)
		dpmaif->dl.fail_reg = reg;
	return -ETIMEDOUT;
}

/* Write a deferred-update register: poll not-ready, write, poll not-ready. */
static int dpmaif_dl_update(struct ccci_dpmaif *dpmaif, u32 reg, u32 value)
{
	void __iomem *pd = dpmaif_pd_dl(dpmaif);
	u32 poll = 0;

	while (readl(pd + reg) & CCCI_DPMAIF_DL_ADD_NOT_READY) {
		if (++poll >= CCCI_DPMAIF_DL_POLL_LIMIT)
			return dpmaif_dl_timeout(dpmaif, reg);
	}
	writel(value, pd + reg);
	poll = 0;
	while (readl(pd + reg) & CCCI_DPMAIF_DL_ADD_NOT_READY) {
		if (++poll >= CCCI_DPMAIF_DL_POLL_LIMIT)
			return dpmaif_dl_timeout(dpmaif, reg);
	}
	return 0;
}

/*
 * Keep the software buffer array indexed by the published BAT slot. During
 * refill, move the consumed buffer to the producer slot and keep the old
 * producer buffer in the consumed slot for later use.
 */
static void dpmaif_dl_publish(struct ccci_dpmaif_bat_ring *ring, u32 bid)
{
	struct ccci_dpmaif_bat *entry = (struct ccci_dpmaif_bat *)ring->ring + ring->wr;
	u64 addr = (u64)ring->buf[bid].dma;

	if (bid != ring->wr)
		swap(ring->buf[bid], ring->buf[ring->wr]);
	entry->buffer_addr_ext = upper_32_bits(addr) & 0xff;
	entry->p_buffer_addr = lower_32_bits(addr);
	ring->wr = (ring->wr + 1) % ring->entries;
}

static int dpmaif_dl_bat_program(struct ccci_dpmaif *dpmaif,
				 struct ccci_dpmaif_bat_ring *ring, bool frag)
{
	void __iomem *pd = dpmaif_pd_dl(dpmaif);
	u64 base = (u64)ring->ring_dma;
	u32 value;

	writel(lower_32_bits(base), pd + CCCI_DPMAIF_PD_DL_BAT_INIT_CON0);
	value = readl(pd + CCCI_DPMAIF_PD_DL_BAT_INIT_CON1);
	value &= ~CCCI_DPMAIF_BAT_ADDRH_MSK;
	value |= (upper_32_bits(base) << 24) & CCCI_DPMAIF_BAT_ADDRH_MSK;
	value &= ~CCCI_DPMAIF_BAT_SIZE_MSK;
	value |= ring->entries & CCCI_DPMAIF_BAT_SIZE_MSK;
	value &= ~CCCI_DPMAIF_BAT_EN_MSK;
	writel(value, pd + CCCI_DPMAIF_PD_DL_BAT_INIT_CON1);
	/*
	 * The packet and fragment rings share CON0/CON1 but latch separately: the
	 * same-board driver distinguishes them with BAT_INIT_FRG_INIT and always
	 * sets ALLSET, so the fragment ring needs its own handshake.
	 */
	return dpmaif_dl_update(dpmaif, CCCI_DPMAIF_PD_DL_BAT_INIT,
				CCCI_DPMAIF_DL_BAT_INIT_ALLSET | CCCI_DPMAIF_DL_BAT_INIT_EN |
				(frag ? CCCI_DPMAIF_DL_BAT_FRG_INIT : 0));
}

static int dpmaif_dl_pit_program(struct ccci_dpmaif *dpmaif)
{
	struct ccci_dpmaif_dl *dl = &dpmaif->dl;
	void __iomem *pd = dpmaif_pd_dl(dpmaif);
	u64 base = (u64)dl->pit_dma;
	u32 value;

	writel(lower_32_bits(base), pd + CCCI_DPMAIF_PD_DL_PIT_INIT_CON0);
	value = readl(pd + CCCI_DPMAIF_PD_DL_PIT_INIT_CON1);
	value &= ~CCCI_DPMAIF_PIT_ADDRH_MSK;
	value |= (upper_32_bits(base) << 24) & CCCI_DPMAIF_PIT_ADDRH_MSK;
	value &= ~CCCI_DPMAIF_PIT_SIZE_MSK;
	value |= dl->pit_entries & CCCI_DPMAIF_PIT_SIZE_MSK;
	writel(value, pd + CCCI_DPMAIF_PD_DL_PIT_INIT_CON1);
	value = readl(pd + CCCI_DPMAIF_PD_DL_PIT_INIT_CON3);
	value |= CCCI_DPMAIF_PIT_EN_MSK;
	writel(value, pd + CCCI_DPMAIF_PD_DL_PIT_INIT_CON3);
	/*
	 * Both vendor drivers declare this handshake void and merely log when the
	 * ready bit does not clear, then go on to add entries. Treating it as fatal
	 * would stop the ring where the same-board driver keeps going, so record
	 * the state and let the caller decide from the counters.
	 */
	dl->pit_init_stuck = dpmaif_dl_update(dpmaif, CCCI_DPMAIF_PD_DL_PIT_INIT,
					      CCCI_DPMAIF_DL_PIT_INIT_ALLSET |
					      CCCI_DPMAIF_DL_PIT_INIT_EN) ? 1 : 0;
	return 0;
}

static int dpmaif_dl_add_bat(struct ccci_dpmaif *dpmaif, u32 cnt)
{
	u32 value = (cnt & CCCI_DPMAIF_BAT_SIZE_MSK) | CCCI_DPMAIF_DL_ADD_UPDATE;

	return dpmaif_dl_update(dpmaif, CCCI_DPMAIF_PD_DL_BAT_ADD, value);
}

static int dpmaif_dl_add_frg_bat(struct ccci_dpmaif *dpmaif, u32 cnt)
{
	u32 value = (cnt & CCCI_DPMAIF_BAT_SIZE_MSK) | CCCI_DPMAIF_DL_ADD_UPDATE |
		    CCCI_DPMAIF_DL_BAT_FRG_ADD;

	return dpmaif_dl_update(dpmaif, CCCI_DPMAIF_PD_DL_BAT_ADD, value);
}

/*
 * Open the packet BAT queue. The same-board driver clears the enable bit while
 * it programs the ring, publishes the buffers, and only then enables the queue
 * with an enable-only update, so the modem cannot take a buffer before one has
 * been published.
 */
static int dpmaif_dl_queue_enable(struct ccci_dpmaif *dpmaif, bool enable, bool frag)
{
	void __iomem *pd = dpmaif_pd_dl(dpmaif);
	u32 value = readl(pd + CCCI_DPMAIF_PD_DL_BAT_INIT_CON1);

	if (enable)
		value |= CCCI_DPMAIF_BAT_EN_MSK;
	else
		value &= ~CCCI_DPMAIF_BAT_EN_MSK;
	writel(value, pd + CCCI_DPMAIF_PD_DL_BAT_INIT_CON1);
	return dpmaif_dl_update(dpmaif, CCCI_DPMAIF_PD_DL_BAT_INIT,
				CCCI_DPMAIF_DL_BAT_INIT_EN |
				(frag ? CCCI_DPMAIF_DL_BAT_FRG_INIT : 0));
}

static int dpmaif_dl_add_pit(struct ccci_dpmaif *dpmaif, u32 cnt)
{
	u32 value = (cnt & CCCI_DPMAIF_PIT_SIZE_MSK) | CCCI_DPMAIF_DL_ADD_UPDATE;

	return dpmaif_dl_update(dpmaif, CCCI_DPMAIF_PD_DL_PIT_ADD, value);
}

/*
 * Bring the downlink engine out of the state the bootloader left it in. The
 * vendor does this once per power-on: ungated AP-side clocks, the write-DMA
 * outstanding limit, the level-1 timer mask, and the power flag the ring
 * programming needs before any deferred register will settle.
 */
static void dpmaif_dl_hw_prepare(struct ccci_dpmaif *dpmaif)
{
	void __iomem *wdma = dpmaif->block[1] + CCCI_DPMAIF_PD_WDMA_BASE;
	u32 value;

	/*
	 * The bootloader leaves the DPMAIF protection bit in INFRA_AO_MEM
	 * clear; without it the engine will not settle a ring that lives in
	 * AP DRAM.
	 */
	value = readl(dpmaif_infra_ao_mem(dpmaif));
	writel(value | CCCI_DPMAIF_INFRA_PROT, dpmaif_infra_ao_mem(dpmaif));
	writel(0x7f, dpmaif_ap_misc(dpmaif) + CCCI_DPMAIF_PD_AP_CG_EN);
	value = readl(wdma + CCCI_DPMAIF_PD_WDMA_CHNL_CMD_CON3);
	value &= ~(CCCI_DPMAIF_WDMA_CTRL_OSTD_MSK << CCCI_DPMAIF_WDMA_CTRL_OSTD_OFST);
	value |= (CCCI_DPMAIF_WDMA_CTRL_OSTD_VALUE << CCCI_DPMAIF_WDMA_CTRL_OSTD_OFST);
	writel(value, wdma + CCCI_DPMAIF_PD_WDMA_CHNL_CMD_CON3);
	/* Keep the top half and clear the interval fields below it. */
	value = readl(wdma + CCCI_DPMAIF_PD_WDMA_WR_CMD_CON0) & 0xffff0000;
	writel(value, wdma + CCCI_DPMAIF_PD_WDMA_WR_CMD_CON0);
	writel(CCCI_DPMAIF_AO_UL_L1TIMR0_VAL,
	       dpmaif_ao_ul(dpmaif) + CCCI_DPMAIF_AO_UL_AP_L1TIMR0);
	writel(CCCI_DPMAIF_DL_POWER_ON,
	       dpmaif_pd_dl(dpmaif) + CCCI_DPMAIF_PD_DL_RESERVE_RW);
}

/* AO-side packet limits, thresholds and alignment, as the v2 driver sets them. */
static void dpmaif_dl_tune(struct ccci_dpmaif *dpmaif)
{
	void __iomem *ao = dpmaif_ao_dl(dpmaif);
	void __iomem *pd = dpmaif_pd_dl(dpmaif);
	u32 value;

	value = readl(ao + CCCI_DPMAIF_AO_DL_PKTINFO_CON0);
	value &= ~CCCI_DPMAIF_BAT_BID_MAXCNT_MSK;
	value |= (CCCI_DPMAIF_DL_PKT_BIDCNT << 16) & CCCI_DPMAIF_BAT_BID_MAXCNT_MSK;
	value &= ~CCCI_DPMAIF_BAT_REMAIN_MINSZ_MSK;
	value |= ((CCCI_DPMAIF_DL_BAT_REMAIN / CCCI_DPMAIF_BAT_REMAIN_SZ_BASE) << 8) &
		 CCCI_DPMAIF_BAT_REMAIN_MINSZ_MSK;
	value &= ~CCCI_DPMAIF_CHK_RB_PITNUM_MSK;
	value |= CCCI_DPMAIF_DL_HW_CHK_RB_PIT_NUM & CCCI_DPMAIF_CHK_RB_PITNUM_MSK;
	writel(value, ao + CCCI_DPMAIF_AO_DL_PKTINFO_CON0);
	/* One BAT buffer per packet, so the MTU is the vendor's, not the buffer. */
	writel(CCCI_DPMAIF_DL_MTU_SIZE, ao + CCCI_DPMAIF_AO_DL_PKTINFO_CON1);

	value = readl(ao + CCCI_DPMAIF_AO_DL_PKTINFO_CON2);
	value &= ~CCCI_DPMAIF_PIT_CHK_NUM_MSK;
	value |= (CCCI_DPMAIF_DL_HW_CHK_PIT_NUM << 24) & CCCI_DPMAIF_PIT_CHK_NUM_MSK;
	value &= ~CCCI_DPMAIF_BAT_BUF_SZ_MSK;
	value |= ((CCCI_DPMAIF_DL_BAT_BUF_SZ / CCCI_DPMAIF_BAT_BUF_SZ_BASE) << 8) &
		 CCCI_DPMAIF_BAT_BUF_SZ_MSK;
	value &= ~CCCI_DPMAIF_BAT_RSV_LEN_MSK;
	writel(value, ao + CCCI_DPMAIF_AO_DL_PKTINFO_CON2);

	value = readl(ao + CCCI_DPMAIF_AO_DL_RDY_CHK_THRES);
	value |= CCCI_DPMAIF_CHKSUM_ON_MSK;
	/* Clearing the alignment mask also clears its enable bit; set it after. */
	value &= ~CCCI_DPMAIF_PKT_ALIGN_MSK;
	value |= CCCI_DPMAIF_PKT_ALIGN_EN;
	value |= BIT(13);
	value &= ~CCCI_DPMAIF_BAT_CHECK_THRES_MSK;
	value |= (CCCI_DPMAIF_DL_HW_CHK_BAT_NUM << 16) & CCCI_DPMAIF_BAT_CHECK_THRES_MSK;
	writel(value, ao + CCCI_DPMAIF_AO_DL_RDY_CHK_THRES);

	/*
	 * The fragment ring gets its own thresholds. The same-board driver always
	 * brings one up, and turns the feature on rather than off.
	 */
	value = readl(ao + CCCI_DPMAIF_AO_DL_RDY_CHK_FRG_THRES);
	value &= ~CCCI_DPMAIF_FRG_BAT_BUF_FEATURE_ON_MSK;
	value |= CCCI_DPMAIF_FRG_BAT_BUF_FEATURE_ON_MSK;
	value &= ~CCCI_DPMAIF_FRG_BAT_BUF_SZ_MSK;
	value |= ((CCCI_DPMAIF_DL_FRG_BUF_SZ / CCCI_DPMAIF_BAT_BUF_SZ_BASE) << 8) &
		 CCCI_DPMAIF_FRG_BAT_BUF_SZ_MSK;
	value &= ~CCCI_DPMAIF_FRG_CHECK_THRES_MSK;
	value |= CCCI_DPMAIF_DL_HW_CHK_FRG_NUM & CCCI_DPMAIF_FRG_CHECK_THRES_MSK;
	writel(value, ao + CCCI_DPMAIF_AO_DL_RDY_CHK_FRG_THRES);

	/* BAT cache in PD_DL_BAT_INIT_CON1 bit 22. */
	value = readl(pd + CCCI_DPMAIF_PD_DL_BAT_INIT_CON1);
	value |= BIT(22);
	writel(value, pd + CCCI_DPMAIF_PD_DL_BAT_INIT_CON1);

	/*
	 * Tell the modem where the AP starts reading the shared PIT ring, both in
	 * the ring's own index field and through the notification word it polls.
	 */
	value = readl(pd + CCCI_DPMAIF_PD_DL_PIT_INIT_CON3);
	value &= ~(CCCI_DPMAIF_DL_PIT_WRIDX_MSK << 16);
	value |= (CCCI_DPMAIF_DUMMY_PIT_AIDX & CCCI_DPMAIF_DL_PIT_WRIDX_MSK) << 16;
	writel(value, pd + CCCI_DPMAIF_PD_DL_PIT_INIT_CON3);
	writel(CCCI_DPMAIF_DUMMY_PIT_AIDX | CCCI_DPMAIF_MD_DUMMYPIT_EN,
	       dpmaif_pd_ul(dpmaif) + CCCI_DPMAIF_UL_RESERVE_RW);
	/*
	 * Stop here, exactly where the same-board startup path stops. Its
	 * notification word and UL_RESERVE_AO_RW are written only when it adds
	 * AP PIT entries mid-run, so writing them now would tell the modem about
	 * an update that did not happen, which faults it.
	 */
}

/*
 * Both the AO mask field and the AP_MISC mask register carry the downlink
 * events; the vendor sets them to the same value, so do the same here.
 */
static void dpmaif_dl_irq_enable(struct ccci_dpmaif *dpmaif)
{
	void __iomem *ao = dpmaif_ao_ul(dpmaif);
	void __iomem *misc = dpmaif_ap_misc(dpmaif);
	void __iomem *ao_dl = dpmaif_ao_dl(dpmaif);
	u32 value;

	writel(0xffffffff, misc + CCCI_DPMAIF_PD_AP_DL_L2TISAR0);
	writel(0xffffffff, misc + CCCI_DPMAIF_PD_AP_IP_BUSY);
	writel(CCCI_DPMAIF_DL_INT_EN, ao + CCCI_DPMAIF_AO_UL_APDL_L2TIMCR0);
	writel((u32)~CCCI_DPMAIF_DL_INT_EN, ao + CCCI_DPMAIF_AO_UL_APDL_L2TIMSR0);
	value = readl(ao_dl + CCCI_DPMAIF_AO_DL_RDY_CHK_THRES);
	value &= ~CCCI_DPMAIF_AO_DL_ISR_MSK;
	value |= (~CCCI_DPMAIF_DL_INT_EN) & CCCI_DPMAIF_AO_DL_ISR_MSK;
	writel(value, ao_dl + CCCI_DPMAIF_AO_DL_RDY_CHK_THRES);
}

static void dpmaif_dl_irq_mask(struct ccci_dpmaif *dpmaif)
{
	void __iomem *ao_dl = dpmaif_ao_dl(dpmaif);
	u32 value;

	writel(CCCI_DPMAIF_AO_DL_ISR_MSK,
	       dpmaif_ao_ul(dpmaif) + CCCI_DPMAIF_AO_UL_APDL_L2TIMSR0);
	value = readl(ao_dl + CCCI_DPMAIF_AO_DL_RDY_CHK_THRES);
	value |= CCCI_DPMAIF_AO_DL_ISR_MSK;
	writel(value, ao_dl + CCCI_DPMAIF_AO_DL_RDY_CHK_THRES);
	writel(0xffffffff, dpmaif_ap_misc(dpmaif) + CCCI_DPMAIF_PD_AP_DL_L2TISAR0);
}

static void dpmaif_net_receive(struct net_device *net, const void *data, u32 len)
{
	const u8 *ip = data;
	struct sk_buff *skb;
	__be16 protocol;

	if (len >= sizeof(struct iphdr) && ip[0] >> 4 == 4) {
		protocol = htons(ETH_P_IP);
	} else if (len >= sizeof(struct ipv6hdr) && ip[0] >> 4 == 6) {
		protocol = htons(ETH_P_IPV6);
	} else {
		net->stats.rx_dropped++;
		return;
	}

	skb = netdev_alloc_skb_ip_align(net, len);
	if (!skb) {
		net->stats.rx_dropped++;
		return;
	}
	skb_put_data(skb, data, len);
	skb->protocol = protocol;
	net->stats.rx_packets++;
	net->stats.rx_bytes += len;
	netif_rx(skb);
}

/*
 * Walk every PIT entry the modem has published and hand the buffers back. The
 * When the raw IP interface is up, deliver complete single-buffer packets.
 */
static void dpmaif_dl_drain(struct ccci_dpmaif *dpmaif)
{
	struct ccci_dpmaif_dl *dl = &dpmaif->dl;
	u32 widx, used, i;
	u32 bat_add = 0, frag_add = 0, pit_add = 0;
	u32 channel = 0, cur_len = 0;
	bool last = false;
	u32 last_len = 0;

	widx = readl(dpmaif_ao_dl(dpmaif) + CCCI_DPMAIF_AO_DL_PIT_STA2) &
	       CCCI_DPMAIF_DL_PIT_WRIDX_MSK;
	if (widx >= dl->pit_entries || dl->pit_rd >= dl->pit_entries) {
		dl->overrun++;
		return;
	}
	dma_rmb();
	used = (widx >= dl->pit_rd) ? widx - dl->pit_rd :
	       dl->pit_entries - dl->pit_rd + widx;

	for (i = 0; i < used; i++) {
		struct ccci_dpmaif_pit *pit = (struct ccci_dpmaif_pit *)dl->pit + dl->pit_rd;

		if (pit->packet_type == CCCI_DPMAIF_DES_MSG) {
			struct ccci_dpmaif_msg_pit *msg = (struct ccci_dpmaif_msg_pit *)pit;

			dl->msg_pits++;
			dl->rx_channel = msg->channel_id;
			dl->rx_head = !msg->error_bit && !msg->dp;
			goto next;
		}
		if (pit->ig)
			goto next;

		if (pit->buffer_type) {
			dl->frag_pits++;
			dl->rx_head = false;
			if (pit->buffer_id >= dl->frg.entries) {
				dl->overrun++;
			} else {
				dpmaif_dl_publish(&dl->frg, pit->buffer_id);
				frag_add++;
			}
		} else {
			dl->last_bid = pit->buffer_id;
			if (pit->buffer_id >= dl->bat.entries) {
				dl->overrun++;
			} else {
				const void *data = dl->bat.buf[pit->buffer_id].cpu;
				u32 len = pit->data_len;

				if (len && len <= dl->bat.buf_sz) {
					if (!cur_len) {
						u32 snap = min_t(u32, len, sizeof(dl->snap));

						dl->snap_len = snap;
						memcpy(dl->snap, data, dl->snap_len);
					}
					if (dpmaif->net_rx && dl->rx_head && !pit->c_bit &&
					    dl->rx_channel == dpmaif->net_channel &&
					    len <= ETH_DATA_LEN)
						dpmaif_net_receive(dpmaif->net, data, len);
				}
				dpmaif_dl_publish(&dl->bat, pit->buffer_id);
				bat_add++;
			}
			dl->rx_head = false;
		}
		cur_len += pit->data_len;
		channel = dl->rx_channel;
		/* A cleared c_bit marks the last entry of one packet. */
		if (!pit->c_bit) {
			dl->pkts++;
			dl->bytes += cur_len;
			last_len = cur_len;
			cur_len = 0;
			last = true;
		}
next:
		dl->pit_rd = (dl->pit_rd + 1) % dl->pit_entries;
		pit_add++;
	}
	if (!pit_add)
		return;
	if (last) {
		dl->last_len = last_len;
		dl->last_channel = channel;
	}
	if (bat_add && dpmaif_dl_add_bat(dpmaif, bat_add))
		dl->err++;
	if (frag_add && dpmaif_dl_add_frg_bat(dpmaif, frag_add))
		dl->err++;
	if (dpmaif_dl_add_pit(dpmaif, pit_add))
		dl->err++;
}

static bool dpmaif_dl_irq(struct ccci_dpmaif *dpmaif)
{
	struct ccci_dpmaif_dl *dl = &dpmaif->dl;
	void __iomem *misc = dpmaif_ap_misc(dpmaif);
	u32 status, masked;
	bool handled = false;

	spin_lock(&dl->lock);
	if (!dl->started) {
		spin_unlock(&dl->lock);
		return false;
	}
	/* The modem raises this one when it wants the AP awake. */
	if (readl(misc + CCCI_DPMAIF_PD_AP_IP_BUSY)) {
		writel(0xffffffff, misc + CCCI_DPMAIF_PD_AP_IP_BUSY);
		handled = true;
	}
	status = readl(misc + CCCI_DPMAIF_PD_AP_DL_L2TISAR0);
	masked = readl(dpmaif_ao_ul(dpmaif) + CCCI_DPMAIF_AO_UL_APDL_L2TIMR0);
	status &= ~masked;
	if (status) {
		writel(status, misc + CCCI_DPMAIF_PD_AP_DL_L2TISAR0);
		dl->irqs++;
		dl->last_status = status;
		if (status & CCCI_DPMAIF_DL_INT_MTU_ERR)
			dl->mtu_err++;
		if (status & ~CCCI_DPMAIF_DL_INT_QDONE)
			dl->err++;
		dpmaif_dl_drain(dpmaif);
		handled = true;
	}
	spin_unlock(&dl->lock);
	return handled;
}

static irqreturn_t dpmaif_irq(int irq, void *data)
{
	struct ccci_dpmaif *dpmaif = data;
	bool handled = false;

	if (dpmaif_ul_irq(dpmaif))
		handled = true;
	if (dpmaif_dl_irq(dpmaif))
		handled = true;
	return handled ? IRQ_HANDLED : IRQ_NONE;
}

static int dpmaif_dl_ring_alloc(struct ccci_dpmaif *dpmaif,
				struct ccci_dpmaif_bat_ring *ring,
				u32 entries, u32 buf_sz)
{
	u32 i;
	int ret;

	ring->entries = entries;
	ring->buf_sz = buf_sz;
	ring->wr = 0;
	ring->ring = dma_alloc_coherent(dpmaif->dev,
					entries * sizeof(struct ccci_dpmaif_bat),
					&ring->ring_dma, GFP_KERNEL);
	if (!ring->ring)
		return -ENOMEM;
	/*
	 * One buffer per entry rather than one flat pool. A pool sized for the
	 * same-board geometry is a multi-megabyte contiguous allocation, which
	 * this platform refuses outright, and small buffers also make each entry
	 * an address of its own instead of an offset into one block.
	 */
	ring->pool = dma_pool_create(dev_name(dpmaif->dev), dpmaif->dev, buf_sz,
				     0, 0);
	if (!ring->pool) {
		ret = -ENOMEM;
		goto out_ring;
	}
	ring->buf = kvmalloc_array(entries, sizeof(*ring->buf), GFP_KERNEL);
	if (!ring->buf) {
		ret = -ENOMEM;
		goto out_pool;
	}
	for (i = 0; i < entries; i++) {
		ring->buf[i].cpu = dma_pool_alloc(ring->pool, GFP_KERNEL,
						  &ring->buf[i].dma);
		if (!ring->buf[i].cpu) {
			ret = -ENOMEM;
			goto out_bufs;
		}
	}
	return 0;

out_bufs:
	while (i--)
		dma_pool_free(ring->pool, ring->buf[i].cpu, ring->buf[i].dma);
	kvfree(ring->buf);
	ring->buf = NULL;
out_pool:
	dma_pool_destroy(ring->pool);
	ring->pool = NULL;
out_ring:
	dma_free_coherent(dpmaif->dev, entries * sizeof(struct ccci_dpmaif_bat),
			  ring->ring, ring->ring_dma);
	ring->ring = NULL;
	return ret;
}

static void dpmaif_dl_ring_free(struct ccci_dpmaif *dpmaif,
				struct ccci_dpmaif_bat_ring *ring)
{
	u32 i;

	if (ring->buf) {
		for (i = 0; i < ring->entries; i++)
			if (ring->buf[i].cpu)
				dma_pool_free(ring->pool, ring->buf[i].cpu,
					      ring->buf[i].dma);
		kvfree(ring->buf);
		ring->buf = NULL;
	}
	/* NULL is safe here and the buffers above are the only other user. */
	dma_pool_destroy(ring->pool);
	ring->pool = NULL;
	if (ring->ring)
		dma_free_coherent(dpmaif->dev,
				  ring->entries * sizeof(struct ccci_dpmaif_bat),
				  ring->ring, ring->ring_dma);
	ring->ring = NULL;
}

/* Drop every hardware pointer before the rings are freed. */
static void dpmaif_dl_log_clocks(struct ccci_dpmaif *dpmaif, const char *phase)
{
	u32 bus = U32_MAX, main = U32_MAX, mux = U32_MAX, mode = U32_MAX;

	/* Read IFRAO3/4 gate status, not the observer's estimated clock rates. */
	regmap_read(dpmaif->infra, 0xac, &bus);
	regmap_read(dpmaif->infra, 0xc8, &main);
	if (!IS_ERR(dpmaif->topckgen)) {
		regmap_read(dpmaif->topckgen, 0xe0, &mux);
		regmap_read(dpmaif->topckgen, 0, &mode);
	}
	dev_info(dpmaif->dev, "DL %s: ifrao3=%#x ifrao4=%#x clk_cfg13=%#x clk_mode=%#x\n",
		 phase, bus, main, mux, mode);
}

static int dpmaif_dl_unprogram(struct ccci_dpmaif *dpmaif)
{
	struct ccci_dpmaif_dl *dl = &dpmaif->dl;
	void __iomem *pd = dpmaif_pd_dl(dpmaif);
	u32 value;
	int ret;

	value = readl(pd + CCCI_DPMAIF_PD_DL_PIT_INIT_CON3);
	value &= ~CCCI_DPMAIF_PIT_EN_MSK;
	writel(value, pd + CCCI_DPMAIF_PD_DL_PIT_INIT_CON3);
	ret = dpmaif_dl_update(dpmaif, CCCI_DPMAIF_PD_DL_PIT_INIT, CCCI_DPMAIF_DL_PIT_INIT_EN);
	if (ret)
		return ret;
	ret = readl_poll_timeout(dpmaif_ao_dl(dpmaif) + CCCI_DPMAIF_AO_DL_PIT_STA3,
				 value, !(value & CCCI_DPMAIF_PIT_EN_MSK), 1, 10000);
	if (ret)
		return dpmaif_dl_timeout(dpmaif, CCCI_DPMAIF_AO_DL_PIT_STA3);
	ret = readl_poll_timeout(pd + CCCI_DPMAIF_PD_DL_DBG_STA1,
				 value, value & CCCI_DPMAIF_DL_IDLE, 1, 10000);
	if (ret)
		return dpmaif_dl_timeout(dpmaif, CCCI_DPMAIF_PD_DL_DBG_STA1);
	ret = dpmaif_dl_queue_enable(dpmaif, false, true);
	if (!ret)
		ret = dpmaif_dl_queue_enable(dpmaif, false, false);
	if (ret)
		return ret;

	writel(0, pd + CCCI_DPMAIF_PD_DL_PIT_INIT_CON0);
	value = readl(pd + CCCI_DPMAIF_PD_DL_PIT_INIT_CON1);
	value &= ~CCCI_DPMAIF_PIT_SIZE_MSK;
	value &= ~CCCI_DPMAIF_PIT_ADDRH_MSK;
	writel(value, pd + CCCI_DPMAIF_PD_DL_PIT_INIT_CON1);
	writel(0, pd + CCCI_DPMAIF_PD_DL_BAT_INIT_CON0);
	value = readl(pd + CCCI_DPMAIF_PD_DL_BAT_INIT_CON1);
	value &= ~CCCI_DPMAIF_BAT_SIZE_MSK;
	value &= ~CCCI_DPMAIF_BAT_ADDRH_MSK;
	writel(value, pd + CCCI_DPMAIF_PD_DL_BAT_INIT_CON1);
	writel(0xffffffff, dpmaif_ap_misc(dpmaif) + CCCI_DPMAIF_PD_AP_IP_BUSY);
	dl->pit_rd = 0;
	return 0;
}

static int dpmaif_dl_start(struct ccci_dpmaif *dpmaif, u32 bat_entries,
			   u32 pit_entries, bool before_power)
{
	struct ccci_dpmaif_dl *dl = &dpmaif->dl;
	void __iomem *ao_dl = dpmaif_ao_dl(dpmaif);
	u32 i;
	int ret, stop_ret;

	if (dpmaif->power_blocked != before_power)
		return -ESHUTDOWN;
	/* An unconfirmed stop still owns these DMA mappings and clocks. */
	if (dl->pit)
		return -EBUSY;
	/* Keep one BAT slot empty and the initial PIT AP index within the ring. */
	if (bat_entries <= CCCI_DPMAIF_DL_HW_CHK_BAT_NUM + 1 ||
	    pit_entries <= CCCI_DPMAIF_DUMMY_PIT_AIDX)
		return -EINVAL;
	if (bat_entries > CCCI_DPMAIF_BAT_SIZE_MSK || pit_entries > CCCI_DPMAIF_PIT_SIZE_MSK)
		return -EINVAL;
	ret = clk_prepare_enable(dpmaif->clk_main);
	if (ret)
		return ret;
	ret = clk_prepare_enable(dpmaif->clk_bus);
	if (ret)
		goto out_main;
	ret = dpmaif_dl_ring_alloc(dpmaif, &dl->bat, bat_entries,
				   CCCI_DPMAIF_DL_BAT_BUF_SZ);
	if (ret)
		goto out_bus;
	ret = dpmaif_dl_ring_alloc(dpmaif, &dl->frg, bat_entries,
				   CCCI_DPMAIF_DL_FRG_BUF_SZ);
	if (ret)
		goto out_bat;
	dl->pit_entries = pit_entries;
	dl->pit = dma_alloc_coherent(dpmaif->dev,
				     (size_t)pit_entries * CCCI_DPMAIF_DL_PIT_BYTE_SIZE,
				     &dl->pit_dma, GFP_KERNEL);
	if (!dl->pit) {
		ret = -ENOMEM;
		goto out_frg;
	}

	memset(dl->pit, 0, (size_t)pit_entries * CCCI_DPMAIF_DL_PIT_BYTE_SIZE);
	for (i = 0; i < dl->bat.entries - 1; i++)
		dpmaif_dl_publish(&dl->bat, i);
	for (i = 0; i < dl->frg.entries - 1; i++)
		dpmaif_dl_publish(&dl->frg, i);
	dl->fail_reg = 0;
	dpmaif->reset_pending = true;
	dpmaif_dl_hw_prepare(dpmaif);
	dpmaif_dl_tune(dpmaif);
	dpmaif_dl_log_clocks(dpmaif, "before BAT");
	ret = dpmaif_dl_bat_program(dpmaif, &dl->bat, false);
	if (ret)
		goto out_unprogram;
	ret = dpmaif_dl_bat_program(dpmaif, &dl->frg, true);
	if (ret)
		goto out_unprogram;
	ret = dpmaif_dl_pit_program(dpmaif);
	if (ret)
		goto out_unprogram;
	/*
	 * Start at the hardware read index. The AP availability index was
	 * programmed separately and must not be used as a producer index.
	 */
	dl->pit_rd = (readl(ao_dl + CCCI_DPMAIF_AO_DL_PIT_STA2) >> 16) &
		     CCCI_DPMAIF_DL_PIT_WRIDX_MSK;
	ret = dpmaif_dl_add_frg_bat(dpmaif, dl->frg.entries - 1);
	if (ret)
		goto out_unprogram;
	ret = dpmaif_dl_add_bat(dpmaif, dl->bat.entries - 1);
	if (ret)
		goto out_unprogram;
	ret = dpmaif_dl_queue_enable(dpmaif, true, true);
	if (ret)
		goto out_unprogram;
	ret = dpmaif_dl_queue_enable(dpmaif, true, false);
	if (ret)
		goto out_unprogram;
	spin_lock_irq(&dl->lock);
	dl->started = true;
	spin_unlock_irq(&dl->lock);
	dpmaif_dl_irq_enable(dpmaif);
	ret = dpmaif_irq_get(dpmaif);
	if (ret) {
		spin_lock_irq(&dl->lock);
		dl->started = false;
		spin_unlock_irq(&dl->lock);
		goto out_unprogram;
	}
	return 0;

out_unprogram:
	/* Cleanup submits new commands, which can hide the startup failure. */
	dpmaif_dl_log_clocks(dpmaif, "before cleanup");
	dev_err_ratelimited(dpmaif->dev,
			    "DL startup failed: err=%d reg=%#x BAT_INIT=%#x PIT_INIT=%#x\n",
			    ret, dl->fail_reg,
			    readl(dpmaif_pd_dl(dpmaif) + CCCI_DPMAIF_PD_DL_BAT_INIT),
			    readl(dpmaif_pd_dl(dpmaif) + CCCI_DPMAIF_PD_DL_PIT_INIT));
	dpmaif_dl_irq_mask(dpmaif);
	stop_ret = dpmaif_dl_unprogram(dpmaif);
	if (stop_ret) {
		dev_err(dpmaif->dev, "DL start error %d; stop error %d, retaining DMA buffers\n",
			ret, stop_ret);
		return stop_ret;
	}
	dma_free_coherent(dpmaif->dev, (size_t)pit_entries * CCCI_DPMAIF_DL_PIT_BYTE_SIZE,
			  dl->pit, dl->pit_dma);
	dl->pit = NULL;
out_frg:
	dpmaif_dl_ring_free(dpmaif, &dl->frg);
out_bat:
	dpmaif_dl_ring_free(dpmaif, &dl->bat);
out_bus:
	clk_disable_unprepare(dpmaif->clk_bus);
out_main:
	clk_disable_unprepare(dpmaif->clk_main);
	return ret;
}

static int dpmaif_dl_stop(struct ccci_dpmaif *dpmaif)
{
	struct ccci_dpmaif_dl *dl = &dpmaif->dl;
	u32 pit_entries = dl->pit_entries;
	bool started = dl->started;
	int ret;

	if (!dl->pit)
		return 0;
	/* Mask first, then take the lock: a drain must not outlive the rings. */
	dpmaif_dl_irq_mask(dpmaif);
	spin_lock_irq(&dl->lock);
	dl->started = false;
	dl->rx_head = false;
	spin_unlock_irq(&dl->lock);
	if (started)
		dpmaif_irq_put(dpmaif);
	dl->fail_reg = 0;
	ret = dpmaif_dl_unprogram(dpmaif);
	if (ret)
		return ret;
	dma_free_coherent(dpmaif->dev, (size_t)pit_entries * CCCI_DPMAIF_DL_PIT_BYTE_SIZE,
			  dl->pit, dl->pit_dma);
	dl->pit = NULL;
	dpmaif_dl_ring_free(dpmaif, &dl->frg);
	dpmaif_dl_ring_free(dpmaif, &dl->bat);
	clk_disable_unprepare(dpmaif->clk_bus);
	clk_disable_unprepare(dpmaif->clk_main);
	return 0;
}

static void dpmaif_put_device(void *data)
{
	put_device(data);
}

struct ccci_dpmaif *ccci_dpmaif_find(struct device *consumer)
{
	struct platform_device *pdev;
	struct device_node *node;
	struct ccci_dpmaif *dpmaif;
	int ret;

	if (!of_property_present(consumer->of_node, "mediatek,dpmaif"))
		return NULL;
	node = of_parse_phandle(consumer->of_node, "mediatek,dpmaif", 0);
	if (!node)
		return ERR_PTR(-EINVAL);
	if (!of_device_is_compatible(node, "mediatek,mt6893-dpmaif")) {
		of_node_put(node);
		return ERR_PTR(-EINVAL);
	}
	if (!of_device_is_available(node)) {
		of_node_put(node);
		return NULL;
	}
	pdev = of_find_device_by_node(node);
	of_node_put(node);
	if (!pdev)
		return ERR_PTR(-EPROBE_DEFER);
	ret = devm_add_action_or_reset(consumer, dpmaif_put_device, &pdev->dev);
	if (ret)
		return ERR_PTR(ret);
	if (!device_link_add(consumer, &pdev->dev, DL_FLAG_AUTOREMOVE_CONSUMER))
		return ERR_PTR(-ENOMEM);
	dpmaif = platform_get_drvdata(pdev);
	return dpmaif ?: ERR_PTR(-EPROBE_DEFER);
}

void ccci_dpmaif_allow_start(struct ccci_dpmaif *dpmaif)
{
	if (!dpmaif)
		return;
	mutex_lock(&dpmaif->lock);
	dpmaif->power_blocked = false;
	mutex_unlock(&dpmaif->lock);
}

int ccci_dpmaif_quiesce(struct ccci_dpmaif *dpmaif)
{
	int ul_ret, dl_ret;

	if (!dpmaif)
		return 0;
	mutex_lock(&dpmaif->lock);
	if (dpmaif->net_tx || dpmaif->net_rx) {
		mutex_unlock(&dpmaif->lock);
		return -EBUSY;
	}
	/* A start must not race the gap between DMA teardown and MD power-off. */
	dpmaif->power_blocked = true;
	ul_ret = dpmaif_ul_stop(dpmaif);
	dl_ret = dpmaif_dl_stop(dpmaif);
	mutex_unlock(&dpmaif->lock);
	return ul_ret ?: dl_ret;
}

struct dpmaif_reset_sample {
	u32 ao_threshold;
	u32 bat_init;
	u32 pit_init;
	u32 dl_reserve;
};

static struct dpmaif_reset_sample dpmaif_reset_sample(struct ccci_dpmaif *dpmaif)
{
	void __iomem *pd = dpmaif_pd_dl(dpmaif);

	return (struct dpmaif_reset_sample) {
		.ao_threshold = readl(dpmaif_ao_dl(dpmaif) + CCCI_DPMAIF_AO_DL_RDY_CHK_THRES),
		.bat_init = readl(pd + CCCI_DPMAIF_PD_DL_BAT_INIT),
		.pit_init = readl(pd + CCCI_DPMAIF_PD_DL_PIT_INIT),
		.dl_reserve = readl(pd + CCCI_DPMAIF_PD_DL_RESERVE_RW),
	};
}

static int dpmaif_reset_readback(struct ccci_dpmaif *dpmaif,
				 const struct dpmaif_reset_sample *after)
{
	void __iomem *ao = dpmaif_ao_ul(dpmaif);
	void __iomem *dl = dpmaif_ao_dl(dpmaif);

	if ((after->bat_init & CCCI_DPMAIF_DL_BAT_INIT_NOT_READY) ||
	    (after->pit_init & CCCI_DPMAIF_DL_PIT_INIT_NOT_READY))
		return -EBUSY;
	for (unsigned int q = 0; q < 4; q++) {
		if (readl(ao + CCCI_DPMAIF_UL_QSAR(q)) ||
		    (readl(ao + CCCI_DPMAIF_UL_STA0(q)) >> 16))
			return -EIO;
	}
	if (readl(dl + CCCI_DPMAIF_AO_DL_PIT_STA0) ||
	    readl(dl + CCCI_DPMAIF_AO_DL_BAT_STA0) ||
	    readl(dl + CCCI_DPMAIF_AO_DL_FRGBAT_STA0) ||
	    readl(dl + CCCI_DPMAIF_AO_DL_PIT_STA2) ||
	    (readl(dl + CCCI_DPMAIF_AO_DL_PIT_STA3) & CCCI_DPMAIF_PIT_EN_MSK) ||
	    (readl(dl + CCCI_DPMAIF_AO_DL_BAT_STA2) & CCCI_DPMAIF_BAT_SIZE_MSK) ||
	    (readl(dl + CCCI_DPMAIF_AO_DL_FRGBAT_STA2) & CCCI_DPMAIF_BAT_SIZE_MSK))
		return -EIO;
	return 0;
}

static int dpmaif_reset(struct ccci_dpmaif *dpmaif, bool restore_prot)
{
	void __iomem *prot = dpmaif_infra_ao_mem(dpmaif);
	struct dpmaif_reset_sample before, after;
	u32 value;
	int ret;

	if (dpmaif->ul.drb || dpmaif->dl.pit || dpmaif->irq_refs)
		return -EBUSY;
	ret = clk_prepare_enable(dpmaif->clk_main);
	if (ret)
		return ret;
	ret = clk_prepare_enable(dpmaif->clk_bus);
	if (ret)
		goto disable_main;

	value = readl(prot);
	writel(value & ~CCCI_DPMAIF_INFRA_PROT, prot);
	if (readl(prot) & CCCI_DPMAIF_INFRA_PROT) {
		ret = -EIO;
		goto disable_bus;
	}
	before = dpmaif_reset_sample(dpmaif);
	ret = regmap_write(dpmaif->infra, CCCI_DPMAIF_INFRA_RST0_AO, CCCI_DPMAIF_AO_RST_MASK);
	if (!ret)
		ret = regmap_write(dpmaif->infra, CCCI_DPMAIF_INFRA_RST1_AO,
				   CCCI_DPMAIF_AO_RST_MASK);
	if (!ret)
		ret = regmap_write(dpmaif->infra, CCCI_DPMAIF_INFRA_RST0_PD,
				   CCCI_DPMAIF_PD_RST_MASK);
	if (!ret)
		ret = regmap_write(dpmaif->infra, CCCI_DPMAIF_INFRA_RST1_PD,
				   CCCI_DPMAIF_PD_RST_MASK);
	if (!ret) {
		after = dpmaif_reset_sample(dpmaif);
		ret = dpmaif_reset_readback(dpmaif, &after);
		dev_info(dpmaif->dev,
			 "reset readback: AO threshold %#x->%#x PD reserve %#x->%#x BAT_INIT %#x->%#x PIT_INIT %#x->%#x ret=%d\n",
			 before.ao_threshold, after.ao_threshold,
			 before.dl_reserve, after.dl_reserve, before.bat_init,
			 after.bat_init, before.pit_init, after.pit_init, ret);
	}
	if (!ret && restore_prot && (value & CCCI_DPMAIF_INFRA_PROT))
		writel(readl(prot) | CCCI_DPMAIF_INFRA_PROT, prot);
disable_bus:
	clk_disable_unprepare(dpmaif->clk_bus);
disable_main:
	clk_disable_unprepare(dpmaif->clk_main);
	return ret;
}

/* Caller holds the MD lifecycle lock and has read back MD1 OFF. */
int ccci_dpmaif_reset_after_off(struct ccci_dpmaif *dpmaif)
{
	int ret = 0;

	if (!dpmaif)
		return 0;
	mutex_lock(&dpmaif->lock);
	if (!dpmaif->power_blocked)
		ret = -EBUSY;
	else if (dpmaif->reset_pending) {
		ret = dpmaif_reset(dpmaif, false);
		if (!ret)
			dpmaif->reset_pending = false;
	}
	mutex_unlock(&dpmaif->lock);
	return ret;
}

/* MD is powered, prepared and still held. Exclude manual ring changes
 * until the boot owner has started RX and issued the release SMC.
 */
int ccci_dpmaif_release_with_dl(struct ccci_dpmaif *dpmaif,
			       int (*release)(void *), void *data)
{
	int ret;

	if (!dpmaif)
		return -EOPNOTSUPP;
	mutex_lock(&dpmaif->lock);
	if (dpmaif->power_blocked) {
		ret = -ESHUTDOWN;
		goto out;
	}
	ret = dpmaif_reset(dpmaif, true);
	if (!ret)
		ret = dpmaif_dl_start(dpmaif, 8192, CCCI_DPMAIF_DL_PIT_ENTRIES, false);
	if (!ret) {
		dev_info(dpmaif->dev, "DPMAIF reset and DL ready before MD release\n");
		ret = release(data);
	}
	/* Keep any live DMA mappings for the normal quiesce/off path. */
out:
	mutex_unlock(&dpmaif->lock);
	return ret;
}

/* MD1 OFF was verified by the caller, which holds the lifecycle lock. */
int ccci_dpmaif_power_with_dl(struct ccci_dpmaif *dpmaif,
			     int (*power_on)(void *), void *data)
{
	int ret;

	if (!dpmaif)
		return -EOPNOTSUPP;
	mutex_lock(&dpmaif->lock);
	if (dpmaif->reset_pending || dpmaif->dl.pit || dpmaif->ul.drb ||
	    dpmaif->irq_refs) {
		ret = -EBUSY;
		goto out;
	}
	ret = dpmaif_dl_start(dpmaif, 8192, CCCI_DPMAIF_DL_PIT_ENTRIES, true);
	if (!ret) {
		dev_info(dpmaif->dev, "DPMAIF DL ready with MD1 still off\n");
		ret = power_on(data);
		if (!ret)
			dpmaif->power_blocked = false;
	}
out:
	mutex_unlock(&dpmaif->lock);
	return ret;
}

static struct ccci_dpmaif *dpmaif_net_priv(struct net_device *net)
{
	return *(struct ccci_dpmaif **)netdev_priv(net);
}

static int dpmaif_net_open(struct net_device *net)
{
	struct ccci_dpmaif *dpmaif = dpmaif_net_priv(net);

	mutex_lock(&dpmaif->lock);
	/* Rings are explicitly armed by the modem bring-up sequence. */
	if (!dpmaif->ul.started || !dpmaif->dl.started) {
		mutex_unlock(&dpmaif->lock);
		return -ENOLINK;
	}
	spin_lock_irq(&dpmaif->dl.lock);
	dpmaif->net_rx = true;
	spin_unlock_irq(&dpmaif->dl.lock);
	spin_lock_irq(&dpmaif->tx_lock);
	dpmaif->net_tx = true;
	netif_start_queue(net);
	spin_unlock_irq(&dpmaif->tx_lock);
	netif_carrier_on(net);
	mutex_unlock(&dpmaif->lock);
	return 0;
}

static int dpmaif_net_stop(struct net_device *net)
{
	struct ccci_dpmaif *dpmaif = dpmaif_net_priv(net);

	mutex_lock(&dpmaif->lock);
	spin_lock_irq(&dpmaif->tx_lock);
	dpmaif->net_tx = false;
	netif_stop_queue(net);
	spin_unlock_irq(&dpmaif->tx_lock);
	netif_tx_disable(net);
	spin_lock_irq(&dpmaif->dl.lock);
	dpmaif->net_rx = false;
	spin_unlock_irq(&dpmaif->dl.lock);
	netif_carrier_off(net);
	mutex_unlock(&dpmaif->lock);
	return 0;
}

static netdev_tx_t dpmaif_net_xmit(struct sk_buff *skb, struct net_device *net)
{
	struct ccci_dpmaif *dpmaif = dpmaif_net_priv(net);
	unsigned long flags;
	int ret = -EINVAL;

	spin_lock_irqsave(&dpmaif->tx_lock, flags);
	if (dpmaif->net_tx && skb->len && skb->len <= net->mtu &&
	    (skb->protocol == htons(ETH_P_IP) || skb->protocol == htons(ETH_P_IPV6)))
		ret = dpmaif_ul_send(dpmaif, dpmaif->net_channel, skb);
	if (ret == -ENOSPC) {
		netif_stop_queue(net);
		spin_unlock_irqrestore(&dpmaif->tx_lock, flags);
		return NETDEV_TX_BUSY;
	}
	if (ret) {
		net->stats.tx_dropped++;
	} else {
		net->stats.tx_packets++;
		net->stats.tx_bytes += skb->len;
		if (dpmaif_ul_space(dpmaif) < 2)
			netif_stop_queue(net);
	}
	spin_unlock_irqrestore(&dpmaif->tx_lock, flags);
	dev_kfree_skb_any(skb);
	return NETDEV_TX_OK;
}

static const struct net_device_ops dpmaif_net_ops = {
	.ndo_open = dpmaif_net_open,
	.ndo_stop = dpmaif_net_stop,
	.ndo_start_xmit = dpmaif_net_xmit,
};

static void dpmaif_net_setup(struct net_device *net)
{
	net->netdev_ops = &dpmaif_net_ops;
	net->type = ARPHRD_NONE;
	net->flags = IFF_POINTOPOINT | IFF_NOARP;
	net->mtu = ETH_DATA_LEN;
	net->min_mtu = IPV6_MIN_MTU;
	net->max_mtu = ETH_DATA_LEN;
	net->tx_queue_len = 1000;
}

static int dpmaif_net_register(struct ccci_dpmaif *dpmaif)
{
	struct net_device *net;
	int ret;

	/* ccmni0 is currently reserved by the legacy CCIF diagnostic endpoint. */
	net = alloc_netdev(sizeof(struct ccci_dpmaif *), "ccmnidp%d",
			   NET_NAME_ENUM, dpmaif_net_setup);
	if (!net)
		return -ENOMEM;
	*(struct ccci_dpmaif **)netdev_priv(net) = dpmaif;
	SET_NETDEV_DEV(net, dpmaif->dev);
	netif_carrier_off(net);
	dpmaif->net = net;
	ret = register_netdev(net);
	if (ret)
		free_netdev(net);
	return ret;
}

static int dpmaif_collect(struct ccci_dpmaif *dpmaif, struct ccci_dpmaif_sample *sample)
{
	u32 value = 0;
	int ret;

	memset(sample, 0, sizeof(*sample));
	ret = clk_prepare_enable(dpmaif->clk_main);
	if (ret)
		return ret;
	ret = clk_prepare_enable(dpmaif->clk_bus);
	if (ret)
		goto out_main;
	sample->rate_main = clk_get_rate(dpmaif->clk_main);
	sample->rate_bus = clk_get_rate(dpmaif->clk_bus);
	sample->ao_ul_chnl0_sta = readl(dpmaif->block[0] + CCCI_DPMAIF_AO_UL_CHNL0_STA);
	sample->ao_ul_chnl0_sta0 = readl(dpmaif->block[0] + CCCI_DPMAIF_AO_UL_CHNL0_STA0);
	sample->ao_ul_l2timr0 = readl(dpmaif->block[0] + CCCI_DPMAIF_AO_UL_AP_L2TIMR0);
	sample->pd_ul_add_desc = readl(dpmaif->block[1] + CCCI_DPMAIF_PD_UL_ADD_DESC);
	sample->pd_ul_add_desc_ch0 = readl(dpmaif->block[1] + CCCI_DPMAIF_PD_UL_ADD_DESC_CH0);
	sample->pd_md_ip_busy = readl(dpmaif->block[2] + CCCI_DPMAIF_PD_MD_IP_BUSY);
	sample->pd_md_ip_busy_mask = readl(dpmaif->block[2] + CCCI_DPMAIF_PD_MD_IP_BUSY_MASK);
	sample->sram_word0 = readl(dpmaif->block[3]);
	sample->sram_word1 = readl(dpmaif->block[3] + 4);
	/* Infracfg read goes through regmap: the block is shared with other owners. */
	if (!regmap_read(dpmaif->infra, CCCI_DPMAIF_INFRA_CTRL, &value))
		sample->infra_ctrl = value;
	clk_disable_unprepare(dpmaif->clk_bus);
out_main:
	clk_disable_unprepare(dpmaif->clk_main);
	return ret;
}

static ssize_t sample_show(struct device *dev, struct device_attribute *attr, char *buf)
{
	struct ccci_dpmaif *dpmaif = dev_get_drvdata(dev);
	struct ccci_dpmaif_sample sample;
	ssize_t ret;

	mutex_lock(&dpmaif->lock);
	ret = dpmaif_collect(dpmaif, &sample);
	if (!ret)
		ret = ccci_dpmaif_format(&sample, buf, PAGE_SIZE);
	mutex_unlock(&dpmaif->lock);
	return ret;
}
static DEVICE_ATTR_RO(sample);

static ssize_t layout_show(struct device *dev, struct device_attribute *attr, char *buf)
{
	struct ccci_dpmaif *dpmaif = dev_get_drvdata(dev);

	return sysfs_emit(buf,
			  "ao_ul=%pa pd_ul=%pa pd_md_misc=%pa sram=%pa infra_ao_mem=%pa irq=%d\n"
			  "clock_main=%s clock_bus=%s\n",
			  &dpmaif->base[0], &dpmaif->base[1], &dpmaif->base[2],
			  &dpmaif->base[3], &dpmaif->base[4], dpmaif->irq,
			  dpmaif->clk_main ? "dpmaif-main" : "none",
			  dpmaif->clk_bus ? "dpmaif" : "none");
}
static DEVICE_ATTR_RO(layout);

static ssize_t ul_show(struct device *dev, struct device_attribute *attr, char *buf)
{
	struct ccci_dpmaif *dpmaif = dev_get_drvdata(dev);
	struct ccci_dpmaif_ul *ul = &dpmaif->ul;
	u32 rd_words = 0;
	ssize_t ret;

	mutex_lock(&dpmaif->lock);
	spin_lock_irq(&dpmaif->tx_lock);
	if (ul->started)
		rd_words = readl(dpmaif_ao_ul(dpmaif) + CCCI_DPMAIF_UL_STA0(ul->queue)) >> 16;
	ret = sysfs_emit(buf,
			 "started=%u queue=%u entries=%u payload=%u wr=%u sent=%u done=%u irqs=%u err=%u not_ready=%u last_status=0x%08x rd_words=%u len_err=%u md_not_ready=%u md_power_not_ready=%u err_status=0x%08x\n",
			 ul->started, ul->queue, ul->entries, ul->payload_len, ul->wr,
			 ul->sent, ul->done, ul->irqs, ul->err, ul->not_ready,
			 ul->last_status, rd_words, ul->len_err, ul->md_not_ready,
			 ul->md_power_not_ready, ul->err_status);
	spin_unlock_irq(&dpmaif->tx_lock);
	mutex_unlock(&dpmaif->lock);
	return ret;
}
static DEVICE_ATTR_RO(ul);

/*
 * ul_start takes "<queue> <payload bytes>"; the payload is one synthetic frame
 * filled by tx_one, so the operator controls exactly how many bytes the modem
 * is offered. Nothing is sent until tx_one is written.
 */
static ssize_t ul_start_store(struct device *dev, struct device_attribute *attr,
			      const char *buf, size_t count)
{
	struct ccci_dpmaif *dpmaif = dev_get_drvdata(dev);
	struct ccci_dpmaif_ul *ul = &dpmaif->ul;
	unsigned int queue = 0, payload = 64, net_type = 0;
	int ret;

	ret = sscanf(buf, "%u %u %u", &queue, &payload, &net_type);
	if (ret < 1)
		return -EINVAL;
	if (payload < 14 || payload > CCCI_DPMAIF_UL_PAYLOAD_MAX || net_type > 7)
		return -EINVAL;
	mutex_lock(&dpmaif->lock);
	if (ul->drb) {
		mutex_unlock(&dpmaif->lock);
		return -EBUSY;
	}
	ul->entries = CCCI_DPMAIF_UL_DRB_ENTRIES;
	ul->payload_len = payload;
	ul->network_type = net_type;
	ret = dpmaif_ul_start(dpmaif, queue);
	mutex_unlock(&dpmaif->lock);
	return ret ? ret : count;
}
static DEVICE_ATTR_WO(ul_start);

static ssize_t ul_stop_store(struct device *dev, struct device_attribute *attr,
			     const char *buf, size_t count)
{
	struct ccci_dpmaif *dpmaif = dev_get_drvdata(dev);
	int ret;

	mutex_lock(&dpmaif->lock);
	if (dpmaif->net_tx) {
		mutex_unlock(&dpmaif->lock);
		return -EBUSY;
	}
	ret = dpmaif_ul_stop(dpmaif);
	mutex_unlock(&dpmaif->lock);
	return ret ?: count;
}
static DEVICE_ATTR_WO(ul_stop);

/* Fills the uplink payload with a fixed, recognisable frame and sends it once. */
static ssize_t tx_one_store(struct device *dev, struct device_attribute *attr,
			    const char *buf, size_t count)
{
	static const u8 dst[6] = { 0x02, 0x00, 0x00, 0x00, 0x00, 0x01 };
	static const u8 src[6] = { 0x02, 0x00, 0x00, 0x00, 0x00, 0x02 };
	struct ccci_dpmaif *dpmaif = dev_get_drvdata(dev);
	struct ccci_dpmaif_ul *ul = &dpmaif->ul;
	unsigned int channel = 0;
	u8 *frame;
	u32 i;
	int ret;

	ret = kstrtouint(buf, 10, &channel);
	if (ret)
		return ret;
	mutex_lock(&dpmaif->lock);
	if (dpmaif->net_tx) {
		ret = -EBUSY;
		goto out;
	}
	if (!ul->started) {
		ret = -ENOTCONN;
		goto out;
	}
	frame = ul->payload;
	memset(frame, 0, ul->payload_len);
	memcpy(frame, dst, sizeof(dst));
	memcpy(frame + sizeof(dst), src, sizeof(src));
	/* Recognisable ethertype and a counting body; no protocol meaning. */
	frame[12] = 0x88;
	frame[13] = 0xb5;
	for (i = 14; i < ul->payload_len; i++)
		frame[i] = (u8)(i - 14);
	ret = dpmaif_ul_send_probe(dpmaif, channel);
out:
	mutex_unlock(&dpmaif->lock);
	return ret ? ret : count;
}
static DEVICE_ATTR_WO(tx_one);

/*
 * Send an exact byte string up the uplink. The payload of a DPMAIF uplink
 * packet is the bare IP packet; the channel in the message descriptor is the
 * CCMNI index, not the AT channel. Keeping the bytes caller-supplied means the
 * packet can be built and checked on the host instead of in the driver.
 */
static int dpmaif_hexval(char c)
{
	if (c >= '0' && c <= '9')
		return c - '0';
	if (c >= 'a' && c <= 'f')
		return c - 'a' + 10;
	if (c >= 'A' && c <= 'F')
		return c - 'A' + 10;
	return -1;
}

static ssize_t tx_hex_store(struct device *dev, struct device_attribute *attr,
			    const char *buf, size_t count)
{
	struct ccci_dpmaif *dpmaif = dev_get_drvdata(dev);
	struct ccci_dpmaif_ul *ul = &dpmaif->ul;
	const char *hex = buf;
	unsigned int channel = 0;
	bool seen = false;
	u8 *frame;
	size_t len, i;
	int ret;

	/* Parsed in place: the byte string can be the whole payload. */
	while (*hex == ' ' || *hex == '\t')
		hex++;
	while (*hex >= '0' && *hex <= '9') {
		channel = channel * 10 + (*hex - '0');
		seen = true;
		if (++hex - buf > 4)
			return -EINVAL;
	}
	if (!seen || channel > 4)
		return -EINVAL;
	while (*hex == ' ' || *hex == '\t')
		hex++;
	len = strcspn(hex, "\n");
	if (!len || (len & 1) || len / 2 > CCCI_DPMAIF_UL_PAYLOAD_MAX)
		return -EINVAL;
	mutex_lock(&dpmaif->lock);
	if (dpmaif->net_tx) {
		ret = -EBUSY;
		goto out;
	}
	if (!ul->started) {
		ret = -ENOTCONN;
		goto out;
	}
	frame = ul->payload;
	for (i = 0; i < len; i += 2) {
		int hi = dpmaif_hexval(hex[i]);
		int lo = dpmaif_hexval(hex[i + 1]);

		if (hi < 0 || lo < 0) {
			ret = -EINVAL;
			goto out;
		}
		frame[i / 2] = (u8)((hi << 4) | lo);
	}
	ul->payload_len = len / 2;
	ret = dpmaif_ul_send_probe(dpmaif, channel);
out:
	mutex_unlock(&dpmaif->lock);
	return ret ? ret : count;
}
static DEVICE_ATTR_WO(tx_hex);

static ssize_t dl_show(struct device *dev, struct device_attribute *attr, char *buf)
{
	struct ccci_dpmaif *dpmaif = dev_get_drvdata(dev);
	struct ccci_dpmaif_dl *dl = &dpmaif->dl;
	struct ccci_dpmaif_dl_state state;
	unsigned long flags;

	memset(&state, 0, sizeof(state));
	mutex_lock(&dpmaif->lock);
	spin_lock_irqsave(&dl->lock, flags);
	state.bat_entries = dl->bat.entries;
	state.pit_entries = dl->pit_entries;
	state.bat_buf_sz = dl->bat.buf_sz;
	state.frg_entries = dl->frg.entries;
	state.frg_buf_sz = dl->frg.buf_sz;
	state.pit_rd = dl->pit_rd;
	state.bat_wr = dl->bat.wr;
	if (dl->bat.ring)
		state.ring_dma = dl->bat.ring_dma;
	if (dl->bat.buf)
		state.buf_dma = dl->bat.buf[0].dma;
	state.irqs = dl->irqs;
	state.pkts = dl->pkts;
	state.bytes = dl->bytes;
	state.msg_pits = dl->msg_pits;
	state.frag_pits = dl->frag_pits;
	state.err = dl->err;
	state.mtu_err = dl->mtu_err;
	state.overrun = dl->overrun;
	state.pit_init_stuck = dl->pit_init_stuck;
	state.last_status = dl->last_status;
	state.last_len = dl->last_len;
	state.last_channel = dl->last_channel;
	state.last_bid = dl->last_bid;
	state.snap_len = dl->snap_len;
	memcpy(state.snap, dl->snap, sizeof(state.snap));
	spin_unlock_irqrestore(&dl->lock, flags);
	mutex_unlock(&dpmaif->lock);
	return ccci_dpmaif_format_dl(&state, buf, PAGE_SIZE);
}
static DEVICE_ATTR_RO(dl);

/*
 * A raw view of every register the downlink path programs. Kept separate from
 * "dl" so that a ring that refuses to settle can be located from the register
 * that stayed busy instead of being guessed at.
 */
static ssize_t dl_regs_show(struct device *dev, struct device_attribute *attr, char *buf)
{
	struct ccci_dpmaif *dpmaif = dev_get_drvdata(dev);
	void __iomem *ao_dl = dpmaif_ao_dl(dpmaif);
	void __iomem *pd = dpmaif_pd_dl(dpmaif);
	void __iomem *misc = dpmaif_ap_misc(dpmaif);
	size_t n = 0;
	u32 base_off, i;
	int ret;

	mutex_lock(&dpmaif->lock);
	ret = clk_prepare_enable(dpmaif->clk_main);
	if (ret)
		goto out;
	ret = clk_prepare_enable(dpmaif->clk_bus);
	if (ret) {
		clk_disable_unprepare(dpmaif->clk_main);
		goto out;
	}
	n += scnprintf(buf + n, PAGE_SIZE - n, "fail_reg=0x%03x pit_init_stuck=%u\n",
		       dpmaif->dl.fail_reg, dpmaif->dl.pit_init_stuck);
	n += scnprintf(buf + n, PAGE_SIZE - n,
		       "ao_dl: pktinfo0=0x%08x pktinfo1=0x%08x pktinfo2=0x%08x\n",
		       readl(ao_dl + CCCI_DPMAIF_AO_DL_PKTINFO_CON0),
		       readl(ao_dl + CCCI_DPMAIF_AO_DL_PKTINFO_CON1),
		       readl(ao_dl + CCCI_DPMAIF_AO_DL_PKTINFO_CON2));
	n += scnprintf(buf + n, PAGE_SIZE - n,
		       "ao_dl: thres=0x%08x frg_thres=0x%08x bat_sta2=0x%08x pit_sta2=0x%08x\n",
		       readl(ao_dl + CCCI_DPMAIF_AO_DL_RDY_CHK_THRES),
		       readl(ao_dl + CCCI_DPMAIF_AO_DL_RDY_CHK_FRG_THRES),
		       readl(ao_dl + CCCI_DPMAIF_AO_DL_BAT_STA2),
		       readl(ao_dl + CCCI_DPMAIF_AO_DL_PIT_STA2));
	n += scnprintf(buf + n, PAGE_SIZE - n,
		       "ao_dl: pit_sta0=0x%08x pit_sta1=0x%08x pit_sta3=0x%08x\n",
		       readl(ao_dl + CCCI_DPMAIF_AO_DL_PIT_STA0),
		       readl(ao_dl + CCCI_DPMAIF_AO_DL_PIT_STA1),
		       readl(ao_dl + CCCI_DPMAIF_AO_DL_PIT_STA3));
	n += scnprintf(buf + n, PAGE_SIZE - n,
		       "ao_dl: bat_sta1=0x%08x frgbat_sta1=0x%08x frgbat_sta2=0x%08x pit_sta4=0x%08x\n",
		       readl(ao_dl + CCCI_DPMAIF_AO_DL_BAT_STA1),
		       readl(ao_dl + CCCI_DPMAIF_AO_DL_FRGBAT_STA1),
		       readl(ao_dl + CCCI_DPMAIF_AO_DL_FRGBAT_STA2),
		       readl(ao_dl + CCCI_DPMAIF_AO_DL_PIT_STA4));
	n += scnprintf(buf + n, PAGE_SIZE - n,
		       "pd_dl: bat_init=0x%08x con0=0x%08x con1=0x%08x add=0x%08x reserve=0x%08x\n",
		       readl(pd + CCCI_DPMAIF_PD_DL_BAT_INIT),
		       readl(pd + CCCI_DPMAIF_PD_DL_BAT_INIT_CON0),
		       readl(pd + CCCI_DPMAIF_PD_DL_BAT_INIT_CON1),
		       readl(pd + CCCI_DPMAIF_PD_DL_BAT_ADD),
		       readl(pd + CCCI_DPMAIF_PD_DL_RESERVE_RW));
	n += scnprintf(buf + n, PAGE_SIZE - n,
		       "pd_dl: pit_init=0x%08x con0=0x%08x con1=0x%08x con3=0x%08x add=0x%08x\n",
		       readl(pd + CCCI_DPMAIF_PD_DL_PIT_INIT),
		       readl(pd + CCCI_DPMAIF_PD_DL_PIT_INIT_CON0),
		       readl(pd + CCCI_DPMAIF_PD_DL_PIT_INIT_CON1),
		       readl(pd + CCCI_DPMAIF_PD_DL_PIT_INIT_CON3),
		       readl(pd + CCCI_DPMAIF_PD_DL_PIT_ADD));
	n += scnprintf(buf + n, PAGE_SIZE - n,
		       "ap_misc: cg_en=0x%08x ip_busy=0x%08x dl_sta=0x%08x dl_mask=0x%08x\n",
		       readl(misc + CCCI_DPMAIF_PD_AP_CG_EN),
		       readl(misc + CCCI_DPMAIF_PD_AP_IP_BUSY),
		       readl(misc + CCCI_DPMAIF_PD_AP_DL_L2TISAR0),
		       readl(dpmaif_ao_ul(dpmaif) + CCCI_DPMAIF_AO_UL_APDL_L2TIMR0));
	n += scnprintf(buf + n, PAGE_SIZE - n, "infra_ao_mem=0x%08x\n",
		       readl(dpmaif_infra_ao_mem(dpmaif)));
	n += scnprintf(buf + n, PAGE_SIZE - n,
		       "md_misc: rb_pit_init=0x%08x cfg0=0x%08x cfg1=0x%08x\n",
		       readl(dpmaif->block[2] + 0x100), readl(dpmaif->block[2]),
		       readl(dpmaif->block[2] + 0x04));
	/* A flat dump of both downlink windows, eight words per line. */
	for (base_off = 0; base_off < 0x100; base_off += 32) {
		n += scnprintf(buf + n, PAGE_SIZE - n, "pd_dl %02x:", base_off);
		for (i = 0; i < 32; i += 4)
			n += scnprintf(buf + n, PAGE_SIZE - n, " %08x",
				       readl(pd + base_off + i));
		n += scnprintf(buf + n, PAGE_SIZE - n, "\n");
	}
	for (base_off = 0; base_off < 0x60; base_off += 32) {
		n += scnprintf(buf + n, PAGE_SIZE - n, "ao_dl %02x:", base_off);
		for (i = 0; i < 32; i += 4)
			n += scnprintf(buf + n, PAGE_SIZE - n, " %08x",
				       readl(ao_dl + base_off + i));
		n += scnprintf(buf + n, PAGE_SIZE - n, "\n");
	}
	clk_disable_unprepare(dpmaif->clk_bus);
	clk_disable_unprepare(dpmaif->clk_main);
	ret = n;
out:
	mutex_unlock(&dpmaif->lock);
	return ret;
}
static DEVICE_ATTR_RO(dl_regs);

/*
 * dl_start takes an optional "<bat entries> <pit entries>". Both defaults stay
 * above the hardware check thresholds, and nothing is received until the modem
 * itself publishes a PIT entry.
 */
static ssize_t dl_start_store(struct device *dev, struct device_attribute *attr,
			      const char *buf, size_t count)
{
	struct ccci_dpmaif *dpmaif = dev_get_drvdata(dev);
	unsigned int bat = CCCI_DPMAIF_DL_BAT_ENTRIES, pit = CCCI_DPMAIF_DL_PIT_ENTRIES;
	char tmp[32];
	char *rest = tmp;
	int ret;

	if (!count || count >= sizeof(tmp))
		return -EINVAL;
	memcpy(tmp, buf, count);
	tmp[count] = '\0';
	ret = kstrtouint(strsep(&rest, " \t\n"), 10, &bat);
	if (ret)
		return ret;
	if (rest && *rest && *rest != '\n') {
		ret = kstrtouint(strsep(&rest, " \t\n"), 10, &pit);
		if (ret)
			return ret;
	}
	mutex_lock(&dpmaif->lock);
	ret = dpmaif_dl_start(dpmaif, bat, pit, false);
	mutex_unlock(&dpmaif->lock);
	return ret ? ret : count;
}
static DEVICE_ATTR_WO(dl_start);

/*
 * A bounded register window for bring-up. It only reaches the five blocks the
 * driver already maps, only at word offsets, and only while neither ring is
 * running, so a mistaken write cannot land in memory the hardware is using.
 * "peek <block> <offset>" reads, "poke <block> <offset> <value>" writes.
 */
static int dpmaif_parse_window(const char *buf, unsigned int *block,
			       unsigned int *offset, unsigned int *value,
			       bool need_value)
{
	char tmp[48];
	char *rest = tmp;
	int ret;

	if (!buf || strlen(buf) >= sizeof(tmp))
		return -EINVAL;
	strscpy(tmp, buf, sizeof(tmp));
	ret = kstrtouint(strsep(&rest, " \t\n"), 10, block);
	if (ret)
		return ret;
	if (!rest)
		return -EINVAL;
	ret = kstrtouint(strsep(&rest, " \t\n"), 0, offset);
	if (ret)
		return ret;
	if (*block >= CCCI_DPMAIF_BLOCKS || *offset >= 0x1000 || (*offset & 3))
		return -EINVAL;
	if (!need_value)
		return 0;
	if (!rest)
		return -EINVAL;
	return kstrtouint(strsep(&rest, " \t\n"), 0, value);
}

static ssize_t peek_store(struct device *dev, struct device_attribute *attr,
			  const char *buf, size_t count)
{
	struct ccci_dpmaif *dpmaif = dev_get_drvdata(dev);
	unsigned int block, offset, value;
	int ret;

	ret = dpmaif_parse_window(buf, &block, &offset, &value, false);
	if (ret)
		return ret;
	mutex_lock(&dpmaif->lock);
	ret = clk_prepare_enable(dpmaif->clk_main);
	if (ret)
		goto out;
	ret = clk_prepare_enable(dpmaif->clk_bus);
	if (ret) {
		clk_disable_unprepare(dpmaif->clk_main);
		goto out;
	}
	value = readl(dpmaif->block[block] + offset);
	clk_disable_unprepare(dpmaif->clk_bus);
	clk_disable_unprepare(dpmaif->clk_main);
	dpmaif->peek_block = block;
	dpmaif->peek_offset = offset;
	dpmaif->peek_value = value;
	ret = count;
out:
	mutex_unlock(&dpmaif->lock);
	return ret;
}
static DEVICE_ATTR_WO(peek);

static ssize_t peek_value_show(struct device *dev, struct device_attribute *attr, char *buf)
{
	struct ccci_dpmaif *dpmaif = dev_get_drvdata(dev);

	return sysfs_emit(buf, "block=%u offset=0x%03x value=0x%08x\n",
			  dpmaif->peek_block, dpmaif->peek_offset, dpmaif->peek_value);
}
static DEVICE_ATTR_RO(peek_value);

static ssize_t poke_store(struct device *dev, struct device_attribute *attr,
			  const char *buf, size_t count)
{
	struct ccci_dpmaif *dpmaif = dev_get_drvdata(dev);
	unsigned int block, offset, value;
	int ret;

	ret = dpmaif_parse_window(buf, &block, &offset, &value, true);
	if (ret)
		return ret;
	mutex_lock(&dpmaif->lock);
	if (dpmaif->power_blocked) {
		ret = -ESHUTDOWN;
		goto out;
	}
	if (dpmaif->net_tx) {
		mutex_unlock(&dpmaif->lock);
		return -EBUSY;
	}
	if (dpmaif->ul.drb || dpmaif->dl.pit) {
		ret = -EBUSY;
		goto out;
	}
	ret = clk_prepare_enable(dpmaif->clk_main);
	if (ret)
		goto out;
	ret = clk_prepare_enable(dpmaif->clk_bus);
	if (ret) {
		clk_disable_unprepare(dpmaif->clk_main);
		goto out;
	}
	writel(value, dpmaif->block[block] + offset);
	clk_disable_unprepare(dpmaif->clk_bus);
	clk_disable_unprepare(dpmaif->clk_main);
	ret = count;
out:
	mutex_unlock(&dpmaif->lock);
	return ret;
}
static DEVICE_ATTR_WO(poke);

/*
 * The vendor's own recovery for a wedged DPMAIF: drop the memory protection,
 * pulse both domain resets, and let the caller re-program from scratch. Only
 * allowed with neither ring running, because it wipes every hardware pointer.
 */
static ssize_t reset_store(struct device *dev, struct device_attribute *attr,
			   const char *buf, size_t count)
{
	struct ccci_dpmaif *dpmaif = dev_get_drvdata(dev);
	int ret;

	mutex_lock(&dpmaif->lock);
	ret = dpmaif->power_blocked ? -ESHUTDOWN : dpmaif_reset(dpmaif, true);
	mutex_unlock(&dpmaif->lock);
	return ret ?: count;
}
static DEVICE_ATTR_WO(reset);

static ssize_t dl_stop_store(struct device *dev, struct device_attribute *attr,
			     const char *buf, size_t count)
{
	struct ccci_dpmaif *dpmaif = dev_get_drvdata(dev);
	int ret;

	mutex_lock(&dpmaif->lock);
	if (dpmaif->net_tx) {
		mutex_unlock(&dpmaif->lock);
		return -EBUSY;
	}
	ret = dpmaif_dl_stop(dpmaif);
	mutex_unlock(&dpmaif->lock);
	return ret ?: count;
}
static DEVICE_ATTR_WO(dl_stop);

static ssize_t net_channel_show(struct device *dev, struct device_attribute *attr,
				char *buf)
{
	struct ccci_dpmaif *dpmaif = dev_get_drvdata(dev);
	u8 channel;

	mutex_lock(&dpmaif->lock);
	channel = dpmaif->net_channel;
	mutex_unlock(&dpmaif->lock);

	return sysfs_emit(buf, "%u\n", channel);
}

static ssize_t net_channel_store(struct device *dev, struct device_attribute *attr,
				 const char *buf, size_t count)
{
	struct ccci_dpmaif *dpmaif = dev_get_drvdata(dev);
	u8 channel;
	int ret;

	ret = kstrtou8(buf, 10, &channel);
	if (ret)
		return ret;

	mutex_lock(&dpmaif->lock);
	/* Open/stop hold this mutex while enabling or quiescing both paths. */
	if (dpmaif->net_tx || dpmaif->net_rx)
		ret = -EBUSY;
	else
		dpmaif->net_channel = channel;
	mutex_unlock(&dpmaif->lock);

	return ret ? ret : count;
}
static DEVICE_ATTR_RW(net_channel);

static struct attribute *dpmaif_attrs[] = {
	&dev_attr_sample.attr,
	&dev_attr_layout.attr,
	&dev_attr_ul.attr,
	&dev_attr_ul_start.attr,
	&dev_attr_ul_stop.attr,
	&dev_attr_net_channel.attr,
	&dev_attr_tx_one.attr,
	&dev_attr_tx_hex.attr,
	&dev_attr_dl.attr,
	&dev_attr_dl_regs.attr,
	&dev_attr_dl_start.attr,
	&dev_attr_dl_stop.attr,
	&dev_attr_peek.attr,
	&dev_attr_peek_value.attr,
	&dev_attr_poke.attr,
	&dev_attr_reset.attr,
	NULL,
};
ATTRIBUTE_GROUPS(dpmaif);

static int dpmaif_probe(struct platform_device *pdev)
{
	static const char * const names[CCCI_DPMAIF_BLOCKS] = {
		"ao-ul", "pd-ul", "pd-md-misc", "sram", "infra-ao-mem",
	};
	struct ccci_dpmaif *dpmaif;
	struct device *dev = &pdev->dev;
	int i, ret;

	dpmaif = devm_kzalloc(dev, sizeof(*dpmaif), GFP_KERNEL);
	if (!dpmaif)
		return -ENOMEM;
	dpmaif->dev = dev;
	mutex_init(&dpmaif->lock);
	spin_lock_init(&dpmaif->dl.lock);
	spin_lock_init(&dpmaif->tx_lock);
	for (i = 0; i < CCCI_DPMAIF_BLOCKS; i++) {
		struct resource *res;

		res = platform_get_resource_byname(pdev, IORESOURCE_MEM, names[i]);
		if (!res)
			return dev_err_probe(dev, -ENODEV, "no %s region\n", names[i]);
		dpmaif->base[i] = res->start;
		dpmaif->block[i] = devm_platform_ioremap_resource_byname(pdev, names[i]);
		if (IS_ERR(dpmaif->block[i]))
			return dev_err_probe(dev, PTR_ERR(dpmaif->block[i]),
					     "cannot map %s\n", names[i]);
	}
	dpmaif->irq = platform_get_irq(pdev, 0);
	if (dpmaif->irq < 0)
		return dev_err_probe(dev, dpmaif->irq, "no interrupt\n");
	dpmaif->infra = syscon_regmap_lookup_by_phandle(dev->of_node, "mediatek,infracfg");
	if (IS_ERR(dpmaif->infra))
		return dev_err_probe(dev, PTR_ERR(dpmaif->infra), "no infracfg regmap\n");
	dpmaif->topckgen = syscon_regmap_lookup_by_compatible("mediatek,mt6893-topckgen");
	dpmaif->clk_main = devm_clk_get(dev, "dpmaif-main");
	if (IS_ERR(dpmaif->clk_main))
		return dev_err_probe(dev, PTR_ERR(dpmaif->clk_main), "no dpmaif-main clock\n");
	dpmaif->clk_bus = devm_clk_get(dev, "dpmaif");
	if (IS_ERR(dpmaif->clk_bus))
		return dev_err_probe(dev, PTR_ERR(dpmaif->clk_bus), "no dpmaif clock\n");
	ret = dpmaif_net_register(dpmaif);
	if (ret)
		return dev_err_probe(dev, ret, "cannot register raw IP netdev\n");
	platform_set_drvdata(pdev, dpmaif);
	dev_info(dev, "dpmaif observed: irq=%d, %d blocks mapped, no register written\n",
		 dpmaif->irq, CCCI_DPMAIF_BLOCKS);
	return 0;
}

static void dpmaif_remove(struct platform_device *pdev)
{
	struct ccci_dpmaif *dpmaif = platform_get_drvdata(pdev);

	unregister_netdev(dpmaif->net);
	mutex_lock(&dpmaif->lock);
	if (dpmaif_dl_stop(dpmaif))
		dev_err(dpmaif->dev, "DL stop unconfirmed; retaining DMA buffers\n");
	if (dpmaif_ul_stop(dpmaif))
		dev_err(dpmaif->dev, "UL stop unconfirmed; retaining DMA buffers\n");
	mutex_unlock(&dpmaif->lock);
	free_netdev(dpmaif->net);
}

static const struct of_device_id dpmaif_of_ids[] = {
	{ .compatible = "mediatek,mt6893-dpmaif" },
	{ }
};
MODULE_DEVICE_TABLE(of, dpmaif_of_ids);

static struct platform_driver dpmaif_driver = {
	.probe = dpmaif_probe,
	.remove = dpmaif_remove,
	.driver = {
		.name = "op6893-dpmaif",
		.suppress_bind_attrs = true,
		.of_match_table = dpmaif_of_ids,
		.dev_groups = dpmaif_groups,
	},
};
module_platform_driver(dpmaif_driver);

MODULE_DESCRIPTION("op6893 DPMAIF uplink and downlink observation path");
MODULE_LICENSE("GPL");
