// SPDX-License-Identifier: GPL-2.0-only
/* Same-board CCIF0 reset and SRAM initialization; not the SCP CCIF2 bridge. */
#include <linux/bitops.h>
#include <linux/errno.h>

#include "ccci-ccif-hw.h"
#include "ccci-ccif-ring.h"

int ccci_ccif_hw_reset(const struct ccci_ccif_hw_ops *ops, void *context,
		       const struct ccci_ccif_layout *layout)
{
	u32 value;
	int ret;

	if (!ops || !ops->write || !ops->read || !ops->fence || !layout ||
	    !layout->debug_size || (u64)layout->debug_md + layout->debug_size > 0x100000000ULL)
		return -EINVAL;
	ret = ops->write(context, 0, 0x150, BIT(8));
	if (!ret)
		ret = ops->write(context, 0, 0x154, BIT(8));
	if (ret)
		return ret;
	for (u32 offset = 0x100; offset < 0x300; offset += 4) {
		ret = ops->write(context, 1, offset, 0);
		if (!ret)
			ret = ops->write(context, 2, offset, 0);
		if (ret)
			return ret;
	}
	ret = ops->write(context, 1, 0x2f4, 0x7274626e);
	if (!ret)
		ret = ops->write(context, 1, 0x2f8, layout->debug_md);
	if (!ret)
		ret = ops->write(context, 1, 0x2fc, layout->debug_size);
	if (ret)
		return ret;
	ops->fence(context);
	/* AP/MD SRAM views alias: the AP debug footer is visible through both. */
	for (u32 offset = 0x100; offset < 0x300; offset += 4) {
		u32 expected = offset == 0x2f4 ? 0x7274626e :
			       offset == 0x2f8 ? layout->debug_md :
			       offset == 0x2fc ? layout->debug_size : 0;

		ret = ops->read(context, 1, offset, &value);
		if (ret)
			return ret;
		if (value != expected)
			return -EIO;
		ret = ops->read(context, 2, offset, &value);
		if (ret)
			return ret;
		if (value != expected)
			return -EIO;
	}
	/* Reset must clear BUSY and receive notifications in both directions. */
	for (u32 bank = 1; bank <= 2; bank++) {
		ret = ops->read(context, bank, 4, &value);
		if (ret)
			return ret;
		if (value)
			return -EBUSY;
		ret = ops->read(context, bank, 0x10, &value);
		if (ret)
			return ret;
		if (value)
			return -EBUSY;
	}
	return 0;
}
