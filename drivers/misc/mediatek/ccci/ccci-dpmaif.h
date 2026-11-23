/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef __CCCI_DPMAIF_H
#define __CCCI_DPMAIF_H

#include <linux/bits.h>
#include <linux/types.h>

/*
 * Same-board register offsets, from the vendor dpmaif_reg.h. Each block is
 * mapped separately, so the vendor base macros all resolve to zero here.
 */
#define CCCI_DPMAIF_AO_UL_CHNL0_STA	0x00
#define CCCI_DPMAIF_AO_UL_CHNL0_STA0	0x60
#define CCCI_DPMAIF_AO_UL_AP_L2TIMR0	0x80
#define CCCI_DPMAIF_AO_UL_AP_L2TIMCR0	0x84
#define CCCI_DPMAIF_AO_UL_AP_L2TIMSR0	0x88
#define CCCI_DPMAIF_PD_UL_ADD_DESC	0x00
#define CCCI_DPMAIF_PD_UL_ADD_DESC_CH0	0xb0
/* The vendor derives this from pd_ul_base + 0x400 rather than mapping it. */
#define CCCI_DPMAIF_PD_MISC_BASE	0x400
#define CCCI_DPMAIF_PD_AP_UL_L2TISAR0	0x00
#define CCCI_DPMAIF_PD_MD_IP_BUSY	0x00
#define CCCI_DPMAIF_PD_MD_IP_BUSY_MASK	0x40

/* Uplink ring programming, from the same-board v2 driver. */
#define CCCI_DPMAIF_AO_UL_CHNL_ARB0	0x1c
#define CCCI_DPMAIF_AO_UL_CHNL0_CON0	0x20
#define CCCI_DPMAIF_AO_UL_CHNL0_CON1	0x24
#define CCCI_DPMAIF_UL_STA0(q)		(CCCI_DPMAIF_AO_UL_CHNL0_STA + 0x04 * (q))
#define CCCI_DPMAIF_UL_QSAR(q)		(CCCI_DPMAIF_AO_UL_CHNL0_CON0 + 0x10 * (q))
#define CCCI_DPMAIF_UL_DRBSIZE(q)	(CCCI_DPMAIF_AO_UL_CHNL0_CON1 + 0x10 * (q))
#define CCCI_DPMAIF_UL_ADD_DESC(q)	(CCCI_DPMAIF_PD_UL_ADD_DESC_CH0 + 0x04 * (q))
#define CCCI_DPMAIF_DRB_SIZE_MSK	0x0000ffff
#define CCCI_DPMAIF_DRB_ADDRH_MSK	0xff000000
#define CCCI_DPMAIF_UL_ADD_UPDATE	BIT(31)
#define CCCI_DPMAIF_UL_ADD_NOT_READY	BIT(31)
#define CCCI_DPMAIF_UL_INT_DONE(q)	BIT(q)
#define CCCI_DPMAIF_UL_INT_QDONE_MSK	(0x1f << 0)
#define CCCI_DPMAIF_UL_INT_ERR_MSK	(0x1f << 20)
#define CCCI_DPMAIF_UL_INT_MD_NOTREADY_MSK	(0x1f << 10)
#define CCCI_DPMAIF_UL_INT_MD_PWR_NOTREADY_MSK	(0x1f << 15)
#define CCCI_DPMAIF_UL_INT_EN		(CCCI_DPMAIF_UL_INT_QDONE_MSK | \
					 CCCI_DPMAIF_UL_INT_ERR_MSK | \
					 CCCI_DPMAIF_UL_INT_MD_NOTREADY_MSK | \
					 CCCI_DPMAIF_UL_INT_MD_PWR_NOTREADY_MSK)
/* The uplink engine reports idle in bits 31:26 of its debug status word. */
#define CCCI_DPMAIF_PD_UL_DBG_STA2	0x88
#define CCCI_DPMAIF_UL_STS_CUR_SHIFT	26
#define CCCI_DPMAIF_UL_IDLE_STS_MSK	0x3f
#define CCCI_DPMAIF_UL_IDLE_STS		0x01
#define CCCI_DPMAIF_UL_DRB_ENTRIES	2048
#define CCCI_DPMAIF_UL_DRB_WORDS	2
#define CCCI_DPMAIF_UL_PAYLOAD_MAX	1600
#define CCCI_DPMAIF_UL_POLL_LIMIT	1600000

#define CCCI_DPMAIF_DES_PD		0x0
#define CCCI_DPMAIF_DES_MSG		0x1

/*
 * Downlink. The vendor derives three more blocks from the two that the device
 * tree already describes, so they need no extra region: the AO downlink block
 * sits at ao_ul + 0x400, the MD-side AO block at ao_ul + 0x800 and the PD
 * downlink block at pd_ul + 0x100.
 */
#define CCCI_DPMAIF_AO_DL_BASE		0x400
#define CCCI_DPMAIF_AO_MD_DL_BASE	0x800
#define CCCI_DPMAIF_PD_DL_BASE		0x100
#define CCCI_DPMAIF_PD_WDMA_BASE	0x300

#define CCCI_DPMAIF_AO_DL_PKTINFO_CON0	0x00
#define CCCI_DPMAIF_AO_DL_PKTINFO_CON1	0x04
#define CCCI_DPMAIF_AO_DL_PKTINFO_CON2	0x08
#define CCCI_DPMAIF_AO_DL_RDY_CHK_THRES	0x0c
#define CCCI_DPMAIF_AO_DL_BAT_STA0	0x10
#define CCCI_DPMAIF_AO_DL_BAT_STA1	0x14
#define CCCI_DPMAIF_AO_DL_BAT_STA2	0x18
#define CCCI_DPMAIF_AO_DL_PIT_STA0	0x20
#define CCCI_DPMAIF_AO_DL_PIT_STA1	0x24
#define CCCI_DPMAIF_AO_DL_PIT_STA2	0x28
#define CCCI_DPMAIF_AO_DL_PIT_STA3	0x2c
#define CCCI_DPMAIF_AO_DL_RDY_CHK_FRG_THRES	0x30
#define CCCI_DPMAIF_AO_DL_FRGBAT_STA0	0x40
#define CCCI_DPMAIF_AO_DL_FRGBAT_STA1	0x44
#define CCCI_DPMAIF_AO_DL_FRGBAT_STA2	0x48
#define CCCI_DPMAIF_AO_DL_PIT_STA4	0x54

#define CCCI_DPMAIF_PD_DL_BAT_INIT	0x00
#define CCCI_DPMAIF_PD_DL_BAT_ADD	0x04
#define CCCI_DPMAIF_PD_DL_BAT_INIT_CON0	0x08
#define CCCI_DPMAIF_PD_DL_BAT_INIT_CON1	0x0c
#define CCCI_DPMAIF_PD_DL_PIT_INIT	0x20
#define CCCI_DPMAIF_PD_DL_PIT_ADD	0x24
#define CCCI_DPMAIF_PD_DL_PIT_INIT_CON0	0x28
#define CCCI_DPMAIF_PD_DL_PIT_INIT_CON1	0x2c
#define CCCI_DPMAIF_PD_DL_PIT_INIT_CON2	0x30
#define CCCI_DPMAIF_PD_DL_PIT_INIT_CON3	0x34

/* The downlink status and mask live in AP_MISC, next to the uplink ones. */
#define CCCI_DPMAIF_PD_AP_DL_L2TISAR0	0x50
#define CCCI_DPMAIF_PD_AP_IP_BUSY	0x60
#define CCCI_DPMAIF_PD_AP_CG_EN		0x68
#define CCCI_DPMAIF_AO_UL_AP_L1TIMR0	0x8c
#define CCCI_DPMAIF_AO_UL_APDL_L2TIMR0	0x90
#define CCCI_DPMAIF_AO_UL_APDL_L2TIMCR0	0x94
#define CCCI_DPMAIF_AO_UL_APDL_L2TIMSR0	0x98

/*
 * The downlink engine keeps two state bits that the uplink path never needs:
 * an AP-side clock gate and a power on/off flag. The vendor sets both before
 * touching the BAT/PIT rings, and leaves the flag non-zero so that its own
 * power-down check reads "powered".
 */
#define CCCI_DPMAIF_PD_DL_RESERVE_RW	0xa8
#define CCCI_DPMAIF_PD_DL_DBG_STA1	0xb4
#define CCCI_DPMAIF_DL_IDLE		BIT(25)
#define CCCI_DPMAIF_DL_POWER_ON		0xff
#define CCCI_DPMAIF_PD_WDMA_WR_CMD_CON0	0x000
#define CCCI_DPMAIF_PD_WDMA_CHNL_CMD_CON3	0x014
#define CCCI_DPMAIF_WDMA_CTRL_OSTD_OFST	28
#define CCCI_DPMAIF_WDMA_CTRL_OSTD_MSK	0xf
#define CCCI_DPMAIF_WDMA_CTRL_OSTD_VALUE	0xe
#define CCCI_DPMAIF_AO_UL_L1TIMR0_VAL	((BIT(9) | BIT(10) | BIT(15) | BIT(16)))

/*
 * INFRA_AO_MEM word 0: the vendor sets this bit before the first ring write and
 * calls it the DPMAIF protection bit.
 */
#define CCCI_DPMAIF_INFRA_PROT		BIT(10)

/*
 * Infracfg reset words for the two DPMAIF domains. The vendor pulses these to
 * recover a wedged engine; the AO word lives at 0x140/0x144 and the PD word at
 * 0x150/0x154 of the same syscon the driver already has a regmap for.
 */
#define CCCI_DPMAIF_INFRA_RST0_AO	0x140
#define CCCI_DPMAIF_INFRA_RST1_AO	0x144
#define CCCI_DPMAIF_AO_RST_MASK		BIT(6)
#define CCCI_DPMAIF_INFRA_RST0_PD	0x150
#define CCCI_DPMAIF_INFRA_RST1_PD	0x154
#define CCCI_DPMAIF_PD_RST_MASK		BIT(2)

#define CCCI_DPMAIF_DL_INT_QDONE	BIT(0)
#define CCCI_DPMAIF_DL_INT_MTU_ERR	BIT(6)
#define CCCI_DPMAIF_DL_INT_EN		(CCCI_DPMAIF_DL_INT_QDONE | \
					 CCCI_DPMAIF_DL_INT_MTU_ERR)
/* Mask field inside AO_DL_RDY_CHK_THRES; a set bit there masks the event. */
#define CCCI_DPMAIF_AO_DL_ISR_MSK	0x7f

#define CCCI_DPMAIF_DL_BAT_INIT_ALLSET	BIT(0)
#define CCCI_DPMAIF_DL_BAT_FRG_INIT	BIT(16)
#define CCCI_DPMAIF_DL_BAT_INIT_EN	BIT(31)
#define CCCI_DPMAIF_DL_BAT_INIT_NOT_READY	BIT(31)
#define CCCI_DPMAIF_DL_PIT_INIT_ALLSET	BIT(0)
#define CCCI_DPMAIF_DL_PIT_INIT_EN	BIT(31)
#define CCCI_DPMAIF_DL_PIT_INIT_NOT_READY	BIT(31)
#define CCCI_DPMAIF_DL_ADD_UPDATE	BIT(31)
/* The fragment ring uses its own latch and its own add doorbell bit. */
#define CCCI_DPMAIF_DL_BAT_FRG_ADD	BIT(16)
#define CCCI_DPMAIF_DL_ADD_NOT_READY	BIT(31)

#define CCCI_DPMAIF_BAT_EN_MSK		BIT(16)
#define CCCI_DPMAIF_BAT_SIZE_MSK	0x0000ffff
#define CCCI_DPMAIF_BAT_ADDRH_MSK	0xff000000
#define CCCI_DPMAIF_PIT_EN_MSK		BIT(0)
#define CCCI_DPMAIF_PIT_SIZE_MSK	0x0000ffff
#define CCCI_DPMAIF_PIT_ADDRH_MSK	0xff000000

#define CCCI_DPMAIF_BAT_BUF_SZ_MSK	0x0001ff00
#define CCCI_DPMAIF_BAT_BUF_SZ_BASE	128
#define CCCI_DPMAIF_BAT_RSV_LEN_MSK	0x000000ff
#define CCCI_DPMAIF_BAT_CHECK_THRES_MSK	(0x3f << 16)
#define CCCI_DPMAIF_BAT_BID_MAXCNT_MSK	0xffff0000
#define CCCI_DPMAIF_BAT_REMAIN_MINSZ_MSK	0x0000ff00
#define CCCI_DPMAIF_BAT_REMAIN_SZ_BASE	16
#define CCCI_DPMAIF_PIT_CHK_NUM_MSK	0xff000000
#define CCCI_DPMAIF_CHK_RB_PITNUM_MSK	0x000000ff
#define CCCI_DPMAIF_PKT_ALIGN_MSK	(0x3 << 22)
#define CCCI_DPMAIF_PKT_ALIGN_EN	BIT(23)
#define CCCI_DPMAIF_CHKSUM_ON_MSK	BIT(31)
#define CCCI_DPMAIF_FRG_BAT_BUF_FEATURE_ON_MSK	BIT(28)
#define CCCI_DPMAIF_FRG_BAT_BUF_SZ_MSK	(0xff << 8)
#define CCCI_DPMAIF_FRG_CHECK_THRES_MSK	0x000000ff

#define CCCI_DPMAIF_DL_PIT_WRIDX_MSK	0x0000ffff
#define CCCI_DPMAIF_DL_BAT_WRIDX_MSK	0x0000ffff

/*
 * The v2 driver hands the modem the AP's starting PIT index through two
 * registers before it programs the ring: the index field of PIT_INIT_CON3 and
 * the reserve word in the uplink block, which the modem reads as a notification.
 */
#define CCCI_DPMAIF_UL_RESERVE_RW	0xa8
#define CCCI_DPMAIF_UL_RESERVE_AO_RW	0xac
/*
 * The modem-side reorder unit is told about a changed AP index through this
 * status word; the same-board driver writes it after every index update and
 * says so explicitly in a comment.
 */
#define CCCI_DPMAIF_PD_MD_L1TIMSR0	0x1c
#define CCCI_DPMAIF_MD_NOTIFY		BIT(0)
#define CCCI_DPMAIF_DUMMY_PIT_AIDX	1024
#define CCCI_DPMAIF_MD_DUMMYPIT_EN	BIT(31)

/*
 * Same-board tuning values. The vendor derives its thresholds from a BAT ring
 * of 8192 entries and a PIT ring of twice that; the BAT ring is only indexed by
 * the AP, so it can be far smaller, but the PIT ring is shared geometry and
 * keeps the vendor size.
 */
#define CCCI_DPMAIF_DL_BAT_ENTRIES	256
#define CCCI_DPMAIF_DL_PIT_ENTRIES	16384
#define CCCI_DPMAIF_DL_BAT_BUF_SZ	(128 * 13)
#define CCCI_DPMAIF_DL_FRG_BUF_SZ	(128 * 15)
#define CCCI_DPMAIF_DL_MTU_SIZE		(3 * 1024 + 8)
#define CCCI_DPMAIF_DL_BAT_REMAIN	64
#define CCCI_DPMAIF_DL_PKT_BIDCNT	1
#define CCCI_DPMAIF_DL_HW_CHK_BAT_NUM	62
#define CCCI_DPMAIF_DL_HW_CHK_FRG_NUM	62
#define CCCI_DPMAIF_DL_HW_CHK_PIT_NUM	(62 * 2)
#define CCCI_DPMAIF_DL_HW_CHK_RB_PIT_NUM	64
#define CCCI_DPMAIF_DL_PIT_BYTE_SIZE	16
#define CCCI_DPMAIF_DL_SNAP_LEN		64
/*
 * The downlink doorbells are polled from the interrupt handler, so their wait
 * is deliberately short: a stalled ring must be reported, not spin with
 * interrupts disabled.
 */
#define CCCI_DPMAIF_DL_POLL_LIMIT	10000

/* Two-word BAT entry, then the four-word v2 normal PIT entry. */
struct ccci_dpmaif_bat {
	u32 p_buffer_addr;
	u32 buffer_addr_ext:8;
	u32 reserved:24;
};

/*
 * A message PIT carries no payload: it names the channel the following normal
 * entries belong to. Its fields sit at different offsets than the normal ones,
 * and it occupies the same 16-byte slot.
 */
struct ccci_dpmaif_msg_pit {
	u32 packet_type:1;
	u32 c_bit:1;
	u32 check_sum:2;
	u32 error_bit:1;
	u32 reserved1:11;
	u32 channel_id:8;
	u32 network_type:3;
	u32 reserved2:4;
	u32 dp:1;
	u32 count_l:16;
	u32 reserved3:16;
};

struct ccci_dpmaif_pit {
	u32 packet_type:1;
	u32 c_bit:1;
	u32 buffer_type:1;
	u32 buffer_id:13;
	u32 data_len:16;
	u32 p_data_addr;
	u32 data_addr_ext;
	u32 pit_seq:16;
	u32 ig:1;
	u32 reserved2:7;
	u32 ulq_done:6;
	u32 dlq_done:2;
};

/* Two-word uplink descriptors; the layout is the vendor's, bit for bit. */
struct ccci_dpmaif_drb_pd {
	u32 dtyp:2;
	u32 c_bit:1;
	u32 reserved:5;
	u32 data_addr_ext:8;
	u32 data_len:16;
	u32 p_data_addr;
};

struct ccci_dpmaif_drb_msg {
	u32 dtyp:2;
	u32 c_bit:1;
	u32 reserved:13;
	u32 packet_len:16;
	u32 count_l:16;
	u32 channel_id:8;
	u32 network_type:3;
	u32 r:1;
	u32 ipv4:1;
	u32 l4:1;
	u32 rsv:2;
};

/*
 * Infracfg busy control. The vendor ORs a two-bit field at 12 and then clears
 * bit 13, so a post-init read normally shows bit 12 set and bit 13 clear: bit
 * 13 is the MD to AP wakeup event and is not part of the mask that stays set.
 */
#define CCCI_DPMAIF_INFRA_CTRL		0xc00
#define CCCI_DPMAIF_IP_BUSY_MASK	(0x3 << 12)
#define CCCI_DPMAIF_IP_BUSY_SHIFT	12
#define CCCI_DPMAIF_WAKEUP_EVENT	BIT(13)

/* One read-only sample of the block; every field is a raw register value. */
struct ccci_dpmaif_sample {
	u32 ao_ul_chnl0_sta;
	u32 ao_ul_chnl0_sta0;
	u32 ao_ul_l2timr0;
	u32 pd_ul_add_desc;
	u32 pd_ul_add_desc_ch0;
	u32 pd_md_ip_busy;
	u32 pd_md_ip_busy_mask;
	u32 sram_word0;
	u32 sram_word1;
	u32 infra_ctrl;
	unsigned long rate_main;
	unsigned long rate_bus;
};

/* Everything the downlink renderer needs, with no reference to the driver. */
struct ccci_dpmaif_dl_state {
	u32 bat_entries;
	u32 pit_entries;
	u32 bat_buf_sz;
	u32 frg_entries;
	u32 frg_buf_sz;
	u32 pit_rd;
	u32 bat_wr;
	/* Where the ring and its first buffer actually landed, for reachability. */
	u64 ring_dma;
	u64 buf_dma;
	u32 irqs;
	u32 pkts;
	u32 bytes;
	u32 msg_pits;
	u32 frag_pits;
	u32 err;
	u32 mtu_err;
	u32 overrun;
	u32 pit_init_stuck;
	u32 last_status;
	u32 last_len;
	u32 last_channel;
	u32 last_bid;
	u32 snap_len;
	u8 snap[CCCI_DPMAIF_DL_SNAP_LEN];
};

struct device;
struct ccci_dpmaif;

#if IS_ENABLED(CONFIG_OP6893_CCCI_DPMAIF)
struct ccci_dpmaif *ccci_dpmaif_find(struct device *consumer);
void ccci_dpmaif_allow_start(struct ccci_dpmaif *dpmaif);
int ccci_dpmaif_quiesce(struct ccci_dpmaif *dpmaif);
int ccci_dpmaif_reset_after_off(struct ccci_dpmaif *dpmaif);
int ccci_dpmaif_release_with_dl(struct ccci_dpmaif *dpmaif,
			       int (*release)(void *), void *data);
int ccci_dpmaif_power_with_dl(struct ccci_dpmaif *dpmaif,
			     int (*power_on)(void *), void *data);
/* Pure formatter, no I/O: one line per sampled register plus the masks. */
ssize_t ccci_dpmaif_format(const struct ccci_dpmaif_sample *sample, char *buf, size_t size);
/* Same idea for the downlink counters and the retained packet head. */
ssize_t ccci_dpmaif_format_dl(const struct ccci_dpmaif_dl_state *dl, char *buf, size_t size);
#else
static inline struct ccci_dpmaif *ccci_dpmaif_find(struct device *consumer) { return NULL; }
static inline void ccci_dpmaif_allow_start(struct ccci_dpmaif *dpmaif) { }
static inline int ccci_dpmaif_quiesce(struct ccci_dpmaif *dpmaif) { return 0; }
static inline int ccci_dpmaif_reset_after_off(struct ccci_dpmaif *dpmaif) { return 0; }

static inline int ccci_dpmaif_release_with_dl(struct ccci_dpmaif *dpmaif,
					     int (*release)(void *), void *data)
{
	return -EOPNOTSUPP;
}

static inline int ccci_dpmaif_power_with_dl(struct ccci_dpmaif *dpmaif,
					   int (*power_on)(void *), void *data)
{
	return -EOPNOTSUPP;
}

static inline ssize_t
ccci_dpmaif_format(const struct ccci_dpmaif_sample *sample, char *buf, size_t size)
{
	return -EOPNOTSUPP;
}

static inline ssize_t
ccci_dpmaif_format_dl(const struct ccci_dpmaif_dl_state *dl, char *buf, size_t size)
{
	return -EOPNOTSUPP;
}
#endif
#endif
