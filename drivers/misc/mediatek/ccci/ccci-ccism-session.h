/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef __CCCI_CCISM_SESSION_H
#define __CCCI_CCISM_SESSION_H

#include "ccci-ccism.h"

struct device;
struct ccci_ccism_session;

struct ccci_ccism_session_status {
	enum ccci_ccism_state state;
	u64 generation;
	int error;
	int cleanup_error;
	int queue_error;
	u32 queued;
	bool armed;
	bool md_open;
	bool scp_open;
	bool bridge_touched;
};

/* Worker callback: may close RX admission, never take the parent MD mutex. */
typedef void (*ccci_ccism_session_fault_fn)(void *context, int error);

/* No hardware access. arm() takes the real acquired SCP lease generation. */
struct ccci_ccism_session *
ccci_ccism_session_create(struct device *dev, const struct ccci_ccism_ops *ops,
			  void *context, const struct ccci_ccism_plan *plan,
			  ccci_ccism_session_fault_fn fault);
int ccci_ccism_session_arm(struct ccci_ccism_session *session, u64 generation);
/* These copy scalar/header data, without calling the core or taking a mutex. */
int ccci_ccism_session_md_ready(struct ccci_ccism_session *session);
int ccci_ccism_session_md_event(struct ccci_ccism_session *session,
				const struct ccci_ccif_event *event);
/* Matches ccci_scp_state_fn; safe under the SCP provider's mutex. */
void ccci_ccism_session_scp_notify(void *context, u64 generation, u32 state, int error);
/* Returns after same-generation STOP cleanup, or an error. May sleep. */
int ccci_ccism_session_stop(struct ccci_ccism_session *session, bool md_stopped);
void ccci_ccism_session_status(struct ccci_ccism_session *session,
			       struct ccci_ccism_session_status *status);
/*
 * Lifecycle calls must be externally serialized. Quiesce refuses a retained
 * bridge, leaving its STOP receiver alive. Before destroy, release the SCP
 * client and join all producers: closing admission alone cannot pin an object
 * against a producer that still holds its address. Failed destroy retains it.
 */
int ccci_ccism_session_quiesce(struct ccci_ccism_session *session);
/* After quiesce and SCP lease release, with the MD producer also joined. */
int ccci_ccism_session_rearm(struct ccci_ccism_session *session, bool commit);
int ccci_ccism_session_destroy(struct ccci_ccism_session *session);

#endif
