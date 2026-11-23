// SPDX-License-Identifier: GPL-2.0-only
/*
 * Pure text formatter for one DPMAIF sample. Split out so the exact rendering
 * can be exercised on the host without any driver, clock or MMIO shim.
 */
#include <linux/errno.h>
#include <linux/kernel.h>
#include <linux/types.h>

#include "ccci-dpmaif.h"

ssize_t ccci_dpmaif_format(const struct ccci_dpmaif_sample *sample, char *buf, size_t size)
{
	int ret;

	if (!sample || !buf || !size)
		return -EINVAL;
	/*
	 * snprintf reports the length it would have written, so a buffer that is
	 * too small is an explicit -ENOSPC rather than a silently cut dump.
	 */
	ret = snprintf(buf, size,
		       "ao_ul_chnl0_sta=0x%08x ao_ul_chnl0_sta0=0x%08x ao_ul_l2timr0=0x%08x\n"
		       "pd_ul_add_desc=0x%08x pd_ul_add_desc_ch0=0x%08x\n"
		       "pd_md_ip_busy=0x%08x pd_md_ip_busy_mask=0x%08x\n"
		       "sram_raw0=0x%08x sram_raw1=0x%08x infra_ctrl=0x%08x\n"
		       "busy_mask_field=%u wakeup_event_bit=%u md_busy_mask_field=%u\n"
		       "clk_main=%lu clk_bus=%lu\n",
		       sample->ao_ul_chnl0_sta, sample->ao_ul_chnl0_sta0,
		       sample->ao_ul_l2timr0, sample->pd_ul_add_desc,
		       sample->pd_ul_add_desc_ch0, sample->pd_md_ip_busy,
		       sample->pd_md_ip_busy_mask, sample->sram_word0,
		       sample->sram_word1, sample->infra_ctrl,
		       (sample->infra_ctrl & CCCI_DPMAIF_IP_BUSY_MASK) >>
			       CCCI_DPMAIF_IP_BUSY_SHIFT,
		       !!(sample->infra_ctrl & CCCI_DPMAIF_WAKEUP_EVENT),
		       (sample->pd_md_ip_busy_mask & CCCI_DPMAIF_IP_BUSY_MASK) >>
			       CCCI_DPMAIF_IP_BUSY_SHIFT,
		       sample->rate_main, sample->rate_bus);
	if (ret < 0)
		return ret;
	if ((size_t)ret >= size)
		return -ENOSPC;
	return ret;
}

ssize_t ccci_dpmaif_format_dl(const struct ccci_dpmaif_dl_state *dl, char *buf, size_t size)
{
	char snap[2 * CCCI_DPMAIF_DL_SNAP_LEN + 1];
	int ret;
	u32 i;

	if (!dl || !buf || !size)
		return -EINVAL;
	if (dl->snap_len > CCCI_DPMAIF_DL_SNAP_LEN)
		return -EINVAL;
	for (i = 0; i < dl->snap_len; i++)
		snprintf(snap + 2 * i, 3, "%02x", dl->snap[i]);
	snap[2 * dl->snap_len] = '\0';
	ret = snprintf(buf, size,
		       "bat=%u pit=%u buf=%u frg=%u/%u\n"
		       "pit_rd=%u bat_wr=%u\n"
		       "ring_dma=%#llx buf_dma=%#llx\n"
		       "irqs=%u pkts=%u bytes=%u msg_pits=%u frag_pits=%u\n"
		       "err=%u mtu_err=%u overrun=%u pit_init_stuck=%u last_status=0x%08x\n"
		       "last_len=%u last_channel=%u last_bid=%u snap_len=%u\n"
		       "snap=%s\n",
		       dl->bat_entries, dl->pit_entries, dl->bat_buf_sz,
		       dl->frg_entries, dl->frg_buf_sz, dl->pit_rd, dl->bat_wr,
		       (unsigned long long)dl->ring_dma,
		       (unsigned long long)dl->buf_dma,
		       dl->irqs, dl->pkts, dl->bytes, dl->msg_pits, dl->frag_pits,
		       dl->err, dl->mtu_err, dl->overrun, dl->pit_init_stuck,
		       dl->last_status,
		       dl->last_len, dl->last_channel, dl->last_bid, dl->snap_len, snap);
	if (ret < 0)
		return ret;
	if ((size_t)ret >= size)
		return -ENOSPC;
	return ret;
}
