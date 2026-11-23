// SPDX-License-Identifier: GPL-2.0-only
/* Same-board external 32 kHz crystal fact; absence is never a default. */
#include <linux/device.h>
#include <linux/errno.h>
#include <linux/nvmem-consumer.h>
#include <linux/of.h>

#include "ccci-rtc32k.h"

int ccci_rtc32k_read(struct device *dev, enum ccci_runtime_crystal *crystal,
		     enum ccci_rtc32k_source *source)
{
	struct nvmem_cell *cell;
	u8 *value;
	size_t length;
	int ret;

	if (!dev || !crystal || !source)
		return -EINVAL;
	*crystal = CCCI_RUNTIME_CRYSTAL_UNKNOWN;
	*source = CCCI_RTC32K_SOURCE_NONE;
	cell = nvmem_cell_get(dev, "external-32k");
	if (IS_ERR(cell)) {
		ret = PTR_ERR(cell);
		if (ret != -ENOENT)
			return ret;
		/* Explicit board fact only; no static zero or CONN strap inference. */
		if (!of_property_present(dev->of_node, "mediatek,no-external-32k"))
			return -ENODATA;
		*crystal = CCCI_RUNTIME_CRYSTAL_ABSENT;
		*source = CCCI_RTC32K_SOURCE_DT;
		return 0;
	}
	value = nvmem_cell_read(cell, &length);
	nvmem_cell_put(cell);
	if (IS_ERR(value))
		return PTR_ERR(value);
	if (!length) {
		kfree(value);
		return -ENODATA;
	}
	*crystal = value[0] ? CCCI_RUNTIME_CRYSTAL_PRESENT :
			      CCCI_RUNTIME_CRYSTAL_ABSENT;
	*source = CCCI_RTC32K_SOURCE_NVMEM;
	kfree(value);
	return 0;
}
