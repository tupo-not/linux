// SPDX-License-Identifier: GPL-2.0-only
/* Sleepable CCISM sequencing separated from both MD and SCP receive locks. */
#include <linux/completion.h>
#include <linux/err.h>
#include <linux/ktime.h>
#include <linux/mutex.h>
#include <linux/slab.h>
#include <linux/spinlock.h>
#include <linux/string.h>
#include <linux/workqueue.h>

#include "ccci-ccism-session.h"

#define CCISM_SESSION_EVENTS 32
#define CCISM_SESSION_POLL_MS 1000

enum ccism_session_event_type {
	CCISM_SESSION_MD_READY,
	CCISM_SESSION_MD_EVENT,
	CCISM_SESSION_SCP_EVENT,
};

struct ccism_session_event {
	enum ccism_session_event_type type;
	u64 generation;
	u32 state;
	int error;
	struct ccci_ccif_event md;
	u8 packet[CCCI_CCIF_HEADER_SIZE];
};

struct ccci_ccism_session {
	/* Serializes core transitions and every sleepable hardware callback. */
	struct mutex lock;
	/* Protects only admission and bounded copies; never nests a mutex. */
	spinlock_t queue_lock;
	struct delayed_work work;
	struct completion stop_done;
	struct ccci_ccism core;
	const struct ccci_ccism_ops *ops;
	void *context;
	struct ccci_ccism_plan plan;
	u64 generation;
	ccci_ccism_session_fault_fn fault;
	struct ccism_session_event queue[CCISM_SESSION_EVENTS];
	u32 head;
	u32 count;
	int queue_error;
	bool queue_fault_pending;
	bool armed;
	bool md_open;
	bool scp_open;
	bool quiesced;
	bool fault_notified;
};

static u64 ccism_session_now(void)
{
	return ktime_to_ms(ktime_get_boottime());
}

static void ccism_session_report(struct ccci_ccism_session *session)
{
	unsigned long flags;

	if (session->core.error && !session->fault_notified) {
		session->fault_notified = true;
		spin_lock_irqsave(&session->queue_lock, flags);
		session->md_open = false;
		spin_unlock_irqrestore(&session->queue_lock, flags);
		if (session->fault)
			session->fault(session->context, session->core.error);
	}
}

static int ccism_session_enqueue(struct ccci_ccism_session *session,
				 const struct ccism_session_event *event)
{
	unsigned long flags;
	bool scp = event->type == CCISM_SESSION_SCP_EVENT;
	int ret = 0;

	spin_lock_irqsave(&session->queue_lock, flags);
	if (scp && !session->armed && !session->quiesced) {
		/* Do not lose a provider fault between lease acquire and arm. */
		if (!session->queue_error)
			session->queue_error = event->error < 0 ? event->error : -EPROTO;
		ret = session->queue_error;
	} else if (scp ? !session->scp_open : !session->md_open) {
		ret = session->queue_error ? session->queue_error : -ESHUTDOWN;
	} else if (session->count == CCISM_SESSION_EVENTS) {
		/* No blocking or hardware calls under a provider's receive lock. */
		if (!session->queue_error)
			session->queue_error = -ENOSPC;
		session->queue_fault_pending = true;
		session->md_open = false;
		ret = session->queue_error;
		mod_delayed_work(system_dfl_wq, &session->work, 0);
	} else {
		session->queue[(session->head + session->count) % CCISM_SESSION_EVENTS] = *event;
		session->count++;
		mod_delayed_work(system_dfl_wq, &session->work, 0);
	}
	spin_unlock_irqrestore(&session->queue_lock, flags);
	return ret;
}

/* Called only with the session mutex; queue_lock never encloses core calls. */
static bool ccism_session_take(struct ccci_ccism_session *session,
			       struct ccism_session_event *event)
{
	unsigned long flags;
	int error = 0;
	bool taken = false;

	spin_lock_irqsave(&session->queue_lock, flags);
	if (session->queue_fault_pending) {
		error = session->queue_error;
		session->queue_fault_pending = false;
		/* A lost event invalidates the whole pending protocol history. */
		session->head = 0;
		session->count = 0;
	} else if (session->count) {
		*event = session->queue[session->head];
		session->head = (session->head + 1) % CCISM_SESSION_EVENTS;
		session->count--;
		taken = true;
	}
	spin_unlock_irqrestore(&session->queue_lock, flags);
	if (error)
		ccci_ccism_fault(&session->core, error);
	return taken;
}

static void ccism_session_drain(struct ccci_ccism_session *session)
{
	struct ccism_session_event event;
	u32 budget = CCISM_SESSION_EVENTS;
	u64 now;

	while (budget-- && ccism_session_take(session, &event)) {
		now = ccism_session_now();
		switch (event.type) {
		case CCISM_SESSION_MD_READY:
			ccci_ccism_md_ready(&session->core, now);
			break;
		case CCISM_SESSION_MD_EVENT:
			event.md.data = event.packet;
			ccci_ccism_md_event(&session->core, &event.md, now);
			break;
		case CCISM_SESSION_SCP_EVENT:
			if (event.error)
				ccci_ccism_fault(&session->core, event.error);
			else
				ccci_ccism_scp_state(&session->core, event.generation,
						     event.state, now);
			break;
		}
	}
}

static void ccism_session_schedule(struct ccci_ccism_session *session)
{
	unsigned long flags;
	u64 now, delay = CCISM_SESSION_POLL_MS;
	enum ccci_ccism_state state = session->core.state;
	bool ticking = state != CCCI_CCISM_IDLE && state != CCCI_CCISM_STOPPED &&
		       state != CCCI_CCISM_FAILED;

	if (ticking && state != CCCI_CCISM_ARMED && state != CCCI_CCISM_RUNNING) {
		now = ccism_session_now();
		delay = session->core.deadline_ms > now ?
			min_t(u64, delay, session->core.deadline_ms - now) : 0;
	}
	spin_lock_irqsave(&session->queue_lock, flags);
	if (!session->quiesced && (ticking || session->count || session->queue_fault_pending))
		mod_delayed_work(system_dfl_wq, &session->work,
				 session->count || session->queue_fault_pending ? 0 :
				 msecs_to_jiffies(delay));
	spin_unlock_irqrestore(&session->queue_lock, flags);
}

static void ccism_session_work(struct work_struct *work)
{
	struct ccci_ccism_session *session =
		container_of(to_delayed_work(work), struct ccci_ccism_session, work);
	bool stopping;

	mutex_lock(&session->lock);
	if (session->quiesced) {
		mutex_unlock(&session->lock);
		return;
	}
	stopping = session->core.state == CCCI_CCISM_WAIT_STOP;
	ccism_session_drain(session);
	ccci_ccism_tick(&session->core, ccism_session_now());
	ccism_session_report(session);
	ccism_session_schedule(session);
	if (stopping && session->core.state != CCCI_CCISM_WAIT_STOP)
		complete_all(&session->stop_done);
	mutex_unlock(&session->lock);
}

struct ccci_ccism_session *
ccci_ccism_session_create(struct device *dev, const struct ccci_ccism_ops *ops,
			  void *context, const struct ccci_ccism_plan *plan,
			  ccci_ccism_session_fault_fn fault)
{
	struct ccci_ccism_session *session;

	if (!dev || !ops || !plan)
		return ERR_PTR(-EINVAL);
	session = kzalloc_obj(*session);
	if (!session)
		return ERR_PTR(-ENOMEM);
	mutex_init(&session->lock);
	spin_lock_init(&session->queue_lock);
	INIT_DELAYED_WORK(&session->work, ccism_session_work);
	init_completion(&session->stop_done);
	session->ops = ops;
	session->context = context;
	session->plan = *plan;
	session->fault = fault;
	return session;
}

int ccci_ccism_session_arm(struct ccci_ccism_session *session, u64 generation)
{
	unsigned long flags;
	int ret;

	mutex_lock(&session->lock);
	if (session->armed || session->quiesced) {
		ret = session->quiesced ? -ESHUTDOWN : -EALREADY;
		goto unlock;
	}
	if (!generation) {
		ret = -EINVAL;
		goto unlock;
	}
	if (generation <= session->generation) {
		ret = -ESTALE;
		goto unlock;
	}
	spin_lock_irqsave(&session->queue_lock, flags);
	ret = session->queue_error;
	spin_unlock_irqrestore(&session->queue_lock, flags);
	if (ret)
		goto unlock;
	ret = ccci_ccism_arm(&session->core, session->ops, session->context,
			     &session->plan, generation, ccism_session_now());
	if (!ret) {
		spin_lock_irqsave(&session->queue_lock, flags);
		session->armed = true;
		session->generation = generation;
		ret = session->queue_error;
		if (!ret) {
			session->md_open = true;
			session->scp_open = true;
		}
		spin_unlock_irqrestore(&session->queue_lock, flags);
		if (ret)
			ccci_ccism_fault(&session->core, ret);
		ccism_session_report(session);
		ccism_session_schedule(session);
	}
unlock:
	mutex_unlock(&session->lock);
	return ret;
}

int ccci_ccism_session_md_ready(struct ccci_ccism_session *session)
{
	struct ccism_session_event event = { .type = CCISM_SESSION_MD_READY };

	return ccism_session_enqueue(session, &event);
}

int ccci_ccism_session_md_event(struct ccci_ccism_session *session,
				const struct ccci_ccif_event *event)
{
	struct ccism_session_event queued = { .type = CCISM_SESSION_MD_EVENT };

	if (!event || !event->data || event->size != sizeof(queued.packet))
		return -EINVAL;
	queued.md = *event;
	queued.md.data = NULL;
	memcpy(queued.packet, event->data, sizeof(queued.packet));
	return ccism_session_enqueue(session, &queued);
}

void ccci_ccism_session_scp_notify(void *context, u64 generation, u32 state, int error)
{
	struct ccci_ccism_session *session = context;
	struct ccism_session_event event = {
		.type = CCISM_SESSION_SCP_EVENT,
		.generation = generation,
		.state = state,
		.error = error,
	};

	ccism_session_enqueue(session, &event);
}

/* MD is stopped: do not execute queued forward traffic or accept an old STOP. */
static void ccism_session_discard(struct ccci_ccism_session *session)
{
	unsigned long flags;
	int error = 0;
	u32 i;

	spin_lock_irqsave(&session->queue_lock, flags);
	session->md_open = false;
	if (session->queue_fault_pending)
		error = session->queue_error;
	for (i = 0; i < session->count && !error; i++) {
		struct ccism_session_event *event =
			&session->queue[(session->head + i) % CCISM_SESSION_EVENTS];

		if (event->type != CCISM_SESSION_SCP_EVENT)
			continue;
		if (event->error)
			error = event->error;
		else if (event->generation != session->generation)
			error = -ESTALE;
	}
	session->count = 0;
	session->head = 0;
	session->queue_fault_pending = false;
	spin_unlock_irqrestore(&session->queue_lock, flags);
	if (error)
		ccci_ccism_fault(&session->core, error);
}

static int ccism_session_begin_stop(struct ccci_ccism_session *session)
{
	int ret;

	/* A repeat must not discard the queued reply to the first STOP. */
	if (session->core.stop_attempted) {
		if (session->core.state == CCCI_CCISM_WAIT_STOP)
			return 0;
		return session->core.cleanup_error ?: session->core.error;
	}
	ccism_session_discard(session);
	ret = ccci_ccism_stop(&session->core, true, ccism_session_now());
	ccism_session_report(session);
	ccism_session_schedule(session);
	return ret;
}

int ccci_ccism_session_stop(struct ccci_ccism_session *session, bool md_stopped)
{
	bool waiting;
	int ret;

	if (!md_stopped)
		return -EBUSY;
	mutex_lock(&session->lock);
	ret = session->core.state == CCCI_CCISM_STOPPED ? 0 :
		ccism_session_begin_stop(session);
	waiting = session->core.state == CCCI_CCISM_WAIT_STOP;
	mutex_unlock(&session->lock);
	if (ret || !waiting)
		return ret;

	/* Leave both session and SCP provider locks available to the receiver. */
	if (!wait_for_completion_timeout(&session->stop_done,
					 msecs_to_jiffies(CCCI_CCISM_STOP_MS)))
		return -ETIMEDOUT;

	mutex_lock(&session->lock);
	if (session->core.state != CCCI_CCISM_STOPPED)
		ret = session->core.cleanup_error ?: session->core.error;
	mutex_unlock(&session->lock);
	return ret;
}

void ccci_ccism_session_status(struct ccci_ccism_session *session,
			       struct ccci_ccism_session_status *status)
{
	unsigned long flags;

	mutex_lock(&session->lock);
	*status = (struct ccci_ccism_session_status) {
		.state = session->core.state,
		.generation = session->generation,
		.error = session->core.error,
		.cleanup_error = session->core.cleanup_error,
		.armed = session->armed,
		.bridge_touched = session->core.bridge_touched,
	};
	spin_lock_irqsave(&session->queue_lock, flags);
	status->queued = session->count;
	status->queue_error = session->queue_error;
	status->md_open = session->md_open;
	status->scp_open = session->scp_open;
	spin_unlock_irqrestore(&session->queue_lock, flags);
	mutex_unlock(&session->lock);
}

int ccci_ccism_session_quiesce(struct ccci_ccism_session *session)
{
	unsigned long flags;
	int ret = 0;

	mutex_lock(&session->lock);
	if (session->core.bridge_touched || session->core.state == CCCI_CCISM_WAIT_STOP) {
		ret = -EBUSY;
	} else {
		spin_lock_irqsave(&session->queue_lock, flags);
		session->quiesced = true;
		session->md_open = false;
		session->scp_open = false;
		session->count = 0;
		session->queue_fault_pending = false;
		spin_unlock_irqrestore(&session->queue_lock, flags);
	}
	mutex_unlock(&session->lock);
	if (!ret)
		cancel_delayed_work_sync(&session->work);
	return ret;
}

int ccci_ccism_session_rearm(struct ccci_ccism_session *session, bool commit)
{
	unsigned long flags;
	int ret = 0;

	mutex_lock(&session->lock);
	spin_lock_irqsave(&session->queue_lock, flags);
	if (!session->quiesced || session->core.state != CCCI_CCISM_STOPPED ||
	    session->core.error || session->core.cleanup_error || session->queue_error) {
		ret = -EBUSY;
	} else if (commit) {
		memset(&session->core, 0, sizeof(session->core));
		memzero_explicit(session->queue, sizeof(session->queue));
		session->head = 0;
		session->quiesced = false;
		session->armed = false;
		session->fault_notified = false;
		reinit_completion(&session->stop_done);
	}
	spin_unlock_irqrestore(&session->queue_lock, flags);
	mutex_unlock(&session->lock);
	return ret;
}

int ccci_ccism_session_destroy(struct ccci_ccism_session *session)
{
	int ret;

	ret = ccci_ccism_session_quiesce(session);
	if (!ret)
		kfree(session);
	return ret;
}
