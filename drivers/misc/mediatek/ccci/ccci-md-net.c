// SPDX-License-Identifier: GPL-2.0-only
/*
 * CCMNI data port for the same-board CCCI net channel. The kernel owns the
 * CCCI header and the netdev plumbing; nothing here invents a packet, and the
 * transmit path defers to a workqueue because ndo_start_xmit is atomic while
 * the CCIF transmit takes the owner mutex.
 *
 * The same-board ABI carries CCMNI over DPMAIF on MD_GENERATION 6297
 * (ccci_hif.h: MD1_NET_HIF is DPMAIF unless the generation is 6293), and the MD
 * asserts in its nccci service on any CCIF channel-22 frame, on the shared queue
 * and on the remapped one alike. This path therefore stays behind the operator
 * gate as a probe; it is not the data transport and no packet is claimed to
 * have reached the MD.
 */
#include <linux/device.h>
#include <linux/etherdevice.h>
#include <linux/if_ether.h>
#include <linux/kernel.h>
#include <linux/mutex.h>
#include <linux/netdevice.h>
#include <linux/skbuff.h>
#include <linux/spinlock.h>
#include <linux/string.h>
#include <linux/workqueue.h>

#include "ccci-ccif.h"
#include "ccci-md-net.h"
#include "ccci-rpc-lhif.h"

/* Same-board CCMNI1: RX 20, TX 22, index 0; the uplink queue comes from the
 * MD's own LHIF remap request and defaults to the shared control queue.
 */
#define CCCI_NET_RX_CHANNEL	20
#define CCCI_NET_INDEX		0
#define CCCI_NET_QUEUE_DEPTH	64

struct ccci_md_net {
	struct device *dev;
	struct ccci_ccif *ccif;
	struct net_device *ndev;
	/* Serializes lifecycle and the transmit worker against stop. */
	struct mutex lock;
	/* tx_queue.lock also protects the xmit admission gate. */
	struct sk_buff_head tx_queue;
	bool accepting;
	bool rearmable;
	struct work_struct tx_work;
	int error;
	bool hs2;
	bool stopped;
	bool opened;
	/* Operator gate: the data path stays silent until explicitly enabled. */
	bool enabled;
	u32 tx_packets;
	u32 tx_bytes;
	u32 tx_dropped;
	u32 tx_busy;
	u32 rx_packets;
	u32 rx_bytes;
	u32 rx_dropped;
};

/* net->lock serializes gate changes and worker wakeups. */
static void net_update_tx(struct ccci_md_net *net)
{
	unsigned long flags;

	lockdep_assert_held(&net->lock);
	spin_lock_irqsave(&net->tx_queue.lock, flags);
	net->accepting = net->opened && net->enabled && net->hs2 &&
			 !net->stopped && !net->error;
	if (net->accepting) {
		netif_carrier_on(net->ndev);
		netif_wake_queue(net->ndev);
	} else {
		netif_stop_queue(net->ndev);
		netif_carrier_off(net->ndev);
	}
	spin_unlock_irqrestore(&net->tx_queue.lock, flags);
}

static void net_tx_work(struct work_struct *work)
{
	struct ccci_md_net *net = container_of(work, struct ccci_md_net, tx_work);
	struct sk_buff *skb;

	while ((skb = skb_dequeue(&net->tx_queue)) != NULL) {
		int ret;

		mutex_lock(&net->lock);
		ret = net->error;
		if (!ret && (!net->hs2 || !net->opened || !net->enabled))
			ret = -ENOTCONN;
		if (!ret) {
			/* Only a resolved remap replaces the shared queue default. */
			u32 queue = CCCI_CCIF_NET_QUEUE;

			ccci_rpc_lhif_queue(CCCI_NET_INDEX, &queue);
			ret = ccci_ccif_net_send(net->ccif, CCCI_NET_INDEX, queue,
						 skb->data, skb->len);
		}
		if (ret == -EBUSY) {
			/* Backpressure: keep the packet for the next doorbell. */
			net->tx_busy++;
			mutex_unlock(&net->lock);
			skb_queue_head(&net->tx_queue, skb);
			break;
		}
		if (ret && ret != -ENOTCONN) {
			net->error = ret;
			net->tx_dropped++;
		} else if (ret) {
			net->tx_dropped++;
		} else {
			net->tx_packets++;
			net->tx_bytes += skb->len;
		}
		mutex_unlock(&net->lock);
		dev_kfree_skb_any(skb);
		if (ret && ret != -ENOTCONN)
			break;
	}
	mutex_lock(&net->lock);
	net_update_tx(net);
	mutex_unlock(&net->lock);
}

static netdev_tx_t net_start_xmit(struct sk_buff *skb, struct net_device *ndev)
{
	struct ccci_md_net *net = netdev_priv(ndev);
	unsigned long flags;
	bool full;

	if (skb->len < ETH_ZLEN || skb->len > CCCI_CCIF_NET_MAX) {
		dev_kfree_skb_any(skb);
		ndev->stats.tx_dropped++;
		return NETDEV_TX_OK;
	}
	spin_lock_irqsave(&net->tx_queue.lock, flags);
	if (!net->accepting) {
		spin_unlock_irqrestore(&net->tx_queue.lock, flags);
		return NETDEV_TX_BUSY;
	}
	full = skb_queue_len(&net->tx_queue) >= CCCI_NET_QUEUE_DEPTH;
	if (!full) {
		__skb_queue_tail(&net->tx_queue, skb);
		schedule_work(&net->tx_work);
	}
	spin_unlock_irqrestore(&net->tx_queue.lock, flags);
	if (full)
		return NETDEV_TX_BUSY;
	ndev->stats.tx_packets++;
	ndev->stats.tx_bytes += skb->len;
	return NETDEV_TX_OK;
}

static int net_open(struct net_device *ndev)
{
	struct ccci_md_net *net = netdev_priv(ndev);

	mutex_lock(&net->lock);
	if (net->stopped) {
		mutex_unlock(&net->lock);
		return -ESHUTDOWN;
	}
	net->opened = true;
	net_update_tx(net);
	mutex_unlock(&net->lock);
	return 0;
}

static int net_close(struct net_device *ndev)
{
	struct ccci_md_net *net = netdev_priv(ndev);

	mutex_lock(&net->lock);
	net->opened = false;
	net_update_tx(net);
	mutex_unlock(&net->lock);
	return 0;
}

static const struct net_device_ops net_ops = {
	.ndo_open = net_open,
	.ndo_stop = net_close,
	.ndo_start_xmit = net_start_xmit,
};

static void net_setup(struct net_device *ndev)
{
	ether_setup(ndev);
	ndev->netdev_ops = &net_ops;
	ndev->flags |= IFF_NOARP;
	ndev->needed_headroom = CCCI_CCIF_HEADER;
	ndev->mtu = 1500;
}

/* MD -> AP: the payload after the CCCI header becomes one ethernet frame. */
int ccci_md_net_receive(struct ccci_md_net *net, const struct ccci_ccif_event *event)
{
	const u8 *bytes;
	struct sk_buff *skb;
	u32 length;

	if (!net || !event)
		return -EINVAL;
	if (event->header.channel != CCCI_NET_RX_CHANNEL ||
	    event->size <= CCCI_CCIF_HEADER)
		return 0;
	bytes = event->data;
	length = event->size - CCCI_CCIF_HEADER;
	if (length < ETH_ZLEN || length > CCCI_CCIF_NET_MAX) {
		net->rx_dropped++;
		return 0;
	}
	skb = netdev_alloc_skb_ip_align(net->ndev, length);
	if (!skb) {
		net->rx_dropped++;
		return -ENOMEM;
	}
	memcpy(skb_put(skb, length), bytes + CCCI_CCIF_HEADER, length);
	skb->dev = net->ndev;
	skb->protocol = eth_type_trans(skb, net->ndev);
	net->rx_packets++;
	net->rx_bytes += length;
	netif_rx(skb);
	return 0;
}

int ccci_md_net_hs2(struct ccci_md_net *net)
{
	if (!net)
		return 0;
	/* Queue remaps describe one MD generation; the MD asks again after HS2. */
	ccci_rpc_lhif_reset();
	mutex_lock(&net->lock);
	if (!net->error)
		net->hs2 = true;
	net_update_tx(net);
	mutex_unlock(&net->lock);
	return net->error;
}

void ccci_md_net_abort(struct ccci_md_net *net, int error)
{
	if (!net)
		return;
	mutex_lock(&net->lock);
	if (!net->error)
		net->error = error < 0 ? error : -EIO;
	net_update_tx(net);
	mutex_unlock(&net->lock);
}

int ccci_md_net_stop(struct ccci_md_net *net, bool md_stopped)
{

	if (!net)
		return 0;
	if (!md_stopped)
		return -EBUSY;
	mutex_lock(&net->lock);
	net->stopped = true;
	if (!net->error)
		net->error = -ESHUTDOWN;
	net_update_tx(net);
	mutex_unlock(&net->lock);
	/* Synchronize xmit after closing the gate, then join its worker. */
	netif_tx_disable(net->ndev);
	cancel_work_sync(&net->tx_work);
	mutex_lock(&net->lock);
	net->tx_dropped += skb_queue_len(&net->tx_queue);
	skb_queue_purge(&net->tx_queue);
	net->rearmable = true;
	mutex_unlock(&net->lock);
	return 0;
}

int ccci_md_net_offline(struct ccci_md_net *net)
{
	return ccci_md_net_stop(net, true);
}

int ccci_md_net_rearm(struct ccci_md_net *net, bool commit)
{
	int ret = 0;

	if (!net)
		return 0;
	mutex_lock(&net->lock);
	if (!net->rearmable || net->opened) {
		ret = -EBUSY;
	} else if (commit) {
		net->enabled = false;
		net->hs2 = false;
		net->stopped = false;
		net->rearmable = false;
		net->error = 0;
		net->tx_packets = 0;
		net->tx_bytes = 0;
		net->tx_dropped = 0;
		net->tx_busy = 0;
		net->rx_packets = 0;
		net->rx_bytes = 0;
		net->rx_dropped = 0;
	}
	mutex_unlock(&net->lock);
	return ret;
}

/* Operator gate for the data path; refuse after the generation stopped. */
int ccci_md_net_enable(struct ccci_md_net *net, bool enable)
{
	if (!net)
		return -EINVAL;
	mutex_lock(&net->lock);
	if (net->stopped || net->error) {
		mutex_unlock(&net->lock);
		return -ESHUTDOWN;
	}
	net->enabled = enable;
	net_update_tx(net);
	mutex_unlock(&net->lock);
	return 0;
}

ssize_t ccci_md_net_status(struct ccci_md_net *net, char *buf, size_t size)
{
	struct ccci_rpc_lhif_state lhif;
	u32 queue = CCCI_CCIF_NET_QUEUE;
	size_t used;
	int ret;

	if (!net || !buf || !size)
		return -EOPNOTSUPP;
	ccci_rpc_lhif_state(&lhif);
	ccci_rpc_lhif_queue(CCCI_NET_INDEX, &queue);
	ret = scnprintf(buf, size,
			"hs2=%u opened=%u enabled=%u stopped=%u error=%d queued=%u tx=%u tx_bytes=%u tx_busy=%u tx_dropped=%u rx=%u rx_bytes=%u rx_dropped=%u tx_queue=%u lhif_requests=%u lhif_malformed=%u lhif_unknown=%u lhif_ports=%u\n",
			net->hs2, net->opened, net->enabled, net->stopped, net->error,
			skb_queue_len(&net->tx_queue), net->tx_packets, net->tx_bytes,
			net->tx_busy, net->tx_dropped, net->rx_packets, net->rx_bytes,
			net->rx_dropped, queue, lhif.requests, lhif.malformed,
			lhif.unknown, lhif.ports);
	if (ret < 0 || (size_t)ret >= size)
		return ret;
	used = ret;
	for (u32 i = 0; i < lhif.ports && used < size; i++)
		used += scnprintf(buf + used, size - used,
				  "lhif%u net_if=%u lhif_q=%u queue=%u requests=%u\n", i,
				  lhif.port[i].net_if, lhif.port[i].lhif_q,
				  lhif.port[i].queue, lhif.port[i].requests);
	return used;
}

static void net_unregister(void *context)
{
	struct ccci_md_net *net = context;

	ccci_md_net_stop(net, true);
	unregister_netdev(net->ndev);
	free_netdev(net->ndev);
}

struct ccci_md_net *ccci_md_net_create(struct device *dev, struct ccci_ccif *ccif)
{
	struct ccci_md_net *net;
	struct net_device *ndev;
	int ret;

	if (!dev || !ccif)
		return ERR_PTR(-EINVAL);
	ndev = alloc_netdev(sizeof(*net), "ccmni%d", NET_NAME_ENUM, net_setup);
	if (!ndev)
		return ERR_PTR(-ENOMEM);
	net = netdev_priv(ndev);
	net->dev = dev;
	net->ccif = ccif;
	net->ndev = ndev;
	mutex_init(&net->lock);
	skb_queue_head_init(&net->tx_queue);
	INIT_WORK(&net->tx_work, net_tx_work);
	SET_NETDEV_DEV(ndev, dev);
	ret = register_netdev(ndev);
	if (ret) {
		free_netdev(ndev);
		return ERR_PTR(ret);
	}
	ret = devm_add_action_or_reset(dev, net_unregister, net);
	if (ret)
		return ERR_PTR(ret);
	return net;
}
