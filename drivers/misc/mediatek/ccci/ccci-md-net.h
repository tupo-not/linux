/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef __CCCI_MD_NET_H
#define __CCCI_MD_NET_H

#include <linux/errno.h>
#include <linux/kconfig.h>
#include <linux/types.h>

struct device;
struct ccci_ccif;
struct ccci_ccif_event;
struct net_device;
struct ccci_md_net;

#if IS_ENABLED(CONFIG_OP6893_CCCI_DISPATCH)
/* Registers one CCMNI netdev; starts no MD, no handshake and no queue. */
struct ccci_md_net *ccci_md_net_create(struct device *dev, struct ccci_ccif *ccif);
/*
 * MD -> AP data frame (CCCI_CCMNI1_RX). Called from the RX worker with the
 * dispatcher mutex held: bounded allocation, no parent lock, no sleeping wait.
 */
int ccci_md_net_receive(struct ccci_md_net *net, const struct ccci_ccif_event *event);
/* Accepted HS2: the data path may transmit and report carrier. */
int ccci_md_net_hs2(struct ccci_md_net *net);
void ccci_md_net_abort(struct ccci_md_net *net, int error);
int ccci_md_net_stop(struct ccci_md_net *net, bool md_stopped);
int ccci_md_net_offline(struct ccci_md_net *net);
int ccci_md_net_rearm(struct ccci_md_net *net, bool commit);
int ccci_md_net_enable(struct ccci_md_net *net, bool enable);
ssize_t ccci_md_net_status(struct ccci_md_net *net, char *buf, size_t size);
#else
static inline struct ccci_md_net *
ccci_md_net_create(struct device *dev, struct ccci_ccif *ccif) { return NULL; }
static inline int ccci_md_net_receive(struct ccci_md_net *net,
				      const struct ccci_ccif_event *event)
{
	return 0;
}

static inline int ccci_md_net_hs2(struct ccci_md_net *net) { return 0; }
static inline void ccci_md_net_abort(struct ccci_md_net *net, int error) { }
static inline int ccci_md_net_stop(struct ccci_md_net *net, bool md_stopped) { return 0; }
static inline int ccci_md_net_rearm(struct ccci_md_net *net, bool commit)
{
	return 0;
}
static inline int ccci_md_net_offline(struct ccci_md_net *net) { return 0; }
static inline int ccci_md_net_enable(struct ccci_md_net *net, bool enable)
{
	return -EOPNOTSUPP;
}

static inline ssize_t ccci_md_net_status(struct ccci_md_net *net, char *buf, size_t size)
{
	return -EOPNOTSUPP;
}
#endif
#endif
