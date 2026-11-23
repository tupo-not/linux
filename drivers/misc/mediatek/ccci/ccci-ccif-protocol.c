// SPDX-License-Identifier: GPL-2.0-only
/* Same-board 6297 CCCI header and SRAM feature query, without C bitfields. */
#include <linux/errno.h>
#include <linux/string.h>
#include <linux/unaligned.h>

#include "ccci-ccif-protocol.h"

int ccci_ccif_header_decode(const void *data, u32 size, struct ccci_ccif_header *header)
{
	const u8 *bytes = data;
	u32 channel;

	if (!header)
		return -EINVAL;
	memset(header, 0, sizeof(*header));
	if (!data || size < CCCI_CCIF_HEADER_SIZE)
		return -EINVAL;
	header->data[0] = get_unaligned_le32(bytes);
	header->data[1] = get_unaligned_le32(bytes + 4);
	channel = get_unaligned_le32(bytes + 8);
	header->channel = channel & 0xffff;
	header->sequence = (channel >> 16) & 0x7fff;
	header->assert_sequence = channel >> 31;
	header->reserved = get_unaligned_le32(bytes + 12);
	return 0;
}

enum ccci_ccif_control ccci_ccif_control_decode(const struct ccci_ccif_header *header)
{
	if (!header || header->channel)
		return CCCI_CCIF_CONTROL_OTHER;
	switch (header->data[1]) {
	case 0:
		return header->reserved == CCCI_CCIF_INIT_CHECK ?
			CCCI_CCIF_CONTROL_HS1 : CCCI_CCIF_CONTROL_HS2;
	case 4:
		return CCCI_CCIF_CONTROL_EXCEPTION;
	case 5:
		return CCCI_CCIF_CONTROL_VERSION_ERROR;
	case 6:
		return CCCI_CCIF_CONTROL_EXCEPTION_OK;
	case 8:
		return CCCI_CCIF_CONTROL_EXCEPTION_PASS;
	default:
		return CCCI_CCIF_CONTROL_OTHER;
	}
}

int ccci_ccif_hs1_decode(const void *data, u32 size, u8 features[CCCI_CCIF_FEATURES])
{
	const u8 *bytes = data;
	struct ccci_ccif_header header;
	int ret;

	if (!features)
		return -EINVAL;
	memset(features, 0, CCCI_CCIF_FEATURES);
	if (size != CCCI_CCIF_HS1_SIZE)
		return -EMSGSIZE;
	ret = ccci_ccif_header_decode(data, size, &header);
	if (ret)
		return ret;
	if (ccci_ccif_control_decode(&header) != CCCI_CCIF_CONTROL_HS1 ||
	    get_unaligned_le32(bytes + 16) != 0x49434343 ||
	    get_unaligned_le32(bytes + 84) != 0x49434343)
		return -EBADMSG;
	/* Preserve support/version nibbles, including future feature versions. */
	memcpy(features, bytes + 20, CCCI_CCIF_FEATURES);
	return 0;
}
