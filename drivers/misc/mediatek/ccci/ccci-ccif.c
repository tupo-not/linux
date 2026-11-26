// SPDX-License-Identifier: GPL-2.0-only
/* Ordinary CCIF owner: bounded mappings, preparation and explicit receive lifecycle. */
#include <linux/delay.h>
#include <linux/interrupt.h>
#include <linux/io.h>
#include <linux/irq.h>
#include <linux/mutex.h>
#include <linux/platform_device.h>
#include <linux/regmap.h>
#include <linux/slab.h>
#include <linux/sysfs.h>
#include <linux/unaligned.h>

#include "ccci-ccif.h"
#include "ccci-ccif-control.h"
#include "ccci-ccif-hw.h"
#include "ccci-ccif-ring.h"
#include "ccci-ccif-rx.h"
#include "ccci-smem.h"

struct ccci_ccif_bank {
	void __iomem *base;
	u32 size;
};

struct ccci_ccif {
	/* Never acquire the control or parent MD lock while holding this mutex. */
	struct mutex tx_lock;
	bool tx_active;
	u16 tx_sequence;
	u16 service_sequence[4];
	int tx_error;
	struct ccci_ccif_rx *rx;
	struct ccci_ccif_controller *control;
	struct regmap *infra;
	void __iomem *ap;
	void __iomem *md;
	struct ccci_ccif_bank bank[CCCI_CCIF_BANKS];
	struct ccci_ccif_layout layout;
	struct ccci_ccif_ring ring[CCCI_CCIF_BANKS][CCCI_CCIF_QUEUES];
	int irq[2];
	const char *step;
	int error;
	bool prepared;
	bool attempted;
	bool quiesced_at_stop;
};

static int ccif_bank_read(void *context, u32 offset, u32 *value)
{
	struct ccci_ccif_bank *bank = context;

	if ((offset & 3) || offset > bank->size || bank->size - offset < 4)
		return -ERANGE;
	*value = readl(bank->base + offset);
	return 0;
}

static int ccif_bank_write(void *context, u32 offset, u32 value)
{
	struct ccci_ccif_bank *bank = context;

	if ((offset & 3) || offset > bank->size || bank->size - offset < 4)
		return -ERANGE;
	writel(value, bank->base + offset);
	return 0;
}

static int ccif_bank_from(void *context, u32 offset, void *to, u32 size)
{
	struct ccci_ccif_bank *bank = context;

	if (offset > bank->size || size > bank->size - offset)
		return -ERANGE;
	memcpy_fromio(to, bank->base + offset, size);
	return 0;
}

static int ccif_bank_to(void *context, u32 offset, const void *from, u32 size)
{
	struct ccci_ccif_bank *bank = context;

	if (offset > bank->size || size > bank->size - offset)
		return -ERANGE;
	memcpy_toio(bank->base + offset, from, size);
	return 0;
}

static int ccif_bank_zero(void *context, u32 offset, u32 size)
{
	struct ccci_ccif_bank *bank = context;

	if (offset > bank->size || size > bank->size - offset)
		return -ERANGE;
	memset_io(bank->base + offset, 0, size);
	return 0;
}

static void ccif_bank_barrier(void *context, bool publish)
{
	if (publish) {
		/* Complete WC reads and writes before publishing the AP-owned pointer. */
		mb();
	} else {
		/* Observe peer data after sampling its producer/consumer pointer. */
		rmb();
	}
}

/* Report the rejected control word, without another MMIO read or a payload dump. */
static void ccif_bank_bad_control(void *context, bool tx, u32 offset, u32 value, u32 expected)
{
	pr_err_ratelimited("ccci: invalid CCIF %s ring word offset=%#x value=%#x expected_or_limit=%#x\n",
			   tx ? "TX" : "RX", offset, value, expected);
}

static void ccif_bank_bad_tx_read(void *context, u32 offset, u32 read, u32 write,
				  u32 length, int sample_error, u32 sample, unsigned int retries)
{
	if (!sample_error && sample == (read + 4 == length ? 0 : read + 4)) {
		pr_info_ratelimited("ccci: CCIF TX read recovered offset=%#x read=%#x reread=%#x retries=%u\n",
				    offset, read, sample, retries);
		return;
	}
	pr_err_ratelimited("ccci: invalid CCIF TX read offset=%#x read=%#x write=%#x length=%#x reread=%#x reread_error=%d retries=%u\n",
			   offset, read, write, length, sample, sample_error, retries);
}

static void ccif_bank_wait_tx_read(void *context)
{
	udelay(10);
}

static const struct ccci_ccif_ring_ops ccif_ring_ops = {
	.read32 = ccif_bank_read,
	.write32 = ccif_bank_write,
	.copy_from = ccif_bank_from,
	.copy_to = ccif_bank_to,
	.zero = ccif_bank_zero,
	.fence = ccif_bank_barrier,
	.bad_control = ccif_bank_bad_control,
	.bad_tx_read = ccif_bank_bad_tx_read,
	.wait_tx_read = ccif_bank_wait_tx_read,
};

static int ccif_hw_write(void *context, u32 bank, u32 offset, u32 value)
{
	struct ccci_ccif *ccif = context;

	if (!bank) {
		if ((offset != 0x150 && offset != 0x154) || value != BIT(8))
			return -EINVAL;
		return regmap_write(ccif->infra, offset, value);
	}
	if (bank > 2 || offset < 0x100 || offset >= 0x300 || (offset & 3))
		return -EINVAL;
	writel(value, (bank == 1 ? ccif->ap : ccif->md) + offset);
	return 0;
}

static int ccif_hw_read(void *context, u32 bank, u32 offset, u32 *value)
{
	struct ccci_ccif *ccif = context;

	if (bank < 1 || bank > 2 || (offset & 3) ||
	    !((offset >= 0x100 && offset < 0x300) || offset == 4 || offset == 0x10))
		return -EINVAL;
	*value = readl((bank == 1 ? ccif->ap : ccif->md) + offset);
	return 0;
}

static void ccif_hw_barrier(void *context)
{
	/* Order reset/SRAM stores before explicit device readback. */
	mb();
}

static const struct ccci_ccif_hw_ops ccif_hw_ops = {
	.write = ccif_hw_write,
	.read = ccif_hw_read,
	.fence = ccif_hw_barrier,
};

static irqreturn_t ccif_irq(int irq, void *context)
{
	struct ccci_ccif *ccif = context;

	ccci_ccif_rx_interrupt(ccif->rx, irq);
	return IRQ_HANDLED;
}

static void ccif_irq_flags_clear(void *context)
{
	struct ccci_ccif *ccif = context;

	for (u32 i = 0; i < 2; i++)
		irq_clear_status_flags(ccif->irq[i], IRQ_DISABLE_UNLAZY);
}

static void ccif_tx_gate(struct ccci_ccif *ccif, bool active)
{
	mutex_lock(&ccif->tx_lock);
	ccif->tx_active = active && !ccif->tx_error;
	mutex_unlock(&ccif->tx_lock);
}

void ccci_ccif_abort(struct ccci_ccif *ccif, int error)
{
	if (!ccif)
		return;
	if (error >= 0)
		error = -EIO;
	mutex_lock(&ccif->tx_lock);
	ccif->tx_active = false;
	if (!ccif->tx_error)
		WRITE_ONCE(ccif->tx_error, error);
	error = ccif->tx_error;
	mutex_unlock(&ccif->tx_lock);
	ccci_ccif_rx_abort(ccif->rx, error);
}

/* No parent or control mutex: this can be called from the RX port callback. */
int ccci_ccif_system_send(struct ccci_ccif *ccif, u32 message, u32 reserved)
{
	u8 packet[CCCI_CCIF_HEADER_SIZE];
	int ret;

	if (!ccif || (message != 0x119 && message != 0x11b))
		return -EINVAL;
	mutex_lock(&ccif->tx_lock);
	if (ccif->tx_error) {
		ret = ccif->tx_error;
		goto out;
	}
	if (!ccif->tx_active) {
		ret = -ESHUTDOWN;
		goto out;
	}
	ret = ccci_ccif_rx_error(ccif->rx);
	if (ret)
		goto out;
	/* Check before publishing: a busy queue can be retried without duplication. */
	if (readl(ccif->ap + 4) & BIT(0)) {
		ret = -EBUSY;
		goto out;
	}
	put_unaligned_le32(0xffffffff, packet);
	put_unaligned_le32(message, packet + 4);
	put_unaligned_le32(3 | ((u32)ccif->tx_sequence << 16) | BIT(31), packet + 8);
	put_unaligned_le32(reserved, packet + 12);
	ret = ccci_ccif_ring_transmit(&ccif->ring[CCCI_CCIF_NORMAL][0], packet,
				      sizeof(packet));
	if (ret == -ENOSPC)
		goto out;
	if (ret) {
		/* A partial write/publication has no safe implicit retry or rollback. */
		WRITE_ONCE(ccif->tx_error, ret);
		ccif->tx_active = false;
		ccci_ccif_rx_abort(ccif->rx, ret);
		goto out;
	}
	ccif->tx_sequence = (ccif->tx_sequence + 1) & 0x7fff;
	/* Publish WC packet and producer pointer before ringing ordinary queue 0. */
	mb();
	writel(BIT(0), ccif->ap + 4);
	writel(0, ccif->ap + 0xc);
	ret = 0;
out:
	mutex_unlock(&ccif->tx_lock);
	return ret;
}

static bool ccci_ccif_faulted(struct ccci_ccif *ccif)
{
	return ccif->error || READ_ONCE(ccif->tx_error) || ccci_ccif_rx_error(ccif->rx) ||
	       ccci_ccif_control_error(ccif->control);
}

static int ccif_fail(struct ccci_ccif *ccif, int ret)
{
	ccif->error = ret;
	return ret;
}

static int ccif_irqs_masked(struct ccci_ccif *ccif)
{
	bool masked;
	int ret;

	for (u32 i = 0; i < 2; i++) {
		ret = irq_get_irqchip_state(ccif->irq[i], IRQCHIP_STATE_MASKED, &masked);
		if (ret)
			return ret;
		if (!masked)
			return -EBUSY;
	}
	return 0;
}

int ccci_ccif_prepare(struct ccci_ccif *ccif)
{
	int ret;

	if (ccci_ccif_faulted(ccif))
		return -EIO;
	if (ccif->attempted)
		return -EALREADY;
	ret = ccif_irqs_masked(ccif);
	if (ret)
		return ret;
	ccif->attempted = true;
	ccif->step = "format-rings";
	for (u32 bank = 0; bank < CCCI_CCIF_BANKS; bank++) {
		for (u32 q = 0; q < CCCI_CCIF_QUEUES; q++) {
			ret = ccci_ccif_ring_format(&ccif->ring[bank][q]);
			if (!ret)
				ret = ccci_ccif_ring_verify_empty(&ccif->ring[bank][q]);
			if (ret)
				return ccif_fail(ccif, ret);
		}
	}
	ccif->step = "reset-and-sram";
	ret = ccci_ccif_hw_reset(&ccif_hw_ops, ccif, &ccif->layout);
	if (ret)
		return ccif_fail(ccif, ret);
	ccif->prepared = true;
	ccif->step = "prepared-irqs-masked";
	return 0;
}

int ccci_ccif_receive_start(struct ccci_ccif *ccif, ccci_ccif_consume_fn consume, void *context)
{
	int ret;

	if (ccci_ccif_faulted(ccif))
		return -EIO;
	if (!ccif->prepared)
		return -EAGAIN;
	ret = ccif_irqs_masked(ccif);
	if (ret)
		return ret;
	ccif_tx_gate(ccif, true);
	ret = ccci_ccif_rx_start(ccif->rx, consume, context);
	if (ret)
		ccif_tx_gate(ccif, false);
	if (!ret)
		ccif->step = "receive-started";
	return ret;
}

int ccci_ccif_set_fault(struct ccci_ccif *ccif, ccci_ccif_control_fault_fn fault, void *context)
{
	return ccci_ccif_control_set_fault(ccif->control, fault, context);
}

int ccci_ccif_handshake_start(struct ccci_ccif *ccif, const struct ccci_runtime_offer *offers,
			      ccci_ccif_consume_fn port, void *context)
{
	int ret;

	if (ccci_ccif_faulted(ccif))
		return -EIO;
	if (!ccif->prepared)
		return -EAGAIN;
	ret = ccif_irqs_masked(ccif);
	if (ret)
		return ret;
	ccif_tx_gate(ccif, true);
	ret = ccci_ccif_control_start(ccif->control, offers, port, context);
	if (ret)
		ccif_tx_gate(ccif, false);
	if (!ret)
		ccif->step = "handshake-started";
	return ret;
}

void ccci_ccif_quiesce_rx(struct ccci_ccif *ccif)
{
	/* Consumer callbacks must never acquire the parent MD owner mutex. */
	ccci_ccif_control_quiesce(ccif->control);
	ccci_ccif_rx_stop(ccif->rx);
}

/* Close admission without resetting a CCIF peer that may still be active. */
int ccci_ccif_park(struct ccci_ccif *ccif)
{
	ccif_tx_gate(ccif, false);
	return ccif_irqs_masked(ccif);
}

/* Parent has independently confirmed MD1 OFF after parking the transport. */
void ccci_ccif_powered_off(struct ccci_ccif *ccif)
{
	if (!ccif->prepared)
		return;
	ccif->prepared = false;
	ccif->quiesced_at_stop = true;
	ccif->step = "off-irqs-masked";
}

int ccci_ccif_stop(struct ccci_ccif *ccif)
{
	int ret;

	ccci_ccif_quiesce_rx(ccif);
	/* Drain any sender before resetting/powering off. */
	ccif_tx_gate(ccif, false);
	/* Protocol faults stay latched for start, but must not prevent shutdown. */
	if (ccif->error)
		return -EIO;
	if (!ccif->prepared)
		return 0;
	ret = ccif_irqs_masked(ccif);
	if (ret)
		return ccif_fail(ccif, ret);
	ccif->step = "stop-reset";
	ret = ccci_ccif_hw_reset(&ccif_hw_ops, ccif, &ccif->layout);
	if (ret)
		return ccif_fail(ccif, ret);
	ccif->prepared = false;
	ccif->quiesced_at_stop = true;
	ccif->step = "stopped-irqs-masked";
	return 0;
}

int ccci_ccif_rearm(struct ccci_ccif *ccif, bool commit)
{
	int ret;

	if (!ccif->quiesced_at_stop || ccif->prepared || ccci_ccif_faulted(ccif))
		return -EBUSY;
	ret = ccif_irqs_masked(ccif);
	if (ret)
		return ret;
	ret = ccci_ccif_rx_rearm(ccif->rx, commit);
	if (!ret)
		ret = ccci_ccif_control_rearm(ccif->control, commit);
	if (ret || !commit)
		return ret;

	mutex_lock(&ccif->tx_lock);
	ccif->tx_sequence = 0;
	memset(ccif->service_sequence, 0, sizeof(ccif->service_sequence));
	mutex_unlock(&ccif->tx_lock);
	ccif->attempted = false;
	ccif->quiesced_at_stop = false;
	ccif->step = "rearmed-irqs-masked";
	return 0;
}

ssize_t ccci_ccif_status(struct ccci_ccif *ccif, char *buf)
{
	return sysfs_emit(buf,
			  "step=%s error=%d attempted=%u prepared=%u rx_error=%d irq_armed=%u tx_error=%d tx_sequence=%u normal_bytes=%u exception_bytes=%u debug_md=%#x debug_size=%u cpu_release_issued=0\n",
			  ccif->step, ccif->error, ccif->attempted, ccif->prepared,
			  ccci_ccif_rx_error(ccif->rx),
			  ccci_ccif_rx_armed(ccif->rx), READ_ONCE(ccif->tx_error),
			  READ_ONCE(ccif->tx_sequence),
			  ccif->layout.used[0], ccif->layout.used[1],
			  ccif->layout.debug_md, ccif->layout.debug_size);
}

ssize_t ccci_ccif_fs_tx_status(struct ccci_ccif *ccif, char *buf)
{
	u32 read, write;
	ssize_t ret;

	if (!ccif->prepared)
		return -ENODATA;

	mutex_lock(&ccif->tx_lock);
	ret = ccci_ccif_ring_tx_positions(&ccif->ring[CCCI_CCIF_NORMAL][4], &read, &write);
	if (!ret)
		ret = sysfs_emit(buf, "tx_read=%u tx_write=%u busy=%u next_seq=%u\n",
				 read, write, !!(readl(ccif->ap + 4) & BIT(4)),
				 ccif->service_sequence[0]);
	mutex_unlock(&ccif->tx_lock);
	return ret;
}

/*
 * One normal-bank transmit with the shared sequence/busy/doorbell rules. The
 * caller has already validated the packet and owns the copy.
 */
static int ccif_transmit(struct ccci_ccif *ccif, u8 *copy, u32 size, u32 channel,
			 u32 queue, u32 index, bool hs2_seen)
{
	u32 busy;
	int ret;

	mutex_lock(&ccif->tx_lock);
	ret = ccif->tx_error;
	if (ret)
		goto out;
	if (!ccif->tx_active) {
		ret = -ESHUTDOWN;
		goto out;
	}
	ret = ccci_ccif_rx_error(ccif->rx);
	if (ret)
		goto out;
	/* One read of the AP busy word: it decides the send and feeds the log. */
	busy = readl(ccif->ap + 4);
	if (busy & BIT(queue)) {
		ret = -EBUSY;
		pr_info_ratelimited("ccci: service send deferred: channel=%u queue=%u busy=%#x\n",
				    channel, queue, busy);
		goto out;
	}
	/* HIF owns sequence per channel; FS/RPC assert only while awaiting HS2. */
	put_unaligned_le32(channel | ((u32)ccif->service_sequence[index] << 16) |
			   (hs2_seen ? 0 : BIT(31)), copy + 8);
	ret = ccci_ccif_ring_transmit(&ccif->ring[CCCI_CCIF_NORMAL][queue], copy, size);
	if (ret == -ENOSPC) {
		pr_info_ratelimited("ccci: service ring full: channel=%u queue=%u size=%u\n",
				    channel, queue, size);
		goto out;
	}
	if (ret) {
		pr_err("ccci: service transmit failed: channel=%u queue=%u error=%d\n",
		       channel, queue, ret);
		WRITE_ONCE(ccif->tx_error, ret);
		ccif->tx_active = false;
		ccci_ccif_rx_abort(ccif->rx, ret);
		goto out;
	}
	ccif->service_sequence[index] = (ccif->service_sequence[index] + 1) & 0x7fff;
	/* Complete WC payload and producer stores before ordinary queue doorbell. */
	mb();
	writel(BIT(queue), ccif->ap + 4);
	writel(queue, ccif->ap + 0xc);
	ret = 0;
out:
	mutex_unlock(&ccif->tx_lock);
	return ret;
}

/* Packet codec and leased request ownership are checked by the service caller. */
int ccci_ccif_service_send(struct ccci_ccif *ccif, const void *packet, u32 size,
			   bool hs2_seen)
{
	const u8 *input = packet;
	u8 *copy;
	u32 channel, queue, index;
	int ret;

	if (!ccif || !packet || size <= 20 || size > 20 + 0xd80 ||
	    get_unaligned_le32(input + 4) != size)
		return -EINVAL;
	channel = get_unaligned_le32(input + 8) & 0xffff;
	if (channel == 15) {
		if (get_unaligned_le32(input + 12) >= 5)
			return -ERANGE;
		queue = 4;
		index = 0;
	} else if (channel == 33) {
		if (size < 24 || size > 2048 || get_unaligned_le32(input + 12) > 2)
			return -ERANGE;
		queue = 1;
		index = 1;
	} else {
		return -EOPNOTSUPP;
	}
	copy = kmemdup(packet, size, GFP_KERNEL);
	if (!copy)
		return -ENOMEM;
	ret = ccif_transmit(ccif, copy, size, channel, queue, index, hs2_seen);
	kfree_sensitive(copy);
	return ret;
}

/* AP_MD_CCB_WAKEUP doorbell: bare H2D_RINGQ7 notification, no ring payload.
 * The vendor sends this after draining CCB so the MD can reclaim its buffers. */
#define CCCI_CCIF_CCB_CHANNEL	7

int ccci_ccif_ccb_wakeup(struct ccci_ccif *ccif)
{
	u32 busy;

	if (!ccif)
		return -EINVAL;
	mutex_lock(&ccif->tx_lock);
	if (ccif->tx_error || !ccif->tx_active) {
		mutex_unlock(&ccif->tx_lock);
		return -ESHUTDOWN;
	}
	busy = readl(ccif->ap + 4);
	if (busy & BIT(CCCI_CCIF_CCB_CHANNEL)) {
		mutex_unlock(&ccif->tx_lock);
		return -EBUSY;
	}
	writel(BIT(CCCI_CCIF_CCB_CHANNEL), ccif->ap + 4);
	writel(CCCI_CCIF_CCB_CHANNEL, ccif->ap + 0xc);
	mutex_unlock(&ccif->tx_lock);
	return 0;
}

/*
 * AT/MUX payload on CCCI_UART2_TX (channel 12, queue 5). The header is built
 * here exactly as the same-board char port does for a raw byte write, so
 * userspace owns only the 0710 stream and never the CCCI header.
 */
int ccci_ccif_at_send(struct ccci_ccif *ccif, const void *payload, u32 size,
		      bool hs2_seen)
{
	u8 *copy;
	int ret;

	if (!ccif || !payload || !size || size > CCCI_CCIF_AT_MAX)
		return -EINVAL;
	copy = kmalloc(CCCI_CCIF_HEADER + size, GFP_KERNEL);
	if (!copy)
		return -ENOMEM;
	put_unaligned_le32(0, copy);
	put_unaligned_le32(CCCI_CCIF_HEADER + size, copy + 4);
	put_unaligned_le32(CCCI_CCIF_AT_CHANNEL, copy + 8);
	put_unaligned_le32(0, copy + 12);
	memcpy(copy + CCCI_CCIF_HEADER, payload, size);
	ret = ccif_transmit(ccif, copy, CCCI_CCIF_HEADER + size, CCCI_CCIF_AT_CHANNEL,
			    CCCI_CCIF_AT_QUEUE, 2, hs2_seen);
	kfree_sensitive(copy);
	return ret;
}

/*
 * CCMNI payload on the net channel (22, queue chosen by the MD's LHIF remap).
 * The header is built here exactly as the same-board net port does: data[0] is
 * the CCMNI index and data[1] the total length including the header.
 */
int ccci_ccif_net_send(struct ccci_ccif *ccif, u32 index, u32 queue,
		       const void *payload, u32 size)
{
	u8 *copy;
	int ret;

	if (!ccif || !payload || size < CCCI_CCIF_NET_MIN || size > CCCI_CCIF_NET_MAX ||
	    index > CCCI_CCIF_NET_INDEX_MAX || queue >= CCCI_CCIF_QUEUES)
		return -EINVAL;
	copy = kmalloc(CCCI_CCIF_HEADER + size, GFP_KERNEL);
	if (!copy)
		return -ENOMEM;
	put_unaligned_le32(index, copy);
	put_unaligned_le32(CCCI_CCIF_HEADER + size, copy + 4);
	put_unaligned_le32(CCCI_CCIF_NET_CHANNEL, copy + 8);
	put_unaligned_le32(0, copy + 12);
	memcpy(copy + CCCI_CCIF_HEADER, payload, size);
	ret = ccif_transmit(ccif, copy, CCCI_CCIF_HEADER + size, CCCI_CCIF_NET_CHANNEL,
			    queue, 3, true);
	kfree_sensitive(copy);
	return ret;
}

ssize_t ccci_ccif_handshake_status(struct ccci_ccif *ccif, char *buf)
{
	return ccci_ccif_control_status(ccif->control, buf);
}

struct ccci_ccif *ccci_ccif_create(struct platform_device *pdev,
				   const struct ccci_smem_layout *smem, struct regmap *infra)
{
	static const char * const names[] = { "ccif-ap", "ccif-md" };
	static const resource_size_t addresses[] = { 0x10209000, 0x1020a000 };
	const struct ccci_smem_region *normal = &smem->users[CCCI_SMEM_CCISM_MCU];
	const struct ccci_smem_region *exception = &smem->users[CCCI_SMEM_CCISM_EXP];
	struct device *dev = &pdev->dev;
	struct ccci_ccif *ccif;
	void __iomem *mapping;
	int ret;

	ccif = devm_kzalloc(dev, sizeof(*ccif), GFP_KERNEL);
	if (!ccif)
		return ERR_PTR(-ENOMEM);
	mutex_init(&ccif->tx_lock);
	ccif->infra = infra;
	ccif->step = "reserved";
	ret = ccci_ccif_plan(smem, &ccif->layout);
	if (ret)
		return ERR_PTR(ret);
	for (u32 i = 0; i < 2; i++) {
		struct resource *res = platform_get_resource_byname(pdev, IORESOURCE_MEM, names[i]);

		if (!res || res->start != addresses[i] || resource_size(res) != 0x1000)
			return ERR_PTR(-EINVAL);
		mapping = devm_ioremap_resource(dev, res);
		if (IS_ERR(mapping))
			return ERR_CAST(mapping);
		if (!i)
			ccif->ap = mapping;
		else
			ccif->md = mapping;
		ccif->irq[i] = platform_get_irq(pdev, i);
		if (ccif->irq[i] < 0)
			return ERR_PTR(ccif->irq[i]);
		if (irq_get_trigger_type(ccif->irq[i]) != IRQ_TYPE_LEVEL_HIGH ||
		    (i && ccif->irq[i] == ccif->irq[0]))
			return ERR_PTR(-EINVAL);
		if (!irq_get_irq_data(ccif->irq[i]) ||
		    irqd_to_hwirq(irq_get_irq_data(ccif->irq[i])) != 226 + i)
			return ERR_PTR(-EINVAL);
	}
	/* These adjacent unaligned subregions share a page: map them together WC. */
	if (normal->ap + normal->size != exception->ap ||
	    (u64)normal->size + exception->size > U32_MAX)
		return ERR_PTR(-ERANGE);
	if (!devm_request_mem_region(dev, normal->ap, normal->size + exception->size,
				     dev_name(dev)))
		return ERR_PTR(-EBUSY);
	mapping = devm_ioremap_wc(dev, normal->ap, normal->size + exception->size);
	if (!mapping)
		return ERR_PTR(-ENOMEM);
	ccif->bank[0] = (struct ccci_ccif_bank) { mapping, normal->size };
	ccif->bank[1] = (struct ccci_ccif_bank) { mapping + normal->size, exception->size };
	for (u32 bank = 0; bank < CCCI_CCIF_BANKS; bank++) {
		for (u32 q = 0; q < CCCI_CCIF_QUEUES; q++) {
			ret = ccci_ccif_ring_init(&ccif->ring[bank][q], &ccif_ring_ops,
						  &ccif->bank[bank], &ccif->layout.slots[bank][q],
						  ccif->bank[bank].size);
			if (ret)
				return ERR_PTR(ret);
		}
	}
	ccif->rx = ccci_ccif_rx_create(dev, ccif->ap, ccif->ring[CCCI_CCIF_NORMAL], ccif->irq);
	if (IS_ERR(ccif->rx))
		return ERR_CAST(ccif->rx);
	ccif->control = ccci_ccif_control_create(dev, ccif->ap, ccif->rx, smem);
	if (IS_ERR(ccif->control))
		return ERR_CAST(ccif->control);
	/* Clear after devm_free_irq on failure, since actions unwind in reverse. */
	ret = devm_add_action_or_reset(dev, ccif_irq_flags_clear, ccif);
	if (ret)
		return ERR_PTR(ret);
	for (u32 i = 0; i < 2; i++) {
		irq_set_status_flags(ccif->irq[i], IRQ_DISABLE_UNLAZY);
		ret = devm_request_irq(dev, ccif->irq[i], ccif_irq, IRQF_NO_AUTOEN,
				       names[i], ccif);
		if (ret)
			return ERR_PTR(ret);
	}
	return ccif;
}
