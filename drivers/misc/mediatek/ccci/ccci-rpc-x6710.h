/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef __CCCI_RPC_X6710_H
#define __CCCI_RPC_X6710_H

struct ccci_rpc_provider;

/*
 * Immutable facts transcribed from the Infinix X6710 stock Android FDT
 * (device.dts: /md_attr_node, /soc/gpio, /soc/MD1_SIM1_HOT_PLUG_EINT,
 * /soc/MD1_SIM2_HOT_PLUG_EINT).  Caller must match infinix,x6710.
 * No ADC provider: the stock FDT does not expose an audited naming table.
 */
const struct ccci_rpc_provider *ccci_rpc_x6710_provider(void);

#endif
