/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef __CCCI_SCP_MMIO_H
#define __CCCI_SCP_MMIO_H

#include "ccci-scp-rx.h"

struct platform_device;
struct ccci_scp_mmio;

/*
 * Reserve six complete 4 KiB resources named scp-cfg and mbox0..4. Use
 * non-posted mappings without fallback; do not mix with the old cfg mapping
 * or pending observer. No register access. On failure the caller MUST abort
 * probe so devres releases any partial claims, not continue or retry.
 * The resource owner's separate MMIO_RESERVE opt-in only maps these pages;
 * it does not prepare a generation or invoke the RX operations.
 */
int ccci_scp_mmio_reserve(struct platform_device *pdev,
			  struct ccci_scp_mmio **result);

/*
 * All operations, including callbacks, use the unique owner's serialization.
 * check verifies clock/vote/lifetime and the owned reset generation, NOT READY
 * (READY itself needs RX). It must not reenter this backend or change mappings.
 * Caller must establish a real new reset generation, reject old messages, and
 * quiesce IRQ/work/clients before prepare. A numeric increment is insufficient.
 * prepare performs no I/O/check callback. Layout must be an immutable valid
 * build_layout result. Invalidate is mandatory on any surrounding drain fault.
 */
int ccci_scp_mmio_prepare(struct ccci_scp_mmio *mmio,
			  const struct ccci_scp_layout *layout, u64 generation,
			  int (*check)(void *context, u64 generation), void *context);
void ccci_scp_mmio_invalidate(struct ccci_scp_mmio *mmio);

/* Owner lock held, receive STOP and both cores asserted throughout callback. */
int ccci_scp_mmio_discard(struct ccci_scp_mmio *mmio,
			  const struct ccci_scp_layout *layout,
			  int (*check)(void *context), void *context, u32 *discarded);
int ccci_scp_mmio_tx_idle(struct ccci_scp_mmio *mmio);

/* Owner holds READY, AP awake and serialization; bounded MD1 APCCCI messages. */
int ccci_scp_mmio_apccci(struct ccci_scp_mmio *mmio,
			 const struct ccci_scp_layout *layout, u64 generation, u16 op, u32 data);
int ccci_scp_mmio_sleep_off(struct ccci_scp_mmio *mmio,
			    const struct ccci_scp_layout *layout, u64 generation);
/* Compatibility entry for the original one-shot MD-invalid test. */
int ccci_scp_mmio_md_invalid(struct ccci_scp_mmio *mmio,
			     const struct ccci_scp_layout *layout, u64 generation);

/* Owner serialization required; report software state without register I/O. */
ssize_t ccci_scp_mmio_status(struct ccci_scp_mmio *mmio, char *buf);
ssize_t ccci_scp_mmio_cfg_status(struct ccci_scp_mmio *mmio, char *buf);

/*
 * Only for queue_init: deliver is NULL, never a direct rx_init destination.
 * Port contexts persist across prepares; they are NOT generation tokens.
 * Owner/drain must reject stale queued generations before invoking these ops.
 */
const struct ccci_scp_rx_ops *ccci_scp_mmio_ops(void);
void *ccci_scp_mmio_port(struct ccci_scp_mmio *mmio, u32 mbox);

#endif
