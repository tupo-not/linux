/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef __CCCI_SERVICE_ENDPOINT_H
#define __CCCI_SERVICE_ENDPOINT_H

#include <linux/errno.h>
#include <linux/kconfig.h>
#include "ccci-service-session.h"

struct ccci_service_endpoint;
struct ccci_rpc_provider;

/* Private experimental record, all fields little endian; no native pointers. */
#define CCCI_SERVICE_RECORD_VERSION 1
#define CCCI_SERVICE_RECORD_HEADER 32

#if IS_ENABLED(CONFIG_OP6893_CCCI_SERVICES)
/* Registers a root-only exclusive backend, but never starts MD or a session. */
struct ccci_service_endpoint *
ccci_service_endpoint_create(struct device *dev, struct ccci_ccif *ccif);
/* Immutable provider/table storage must survive stopped cleanup and joins. */
int ccci_service_endpoint_set_rpc(struct ccci_service_endpoint *endpoint,
				  const struct ccci_rpc_provider *provider);
int ccci_service_endpoint_reserve(struct ccci_service_endpoint *endpoint, u64 generation);
/* Future MD owner only: actual WAIT_HS2, externally serialized lifecycle. */
int ccci_service_endpoint_arm(struct ccci_service_endpoint *endpoint, u64 generation);
int ccci_service_endpoint_receive(struct ccci_service_endpoint *endpoint, u64 generation,
				  const struct ccci_ccif_event *event);
int ccci_service_endpoint_hs2(struct ccci_service_endpoint *endpoint, u64 generation);
int ccci_service_endpoint_abort(struct ccci_service_endpoint *endpoint,
				u64 generation, int error);
int ccci_service_endpoint_offline(struct ccci_service_endpoint *endpoint);
ssize_t ccci_service_endpoint_status(struct ccci_service_endpoint *endpoint, char *buf);
/* No endpoint mutex held: the stop callback revokes and joins file operations. */
void ccci_service_endpoint_park(struct ccci_service_endpoint *endpoint);
int ccci_service_endpoint_stop(struct ccci_service_endpoint *endpoint, bool md_stopped);
/* After stopped cleanup and producer joins; requires the old FD to be closed. */
int ccci_service_endpoint_rearm(struct ccci_service_endpoint *endpoint, bool commit);
#else
static inline struct ccci_service_endpoint *
ccci_service_endpoint_create(struct device *dev, struct ccci_ccif *ccif) { return NULL; }
static inline int ccci_service_endpoint_set_rpc(struct ccci_service_endpoint *endpoint,
						const struct ccci_rpc_provider *provider)
{
	return -EOPNOTSUPP;
}

static inline void ccci_service_endpoint_park(struct ccci_service_endpoint *endpoint) { }
static inline int ccci_service_endpoint_offline(struct ccci_service_endpoint *endpoint)
{
	return 0;
}

static inline ssize_t ccci_service_endpoint_status(struct ccci_service_endpoint *endpoint,
						   char *buf)
{
	return -EOPNOTSUPP;
}
#endif
#endif
