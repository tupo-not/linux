// SPDX-License-Identifier: GPL-2.0-only
/* MT6893 RV mailbox layout and CCCI wire format; no hardware operations. */

#include <linux/errno.h>
#include <linux/limits.h>
#include <linux/string.h>
#include <linux/unaligned.h>

#include "ccci-scp-protocol.h"

static int ccci_scp_add_pins(const u32 *cells, u32 count, bool receive,
			     u32 mboxes, struct ccci_scp_pin *pins,
			     u8 *used_words)
{
	u32 i, j, stride = receive ? 4 : 3;
	u32 channel, mbox, words, padded;

	for (i = 0; i < count; i++, cells += stride) {
		channel = cells[0];
		mbox = cells[1];
		words = cells[2];
		if (channel >= CCCI_SCP_MAX_CHANNELS || mbox >= mboxes ||
		    !words || words > CCCI_SCP_MBOX_WORDS ||
		    (receive && cells[3] > 1))
			return -EINVAL;
		/* The vendor layout stops searching at the next mailbox. */
		if (i && mbox < pins[i - 1].mbox)
			return -EINVAL;
		for (j = 0; j < i; j++)
			if (pins[j].channel == channel)
				return -EEXIST;

		padded = (words + 1) & ~1U;
		if (padded > CCCI_SCP_MBOX_WORDS - used_words[mbox])
			return -E2BIG;
		pins[i].channel = channel;
		pins[i].mbox = mbox;
		pins[i].words = words;
		pins[i].offset = used_words[mbox];
		pins[i].irq = used_words[mbox] / 2;
		pins[i].response = receive ? cells[3] : 0;
		used_words[mbox] += padded;
	}
	return 0;
}

int ccci_scp_build_layout(const u32 *tx, size_t tx_cells,
			  const u32 *rx, size_t rx_cells, u32 mboxes,
			  struct ccci_scp_layout *layout)
{
	int ret;

	if (!layout)
		return -EINVAL;
	memset(layout, 0, sizeof(*layout));
	if (!tx || !rx || !tx_cells || !rx_cells || tx_cells % 3 || rx_cells % 4 ||
	    tx_cells / 3 > CCCI_SCP_MAX_CHANNELS ||
	    rx_cells / 4 > CCCI_SCP_MAX_CHANNELS ||
	    !mboxes || mboxes > CCCI_SCP_MAX_MBOXES)
		return -EINVAL;

	ret = ccci_scp_add_pins(tx, tx_cells / 3, false, mboxes, layout->tx,
				layout->used_words);
	if (!ret)
		ret = ccci_scp_add_pins(rx, rx_cells / 4, true, mboxes, layout->rx,
					layout->used_words);
	if (ret) {
		memset(layout, 0, sizeof(*layout));
		return ret;
	}
	layout->tx_count = tx_cells / 3;
	layout->rx_count = rx_cells / 4;
	return 0;
}

int ccci_scp_apccci_pins(const struct ccci_scp_layout *layout,
			 const struct ccci_scp_pin **tx,
			 const struct ccci_scp_pin **rx)
{
	const struct ccci_scp_pin *send = NULL, *recv = NULL;
	u32 i;

	if (!tx || !rx)
		return -EINVAL;
	*tx = NULL;
	*rx = NULL;
	if (!layout || layout->tx_count > CCCI_SCP_MAX_CHANNELS ||
	    layout->rx_count > CCCI_SCP_MAX_CHANNELS)
		return -EINVAL;
	for (i = 0; i < layout->tx_count; i++)
		if (layout->tx[i].channel == CCCI_SCP_APCCCI_TX)
			send = &layout->tx[i];
	for (i = 0; i < layout->rx_count; i++)
		if (layout->rx[i].channel == CCCI_SCP_APCCCI_RX)
			recv = &layout->rx[i];
	if (!send || !recv)
		return -ENOENT;
	if ((send->words != 2 && send->words != 4) || recv->words != 2 ||
	    recv->response)
		return -EPROTONOSUPPORT;
	*tx = send;
	*rx = recv;
	return 0;
}

int ccci_scp_shm_u32(u64 base, u64 size, u32 *address)
{
	if (!address)
		return -EINVAL;
	*address = 0;
	if (!base || !size || (base & 3) || (size & 3))
		return -EINVAL;
	if (base > U32_MAX || size - 1 > U32_MAX - base)
		return -ERANGE;
	*address = base;
	return 0;
}

int ccci_scp_encode(void *buffer, size_t capacity, u32 words, u16 op, u32 data)
{
	u8 message[16] = {};
	u32 length;

	if (!buffer || (words != 2 && words != 4))
		return -EINVAL;
	if (op != CCCI_SCP_OP_MD_STATE && op != CCCI_SCP_OP_SHM_INIT)
		return -EOPNOTSUPP;
	if ((op == CCCI_SCP_OP_MD_STATE && data > 3) ||
	    (op == CCCI_SCP_OP_SHM_INIT && (!data || (data & 3))))
		return -EINVAL;
	length = words * sizeof(u32);
	if (capacity < length)
		return -EMSGSIZE;
	/* MD1 uses zero-based md_id 0, not the user-visible modem number. */
	put_unaligned_le16(op, message + 2);
	put_unaligned_le32(data, message + 4);
	if (words == 4) {
		put_unaligned_le32(0xabcddcba, message + 8);
		put_unaligned_le32(0xaabbccdd, message + 12);
	}
	memcpy(buffer, message, length);
	return length;
}

int ccci_scp_decode_state(const void *buffer, size_t length, u32 *state)
{
	const u8 *message = buffer;
	u32 value;

	if (!state)
		return -EINVAL;
	*state = 0;
	if (!buffer || length != 8)
		return -EMSGSIZE;
	if (get_unaligned_le16(message) ||
	    get_unaligned_le16(message + 2) != CCCI_SCP_OP_STATE)
		return -EPROTO;
	value = get_unaligned_le32(message + 4);
	if (value < CCCI_SCP_STATE_BOOTING || value > CCCI_SCP_STATE_STOP)
		return -EPROTO;
	*state = value;
	return 0;
}
