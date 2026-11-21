// SPDX-License-Identifier: GPL-2.0-only
/* WWAN port multiplexing for CCCI channels. */

#include <linux/slab.h>
#include <linux/wwan.h>
#include "port_proxy.h"

#define CCCI_MTU 16 * 1024

static int mtk_port_start(struct wwan_port *port)
{
	return 0;
}

static void mtk_port_stop(struct wwan_port *port)
{
}

static int mtk_port_tx(struct wwan_port *wwan_port, struct sk_buff *skb)
{
	struct mtk_ccci_port *port = wwan_port_get_drvdata(wwan_port);
	struct ccci_header *hdr;

	if (!port->descr->preserve_header) {
		if (skb_headroom(skb) < sizeof(*hdr)) {
			struct sk_buff *n = skb_realloc_headroom(skb, sizeof(*hdr));

			if (!n) {
				kfree_skb(skb);
				return -ENOMEM;
			}
			kfree_skb(skb);
			skb = n;
		}

		skb_push(skb, sizeof(*hdr));
		hdr = (struct ccci_header *)skb->data;
		hdr->packet_header = cpu_to_le32(CCCI_MAGIC_NUM);
		hdr->packet_len = cpu_to_le32(skb->len);
		hdr->channel = port->descr->tx_channel;
		hdr->unused = 0;
		hdr->seq_no = cpu_to_le16(port->seq_no++ | 0x8000);
		hdr->ex_msg = 0;
	} else if (skb->len < sizeof(*hdr)) {
		/* ModemManager probes the FS port as AT: drop short writes. */
		kfree_skb(skb);
		return 0;
	}

	ccif_send_skb(port->ccif, port->descr->tx_queue, skb);
	return 0;
}

static const struct wwan_port_ops mtk_port_ops = {
	.start = mtk_port_start,
	.stop = mtk_port_stop,
	.tx = mtk_port_tx,
};

static const struct wwan_port_caps mtk_normal_caps = {
	.frag_len = CCCI_MTU - sizeof(struct ccci_header),
	.headroom_len = sizeof(struct ccci_header),
};

static const struct wwan_port_caps mtk_raw_caps = {
	.frag_len = CCCI_MTU,
	.headroom_len = 0,
};

int port_proxy_init(struct port_proxy *proxy, struct ccif *ccif,
		    const struct mtk_modem_port_desc *descrs, size_t n_descrs)
{
	struct mtk_ccci_port *port;
	const struct mtk_modem_port_desc *descr;
	int error;
	size_t i;

	memset(proxy, 0, sizeof(*proxy));
	proxy->ccif = ccif;

	proxy->ports = kzalloc(sizeof(*proxy->ports) * ccif->nr_queues,
			       GFP_KERNEL);
	if (!proxy->ports)
		return -ENOMEM;

	for (i = 0; i < n_descrs; i++) {
		descr = &descrs[i];
		port = &proxy->ports[descr->rx_queue][descr->rx_channel];
		port->ccif = ccif;
		port->descr = descr;

		port->port = wwan_create_port(ccif->dev, descr->type,
					      &mtk_port_ops,
					      (struct wwan_port_caps *)
					      (descr->preserve_header ?
					       &mtk_raw_caps : &mtk_normal_caps),
					      port);
		if (IS_ERR(port->port)) {
			error = PTR_ERR(port->port);
			goto err_free;
		}
	}

	return 0;

err_free:
	while (i--) {
		descr = &descrs[i];
		port = &proxy->ports[descr->rx_queue][descr->rx_channel];
		if (port->port)
			wwan_remove_port(port->port);
	}
	kfree(proxy->ports);
	return error;
}

void port_proxy_deinit(struct port_proxy *proxy)
{
	u32 i, j;
	struct mtk_ccci_port *port;

	for (i = 0; i < proxy->ccif->nr_queues; i++)
		for (j = 0; j < 256; j++) {
			port = &proxy->ports[i][j];
			if (port->port)
				wwan_remove_port(port->port);
		}

	kfree(proxy->ports);
}

struct sk_buff *port_proxy_poll(struct port_proxy *proxy, u8 *out_queue)
{
	struct sk_buff *skb;
	struct mtk_ccci_port *port;
	struct ccci_header *hdr;

	while ((skb = ccif_poll(proxy->ccif, out_queue))) {
		if (skb->len < sizeof(*hdr))
			return skb;

		hdr = (struct ccci_header *)skb->data;

		/* Magic packets are handled by the core driver, not here. */
		if ((le32_to_cpu(hdr->packet_header) & 0x7fffffff) ||
		    le32_to_cpu(hdr->packet_len) != skb->len)
			return skb;

		port = &proxy->ports[*out_queue][hdr->channel];
		if (!port->port)
			return skb;

		if (!port->descr->preserve_header)
			skb_pull(skb, sizeof(*hdr));

		wwan_port_rx(port->port, skb);
	}

	return NULL;
}
