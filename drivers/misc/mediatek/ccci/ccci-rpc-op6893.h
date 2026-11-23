/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef __CCCI_RPC_OP6893_H
#define __CCCI_RPC_OP6893_H

struct ccci_rpc_provider;

/*
 * Immutable facts from the audited saved op6893 board configuration.
 * Caller must match realme,op6893; not a generic MT6893 provider or live probe.
 * Does not advertise ADC, sample pins, configure EINT or access calibration.
 * Provenance and snapshot applicability: MODEM-RPC-BOARD-FACTS.md.
 */
const struct ccci_rpc_provider *ccci_rpc_op6893_provider(void);

#endif
