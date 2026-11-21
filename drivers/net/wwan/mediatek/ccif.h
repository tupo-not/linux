/* SPDX-License-Identifier: GPL-2.0-only */
/* CCIF transport for the MediaTek SoC modem. */

#ifndef _MTK_CCIF_H
#define _MTK_CCIF_H

#include <linux/types.h>
#include <linux/skbuff.h>
#include <linux/interrupt.h>
#include "mtk_modem.h"

/* AP-side CCIF registers (offsets from ccif_ap_base). */
#define APCCIF_CON		0x00
#define APCCIF_BUSY		0x04
#define APCCIF_START		0x08
#define APCCIF_TCHNUM		0x0C
#define APCCIF_RCHNUM		0x10
#define APCCIF_ACK		0x14
#define APCCIF_CHDATA		0x100

/* Ring buffer guard words. */
#define CCCI_RBF_HEADER		0xA5A5A5A5
#define CCCI_RBF_FOOTER		0x5A5A5A5A

/* Ring-buffer control for one direction. */
struct ccci_ring_ctl {
	u32 read;
	u32 write;
	u32 length;
};

/* A CCCI ring buffer lives in AP<->MD shared memory. */
struct ccci_ringbuf {
	u32 head[2];
	struct ccci_ring_ctl rx_control;
	struct ccci_ring_ctl tx_control;
	u8 data[];
};

struct ccif_queue {
	struct ccif *ccif;
	u8 index;
	u8 ccif_ch;
	struct ccci_ringbuf *rb;
	spinlock_t lock;
};

struct ccif {
	struct device *dev;
	void __iomem *ap_base;
	void __iomem *md_base;
	void __iomem *sram;
	size_t sram_size;
	u32 nr_queues;
	struct ccif_queue *txq;
	struct ccif_queue *rxq;
	atomic_t pending_rx;
	bool started;	/* hardware/SRAM setup done */
};

int ccif_init(struct ccif *ccif, struct device *dev,
	      void __iomem *ap_base, void __iomem *md_base,
	      void __iomem *sram, size_t sram_size, u32 nr_queues,
	      const u32 *rx_sizes, const u32 *tx_sizes);
void ccif_deinit(struct ccif *ccif);
void ccif_handle_irq(struct ccif *ccif);
void ccif_send_skb(struct ccif *ccif, u8 queue, struct sk_buff *skb);
struct sk_buff *ccif_poll(struct ccif *ccif, u8 *out_queue);

#endif /* _MTK_CCIF_H */
