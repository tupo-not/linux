/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef __CCCI_SERVICE_SESSION_H
#define __CCCI_SERVICE_SESSION_H

#include "ccci-service-router.h"
#include "ccci-ccif-rx.h"

struct device;
struct ccci_ccif;
struct ccci_service_session;

struct ccci_service_backend_ops {
	/* Worker context, session mutex held; copy before success, no reentry. */
	int (*submit)(void *context, u64 generation, u64 token,
		      enum ccci_service_kind kind, u32 slot,
		      const void *packet, size_t length);
	/* Worker context, once; no parent/session/endpoint lifecycle mutex. */
	void (*fault)(void *context, u64 generation, int error);
	/*
	 * Stop context after admission closes and work joins, without the session
	 * mutex. Revoke the endpoint and join backend producers before returning.
	 * Must not reenter session lifecycle calls or acquire a caller-held lock.
	 */
	void (*closing)(void *context, u64 generation);
};

struct ccci_service_session_status {
	u64 generation;
	u32 queued;
	u32 tx_packets;
	int tx_error;
	int error;
	bool armed;
	bool accepting;
	bool stopped;
	bool hs2_seen;
};

/* Dormant constructor: no MD/MMIO operation, no backend callback. */
struct ccci_service_session *
ccci_service_session_create(struct device *dev, struct ccci_ccif *ccif,
			    const struct ccci_service_backend_ops *ops,
			    void *context);
/* Owner lifecycle externally serialized; only after actual WAIT_HS2. */
int ccci_service_session_arm(struct ccci_service_session *session, u64 generation);
/* Bounded copies only; no mutex, backend, transport or parent lock acquired. */
int ccci_service_session_receive(struct ccci_service_session *session,
				 u64 generation, const struct ccci_ccif_event *event);
int ccci_service_session_hs2(struct ccci_service_session *session, u64 generation);
/* Success means queued, not transmitted; async rejection faults the session. */
int ccci_service_session_reply(struct ccci_service_session *session, u64 generation,
			       u64 token, enum ccci_service_kind kind, u32 slot,
			      const void *packet, size_t length);
int ccci_service_session_abort(struct ccci_service_session *session, u64 generation,
			       int error);
void ccci_service_session_status(struct ccci_service_session *session,
				 struct ccci_service_session_status *status);
/* Refuses an armed generation, including faults, until true stopped close. */
int ccci_service_session_offline(struct ccci_service_session *session);
/* md_stopped=false has no side effects; true closes admission and wipes data. */
void ccci_service_session_park(struct ccci_service_session *session);
int ccci_service_session_stop(struct ccci_service_session *session, bool md_stopped);
/* Reinitialize a joined session without reopening admission or reusing its generation. */
int ccci_service_session_rearm(struct ccci_service_session *session, bool commit);
/* Stop first; caller must also join MD RX producers before freeing the object. */
int ccci_service_session_destroy(struct ccci_service_session *session);

#endif
