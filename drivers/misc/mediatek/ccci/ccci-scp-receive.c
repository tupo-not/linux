// SPDX-License-Identifier: GPL-2.0-only
/* Linux IRQ/work adapter; dormant until an owner establishes a real reset. */
#include <linux/interrupt.h>
#include <linux/irq.h>
#include <linux/mutex.h>
#include <linux/platform_device.h>
#include <linux/slab.h>
#include <linux/spinlock.h>
#include <linux/sysfs.h>
#include <linux/workqueue.h>

#include "ccci-scp-drain.h"
#include "ccci-scp-irq.h"
#include "ccci-scp-mmio.h"
#include "ccci-scp-receive.h"

struct ccci_scp_receive {
	/* Serializes process start/STOP across the lock-free synchronization gap. */
	struct mutex control;
	/* Serializes worker, MMIO, queue, prerequisites and CPU event copies. */
	struct mutex *owner_lock;
	raw_spinlock_t gate;
	struct work_struct work;
	struct ccci_scp_mmio *mmio;
	struct ccci_scp_queue queue;
	struct ccci_scp_drain drain;
	int (*check)(void *context, u64 generation);
	void *context;
	int (*consume)(void *context, const struct ccci_scp_event *event, int error);
	void *consumer_context;
	int irqs[CCCI_SCP_MAX_MBOXES];
	u32 requested;
	/* One owned disable depth per bit, including the initial NO_AUTOEN depth. */
	u32 disabled;
	u64 generation;
	u64 last_generation;
	u64 queued_generation;
	u64 interrupts;
	u64 unexpected;
	u64 runs;
	int error;
	bool preparing;
	bool armed;
	bool queued;
	bool closing;
	bool terminal;
	bool fault;
	bool quiesced;
};

/* gate held; idempotent, no waits and no unowned extra disable depth. */
static void ccci_scp_receive_mask(struct ccci_scp_receive *receive)
{
	u32 i;

	for (i = 0; i < receive->requested; i++) {
		if (!(receive->disabled & BIT(i))) {
			disable_irq_nosync(receive->irqs[i]);
			receive->disabled |= BIT(i);
		}
	}
}

static int ccci_scp_receive_set_masked(void *context, bool masked)
{
	struct ccci_scp_receive *receive = context;
	unsigned long flags;
	u32 i;
	int ret = 0;

	raw_spin_lock_irqsave(&receive->gate, flags);
	if (masked) {
		ccci_scp_receive_mask(receive);
	} else if (!receive->armed || receive->closing || receive->fault) {
		ret = -ESHUTDOWN;
	} else {
		for (i = 0; i < receive->requested; i++) {
			if (receive->disabled & BIT(i)) {
				receive->disabled &= ~BIT(i);
				enable_irq(receive->irqs[i]);
			}
		}
	}
	raw_spin_unlock_irqrestore(&receive->gate, flags);
	return ret;
}

static int ccci_scp_receive_check(void *context, u64 generation)
{
	struct ccci_scp_receive *receive = context;
	unsigned long flags;
	bool valid;

	/* Called only under owner serialization, including by MMIO callbacks. */
	raw_spin_lock_irqsave(&receive->gate, flags);
	valid = !receive->closing && !receive->fault && generation &&
		generation == receive->generation && (receive->preparing || receive->armed);
	raw_spin_unlock_irqrestore(&receive->gate, flags);
	if (!valid)
		return -ESHUTDOWN;
	return receive->check(receive->context, generation);
}

static const struct ccci_scp_drain_ops ccci_scp_receive_drain_ops = {
	.set_masked = ccci_scp_receive_set_masked,
	.check = ccci_scp_receive_check,
};

/* Owner mutex held; never wait here. Gate closes before scrubbing CPU state. */
static void ccci_scp_receive_fault(struct ccci_scp_receive *receive, int error)
{
	unsigned long flags;

	raw_spin_lock_irqsave(&receive->gate, flags);
	receive->fault = true;
	receive->armed = false;
	receive->preparing = false;
	ccci_scp_receive_mask(receive);
	raw_spin_unlock_irqrestore(&receive->gate, flags);
	receive->error = error;
	ccci_scp_queue_invalidate(&receive->queue);
	ccci_scp_mmio_invalidate(receive->mmio);
	if (receive->consume)
		receive->consume(receive->consumer_context, NULL, error);
}

/* gate held; preserve the original queued token when coalescing interrupts. */
static bool ccci_scp_receive_queue(struct ccci_scp_receive *receive)
{
	if (receive->queued)
		return true;
	receive->queued_generation = receive->generation;
	receive->queued = true;
	if (queue_work(system_dfl_wq, &receive->work))
		return true;
	/* Bookkeeping mismatch: contain, never overwrite a pending work token. */
	receive->fault = true;
	receive->armed = false;
	ccci_scp_receive_mask(receive);
	return false;
}

static irqreturn_t ccci_scp_receive_handler(int number, void *data)
{
	struct ccci_scp_receive *receive = data;
	unsigned long flags;

	raw_spin_lock_irqsave(&receive->gate, flags);
	receive->interrupts++;
	ccci_scp_receive_mask(receive);
	if (receive->closing)
		goto out;
	receive->quiesced = false;
	if (!receive->armed || receive->fault) {
		receive->unexpected++;
		receive->fault = true;
	}
	if (!receive->terminal)
		ccci_scp_receive_queue(receive);
out:
	raw_spin_unlock_irqrestore(&receive->gate, flags);
	return IRQ_HANDLED;
}

static void ccci_scp_receive_work(struct work_struct *work)
{
	struct ccci_scp_receive *receive = container_of(work, struct ccci_scp_receive, work);
	unsigned long flags;
	u64 generation;
	bool valid, closing, fault;
	struct ccci_scp_event event;
	int ret;

	mutex_lock(receive->owner_lock);
	raw_spin_lock_irqsave(&receive->gate, flags);
	generation = receive->queued_generation;
	receive->queued = false;
	valid = receive->armed && !receive->closing && !receive->fault &&
		generation && generation == receive->generation;
	raw_spin_unlock_irqrestore(&receive->gate, flags);
	receive->runs++;
	ret = valid ? ccci_scp_drain_run(&receive->drain, generation) : -ESHUTDOWN;
	/* A handler may have reported a fault just after the drain unmasked. */
	raw_spin_lock_irqsave(&receive->gate, flags);
	closing = receive->closing;
	fault = receive->fault;
	valid = !fault && !closing;
	raw_spin_unlock_irqrestore(&receive->gate, flags);
	if (!ret && valid && receive->consume) {
		while (!(ret = ccci_scp_queue_pop(&receive->queue, generation, &event))) {
			ret = receive->consume(receive->consumer_context, &event, 0);
			memzero_explicit(&event, sizeof(event));
			if (ret)
				break;
		}
		if (ret == -ENODATA)
			ret = 0;
	}
	/* STOP can close admission while a mailbox response is draining. */
	if (closing && !fault && (ret == -ESHUTDOWN || !ret))
		goto out;
	if (ret || !valid)
		ccci_scp_receive_fault(receive, ret ? ret : -ESHUTDOWN);
out:
	mutex_unlock(receive->owner_lock);
}

static void ccci_scp_receive_quiesce(struct ccci_scp_receive *receive, bool terminal)
{
	unsigned long flags;
	u32 i;

	mutex_lock(&receive->control);
	raw_spin_lock_irqsave(&receive->gate, flags);
	receive->closing = true;
	receive->armed = false;
	receive->preparing = false;
	receive->terminal |= terminal;
	ccci_scp_receive_mask(receive);
	raw_spin_unlock_irqrestore(&receive->gate, flags);
	/* Never hold the owner mutex while waiting for its worker or IRQs. */
	for (i = 0; i < receive->requested; i++)
		synchronize_irq(receive->irqs[i]);
	cancel_work_sync(&receive->work);
	mutex_lock(receive->owner_lock);
	ccci_scp_drain_begin_stop(&receive->drain);
	ccci_scp_queue_invalidate(&receive->queue);
	ccci_scp_mmio_invalidate(receive->mmio);
	ccci_scp_drain_finish_stop(&receive->drain);
	raw_spin_lock_irqsave(&receive->gate, flags);
	receive->queued = false;
	receive->queued_generation = 0;
	receive->generation = 0;
	receive->quiesced = true;
	raw_spin_unlock_irqrestore(&receive->gate, flags);
	mutex_unlock(receive->owner_lock);
	mutex_unlock(&receive->control);
}

void ccci_scp_receive_stop(struct ccci_scp_receive *receive)
{
	if (receive)
		ccci_scp_receive_quiesce(receive, false);
}

int ccci_scp_receive_consumer(struct ccci_scp_receive *receive,
			      int (*consume)(void *context,
					     const struct ccci_scp_event *event, int error),
			      void *context)
{
	int ret = 0;

	if (!receive)
		return -EINVAL;
	mutex_lock(&receive->control);
	mutex_lock(receive->owner_lock);
	if (!receive->quiesced || receive->terminal) {
		ret = -EBUSY;
	} else {
		receive->consume = consume;
		receive->consumer_context = context;
	}
	mutex_unlock(receive->owner_lock);
	mutex_unlock(&receive->control);
	return ret;
}

static void ccci_scp_receive_release(void *data)
{
	struct ccci_scp_receive *receive = data;
	u32 i;

	ccci_scp_receive_quiesce(receive, true);
	for (i = 0; i < receive->requested; i++)
		irq_clear_status_flags(receive->irqs[i], IRQ_DISABLE_UNLAZY);
}

int ccci_scp_receive_start(struct ccci_scp_receive *receive,
			   const struct ccci_scp_layout *layout, u64 generation, u32 budget)
{
	const struct ccci_scp_rx_ops *hardware[CCCI_SCP_MAX_MBOXES];
	void *contexts[CCCI_SCP_MAX_MBOXES];
	struct ccci_scp_rx *rx[CCCI_SCP_MAX_MBOXES];
	unsigned long flags;
	u32 i;
	int ret;

	if (!receive || !layout || !generation || !budget ||
	    budget > CCCI_SCP_DRAIN_MAX_ROUNDS)
		return -EINVAL;
	mutex_lock(&receive->control);
	mutex_lock(receive->owner_lock);
	raw_spin_lock_irqsave(&receive->gate, flags);
	ret = -EBUSY;
	if (!receive->quiesced || receive->terminal)
		goto unlock_gate;
	ret = -ESTALE;
	if (generation <= receive->last_generation)
		goto unlock_gate;
	receive->quiesced = false;
	receive->last_generation = generation;
	receive->generation = generation;
	receive->closing = false;
	receive->preparing = true;
	receive->fault = false;
	raw_spin_unlock_irqrestore(&receive->gate, flags);
	receive->error = 0;
	ret = ccci_scp_mmio_prepare(receive->mmio, layout, generation,
				    ccci_scp_receive_check, receive);
	if (ret)
		goto fault;
	for (i = 0; i < CCCI_SCP_MAX_MBOXES; i++) {
		hardware[i] = ccci_scp_mmio_ops();
		contexts[i] = ccci_scp_mmio_port(receive->mmio, i);
		rx[i] = &receive->queue.rx[i];
	}
	ret = ccci_scp_queue_init(&receive->queue, layout, generation,
				  CCCI_SCP_QUEUE_CAPACITY, hardware, contexts);
	if (ret)
		goto fault;
	ret = ccci_scp_drain_prepare(&receive->drain, rx, generation, budget);
	if (ret)
		goto fault;
	raw_spin_lock_irqsave(&receive->gate, flags);
	ret = -ESHUTDOWN;
	if (!receive->fault && !receive->closing) {
		receive->preparing = false;
		receive->armed = true;
		ret = ccci_scp_receive_queue(receive) ? 0 : -EIO;
	}
	raw_spin_unlock_irqrestore(&receive->gate, flags);
	if (ret)
		goto fault;
	goto out;
unlock_gate:
	raw_spin_unlock_irqrestore(&receive->gate, flags);
	goto out;
fault:
	ccci_scp_receive_fault(receive, ret);
out:
	mutex_unlock(receive->owner_lock);
	mutex_unlock(&receive->control);
	return ret;
}

int ccci_scp_receive_pop(struct ccci_scp_receive *receive, u64 generation,
			 struct ccci_scp_event *event)
{
	unsigned long flags;
	int ret;

	if (!receive || !event)
		return -EINVAL;
	mutex_lock(receive->owner_lock);
	if (receive->consume) {
		ret = -EBUSY;
		goto out;
	}
	raw_spin_lock_irqsave(&receive->gate, flags);
	ret = generation && generation == receive->generation ? 0 : -ESTALE;
	raw_spin_unlock_irqrestore(&receive->gate, flags);
	if (ret)
		goto out;
	ret = ccci_scp_receive_check(receive, generation);
	if (!ret)
		ret = ccci_scp_queue_pop(&receive->queue, generation, event);
	/* A prerequisite failure also closes IRQs and invalidates both backends. */
	if (ret && ret != -ENODATA && ret != -ESTALE)
		ccci_scp_receive_fault(receive, ret);
out:
	mutex_unlock(receive->owner_lock);
	return ret;
}

int ccci_scp_receive_reserve(struct platform_device *pdev, struct mutex *owner_lock,
			     struct ccci_scp_mmio *mmio,
			     int (*check)(void *context, u64 generation), void *context,
			     struct ccci_scp_receive **result)
{
	struct ccci_scp_receive *receive;
	u32 i, j;
	int ret, number;

	if (!result)
		return -EINVAL;
	*result = NULL;
	if (!pdev || !owner_lock || !mmio || !check)
		return -EINVAL;
	ret = ccci_scp_irq_validate_specs(pdev);
	if (ret)
		return ret;
	receive = devm_kzalloc(&pdev->dev, sizeof(*receive), GFP_KERNEL);
	if (!receive)
		return -ENOMEM;
	receive->owner_lock = owner_lock;
	receive->mmio = mmio;
	receive->check = check;
	receive->context = context;
	receive->quiesced = true;
	mutex_init(&receive->control);
	raw_spin_lock_init(&receive->gate);
	INIT_WORK(&receive->work, ccci_scp_receive_work);
	ccci_scp_queue_invalidate(&receive->queue);
	ret = ccci_scp_drain_init(&receive->drain, &ccci_scp_receive_drain_ops, receive);
	if (ret)
		return ret;
	for (i = 0; i < CCCI_SCP_MAX_MBOXES; i++) {
		number = ccci_scp_irq_resolve(pdev, i);
		if (number < 0) {
			ret = number;
			goto stop;
		}
		for (j = 0; j < i; j++) {
			if (receive->irqs[j] == number) {
				ret = -EINVAL;
				goto stop;
			}
		}
		receive->irqs[i] = number;
		ret = devm_request_irq(&pdev->dev, number, ccci_scp_receive_handler,
				       IRQF_NO_AUTOEN, "op6893-scp-rx", receive);
		if (ret)
			goto stop;
		/* GIC lacks irq_disable: STOP must mask hardware, not disable lazily. */
		irq_set_status_flags(number, IRQ_DISABLE_UNLAZY);
		receive->disabled |= BIT(i);
		receive->requested++;
	}
	ret = devm_add_action_or_reset(&pdev->dev, ccci_scp_receive_release, receive);
	if (ret)
		return ret;
	*result = receive;
	return 0;
stop:
	ccci_scp_receive_release(receive);
	return ret;
}

ssize_t ccci_scp_receive_status(struct ccci_scp_receive *receive, char *buf)
{
	unsigned long flags;
	u32 i, masked = 0;
	bool value;
	ssize_t length;
	int ret;

	if (!receive)
		return -EOPNOTSUPP;
	for (i = 0; i < receive->requested; i++) {
		ret = irq_get_irqchip_state(receive->irqs[i], IRQCHIP_STATE_MASKED, &value);
		if (ret)
			return ret;
		masked += value;
	}
	mutex_lock(receive->owner_lock);
	raw_spin_lock_irqsave(&receive->gate, flags);
	length = sysfs_emit(buf,
			    "requested=%u gic_masked=%u armed=%u preparing=%u closing=%u fault=%u generation=%llu queued_generation=%llu interrupts=%llu unexpected=%llu runs=%llu queued=%u events=%u error=%d ready_known=0\n",
			    receive->requested, masked, receive->armed, receive->preparing,
			    receive->closing, receive->fault, receive->generation,
			    receive->queued_generation, receive->interrupts, receive->unexpected,
			    receive->runs, receive->queued, receive->queue.count, receive->error);
	raw_spin_unlock_irqrestore(&receive->gate, flags);
	mutex_unlock(receive->owner_lock);
	return length;
}
