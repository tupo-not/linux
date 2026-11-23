/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef __CCCI_SCP_AWAKE_H
#define __CCCI_SCP_AWAKE_H

#include <linux/types.h>

struct regmap;

/* One shared owner for the AP awake bits; never one instance per consumer. */
struct ccci_scp_awake {
	struct regmap *infra;
	u32 users;
	int cleanup_error;
	bool fault;
};

/*
 * No I/O. The provider supplies the uncached infracfg syscon regmap.
 * Initialize only before first use, or after a verified reset and draining
 * all old consumers. Reinitialization alone cannot recover the hardware.
 */
int ccci_scp_awake_init(struct ccci_scp_awake *awake, struct regmap *infra);

/*
 * Sleepable, serialized by the provider with STOP/reset and every AP user.
 * Before either call, the provider must establish real SCP READY and keep
 * its generation and required clocks stable through the entire operation.
 * A successful init or get does not establish READY, TCM or mailbox access.
 * On handshake failure the instance is faulted and must not be retried.
 * A failed final put retains users=1; cleanup_error preserves a second error.
 */
int ccci_scp_awake_get(struct ccci_scp_awake *awake);
int ccci_scp_awake_put(struct ccci_scp_awake *awake);

/* STOP/reset: reject subsequent calls, retain counts, perform no hardware I/O. */
void ccci_scp_awake_invalidate(struct ccci_scp_awake *awake);

#endif
