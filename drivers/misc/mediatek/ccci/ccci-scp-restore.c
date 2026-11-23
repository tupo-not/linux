// SPDX-License-Identifier: GPL-2.0-only
/* Pure CPU layout checks; deliberately no recovery executor or device I/O. */
#include <linux/errno.h>
#include <linux/string.h>

#include "ccci-scp-restore.h"

/* Used only after all spans have been checked as nonempty u32 ranges. */
static bool ccci_scp_restore_overlap(const struct ccci_scp_span *a,
				     const struct ccci_scp_span *b)
{
	return a->base < b->base + b->size && b->base < a->base + a->size;
}

int ccci_scp_plan_restore(const void *snapshot, size_t length, u32 cores,
			  const struct ccci_scp_span *firmware_window,
			  const struct ccci_scp_span *share_window,
			  const struct ccci_scp_span *tcm_window,
			  struct ccci_scp_restore_plan *plan)
{
	struct ccci_scp_restore_plan value = {};
	struct ccci_scp_region region;
	int ret;

	if (!plan)
		return -EINVAL;
	memset(plan, 0, sizeof(*plan));
	if (cores != 2 || !ccci_scp_span_allowed(firmware_window, firmware_window) ||
	    !ccci_scp_span_allowed(share_window, share_window) ||
	    !ccci_scp_span_allowed(tcm_window, tcm_window))
		return -EINVAL;
	/* This helper describes MT6893 RV TCM, not a generic remoteproc loader. */
	if (tcm_window->base != 0x10500000 || tcm_window->size != 0x180000)
		return -EINVAL;
	if (ccci_scp_restore_overlap(firmware_window, share_window) ||
	    ccci_scp_restore_overlap(firmware_window, tcm_window) ||
	    ccci_scp_restore_overlap(share_window, tcm_window))
		return -ERANGE;
	ret = ccci_scp_decode_region(snapshot, length, &region);
	if (ret)
		return ret;
	/* Unlike observation, recovery cannot accept absent required payloads. */
	if (!ccci_scp_span_allowed(firmware_window, &region.loader) ||
	    !ccci_scp_span_allowed(firmware_window, &region.firmware) ||
	    !region.dram.size)
		return -ERANGE;
	ret = ccci_scp_dram_span(&region, firmware_window, &value.dram_mapping);
	if (ret)
		return ret;
	if (ccci_scp_restore_overlap(&region.loader, &region.firmware) ||
	    ccci_scp_restore_overlap(&region.loader, &value.dram_mapping) ||
	    ccci_scp_restore_overlap(&region.firmware, &value.dram_mapping))
		return -ERANGE;
	value.loader_source = region.loader;
	value.firmware = region.firmware;
	value.tcm_clear = *tcm_window;
	value.loader_target = (struct ccci_scp_span) {
		.base = tcm_window->base,
		.size = region.loader.size,
	};
	value.header_target = (struct ccci_scp_span) {
		.base = tcm_window->base + CCCI_SCP_REGION_OFFSET,
		.size = length,
	};
	/* The header intentionally overlays the loader, after the loader copy. */
	if (!ccci_scp_span_allowed(tcm_window, &value.loader_target) ||
	    !ccci_scp_span_allowed(&value.loader_target, &value.header_target))
		return -ERANGE;
	value.dram_clear = (struct ccci_scp_span) {
		.base = region.dram.base,
		.size = value.dram_mapping.size / 2,
	};
	value.dram_source = (struct ccci_scp_span) {
		.base = region.dram.base + value.dram_clear.size,
		.size = region.dram.size,
	};
	value.dram_target = region.dram;
	/* A/V calculate the source; mismatching header metadata remains untrusted. */
	if (region.backup_start != value.dram_source.base ||
	    !ccci_scp_span_allowed(&value.dram_mapping, &value.dram_source))
		return -ERANGE;
	*plan = value;
	return 0;
}
