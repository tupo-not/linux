// SPDX-License-Identifier: GPL-2.0-only
/* 6297 ring geometry and wire format from the same-board MediaTek CCCI ABI. */
#include <linux/errno.h>
#include <linux/kernel.h>
#include <linux/string.h>
#include <linux/unaligned.h>

#include "ccci-ccif-ring.h"
#include "ccci-smem.h"

#define RING_OVERHEAD	40
#define PACKET_HEADER	0xaabbaabb
#define PACKET_FOOTER	0xccddeeff
#define TX_READ_RETRIES	4

/* KiB, including zero-sized slots. 6298 uses a different table. */
static const u16 ccif_sizes[CCCI_CCIF_BANKS][2][CCCI_CCIF_QUEUES] = {
	{
		{ 80, 80, 40, 80, 20, 20, 64, 0, 8 },
		{ 128, 40, 8, 40, 20, 20, 64, 0, 8 },
	}, {
		{ 12, 32, 8, 0, 0, 0, 8 },
		{ 12, 32, 8, 0, 0, 0, 8 },
	},
};

int ccci_ccif_plan(const struct ccci_smem_layout *smem, struct ccci_ccif_layout *layout)
{
	static const u32 ids[] = { CCCI_SMEM_CCISM_MCU, CCCI_SMEM_CCISM_EXP };
	const struct ccci_smem_region *debug;

	if (!layout)
		return -EINVAL;
	memset(layout, 0, sizeof(*layout));
	if (!smem)
		return -EINVAL;
	debug = &smem->users[CCCI_SMEM_MDSS_DBG];
	if (!debug->present || debug->cached || debug->no_map || !debug->size ||
	    (u64)debug->md + debug->size > 0x100000000ULL)
		return -EINVAL;
	for (u32 bank = 0; bank < CCCI_CCIF_BANKS; bank++) {
		const struct ccci_smem_region *region = &smem->users[ids[bank]];

		if (!region->present || region->cached || region->no_map)
			goto invalid;
		for (u32 q = 0; q < CCCI_CCIF_QUEUES; q++) {
			struct ccci_ccif_slot *slot = &layout->slots[bank][q];

			slot->offset = layout->used[bank];
			slot->rx = ccif_sizes[bank][0][q] * 1024;
			slot->tx = ccif_sizes[bank][1][q] * 1024;
			slot->span = RING_OVERHEAD + slot->rx + slot->tx;
			if (slot->offset > region->size ||
			    slot->span > region->size - slot->offset)
				goto invalid;
			layout->used[bank] += slot->span;
		}
	}
	layout->debug_md = debug->md;
	layout->debug_size = debug->size;
	return 0;
invalid:
	memset(layout, 0, sizeof(*layout));
	return -EINVAL;
}

int ccci_ccif_ring_init(struct ccci_ccif_ring *ring, const struct ccci_ccif_ring_ops *ops,
			void *context, const struct ccci_ccif_slot *slot, u32 bank_size)
{
	if (!ring || !ops || !slot || !ops->read32 || !ops->write32 || !ops->copy_from ||
	    !ops->copy_to || !ops->zero || !ops->fence)
		return -EINVAL;
	if ((slot->offset | slot->rx | slot->tx) & 7 ||
	    (u64)slot->rx + slot->tx + RING_OVERHEAD != slot->span ||
	    slot->offset > bank_size || slot->span > bank_size - slot->offset)
		return -ERANGE;
	*ring = (struct ccci_ccif_ring) { .ops = ops, .context = context, .slot = *slot };
	return 0;
}

static int ring_put(struct ccci_ccif_ring *r, u32 offset, u32 value)
{
	return r->ops->write32(r->context, r->slot.offset + offset, value);
}

static int ring_get(struct ccci_ccif_ring *r, u32 offset, u32 *value)
{
	return r->ops->read32(r->context, r->slot.offset + offset, value);
}

int ccci_ccif_ring_format(struct ccci_ccif_ring *r)
{
	int ret;

	ret = r->ops->zero(r->context, r->slot.offset, r->slot.span);
	if (ret)
		return ret;
	ret = ring_put(r, 0, CCCI_CCIF_RING_HEADER);
	if (!ret)
		ret = ring_put(r, 4, CCCI_CCIF_RING_HEADER);
	if (!ret)
		ret = ring_put(r, 16, r->slot.rx);
	if (!ret)
		ret = ring_put(r, 28, r->slot.tx);
	if (!ret)
		ret = ring_put(r, r->slot.span - 8, CCCI_CCIF_RING_FOOTER);
	if (!ret)
		ret = ring_put(r, r->slot.span - 4, CCCI_CCIF_RING_FOOTER);
	if (!ret)
		r->ops->fence(r->context, true);
	return ret;
}

int ccci_ccif_ring_verify_empty(struct ccci_ccif_ring *r)
{
	u32 value;
	int ret;

	r->ops->fence(r->context, false);
	for (u32 offset = 0; offset < 32; offset += 4) {
		u32 expected = offset < 8 ? CCCI_CCIF_RING_HEADER :
			       offset == 16 ? r->slot.rx : offset == 28 ? r->slot.tx : 0;

		ret = ring_get(r, offset, &value);
		if (ret)
			return ret;
		if (value != expected)
			return -EIO;
	}
	for (u32 offset = r->slot.span - 8; offset < r->slot.span; offset += 4) {
		ret = ring_get(r, offset, &value);
		if (ret)
			return ret;
		if (value != CCCI_CCIF_RING_FOOTER)
			return -EIO;
	}
	return 0;
}

static int ring_bad_control(struct ccci_ccif_ring *r, bool tx, u32 offset, u32 value,
			    u32 expected)
{
	if (r->ops->bad_control)
		r->ops->bad_control(r->context, tx, r->slot.offset + offset, value, expected);
	return -EBADMSG;
}

static int ring_control(struct ccci_ccif_ring *r, bool tx, u32 *read, u32 *write)
{
	u32 value, control = tx ? 20 : 8, length = tx ? r->slot.tx : r->slot.rx;
	int ret;

	if (!length)
		return -EOPNOTSUPP;
	for (u32 i = 0; i < 4; i++) {
		u32 offset = i < 2 ? i * 4 : r->slot.span - 8 + (i - 2) * 4;
		u32 expected = i < 2 ? CCCI_CCIF_RING_HEADER : CCCI_CCIF_RING_FOOTER;

		ret = ring_get(r, offset, &value);
		if (ret)
			return ret;
		if (value != expected)
			return ring_bad_control(r, tx, offset, value, expected);
	}
	ret = ring_get(r, control, read);
	if (!ret)
		ret = ring_get(r, control + 4, write);
	if (!ret)
		ret = ring_get(r, control + 8, &value);
	if (ret)
		return ret;
	if (value != length)
		return ring_bad_control(r, tx, control + 8, value, length);
	if (*write >= length || (*write & 7))
		return ring_bad_control(r, tx, control + 4, *write, length);
	if (*read >= length || (*read & 7)) {
		if (tx && *read < length && (*read & 7) == 4 &&
		    *read - 4 != *write) {
			unsigned int retries = 0;
			u32 sample = 0;
			u32 next = *read + 4;

			/* Accept only the next slot boundary actually read from MD. */
			if (next == length)
				next = 0;
			ret = ring_get(r, control, &sample);
			/* A changing but unexpected value is not safe to retry. */
			while (!ret && sample == *read && r->ops->wait_tx_read &&
			       retries < TX_READ_RETRIES) {
				r->ops->wait_tx_read(r->context);
				retries++;
				ret = ring_get(r, control, &sample);
			}
			if (r->ops->bad_tx_read)
				r->ops->bad_tx_read(r->context, r->slot.offset + control,
						    *read, *write, length, ret, sample, retries);
			if (ret || sample != next)
				return -EBADMSG;
			*read = sample;
		} else {
			return ring_bad_control(r, tx, control, *read, length);
		}
	}
	r->ops->fence(r->context, false);
	return 0;
}

int ccci_ccif_ring_tx_positions(struct ccci_ccif_ring *r, u32 *read, u32 *write)
{
	return ring_control(r, true, read, write);
}

static u32 ring_advance(u32 pos, u32 size, u32 length)
{
	/* size <= length and pos < length; use subtraction to avoid overflow. */
	return size < length - pos ? pos + size : size - (length - pos);
}

static int ring_copy(struct ccci_ccif_ring *r, bool tx, u32 pos, void *data, u32 size)
{
	u32 length = tx ? r->slot.tx : r->slot.rx;
	u32 base = r->slot.offset + 32 + (tx ? r->slot.rx : 0);
	u32 first = min(size, length - pos);
	int ret;

	if (tx)
		ret = r->ops->copy_to(r->context, base + pos, data, first);
	else
		ret = r->ops->copy_from(r->context, base + pos, data, first);
	if (ret || size == first)
		return ret;
	if (tx)
		return r->ops->copy_to(r->context, base, data + first, size - first);
	return r->ops->copy_from(r->context, base, data + first, size - first);
}

int ccci_ccif_ring_receive(struct ccci_ccif_ring *r, void *data, u32 capacity, u32 *size)
{
	u8 header[8], footer[8];
	u32 read, write, available, length, span, payload;
	int ret;

	if (!size)
		return -EINVAL;
	*size = 0;
	if (!data)
		return -EINVAL;
	ret = ring_control(r, false, &read, &write);
	if (ret)
		return ret;
	length = r->slot.rx;
	available = write >= read ? write - read : length - read + write;
	if (!available)
		return -ENODATA;
	if (available < 32)
		return -EAGAIN;
	ret = ring_copy(r, false, read, header, sizeof(header));
	if (ret)
		return ret;
	payload = get_unaligned_le32(header + 4);
	if (get_unaligned_le32(header) != PACKET_HEADER || payload < 16 ||
	    length < 32 || payload > length - 16)
		return -EBADMSG;
	span = ALIGN(payload, 8) + 16;
	if (span >= length)
		return -EBADMSG;
	if (span > available)
		return -EAGAIN;
	if (payload > capacity)
		return -ENOSPC;
	ret = ring_copy(r, false, ring_advance(read, span - 8, length), footer, sizeof(footer));
	if (ret)
		return ret;
	if (get_unaligned_le32(footer) != PACKET_FOOTER ||
	    get_unaligned_le32(footer + 4) != PACKET_FOOTER)
		return -EBADMSG;
	ret = ring_copy(r, false, ring_advance(read, 8, length), data, payload);
	if (ret)
		return ret;
	/* Publish AP's pointer only after the complete packet has CPU storage. */
	r->ops->fence(r->context, true);
	ret = ring_put(r, 8, ring_advance(read, span, length));
	if (!ret)
		*size = payload;
	return ret;
}

int ccci_ccif_ring_transmit(struct ccci_ccif_ring *r, const void *data, u32 size)
{
	u8 header[8], footer[8], padding[8] = {};
	u32 read, write, used, length, span, pos;
	int ret;

	if (!data || size < 16 || r->slot.tx < 32 || size > r->slot.tx - 16)
		return -EINVAL;
	ret = ring_control(r, true, &read, &write);
	if (ret)
		return ret;
	length = r->slot.tx;
	used = write >= read ? write - read : length - read + write;
	span = ALIGN(size, 8) + 16;
	/* Retain vendor's strict writable-size comparison and empty/full gap. */
	if (span >= length - used - 1)
		return -ENOSPC;
	put_unaligned_le32(PACKET_HEADER, header);
	put_unaligned_le32(size, header + 4);
	put_unaligned_le32(PACKET_FOOTER, footer);
	put_unaligned_le32(PACKET_FOOTER, footer + 4);
	ret = ring_copy(r, true, write, header, sizeof(header));
	if (ret)
		return ret;
	pos = ring_advance(write, 8, length);
	ret = ring_copy(r, true, pos, (void *)data, size);
	if (ret)
		return ret;
	if (size & 7) {
		ret = ring_copy(r, true, ring_advance(pos, size, length), padding,
				8 - (size & 7));
		if (ret)
			return ret;
	}
	ret = ring_copy(r, true, ring_advance(write, span - 8, length), footer, sizeof(footer));
	if (ret)
		return ret;
	r->ops->fence(r->context, true);
	return ring_put(r, 24, ring_advance(write, span, length));
}
