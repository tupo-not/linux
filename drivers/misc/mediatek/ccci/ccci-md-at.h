/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef __CCCI_MD_AT_H
#define __CCCI_MD_AT_H

#include <linux/errno.h>
#include <linux/kconfig.h>
#include <linux/types.h>

struct device;
struct ccci_ccif;
struct ccci_ccif_event;
struct ccci_md_at;

#if IS_ENABLED(CONFIG_OP6893_CCCI_DISPATCH)
/* Registers a root-only exclusive byte pipe; starts no MD or handshake. */
struct ccci_md_at *ccci_md_at_create(struct device *dev, struct ccci_ccif *ccif);
/*
 * MD -> AP AT/MUX payload (CCCI_UART2_RX). Called from the RX worker with the
 * dispatcher mutex held: no sleeping, no parent lock, bounded copy only.
 */
int ccci_md_at_receive(struct ccci_md_at *at, const struct ccci_ccif_event *event);
/* Accepted HS2: the channel may now transmit. */
int ccci_md_at_hs2(struct ccci_md_at *at);
/* Latched fault: readers and writers see this error from now on. */
void ccci_md_at_abort(struct ccci_md_at *at, int error);
/* Real MD stop proof required; joins the open file before returning. */
int ccci_md_at_stop(struct ccci_md_at *at, bool md_stopped);
int ccci_md_at_offline(struct ccci_md_at *at);
int ccci_md_at_rearm(struct ccci_md_at *at, bool commit);
ssize_t ccci_md_at_status(struct ccci_md_at *at, char *buf, size_t size);
#else
static inline struct ccci_md_at *
ccci_md_at_create(struct device *dev, struct ccci_ccif *ccif) { return NULL; }
static inline int ccci_md_at_receive(struct ccci_md_at *at,
				     const struct ccci_ccif_event *event)
{
	return 0;
}

static inline int ccci_md_at_hs2(struct ccci_md_at *at) { return 0; }
static inline void ccci_md_at_abort(struct ccci_md_at *at, int error) { }
static inline int ccci_md_at_stop(struct ccci_md_at *at, bool md_stopped) { return 0; }
static inline int ccci_md_at_rearm(struct ccci_md_at *at, bool commit)
{
	return 0;
}
static inline int ccci_md_at_offline(struct ccci_md_at *at) { return 0; }
static inline ssize_t ccci_md_at_status(struct ccci_md_at *at, char *buf, size_t size)
{
	return -EOPNOTSUPP;
}
#endif
#endif
