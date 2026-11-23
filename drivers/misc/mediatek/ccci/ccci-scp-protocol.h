/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef __CCCI_SCP_PROTOCOL_H
#define __CCCI_SCP_PROTOCOL_H

#include <linux/types.h>

#define CCCI_SCP_MAX_MBOXES	5
#define CCCI_SCP_MAX_CHANNELS	64
#define CCCI_SCP_MBOX_WORDS	64
#define CCCI_SCP_APCCCI_TX	3
#define CCCI_SCP_APCCCI_RX	7

#define CCCI_SCP_OP_STATE	0
#define CCCI_SCP_OP_MD_STATE	1
#define CCCI_SCP_OP_SHM_INIT	2

#define CCCI_SCP_STATE_BOOTING	1
#define CCCI_SCP_STATE_RBREADY	2
#define CCCI_SCP_STATE_STOP	3

struct ccci_scp_pin {
	u32 channel;
	u8 mbox;
	u8 words;
	u8 offset;
	u8 irq;
	u8 response;
};

struct ccci_scp_layout {
	struct ccci_scp_pin tx[CCCI_SCP_MAX_CHANNELS];
	struct ccci_scp_pin rx[CCCI_SCP_MAX_CHANNELS];
	u32 tx_count;
	u32 rx_count;
	u8 used_words[CCCI_SCP_MAX_MBOXES];
};

/*
 * Native-endian cells: TX triples (channel, mailbox, words), RX quadruples
 * (channel, mailbox, words, response). Only the MT6893 64-bit layout is handled.
 * Inputs and output must not overlap. Output is cleared on failure.
 */
int ccci_scp_build_layout(const u32 *tx, size_t tx_cells,
			  const u32 *rx, size_t rx_cells, u32 mboxes,
			  struct ccci_scp_layout *layout);

/* Input must be an unmodified successful build_layout() result. */
int ccci_scp_apccci_pins(const struct ccci_scp_layout *layout,
			 const struct ccci_scp_pin **tx,
			 const struct ccci_scp_pin **rx);

/* Check only wire representability, not address translation or ownership. */
int ccci_scp_shm_u32(u64 base, u64 size, u32 *address);

/* Return encoded byte count, or an error leaving the destination unchanged. */
int ccci_scp_encode(void *buffer, size_t capacity, u32 words, u16 op, u32 data);

/* Validates a wire message; RBREADY here is not an accepted FSM transition. */
int ccci_scp_decode_state(const void *buffer, size_t length, u32 *state);

#endif
