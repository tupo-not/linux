/* SPDX-License-Identifier: GPL-2.0-only */
/* WWAN port multiplexing for CCCI channels. */

#ifndef _MTK_PORT_PROXY_H
#define _MTK_PORT_PROXY_H

#include "mtk_modem.h"
#include "ccif.h"

struct mtk_ccci_port {
	struct ccif *ccif;
	struct wwan_port *port;
	const struct mtk_modem_port_desc *descr;
	u16 seq_no;
};

struct port_proxy {
	struct ccif *ccif;
	struct mtk_ccci_port (*ports)[256];
};

int port_proxy_init(struct port_proxy *proxy, struct ccif *ccif,
		    const struct mtk_modem_port_desc *descrs, size_t n_descrs);
void port_proxy_deinit(struct port_proxy *proxy);
struct sk_buff *port_proxy_poll(struct port_proxy *proxy, u8 *out_queue);

#endif /* _MTK_PORT_PROXY_H */
