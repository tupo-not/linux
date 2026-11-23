/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef __CCCI_HANDSHAKE_H
#define __CCCI_HANDSHAKE_H

#include "ccci-ccif-rx.h"
#include "ccci-runtime.h"

#define CCCI_HANDSHAKE_TIMEOUT_MS	60000

enum ccci_handshake_state {
	CCCI_HANDSHAKE_IDLE,
	CCCI_HANDSHAKE_WAIT_HS1,
	CCCI_HANDSHAKE_PUBLISH,
	CCCI_HANDSHAKE_WAIT_HS2,
	CCCI_HANDSHAKE_HS2_SEEN,
	CCCI_HANDSHAKE_STOPPED,
	CCCI_HANDSHAKE_FAILED,
};

struct ccci_handshake_ops {
	int (*publish)(void *context, const void *runtime, u32 used, const void *upload);
	/*
	 * Accepted HS1 once, after publication/WAIT_HS2/deadline update; then
	 * accepted HS2 once before subsequent system replies. Exact validated
	 * events are passed through; callback failure contains the handshake.
	 */
	int (*port)(void *context, const struct ccci_ccif_event *event);
};

/* Serialized caller, immutable offers and ops through STOP; no hardware at init. */
struct ccci_handshake {
	const struct ccci_handshake_ops *ops;
	void *context;
	const struct ccci_runtime_offer *offers;
	struct ccci_runtime_plan plan;
	u8 runtime[CCCI_RUNTIME_HALF];
	u8 upload[CCCI_RUNTIME_UPLOAD];
	u8 query[CCCI_RUNTIME_FEATURES];
	u8 negotiated[CCCI_RUNTIME_FEATURES];
	u64 last_ms;
	u64 deadline_ms;
	u32 used;
	u32 rejected;
	enum ccci_handshake_state state;
	int error;
	bool hs1_seen;
	bool hs2_seen;
};

int ccci_handshake_begin(struct ccci_handshake *hs, const struct ccci_handshake_ops *ops,
			 void *context, const struct ccci_runtime_plan *plan,
			 const struct ccci_runtime_offer offers[CCCI_RUNTIME_FEATURES], u64 now_ms);
int ccci_handshake_tick(struct ccci_handshake *hs, u64 now_ms);
int ccci_handshake_consume(struct ccci_handshake *hs, const struct ccci_ccif_event *event,
			   u64 now_ms);
int ccci_handshake_abort(struct ccci_handshake *hs, int error);
void ccci_handshake_stop(struct ccci_handshake *hs);

#endif
