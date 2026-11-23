/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef __CCCI_CCIF_PROTOCOL_H
#define __CCCI_CCIF_PROTOCOL_H

#include <linux/types.h>

#define CCCI_CCIF_HEADER_SIZE	16
#define CCCI_CCIF_FEATURES	64
#define CCCI_CCIF_HS1_SIZE	240
#define CCCI_CCIF_INIT_CHECK	0x5555ffff

enum ccci_ccif_control {
	CCCI_CCIF_CONTROL_OTHER,
	CCCI_CCIF_CONTROL_HS1,
	CCCI_CCIF_CONTROL_HS2,
	CCCI_CCIF_CONTROL_EXCEPTION,
	CCCI_CCIF_CONTROL_VERSION_ERROR,
	CCCI_CCIF_CONTROL_EXCEPTION_OK,
	CCCI_CCIF_CONTROL_EXCEPTION_PASS,
};

struct ccci_ccif_header {
	u32 data[2];
	u32 reserved;
	u16 channel;
	u16 sequence;
	bool assert_sequence;
};

/* CPU copies only; data[1] is a message ID, not an RX payload length. */
int ccci_ccif_header_decode(const void *data, u32 size, struct ccci_ccif_header *header);
enum ccci_ccif_control ccci_ccif_control_decode(const struct ccci_ccif_header *header);
/* Exact 6297 SRAM download: 16-byte header, 72-byte query, 152 reserved bytes. */
int ccci_ccif_hs1_decode(const void *data, u32 size, u8 features[CCCI_CCIF_FEATURES]);

#endif
