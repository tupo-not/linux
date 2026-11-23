// SPDX-License-Identifier: GPL-2.0-only
/* Bounded SCP RX transaction backend; no MMIO/IRQ owner or READY state. */
#include <linux/bitops.h>
#include <linux/errno.h>
#include <linux/string.h>
#include <linux/unaligned.h>

#include "ccci-scp-rx.h"

int ccci_scp_rx_init(struct ccci_scp_rx *rx,
		     const struct ccci_scp_layout *layout, u32 mbox,
		     u32 supported_mask, u64 generation,
		     const struct ccci_scp_rx_ops *ops, void *context)
{
	u64 occupied = 0, channels = 0, span;
	u32 known = 0, padded, i;
	const struct ccci_scp_pin *pin;

	if (!rx)
		return -EINVAL;
	memset(rx, 0, sizeof(*rx));
	/* A failed initialization must not leave an executable instance. */
	rx->fault = true;
	if (!layout || !layout->rx_count ||
	    layout->rx_count > CCCI_SCP_MAX_CHANNELS ||
	    mbox >= CCCI_SCP_MAX_MBOXES || !generation || !supported_mask ||
	    !ops || !ops->pending || !ops->read_words || !ops->post_capture ||
	    !ops->ack || !ops->deliver)
		return -EINVAL;
	for (i = 0; i < layout->rx_count; i++) {
		pin = &layout->rx[i];
		if (pin->mbox != mbox)
			continue;
		if (pin->channel >= CCCI_SCP_MAX_CHANNELS || !pin->words ||
		    pin->words > CCCI_SCP_MBOX_WORDS || pin->offset & 1 ||
		    pin->irq >= 32 || pin->irq != pin->offset / 2 || pin->response > 1)
			return -EINVAL;
		padded = (pin->words + 1) & ~1U;
		if (pin->offset > CCCI_SCP_MBOX_WORDS - padded)
			return -ERANGE;
		span = padded == 64 ? ~0ULL : ((1ULL << padded) - 1) << pin->offset;
		if ((known & BIT(pin->irq)) || (occupied & span) ||
		    (channels & BIT_ULL(pin->channel)))
			return -EEXIST;
		occupied |= span;
		channels |= BIT_ULL(pin->channel);
		known |= BIT(pin->irq);
	}
	if (supported_mask & ~known)
		return -EINVAL;
	rx->layout = layout;
	rx->ops = ops;
	rx->context = context;
	rx->generation = generation;
	rx->known_mask = known;
	rx->supported_mask = supported_mask;
	rx->mbox = mbox;
	rx->fault = false;
	return 0;
}

void ccci_scp_rx_invalidate(struct ccci_scp_rx *rx)
{
	if (rx)
		rx->fault = true;
}

int ccci_scp_rx_step(struct ccci_scp_rx *rx, struct ccci_scp_rx_result *result)
{
	u8 snapshot[CCCI_SCP_MBOX_WORDS * sizeof(u32)];
	const struct ccci_scp_pin *pin;
	u32 i, mask;
	int ret;

	if (!result)
		return -EINVAL;
	memset(result, 0, sizeof(*result));
	if (!rx || rx->fault || !rx->layout || !rx->ops)
		return -ESHUTDOWN;
	ret = rx->ops->pending(rx->context, &result->pending);
	if (ret)
		goto fault;
	mask = result->pending;
	if (!mask)
		return 0;
	result->unexpected = mask & ~rx->supported_mask;
	if (result->unexpected) {
		ret = -EOPNOTSUPP;
		goto fault;
	}
	/* Capture every pending pin before giving SCP permission to overwrite. */
	for (i = 0; i < rx->layout->rx_count; i++) {
		pin = &rx->layout->rx[i];
		if (pin->mbox != rx->mbox || !(mask & BIT(pin->irq)))
			continue;
		ret = rx->ops->read_words(rx->context, pin->offset, pin->words,
					 snapshot + pin->offset * sizeof(u32));
		if (ret)
			goto fault;
		result->copied |= BIT(pin->irq);
	}
	result->post_attempted = true;
	ret = rx->ops->post_capture(rx->context);
	if (ret)
		goto fault;
	result->ack_attempted = true;
	ret = rx->ops->ack(rx->context, result->copied);
	if (ret)
		goto fault;
	result->acknowledged = result->copied;
	for (i = 0; i < rx->layout->rx_count; i++) {
		pin = &rx->layout->rx[i];
		if (pin->mbox != rx->mbox || !(mask & BIT(pin->irq)))
			continue;
		rx->ops->deliver(rx->context, pin,
				 snapshot + pin->offset * sizeof(u32),
				 pin->words * sizeof(u32), rx->generation);
		result->delivered |= BIT(pin->irq);
	}
	return 0;
fault:
	rx->fault = true;
	return ret;
}

int ccci_scp_decode_ready(const struct ccci_scp_pin *pin, const void *buffer,
			  size_t length, u32 expected_tcm_bytes)
{
	/* A/V register the same READY handler for channels 9 and 22. */
	if (!pin || (pin->channel != 9 && pin->channel != 22) ||
	    pin->words != 1 || pin->response)
		return -EPROTONOSUPPORT;
	if (!buffer || length != sizeof(u32))
		return -EMSGSIZE;
	if (!expected_tcm_bytes || expected_tcm_bytes & 3)
		return -EINVAL;
	if (get_unaligned_le32(buffer) != expected_tcm_bytes)
		return -EPROTO;
	return 0;
}
