// SPDX-License-Identifier: GPL-2.0-only
/* Control-wire sequencing only: HS2_SEEN is not a filesystem/SCP/radio READY. */
#include <linux/bitops.h>
#include <linux/errno.h>
#include <linux/limits.h>
#include <linux/string.h>

#include "ccci-handshake.h"

static int handshake_fail(struct ccci_handshake *hs, int error)
{
	if (!hs->error)
		hs->error = error < 0 ? error : -EPROTO;
	hs->state = CCCI_HANDSHAKE_FAILED;
	return hs->error;
}

int ccci_handshake_begin(struct ccci_handshake *hs, const struct ccci_handshake_ops *ops,
			 void *context, const struct ccci_runtime_plan *plan,
			 const struct ccci_runtime_offer offers[CCCI_RUNTIME_FEATURES], u64 now_ms)
{
	int ret;

	if (!hs || !ops || !ops->publish || !ops->port || !plan)
		return -EINVAL;
	if (hs->state != CCCI_HANDSHAKE_IDLE)
		return -EALREADY;
	ret = ccci_runtime_validate(offers);
	if (ret)
		return ret;
	if (!offers[0].supported || !offers[27].supported)
		return -EOPNOTSUPP;
	/* BOOT_INFO directs HS2 to the same control RX channel, not an AT port. */
	if (offers[0].data[0] || offers[0].data[1] || offers[0].data[2] || offers[0].data[3])
		return -EINVAL;
	if (now_ms > U64_MAX - CCCI_HANDSHAKE_TIMEOUT_MS)
		return -ERANGE;
	ret = ccci_runtime_upload(plan, hs->upload, sizeof(hs->upload));
	if (ret)
		return ret;
	hs->ops = ops;
	hs->context = context;
	hs->offers = offers;
	hs->plan = *plan;
	hs->last_ms = now_ms;
	hs->deadline_ms = now_ms + CCCI_HANDSHAKE_TIMEOUT_MS;
	hs->rejected = CCCI_RUNTIME_FEATURES;
	hs->state = CCCI_HANDSHAKE_WAIT_HS1;
	return 0;
}

int ccci_handshake_tick(struct ccci_handshake *hs, u64 now_ms)
{
	if (hs->state == CCCI_HANDSHAKE_FAILED)
		return hs->error;
	if (hs->state != CCCI_HANDSHAKE_WAIT_HS1 && hs->state != CCCI_HANDSHAKE_WAIT_HS2 &&
	    hs->state != CCCI_HANDSHAKE_HS2_SEEN)
		return 0;
	if (now_ms < hs->last_ms)
		return handshake_fail(hs, -ERANGE);
	hs->last_ms = now_ms;
	if (hs->state != CCCI_HANDSHAKE_HS2_SEEN && now_ms >= hs->deadline_ms)
		return handshake_fail(hs, -ETIMEDOUT);
	return 0;
}

int ccci_handshake_consume(struct ccci_handshake *hs, const struct ccci_ccif_event *event,
			   u64 now_ms)
{
	struct ccci_ccif_header header;
	enum ccci_ccif_control kind;
	u8 query[CCCI_RUNTIME_FEATURES];
	int ret;

	if (!event)
		return -EINVAL;
	ret = ccci_handshake_tick(hs, now_ms);
	if (ret)
		return ret;
	if (hs->state != CCCI_HANDSHAKE_WAIT_HS1 && hs->state != CCCI_HANDSHAKE_WAIT_HS2 &&
	    hs->state != CCCI_HANDSHAKE_HS2_SEEN)
		return -ESHUTDOWN;
	if (event->type == CCCI_CCIF_EVENT_NOTIFY) {
		if (event->notifications & ~CCCI_CCIF_NOTIFY_VALID)
			return handshake_fail(hs, -EREMOTEIO);
		ret = hs->ops->port(hs->context, event);
		return ret ? handshake_fail(hs, ret) : 0;
	}
	ret = ccci_ccif_header_decode(event->data, event->size, &header);
	if (ret)
		return handshake_fail(hs, ret);
	kind = ccci_ccif_control_decode(&header);
	if (event->type == CCCI_CCIF_EVENT_SRAM) {
		if (hs->state != CCCI_HANDSHAKE_WAIT_HS1 || kind != CCCI_CCIF_CONTROL_HS1)
			return handshake_fail(hs, -EPROTO);
		ret = ccci_ccif_hs1_decode(event->data, event->size, query);
		if (!ret) {
			hs->hs1_seen = true;
			memcpy(hs->query, query, sizeof(hs->query));
			ret = ccci_runtime_encode(query, hs->offers, hs->runtime,
						  sizeof(hs->runtime), &hs->used,
						  hs->negotiated, &hs->rejected);
		}
		if (ret)
			return handshake_fail(hs, ret);
		/* v2.1 is legal only after actual negotiation of MULTI_MD_MPU. */
		if (hs->negotiated[27] != 2 || (hs->negotiated[0] & 15) != 2)
			return handshake_fail(hs, -EOPNOTSUPP);
		if (now_ms > U64_MAX - CCCI_HANDSHAKE_TIMEOUT_MS)
			return handshake_fail(hs, -ERANGE);
		hs->state = CCCI_HANDSHAKE_PUBLISH;
		ret = hs->ops->publish(hs->context, hs->runtime, hs->used, hs->upload);
		if (ret)
			return handshake_fail(hs, ret);
		hs->state = CCCI_HANDSHAKE_WAIT_HS2;
		hs->deadline_ms = now_ms + CCCI_HANDSHAKE_TIMEOUT_MS;
		/* Services may arm only after runtime publication entered WAIT_HS2. */
		ret = hs->ops->port(hs->context, event);
		return ret ? handshake_fail(hs, ret) : 0;
	}
	if (event->type != CCCI_CCIF_EVENT_RING)
		return handshake_fail(hs, -EPROTO);
	if (!header.channel) {
		if (event->queue || kind == CCCI_CCIF_CONTROL_HS1)
			return handshake_fail(hs, -EPROTO);
		if (kind == CCCI_CCIF_CONTROL_HS2) {
			if (hs->state != CCCI_HANDSHAKE_WAIT_HS2 ||
			    event->size != CCCI_CCIF_HEADER_SIZE)
				return handshake_fail(hs, -EPROTO);
			hs->hs2_seen = true;
			hs->state = CCCI_HANDSHAKE_HS2_SEEN;
			/* Queue post-HS2 services only after this exact wire validation. */
			ret = hs->ops->port(hs->context, event);
			return ret ? handshake_fail(hs, ret) : 0;
		}
		if (kind != CCCI_CCIF_CONTROL_OTHER)
			return handshake_fail(hs, -EREMOTEIO);
	}
	ret = hs->ops->port(hs->context, event);
	return ret ? handshake_fail(hs, ret) : 0;
}

int ccci_handshake_abort(struct ccci_handshake *hs, int error)
{
	if (!hs)
		return -EINVAL;
	return handshake_fail(hs, error);
}

void ccci_handshake_stop(struct ccci_handshake *hs)
{
	if (hs->state != CCCI_HANDSHAKE_FAILED && hs->state != CCCI_HANDSHAKE_IDLE)
		hs->state = CCCI_HANDSHAKE_STOPPED;
}
