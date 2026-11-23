// SPDX-License-Identifier: GPL-2.0-only
/* Initial NO_AUTOEN mailbox IRQ ownership; intentionally no receive/arm path. */
#include <linux/interrupt.h>
#include <linux/irq.h>
#include <linux/mutex.h>
#include <linux/of_irq.h>
#include <linux/platform_device.h>
#include <linux/slab.h>
#include <linux/spinlock.h>
#include <linux/sysfs.h>
#include <linux/workqueue.h>

#include "ccci-scp-irq.h"
#include "ccci-scp-queue.h"

struct ccci_scp_irq {
	/* Shared with the unique cfg/clock/SPM owner, never held across waits. */
	struct mutex *owner_lock;
	/* Serializes hard-handler scheduling against cleanup's closing gate. */
	raw_spinlock_t gate;
	struct work_struct fault_work;
	struct ccci_scp_queue queue;
	int irqs[CCCI_SCP_MAX_MBOXES];
	u32 requested;
	u32 extra_disabled;
	u64 unexpected;
	u64 work_runs;
	bool closing;
};

static int ccci_scp_irq_spec(struct device_node *node, const char *name, u32 box)
{
	struct of_phandle_args spec;
	int index, ret;

	index = of_property_match_string(node, "interrupt-names", name);
	if (index < 0)
		return index;
	ret = of_irq_parse_one(node, index, &spec);
	if (ret)
		return ret;
	ret = -EINVAL;
	if (of_device_is_compatible(spec.np, "arm,gic-v3") &&
	    of_device_is_available(spec.np) && spec.args_count == 3 &&
	    spec.args[0] == 0 && spec.args[1] == 436 + box && spec.args[2] == 4)
		ret = 0;
	of_node_put(spec.np);
	return ret;
}

int ccci_scp_irq_validate_specs(struct platform_device *pdev)
{
	char name[16];
	u32 i;
	int ret;

	if (!pdev)
		return -EINVAL;
	for (i = 0; i < CCCI_SCP_MAX_MBOXES; i++) {
		snprintf(name, sizeof(name), "mbox%u", i);
		ret = ccci_scp_irq_spec(pdev->dev.of_node, name, i);
		if (ret)
			return ret;
	}
	return 0;
}

int ccci_scp_irq_resolve(struct platform_device *pdev, u32 mbox)
{
	struct irq_data *data;
	struct irq_chip *chip;
	char name[16];
	int number;

	if (!pdev || mbox >= CCCI_SCP_MAX_MBOXES)
		return -EINVAL;
	snprintf(name, sizeof(name), "mbox%u", mbox);
	number = platform_get_irq_byname(pdev, name);
	if (number < 0)
		return number;
	if (irq_get_trigger_type(number) != IRQ_TYPE_LEVEL_HIGH)
		return -EINVAL;
	data = irq_get_irq_data(number);
	if (!data || irqd_to_hwirq(data) != 468 + mbox)
		return -EINVAL;
	chip = irq_data_get_irq_chip(data);
	/* Mask/unmask under a raw gate must not enter a sleeping bus lock. */
	if (!chip || chip->irq_bus_lock || chip->irq_bus_sync_unlock)
		return -EOPNOTSUPP;
	return number;
}

/* gate held: all normally have the one disable depth from NO_AUTOEN. */
static void ccci_scp_irq_contain(struct ccci_scp_irq *irq)
{
	u32 i;

	for (i = 0; i < irq->requested; i++) {
		if (irq->extra_disabled & BIT(i))
			continue;
		disable_irq_nosync(irq->irqs[i]);
		irq->extra_disabled |= BIT(i);
	}
}

static irqreturn_t ccci_scp_irq_handler(int number, void *data)
{
	struct ccci_scp_irq *irq = data;
	unsigned long flags;

	/* No mutex, register access, ACK, SMC, queue delivery or READY acceptance. */
	raw_spin_lock_irqsave(&irq->gate, flags);
	irq->unexpected++;
	ccci_scp_irq_contain(irq);
	if (!irq->closing)
		queue_work(system_dfl_wq, &irq->fault_work);
	raw_spin_unlock_irqrestore(&irq->gate, flags);
	return IRQ_HANDLED;
}

static void ccci_scp_irq_fault_work(struct work_struct *work)
{
	struct ccci_scp_irq *irq = container_of(work, struct ccci_scp_irq, fault_work);

	mutex_lock(irq->owner_lock);
	ccci_scp_queue_invalidate(&irq->queue);
	irq->work_runs++;
	mutex_unlock(irq->owner_lock);
}

static void ccci_scp_irq_stop(void *data)
{
	struct ccci_scp_irq *irq = data;
	unsigned long flags;
	u32 i;

	/* No owner mutex while waiting for a handler/worker that may need it. */
	raw_spin_lock_irqsave(&irq->gate, flags);
	irq->closing = true;
	ccci_scp_irq_contain(irq);
	raw_spin_unlock_irqrestore(&irq->gate, flags);
	for (i = 0; i < irq->requested; i++)
		synchronize_irq(irq->irqs[i]);
	cancel_work_sync(&irq->fault_work);
	mutex_lock(irq->owner_lock);
	ccci_scp_queue_invalidate(&irq->queue);
	mutex_unlock(irq->owner_lock);
}

int ccci_scp_irq_reserve(struct platform_device *pdev, struct mutex *owner_lock,
			 struct ccci_scp_irq **result)
{
	struct device *dev;
	struct ccci_scp_irq *irq;
	u32 i, j;
	int number, ret;

	if (!result)
		return -EINVAL;
	*result = NULL;
	if (!pdev || !owner_lock)
		return -EINVAL;
	dev = &pdev->dev;
	irq = devm_kzalloc(dev, sizeof(*irq), GFP_KERNEL);
	if (!irq)
		return -ENOMEM;
	irq->owner_lock = owner_lock;
	raw_spin_lock_init(&irq->gate);
	INIT_WORK(&irq->fault_work, ccci_scp_irq_fault_work);
	ccci_scp_queue_invalidate(&irq->queue);
	/* Validate every specifier before creating any mapping or IRQ action. */
	ret = ccci_scp_irq_validate_specs(pdev);
	if (ret)
		return ret;
	for (i = 0; i < CCCI_SCP_MAX_MBOXES; i++) {
		number = ccci_scp_irq_resolve(pdev, i);
		if (number < 0) {
			ret = number;
			goto stop;
		}
		for (j = 0; j < i; j++) {
			if (number == irq->irqs[j]) {
				ret = -EINVAL;
				goto stop;
			}
		}
		irq->irqs[i] = number;
		ret = devm_request_irq(dev, number, ccci_scp_irq_handler,
				       IRQF_NO_AUTOEN, "op6893-scp-mbox", irq);
		if (ret)
			goto stop;
		irq->requested++;
	}
	/* Newest devres action: quiesce BEFORE devm_free_irq and object release. */
	ret = devm_add_action_or_reset(dev, ccci_scp_irq_stop, irq);
	if (ret)
		return ret;
	*result = irq;
	return 0;
stop:
	ccci_scp_irq_stop(irq);
	/* Probe failure's devres teardown frees the actions already requested. */
	return ret;
}

ssize_t ccci_scp_irq_status(struct ccci_scp_irq *irq, char *buf)
{
	unsigned long flags;
	u64 unexpected, runs;
	u32 i, masked = 0;
	bool closing, value;
	int ret;

	if (!irq)
		return -EOPNOTSUPP;
	/* Read GIC mask state, not SCP/mailbox registers or IRQ pending state. */
	for (i = 0; i < irq->requested; i++) {
		ret = irq_get_irqchip_state(irq->irqs[i], IRQCHIP_STATE_MASKED, &value);
		if (ret)
			return ret;
		masked += value;
	}
	raw_spin_lock_irqsave(&irq->gate, flags);
	unexpected = irq->unexpected;
	closing = irq->closing;
	raw_spin_unlock_irqrestore(&irq->gate, flags);
	mutex_lock(irq->owner_lock);
	runs = irq->work_runs;
	mutex_unlock(irq->owner_lock);
	return sysfs_emit(buf,
			 "requested=%u gic_masked=%u auto_enable=0 arm_supported=0 generation=0 unexpected=%llu work_runs=%llu closing=%u queue_active=0 ready_known=0\n",
			 irq->requested, masked, unexpected, runs, closing);
}
