// SPDX-License-Identifier: GPL-2.0-only
/* Publish runtime records and SRAM response before the 6297 H2D_SRAM doorbell. */
#include <linux/bitops.h>
#include <linux/errno.h>
#include <linux/unaligned.h>

#include "ccci-ccif-tx.h"
#include "ccci-runtime.h"

int ccci_ccif_runtime_send(const struct ccci_ccif_tx_ops *ops, void *context,
			   const void *runtime, u32 used, const void *upload)
{
	const u8 *bytes = upload;
	u32 value;
	int ret;

	if (!ops || !ops->runtime_write || !ops->read || !ops->write || !ops->fence ||
	    !runtime || !upload || used < 8 * CCCI_RUNTIME_FEATURES ||
	    used > CCCI_RUNTIME_HALF || (used & 3) ||
	    get_unaligned_le32(bytes + 4) != CCCI_RUNTIME_UPLOAD ||
	    get_unaligned_le32(bytes + 8) != 1 ||
	    get_unaligned_le32(bytes + 12) != 0x5555ffff ||
	    get_unaligned_le32(bytes + 16) != 0x43434349 ||
	    get_unaligned_le32(bytes + 168) != 0x43434349)
		return -EINVAL;
	ret = ops->read(context, 4, &value);
	if (ret)
		return ret;
	if (value & BIT(15))
		return -EBUSY;
	/* Copy the entire zero-padded AP half, never the peer-owned MD half. */
	for (u32 offset = 0; offset < CCCI_RUNTIME_HALF; offset += 4) {
		ret = ops->runtime_write(context, offset, get_unaligned_le32(runtime + offset));
		if (ret)
			return ret;
	}
	for (u32 offset = 0; offset < CCCI_RUNTIME_UPLOAD; offset += 4) {
		ret = ops->write(context, 0x1f0 + offset, get_unaligned_le32(bytes + offset));
		if (ret)
			return ret;
	}
	ops->fence(context);
	for (u32 offset = 0; offset < CCCI_RUNTIME_UPLOAD; offset += 4) {
		ret = ops->read(context, 0x1f0 + offset, &value);
		if (ret)
			return ret;
		if (value != get_unaligned_le32(bytes + offset))
			return -EIO;
	}
	ret = ops->write(context, 4, BIT(15));
	if (ret)
		return ret;
	/* TCHNUM contains the channel number, not its bit mask. */
	return ops->write(context, 0xc, 15);
}
