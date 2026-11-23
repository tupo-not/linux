/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef __CCCI_SCP_RX_H
#define __CCCI_SCP_RX_H

#include "ccci-scp-protocol.h"

/*
 * Callbacks run in process context under the owner's serialization, without
 * reentry. Prerequisite checks may sleep; never invoke RX from a hard IRQ.
 * read_words copies exactly words*4 wire bytes into a CPU buffer, using ordered
 * MMIO reads. post_capture performs the board RX completion after all copies
 * and before ack (the vendor SCP-to-SPM clear); this is not AP awake.
 * ack writes only mask, completes the ordered write and returns 0.
 * It must not require the bits to stay clear: a new message may already arrive.
 * deliver cannot fail; it must copy transient data before returning. No READY
 * transition is performed here. A future IRQ owner supplies these operations.
 */
struct ccci_scp_rx_ops {
	int (*pending)(void *context, u32 *mask);
	int (*read_words)(void *context, u32 offset, u32 words, void *buffer);
	int (*post_capture)(void *context);
	int (*ack)(void *context, u32 mask);
	void (*deliver)(void *context, const struct ccci_scp_pin *pin,
			const void *buffer, size_t bytes, u64 generation);
};

struct ccci_scp_rx {
	const struct ccci_scp_layout *layout;
	const struct ccci_scp_rx_ops *ops;
	void *context;
	u64 generation;
	u32 known_mask;
	u32 supported_mask;
	u8 mbox;
	bool fault;
};

struct ccci_scp_rx_result {
	u32 pending;
	u32 unexpected;
	u32 copied;
	u32 acknowledged;
	u32 delivered;
	bool post_attempted;
	bool ack_attempted;
};

/*
 * No I/O. layout must be an unmodified successful build_layout() result and,
 * like ops/context, outlive this instance. Initialization failure clears fields and faults rx.
 * supported_mask names pins with destinations already reserved by the owner.
 * Caller establishes a new nonzero reset generation with IRQ masked/synced;
 * it must never reinitialize merely to retry an uncertain acknowledgment.
 */
int ccci_scp_rx_init(struct ccci_scp_rx *rx,
		     const struct ccci_scp_layout *layout, u32 mbox,
		     u32 supported_mask, u64 generation,
		     const struct ccci_scp_rx_ops *ops, void *context);

/*
 * One pending sample, copies, board completion, exact ack, then delivery.
 * Any operational error is sticky until a new owned reset generation; the IRQ
 * wrapper must mask/contain the source on failure, not spin/retry or clear all.
 * Owner serializes this with STOP/reset, clock control and client lifetime.
 * No IRQ wrapper, clock/awake operation or hardware caller is registered here.
 */
int ccci_scp_rx_step(struct ccci_scp_rx *rx, struct ccci_scp_rx_result *result);
void ccci_scp_rx_invalidate(struct ccci_scp_rx *rx);

/* Exact READY0/READY1 payload; success is not a generation/READY acceptance. */
int ccci_scp_decode_ready(const struct ccci_scp_pin *pin, const void *buffer,
			  size_t length, u32 expected_tcm_bytes);

#endif
