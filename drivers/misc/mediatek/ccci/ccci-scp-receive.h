/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef __CCCI_SCP_RECEIVE_H
#define __CCCI_SCP_RECEIVE_H

#include "ccci-scp-queue.h"

struct platform_device;
struct mutex;
struct ccci_scp_mmio;
struct ccci_scp_receive;

/*
 * The optional resource owner REPLACES the legacy IRQ reservation; never request
 * both. Constructor claims five NO_AUTOEN IRQs, with no register I/O
 * or check callback. MMIO and owner mutex must outlive this managed instance.
 * check runs in process context under owner mutex, validates reset and clock/vote/
 * lifetime, never READY, and must not reenter receive APIs or release resources.
 */
int ccci_scp_receive_reserve(struct platform_device *pdev, struct mutex *owner_lock,
			     struct ccci_scp_mmio *mmio,
			     int (*check)(void *context, u64 generation), void *context,
			     struct ccci_scp_receive **result);

/*
 * Process context, WITHOUT owner mutex. These take an internal control mutex.
 * Start requires actual owned reset/old-message isolation, not a numeric bump.
 * It prepares all RX destinations and queues an initial bounded drain; success
 * means scheduled, never READY. Once a start attempt enters preparation, even
 * a failure consumes its generation and requires STOP before a newer attempt.
 * No sysfs start/arm/reset entry exists.
 */
int ccci_scp_receive_start(struct ccci_scp_receive *receive,
			   const struct ccci_scp_layout *layout, u64 generation, u32 budget);
void ccci_scp_receive_stop(struct ccci_scp_receive *receive);

/*
 * Process context WITHOUT owner lock, only while STOP/quiesced. Optional
 * consumer runs under owner lock; no receive reentry or lock inversion.
 * event=NULL reports a contained receive fault. Data is transient CPU storage.
 */
int ccci_scp_receive_consumer(struct ccci_scp_receive *receive,
			      int (*consume)(void *context,
					     const struct ccci_scp_event *event, int error),
			      void *context);

/* CPU event copy only; no client callback/export/READY transition. */
int ccci_scp_receive_pop(struct ccci_scp_receive *receive, u64 generation,
			 struct ccci_scp_event *event);
ssize_t ccci_scp_receive_status(struct ccci_scp_receive *receive, char *buf);

#endif
