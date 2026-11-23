// SPDX-License-Identifier: GPL-2.0-only
/* Non-posted RX register backend; mapping-only caller, no reset or READY. */
#include <linux/bitops.h>
#include <linux/errno.h>
#include <linux/io.h>
#include <linux/delay.h>
#include <linux/of.h>
#include <linux/platform_device.h>
#include <linux/slab.h>
#include <linux/sysfs.h>
#include <linux/unaligned.h>

#include "ccci-scp-mmio.h"

#define CCCI_SCP_MMIO_CFG_BASE	0x10724000
#define CCCI_SCP_MMIO_MBOX_BASE	0x107fb000
#define CCCI_SCP_MMIO_PAGE_BYTES	0x1000
#define CCCI_SCP_MMIO_COMPLETE	0x94
#define CCCI_SCP_MMIO_PENDING	0x10c
#define CCCI_SCP_SLEEP_TX	5
#define CCCI_SCP_SLEEP_IRQ	2

struct ccci_scp_mmio_port {
	struct ccci_scp_mmio *owner;
	void __iomem *base;
	u32 known;
};

struct ccci_scp_mmio {
	void __iomem *cfg;
	struct ccci_scp_mmio_port ports[CCCI_SCP_MAX_MBOXES];
	int (*check)(void *context, u64 generation);
	void *context;
	u64 generation;
	u64 last_generation;
	u64 register_reads;
	u64 register_writes;
	u64 cfg_writes;
	bool prepared;
	bool fault;
};

void ccci_scp_mmio_invalidate(struct ccci_scp_mmio *mmio)
{
	if (!mmio)
		return;
	mmio->prepared = false;
	mmio->fault = true;
	mmio->generation = 0;
}

int ccci_scp_mmio_discard(struct ccci_scp_mmio *mmio,
			  const struct ccci_scp_layout *layout,
			  int (*check)(void *context), void *context, u32 *discarded)
{
	u32 known[CCCI_SCP_MAX_MBOXES] = {};
	u32 pending[CCCI_SCP_MAX_MBOXES], i, remaining;
	int ret;

	if (!mmio || !layout || !check || !discarded || mmio->prepared ||
	    !layout->rx_count || layout->rx_count > CCCI_SCP_MAX_CHANNELS)
		return -EINVAL;
	*discarded = 0;
	for (i = 0; i < layout->rx_count; i++) {
		if (layout->rx[i].mbox >= CCCI_SCP_MAX_MBOXES || layout->rx[i].irq >= 32)
			return -EINVAL;
		known[layout->rx[i].mbox] |= BIT(layout->rx[i].irq);
	}
	ret = check(context);
	if (ret)
		return ret;
	/* Reject unknown sources before touching any notification. No payload read. */
	for (i = 0; i < CCCI_SCP_MAX_MBOXES; i++) {
		pending[i] = readl(mmio->ports[i].base + CCCI_SCP_MMIO_PENDING);
		mmio->register_reads++;
		if (pending[i] & ~known[i])
			return -EPROTO;
	}
	ret = check(context);
	if (ret)
		return ret;
	for (i = 0; i < CCCI_SCP_MAX_MBOXES; i++) {
		if (!pending[i])
			continue;
		writel(1, mmio->cfg + CCCI_SCP_MMIO_COMPLETE);
		/* Complete the board callback before the mailbox W1C. */
		mb();
		writel(pending[i], mmio->ports[i].base + CCCI_SCP_MMIO_PENDING);
		/* Reset remains asserted: readback must observe the completed W1C. */
		mb();
		mmio->register_writes += 2;
		mmio->cfg_writes++;
		*discarded += hweight32(pending[i]);
	}
	for (i = 0; i < CCCI_SCP_MAX_MBOXES; i++) {
		remaining = readl(mmio->ports[i].base + CCCI_SCP_MMIO_PENDING);
		mmio->register_reads++;
		if (remaining)
			return -EAGAIN;
	}
	return check(context);
}

/* Never clear SCP's receive bits from AP. */
int ccci_scp_mmio_tx_idle(struct ccci_scp_mmio *mmio)
{
	mmio->register_reads++;
	return readl(mmio->ports[1].base + 0x100) &
	       (BIT(0) | BIT(CCCI_SCP_SLEEP_IRQ)) ? -EBUSY : 0;
}

static int ccci_scp_mmio_check(struct ccci_scp_mmio_port *port)
{
	struct ccci_scp_mmio *mmio;
	int ret;

	if (!port || !port->owner)
		return -EINVAL;
	mmio = port->owner;
	if (!mmio->prepared || mmio->fault)
		return -ESHUTDOWN;
	ret = mmio->check(mmio->context, mmio->generation);
	if (ret)
		ccci_scp_mmio_invalidate(mmio);
	return ret;
}

static int ccci_scp_mmio_send_pair(struct ccci_scp_mmio_port *port, u32 offset,
				    u32 irq, u32 first, u32 second)
{
	struct ccci_scp_mmio *mmio = port->owner;
	u32 pending, i, mask = BIT(irq);
	int ret;

	ret = ccci_scp_mmio_check(port);
	if (ret)
		return ret;
	/* Vendor reads the SET alias itself; there is no separate status page. */
	pending = readl(port->base + 0x100);
	mmio->register_reads++;
	if (pending & mask)
		return -EBUSY;
	writel(first, port->base + offset * sizeof(u32));
	writel(second, port->base + (offset + 1) * sizeof(u32));
	/* Publish the complete two-word message before notifying SCP. */
	mb();
	writel(mask, port->base + 0x100);
	/* Complete notification before sampling its consumption. */
	mb();
	mmio->register_writes += 3;
	for (i = 0; i < 100; i++) {
		pending = readl(port->base + 0x100);
		mmio->register_reads++;
		if (!(pending & mask))
			return ccci_scp_mmio_check(port);
		usleep_range(1000, 1200);
	}
	/* Uncertain consumption: never retry or clear SCP's receive bit from AP. */
	return -ETIMEDOUT;
}

int ccci_scp_mmio_apccci(struct ccci_scp_mmio *mmio,
			 const struct ccci_scp_layout *layout, u64 generation, u16 op, u32 data)
{
	const struct ccci_scp_pin *tx, *rx;
	u8 message[8];
	int ret;

	if (!mmio || !generation || generation != mmio->generation)
		return -ESTALE;
	ret = ccci_scp_apccci_pins(layout, &tx, &rx);
	if (ret)
		return ret;
	if (tx->mbox != 1 || tx->offset || tx->irq || tx->words != 2)
		return -EPROTO;
	ret = ccci_scp_encode(message, sizeof(message), 2, op, data);
	if (ret != sizeof(message))
		return ret < 0 ? ret : -EPROTO;
	ret = ccci_scp_mmio_send_pair(&mmio->ports[1], 0, 0,
				     get_unaligned_le32(message), get_unaligned_le32(message + 4));
	memzero_explicit(message, sizeof(message));
	return ret;
}

/* SCP_SLEEP_OFF on the same-board DVFS channel, with the unused word zeroed. */
int ccci_scp_mmio_sleep_off(struct ccci_scp_mmio *mmio,
			    const struct ccci_scp_layout *layout, u64 generation)
{
	const struct ccci_scp_pin *tx;

	if (!mmio || !generation || generation != mmio->generation)
		return -ESTALE;
	if (!layout || layout->tx_count > CCCI_SCP_MAX_CHANNELS)
		return -EINVAL;
	for (u32 i = 0; i < layout->tx_count; i++) {
		tx = &layout->tx[i];
		if (tx->channel != CCCI_SCP_SLEEP_TX)
			continue;
		if (tx->mbox != 1 || tx->offset != 4 ||
		    tx->irq != CCCI_SCP_SLEEP_IRQ || tx->words != 2)
			return -EPROTO;
		return ccci_scp_mmio_send_pair(&mmio->ports[1], tx->offset, tx->irq, 0, 0);
	}
	return -ENOENT;
}

int ccci_scp_mmio_md_invalid(struct ccci_scp_mmio *mmio,
			     const struct ccci_scp_layout *layout, u64 generation)
{
	return ccci_scp_mmio_apccci(mmio, layout, generation, CCCI_SCP_OP_MD_STATE, 0);
}

static int ccci_scp_mmio_pending(void *context, u32 *mask)
{
	struct ccci_scp_mmio_port *port = context;
	int ret;

	if (!mask)
		return -EINVAL;
	ret = ccci_scp_mmio_check(port);
	if (ret)
		return ret;
	*mask = readl(port->base + CCCI_SCP_MMIO_PENDING);
	port->owner->register_reads++;
	return 0;
}

static int ccci_scp_mmio_read(void *context, u32 offset, u32 words, void *buffer)
{
	struct ccci_scp_mmio_port *port = context;
	u8 *bytes = buffer;
	u32 i;
	int ret;

	ret = ccci_scp_mmio_check(port);
	if (ret)
		return ret;
	if (!buffer || !words || words > CCCI_SCP_MBOX_WORDS ||
	    offset > CCCI_SCP_MBOX_WORDS - words) {
		ccci_scp_mmio_invalidate(port->owner);
		return -EINVAL;
	}
	/* Never memcpy_fromio: exact aligned 32-bit MMIO, little-endian wire. */
	for (i = 0; i < words; i++) {
		put_unaligned_le32(readl(port->base + (offset + i) * sizeof(u32)),
				   bytes + i * sizeof(u32));
		port->owner->register_reads++;
	}
	return 0;
}

static int ccci_scp_mmio_post(void *context)
{
	struct ccci_scp_mmio_port *port = context;
	int ret;

	ret = ccci_scp_mmio_check(port);
	if (ret)
		return ret;
	/* A/V's RX post callback, not awake or reset's distinct 0xff write. */
	writel(1, port->owner->cfg + CCCI_SCP_MMIO_COMPLETE);
	/* Complete the non-posted store before the different mailbox window. */
	mb();
	port->owner->register_writes++;
	port->owner->cfg_writes++;
	return 0;
}

static int ccci_scp_mmio_ack(void *context, u32 mask)
{
	struct ccci_scp_mmio_port *port = context;
	int ret;

	ret = ccci_scp_mmio_check(port);
	if (ret)
		return ret;
	if (!mask || (mask & ~port->known)) {
		ccci_scp_mmio_invalidate(port->owner);
		return -EINVAL;
	}
	writel(mask, port->base + CCCI_SCP_MMIO_PENDING);
	/* No CLR readback/zero assertion: another message may already be pending. */
	mb();
	port->owner->register_writes++;
	return 0;
}

ssize_t ccci_scp_mmio_status(struct ccci_scp_mmio *mmio, char *buf)
{
	if (!mmio)
		return -EOPNOTSUPP;
	return sysfs_emit(buf,
			 "mapped=6 bytes=24576 nonposted=1 register_reads=%llu register_writes=%llu prepared=%u generation=%llu fault=%u ready_known=0\n",
			 mmio->register_reads, mmio->register_writes, mmio->prepared,
			 mmio->generation, mmio->fault);
}

ssize_t ccci_scp_mmio_cfg_status(struct ccci_scp_mmio *mmio, char *buf)
{
	if (!mmio)
		return -EOPNOTSUPP;
	return sysfs_emit(buf,
			 "reserved=1 bytes=4096 register_reads=0 register_writes=%llu completion_enabled=%u ready_known=0\n",
			 mmio->cfg_writes, mmio->prepared);
}

static const struct ccci_scp_rx_ops ccci_scp_mmio_rx_ops = {
	.pending = ccci_scp_mmio_pending,
	.read_words = ccci_scp_mmio_read,
	.post_capture = ccci_scp_mmio_post,
	.ack = ccci_scp_mmio_ack,
};

const struct ccci_scp_rx_ops *ccci_scp_mmio_ops(void)
{
	return &ccci_scp_mmio_rx_ops;
}

void *ccci_scp_mmio_port(struct ccci_scp_mmio *mmio, u32 mbox)
{
	if (!mmio || mbox >= CCCI_SCP_MAX_MBOXES)
		return NULL;
	return &mmio->ports[mbox];
}

/* Only used to validate layout through rx_init; never passed to a live RX. */
static void ccci_scp_mmio_no_delivery(void *context, const struct ccci_scp_pin *pin,
				      const void *buffer, size_t bytes, u64 generation)
{
}

int ccci_scp_mmio_prepare(struct ccci_scp_mmio *mmio,
			  const struct ccci_scp_layout *layout, u64 generation,
			  int (*check)(void *context, u64 generation), void *context)
{
	struct ccci_scp_rx_ops validator = ccci_scp_mmio_rx_ops;
	u32 known[CCCI_SCP_MAX_MBOXES] = {};
	struct ccci_scp_rx rx;
	u32 i;
	int ret;

	if (!mmio || !layout || !check || !generation)
		return -EINVAL;
	if (mmio->prepared)
		return -EBUSY;
	if (generation <= mmio->last_generation)
		return -ESTALE;
	if (!layout->rx_count || layout->rx_count > CCCI_SCP_MAX_CHANNELS)
		return -EINVAL;
	for (i = 0; i < layout->rx_count; i++) {
		if (layout->rx[i].mbox >= CCCI_SCP_MAX_MBOXES || layout->rx[i].irq >= 32)
			return -EINVAL;
		known[layout->rx[i].mbox] |= BIT(layout->rx[i].irq);
	}
	validator.deliver = ccci_scp_mmio_no_delivery;
	for (i = 0; i < CCCI_SCP_MAX_MBOXES; i++) {
		ret = ccci_scp_rx_init(&rx, layout, i, known[i], generation,
				       &validator, &mmio->ports[i]);
		if (ret)
			return ret;
	}
	for (i = 0; i < CCCI_SCP_MAX_MBOXES; i++)
		mmio->ports[i].known = known[i];
	mmio->check = check;
	mmio->context = context;
	mmio->generation = generation;
	mmio->last_generation = generation;
	mmio->fault = false;
	mmio->prepared = true;
	return 0;
}

int ccci_scp_mmio_reserve(struct platform_device *pdev,
			  struct ccci_scp_mmio **result)
{
	struct resource resources[1 + CCCI_SCP_MAX_MBOXES], *res;
	struct ccci_scp_mmio *mmio;
	void __iomem *mapping;
	char name[16];
	u32 i;

	if (!result)
		return -EINVAL;
	*result = NULL;
	if (!pdev)
		return -EINVAL;
	if (of_property_present(pdev->dev.of_node, "scp-cfgreg-ap-en") ||
	    of_property_present(pdev->dev.of_node, "scp-ipc-wa"))
		return -EOPNOTSUPP;
	/* All descriptions must be valid before acquiring any MMIO resource. */
	for (i = 0; i < ARRAY_SIZE(resources); i++) {
		if (!i)
			snprintf(name, sizeof(name), "scp-cfg");
		else
			snprintf(name, sizeof(name), "mbox%u", i - 1);
		res = platform_get_resource_byname(pdev, IORESOURCE_MEM, name);
		if (!res || resource_size(res) != CCCI_SCP_MMIO_PAGE_BYTES ||
		    res->start != (!i ? CCCI_SCP_MMIO_CFG_BASE :
			CCCI_SCP_MMIO_MBOX_BASE + (i - 1) * CCCI_SCP_MMIO_PAGE_BYTES))
			return -EINVAL;
		resources[i] = *res;
		/* Local copy: never modify OF's shared resource descriptors. */
		resources[i].flags |= IORESOURCE_MEM_NONPOSTED;
	}
	mmio = devm_kzalloc(&pdev->dev, sizeof(*mmio), GFP_KERNEL);
	if (!mmio)
		return -ENOMEM;
	ccci_scp_mmio_invalidate(mmio);
	for (i = 0; i < ARRAY_SIZE(resources); i++) {
		mapping = devm_ioremap_resource(&pdev->dev, &resources[i]);
		if (IS_ERR(mapping))
			return PTR_ERR(mapping);
		if (!i) {
			mmio->cfg = mapping;
		} else {
			mmio->ports[i - 1].base = mapping;
			mmio->ports[i - 1].owner = mmio;
		}
	}
	*result = mmio;
	return 0;
}
