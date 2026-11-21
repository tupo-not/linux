/* SPDX-License-Identifier: GPL-2.0-only */
/* Common definitions for the MediaTek SoC modem driver. */

#ifndef _MTK_MODEM_H
#define _MTK_MODEM_H

#include <linux/types.h>
#include <linux/skbuff.h>
#include <linux/wwan.h>

/* CCCI message header (16 bytes) shared with the modem. */
struct ccci_header {
	__le32 packet_header;
	__le32 packet_len;
	u8 channel;
	u8 unused;
	__le16 seq_no;
	__le32 ex_msg;
};

#define CCCI_MAGIC_NUM		0xFFFFFFFF

/* CCCI logical channel IDs (from vendor ccci_core.h). */
enum {
	CCCI_UART2_RX = 10,
	CCCI_UART2_TX = 12,
	CCCI_FS_RX = 14,
	CCCI_FS_TX = 15,
};

/* A WWAN port bound to a CCCI channel/queue pair. */
struct mtk_modem_port_desc {
	enum wwan_port_type type;
	u8 tx_queue;
	u8 rx_queue;
	u8 tx_channel;
	u8 rx_channel;
	bool preserve_header;	/* true for the raw FS channel */
	const char *name;	/* diagnostic */
};

/* Platform data describing one SoC's modem. */
struct mtk_modem_soc_data {
	const struct mtk_modem_port_desc *ports;
	size_t n_ports;
	u32 nr_queues;
	size_t sram_size;
};

#endif /* _MTK_MODEM_H */
