/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef __CCCI_SCP_RUNTIME_H
#define __CCCI_SCP_RUNTIME_H

#include <linux/completion.h>
#include <linux/mutex.h>
#include <linux/workqueue.h>
#include "ccci-scp-awake.h"

struct ccci_scp_resource_owner;
struct platform_device;
struct device_attribute;
struct device;

typedef void (*ccci_scp_state_fn)(void *context, u64 generation, u32 state, int error);

/* Notify runs under the SCP owner lock: enqueue scalars only; no client API calls. */
struct ccci_scp_client {
	struct ccci_scp_resource_owner *owner;
	ccci_scp_state_fn notify;
	void *context;
	u64 generation;
	u32 tx_completed;
	bool attached;
	bool faulted;
};

struct ccci_scp_runtime {
	/* Outer lifecycle lock; never acquired by the receive callback. */
	struct mutex control;
	struct completion ready_event;
	struct work_struct fault_work;
	struct ccci_scp_resource_owner *owner;
	struct ccci_scp_awake awake;
	struct ccci_scp_client *client;
	bool client_attempted;
	bool clean_stop;
	bool released;
	u64 last_generation;
	const char *step;
	u32 gpr[2];
	u32 logger_events;
	u32 other_events;
	u32 apccci_events;
	u32 apccci_state;
	u32 awake_pairs;
	u32 tx_completed;
	int error;
	int cleanup_error;
	bool active;
	bool ready;
	bool saved;
	bool closing;
};

int ccci_scp_runtime_create(struct platform_device *pdev,
			    struct ccci_scp_resource_owner *owner);
/*
 * Provider lookup from mediatek,scp; reference/device link and devres teardown.
 * No I/O or lease at create. One lease per SCP generation, only after real READY.
 * The owner blocks manual STOP/MD-invalid while attached; faults still revoke
 * READY and notify. Quiesce MD before release, then join queued consumer work
 * before freeing its context; release closes notification admission.
 * check/send/acquire/release are sleepable; never invoke from notify.
 */
struct ccci_scp_client *ccci_scp_client_create(struct device *consumer,
					       ccci_scp_state_fn notify, void *context);
int ccci_scp_client_acquire(struct ccci_scp_client *client, u64 *generation);
int ccci_scp_client_check(struct ccci_scp_client *client, u64 generation);
int ccci_scp_client_send(struct ccci_scp_client *client, u16 op, u32 data);
void ccci_scp_client_release(struct ccci_scp_client *client);

extern struct device_attribute dev_attr_runtime;
extern struct device_attribute dev_attr_runtime_state;

#endif
