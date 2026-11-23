/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef __CCCI_CCIF_H
#define __CCCI_CCIF_H

#include <linux/types.h>
#include <linux/kconfig.h>
#include <linux/errno.h>

#include "ccci-ccif-rx.h"

struct platform_device;
struct ccci_smem_layout;
struct regmap;
struct ccci_ccif;
struct ccci_runtime_offer;

#if IS_ENABLED(CONFIG_OP6893_CCCI_CCIF)
struct ccci_ccif *ccci_ccif_create(struct platform_device *pdev,
				   const struct ccci_smem_layout *smem, struct regmap *infra);
/* Owner mutex held; powered domain, clocks held, MD CPU not released. */
int ccci_ccif_prepare(struct ccci_ccif *ccif);
/* Same power/lifecycle lock; callback restrictions are in ccci-ccif-rx.h. */
int ccci_ccif_receive_start(struct ccci_ccif *ccif, ccci_ccif_consume_fn consume, void *context);
/* MD CPU stopped; caller must establish real SCP/service prerequisites first. */
int ccci_ccif_handshake_start(struct ccci_ccif *ccif, const struct ccci_runtime_offer *offers,
			      ccci_ccif_consume_fn port, void *context);
ssize_t ccci_ccif_handshake_status(struct ccci_ccif *ccif, char *buf);
/* Pre-start only; callback may enqueue work but must not acquire mutexes. */
int ccci_ccif_set_fault(struct ccci_ccif *ccif,
			void (*fault)(void *context, int error), void *context);
/*
 * Sleepable q0/channel 3 CCISM messages only (0x119/0x11b), after real HS2.
 * Caller supplies the lifecycle/HS2 gate; no parent or control lock acquired.
 * EBUSY/ENOSPC publish nothing; other transport failures poison this owner.
 */
int ccci_ccif_system_send(struct ccci_ccif *ccif, u32 message, u32 reserved);
/*
 * Validated FS/RPC reply packets only; normal FS q4/ch15 or RPC q1/ch33.
 * Caller validates protocol/slot/generation and supplies actual HS2 state.
 * Calls are admitted only in WAIT_HS2 or after accepted HS2, never WAIT_HS1.
 * Transport replaces sequence/assert as in the same-board HIF. No MD lock.
 */
int ccci_ccif_service_send(struct ccci_ccif *ccif, const void *packet, u32 size,
			   bool hs2_seen);
/*
 * AT/MUX payload only; builds the same-board char-port header for channel 12
 * on queue 5. Userspace owns the 0710 stream, never the CCCI header. Same
 * admission rules and lock freedom as ccci_ccif_service_send().
 */
#define CCCI_CCIF_HEADER	16
#define CCCI_CCIF_AT_CHANNEL	12
#define CCCI_CCIF_AT_QUEUE	5
#define CCCI_CCIF_AT_MAX	3456
int ccci_ccif_at_send(struct ccci_ccif *ccif, const void *payload, u32 size,
		      bool hs2_seen);
/*
 * CCMNI data payload only; builds the same-board net header for channel 22 on
 * the queue the MD selected for this port. Same admission rules as the
 * service sender. Channel 22 is not this board's data transport -- see
 * ccci-md-net.c -- so callers keep it behind an explicit operator gate.
 */
#define CCCI_CCIF_NET_CHANNEL	22
#define CCCI_CCIF_NET_QUEUE	0
#define CCCI_CCIF_NET_MIN	14
#define CCCI_CCIF_NET_MAX	(1500 + 128)
#define CCCI_CCIF_NET_INDEX_MAX	20
int ccci_ccif_net_send(struct ccci_ccif *ccif, u32 index, u32 queue,
		       const void *payload, u32 size);
/* Sleepable: close TX/RX admission without waiting on consumer or parent lock. */
void ccci_ccif_abort(struct ccci_ccif *ccif, int error);
void ccci_ccif_quiesce_rx(struct ccci_ccif *ccif);
int ccci_ccif_stop(struct ccci_ccif *ccif);
int ccci_ccif_park(struct ccci_ccif *ccif);
void ccci_ccif_powered_off(struct ccci_ccif *ccif);
/* Parent lock held, MD off, all transport users and fault producers joined. */
int ccci_ccif_rearm(struct ccci_ccif *ccif, bool commit);
ssize_t ccci_ccif_status(struct ccci_ccif *ccif, char *buf);
/* Caller holds the parent lifecycle lock and the MD power and clock references. */
ssize_t ccci_ccif_fs_tx_status(struct ccci_ccif *ccif, char *buf);
#else
static inline struct ccci_ccif *ccci_ccif_create(struct platform_device *pdev,
						 const struct ccci_smem_layout *smem,
						 struct regmap *infra)
{
	return NULL;
}

static inline int ccci_ccif_prepare(struct ccci_ccif *ccif) { return -EOPNOTSUPP; }
static inline int ccci_ccif_receive_start(struct ccci_ccif *ccif,
					  ccci_ccif_consume_fn consume, void *context)
{
	return -EOPNOTSUPP;
}

static inline int ccci_ccif_handshake_start(struct ccci_ccif *ccif,
					    const struct ccci_runtime_offer *offers,
					    ccci_ccif_consume_fn port, void *context)
{
	return -EOPNOTSUPP;
}

static inline ssize_t ccci_ccif_handshake_status(struct ccci_ccif *ccif, char *buf)
{
	return -EOPNOTSUPP;
}

static inline int ccci_ccif_system_send(struct ccci_ccif *ccif, u32 message, u32 reserved)
{
	return -EOPNOTSUPP;
}

static inline int ccci_ccif_service_send(struct ccci_ccif *ccif, const void *packet,
					 u32 size, bool hs2_seen)
{
	return -EOPNOTSUPP;
}

static inline int ccci_ccif_at_send(struct ccci_ccif *ccif, const void *payload,
				    u32 size, bool hs2_seen)
{
	return -EOPNOTSUPP;
}

static inline int ccci_ccif_net_send(struct ccci_ccif *ccif, u32 index,
				     const void *payload, u32 size)
{
	return -EOPNOTSUPP;
}

static inline void ccci_ccif_abort(struct ccci_ccif *ccif, int error) { }

static inline void ccci_ccif_quiesce_rx(struct ccci_ccif *ccif) { }
static inline int ccci_ccif_park(struct ccci_ccif *ccif) { return -EOPNOTSUPP; }
static inline void ccci_ccif_powered_off(struct ccci_ccif *ccif) { }
static inline int ccci_ccif_stop(struct ccci_ccif *ccif) { return -EOPNOTSUPP; }
static inline ssize_t ccci_ccif_status(struct ccci_ccif *ccif, char *buf)
{
	return -EOPNOTSUPP;
}

static inline ssize_t ccci_ccif_fs_tx_status(struct ccci_ccif *ccif, char *buf)
{
	return -EOPNOTSUPP;
}
#endif

#endif
