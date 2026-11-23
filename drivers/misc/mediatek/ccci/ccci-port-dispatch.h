/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef __CCCI_PORT_DISPATCH_H
#define __CCCI_PORT_DISPATCH_H

#include <linux/errno.h>
#include <linux/kconfig.h>
#include <linux/types.h>

struct device;
struct ccci_ccif;
struct ccci_ccism_bridge;
struct ccci_service_endpoint;
struct ccci_md_at;
struct ccci_md_net;
struct ccci_runtime_offer;
struct ccci_port_dispatch;

#if IS_ENABLED(CONFIG_OP6893_CCCI_DISPATCH)
/* Dormant construction; no protocol, clocks, lease or register operation. */
struct ccci_port_dispatch *
ccci_port_dispatch_create(struct device *dev, struct ccci_ccif *ccif,
			  struct ccci_ccism_bridge *bridge,
			  struct ccci_service_endpoint *endpoint);
/*
 * Future MD owner, parent lifecycle mutex held, powered/stopped CPU/DMA.
 * Reserves the backend and real SCP lease; does NOT establish ready to boot.
 */
int ccci_port_dispatch_prepare(struct ccci_port_dispatch *dispatch, u64 generation);
/* Stopped MD, prepared CCIF rings. Starts RX/handshake, never releases CPU. */
int ccci_port_dispatch_start(struct ccci_port_dispatch *dispatch,
			     const struct ccci_runtime_offer *offers);
/* Real MD stop proof required. Joins ordinary RX before stopping services. */
int ccci_port_dispatch_stop(struct ccci_port_dispatch *dispatch, bool md_stopped);
int ccci_port_dispatch_offline(struct ccci_port_dispatch *dispatch);
int ccci_port_dispatch_rearm(struct ccci_port_dispatch *dispatch, bool commit);
/* Before start only: the AT byte pipe is immutable through a generation. */
int ccci_port_dispatch_set_at(struct ccci_port_dispatch *dispatch, struct ccci_md_at *at);
/* Before start only: the CCMNI netdev owner is immutable per generation. */
int ccci_port_dispatch_set_net(struct ccci_port_dispatch *dispatch, struct ccci_md_net *net);
ssize_t ccci_port_dispatch_status(struct ccci_port_dispatch *dispatch, char *buf);
/* AT/MUX observation: counts plus an opt-in bounded printable preview. */
ssize_t ccci_port_dispatch_at(struct ccci_port_dispatch *dispatch, char *buf);
int ccci_port_dispatch_at_preview(struct ccci_port_dispatch *dispatch, bool enable);
#else
static inline int ccci_port_dispatch_offline(struct ccci_port_dispatch *dispatch) { return 0; }
static inline int ccci_port_dispatch_set_at(struct ccci_port_dispatch *dispatch,
					    struct ccci_md_at *at) { return -EOPNOTSUPP; }
static inline int ccci_port_dispatch_set_net(struct ccci_port_dispatch *dispatch,
					     struct ccci_md_net *net) { return -EOPNOTSUPP; }
static inline struct ccci_port_dispatch *
ccci_port_dispatch_create(struct device *dev, struct ccci_ccif *ccif,
			  struct ccci_ccism_bridge *bridge,
			  struct ccci_service_endpoint *endpoint) { return NULL; }
static inline ssize_t ccci_port_dispatch_status(struct ccci_port_dispatch *dispatch,
						char *buf) { return -EOPNOTSUPP; }
static inline ssize_t ccci_port_dispatch_at(struct ccci_port_dispatch *dispatch,
					    char *buf) { return -EOPNOTSUPP; }
static inline int ccci_port_dispatch_at_preview(struct ccci_port_dispatch *dispatch,
						bool enable) { return -EOPNOTSUPP; }
#endif
#endif
