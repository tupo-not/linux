// SPDX-License-Identifier: GPL-2.0-only
/*
 * AT/MUX byte pipe for the same-board CCCI UART2 port. The kernel owns only
 * the CCCI header: userspace reads and writes the raw 0710 stream, exactly as
 * the vendor char port does for ttyC0. No framing, no AT parsing, no reply is
 * invented here.
 */
#include <linux/completion.h>
#include <linux/device.h>
#include <linux/fs.h>
#include <linux/kernel.h>
#include <linux/kref.h>
#include <linux/miscdevice.h>
#include <linux/mutex.h>
#include <linux/poll.h>
#include <linux/slab.h>
#include <linux/spinlock.h>
#include <linux/sysfs.h>
#include <linux/string.h>
#include <linux/uaccess.h>
#include <linux/unaligned.h>
#include <linux/wait.h>

#include "ccci-ccif.h"
#include "ccci-md-at.h"

/* Same-board CCCI_UART2_RX: MD -> AP AT/MUX channel. */
#define CCCI_AT_RX_CHANNEL	10
/* Power of two; the whole 0710 stream is a byte pipe, not a record log. */
#define CCCI_AT_RING		16384
#define CCCI_AT_STOP_MS		2000

struct ccci_md_at {
	struct device *dev;
	struct ccci_ccif *ccif;
	struct miscdevice misc;
	struct kref refs;
	/* Serializes file ownership, HS2 and stop/rearm. */
	struct mutex lock;
	/* Ring and counters: shared with the RX worker. */
	spinlock_t ring_lock;
	wait_queue_head_t wait;
	struct completion closed;
	u8 *ring;
	u32 head;
	u32 tail;
	struct file *open_file;
	int error;
	bool removed;
	bool rearmable;
	bool hs2;
	bool stopped;
	u32 rx_frames;
	u32 rx_bytes;
	u32 rx_dropped;
	u32 tx_frames;
	u32 tx_bytes;
	int tx_error;
};

static u32 at_queued(const struct ccci_md_at *at)
{
	return at->head - at->tail;
}

/* MD -> AP: bounded copy of the payload only, never the CCCI header. */
int ccci_md_at_receive(struct ccci_md_at *at, const struct ccci_ccif_event *event)
{
	const u8 *bytes;
	u32 start = CCCI_CCIF_HEADER;
	u32 length;
	unsigned long flags;
	int ret = 0;

	if (!at || !event)
		return -EINVAL;
	if (event->header.channel != CCCI_AT_RX_CHANNEL || event->size <= start)
		return 0;
	bytes = event->data;
	length = event->size - start;
	spin_lock_irqsave(&at->ring_lock, flags);
	if (at->error || at->removed) {
		ret = at->error ? at->error : -ENODEV;
	} else if (at_queued(at) + length > CCCI_AT_RING) {
		/* A dropped byte corrupts the 0710 stream: latch instead of losing. */
		at->rx_dropped++;
		at->error = -ENOSPC;
		ret = at->error;
	} else {
		for (u32 i = 0; i < length; i++)
			at->ring[(at->head + i) & (CCCI_AT_RING - 1)] = bytes[start + i];
		at->head += length;
		at->rx_frames++;
		at->rx_bytes += length;
	}
	spin_unlock_irqrestore(&at->ring_lock, flags);
	if (!ret)
		wake_up_interruptible(&at->wait);
	return ret;
}

int ccci_md_at_hs2(struct ccci_md_at *at)
{
	/* No port created is a legitimate configuration, not a fault. */
	if (!at)
		return 0;
	mutex_lock(&at->lock);
	if (at->error)
		goto out;
	at->hs2 = true;
	wake_up_interruptible(&at->wait);
out:
	mutex_unlock(&at->lock);
	return at->error;
}

void ccci_md_at_abort(struct ccci_md_at *at, int error)
{
	if (!at)
		return;
	mutex_lock(&at->lock);
	if (!at->error)
		at->error = error < 0 ? error : -EIO;
	mutex_unlock(&at->lock);
	wake_up_interruptible_all(&at->wait);
}

int ccci_md_at_offline(struct ccci_md_at *at)
{
	if (!at)
		return 0;
	mutex_lock(&at->lock);
	at->stopped = true;
	if (!at->error)
		at->error = -ESHUTDOWN;
	mutex_unlock(&at->lock);
	wake_up_interruptible_all(&at->wait);
	return 0;
}

int ccci_md_at_stop(struct ccci_md_at *at, bool md_stopped)
{
	bool open;

	if (!at)
		return 0;
	if (!md_stopped)
		return -EBUSY;
	mutex_lock(&at->lock);
	at->stopped = true;
	if (!at->error)
		at->error = -ESHUTDOWN;
	open = at->open_file;
	mutex_unlock(&at->lock);
	wake_up_interruptible_all(&at->wait);
	/* The owner closes on the latched error; refuse to leak an open stream. */
	if (open && !wait_for_completion_timeout(&at->closed,
						 msecs_to_jiffies(CCCI_AT_STOP_MS)))
		return -EBUSY;
	mutex_lock(&at->lock);
	at->rearmable = true;
	mutex_unlock(&at->lock);
	return 0;
}

/* Dispatcher has joined RX; stop prevents a new file until commit. */
int ccci_md_at_rearm(struct ccci_md_at *at, bool commit)
{
	unsigned long flags;
	int ret = 0;

	if (!at)
		return 0;
	mutex_lock(&at->lock);
	spin_lock_irqsave(&at->ring_lock, flags);
	if (at->removed || !at->rearmable || at->open_file) {
		ret = -EBUSY;
	} else if (commit) {
		memzero_explicit(at->ring, CCCI_AT_RING);
		at->head = 0;
		at->tail = 0;
		at->rx_frames = 0;
		at->rx_bytes = 0;
		at->rx_dropped = 0;
		at->tx_frames = 0;
		at->tx_bytes = 0;
		at->tx_error = 0;
		at->hs2 = false;
		at->rearmable = false;
		at->stopped = false;
		at->error = 0;
	}
	spin_unlock_irqrestore(&at->ring_lock, flags);
	mutex_unlock(&at->lock);
	return ret;
}

static int at_open(struct inode *inode, struct file *file)
{
	struct miscdevice *misc = file->private_data;
	struct ccci_md_at *at = container_of(misc, struct ccci_md_at, misc);
	unsigned long flags;
	int ret = 0;

	mutex_lock(&at->lock);
	spin_lock_irqsave(&at->ring_lock, flags);
	if (at->removed) {
		ret = -ENODEV;
	} else if (at->open_file) {
		ret = -EBUSY;
	} else if (at->error) {
		ret = at->error;
	} else {
		at->open_file = file;
		reinit_completion(&at->closed);
	}
	spin_unlock_irqrestore(&at->ring_lock, flags);
	mutex_unlock(&at->lock);
	if (ret)
		return ret;
	file->private_data = at;
	return nonseekable_open(inode, file);
}

static int at_release(struct inode *inode, struct file *file)
{
	struct ccci_md_at *at = file->private_data;
	unsigned long flags;

	if (!at)
		return 0;
	mutex_lock(&at->lock);
	spin_lock_irqsave(&at->ring_lock, flags);
	if (at->open_file == file) {
		at->open_file = NULL;
		complete_all(&at->closed);
	}
	spin_unlock_irqrestore(&at->ring_lock, flags);
	mutex_unlock(&at->lock);
	return 0;
}

static ssize_t at_read(struct file *file, char __user *buffer, size_t count, loff_t *offset)
{
	struct ccci_md_at *at = file->private_data;
	unsigned long flags;
	u32 queued;
	u8 *staging;
	ssize_t ret;

	if (!count)
		return 0;
	for (;;) {
		spin_lock_irqsave(&at->ring_lock, flags);
		queued = at_queued(at);
		if (!queued && !at->error) {
			spin_unlock_irqrestore(&at->ring_lock, flags);
			if (file->f_flags & O_NONBLOCK)
				return -EAGAIN;
			ret = wait_event_interruptible(at->wait,
						       at_queued(at) || at->error);
			if (ret)
				return ret;
			continue;
		}
		if (!queued && at->error) {
			ret = at->error;
			spin_unlock_irqrestore(&at->ring_lock, flags);
			return ret;
		}
		break;
	}
	staging = kmalloc(min_t(size_t, count, queued), GFP_KERNEL);
	if (!staging) {
		spin_unlock_irqrestore(&at->ring_lock, flags);
		return -ENOMEM;
	}
	queued = min_t(u32, queued, count);
	for (u32 i = 0; i < queued; i++)
		staging[i] = at->ring[(at->tail + i) & (CCCI_AT_RING - 1)];
	at->tail += queued;
	spin_unlock_irqrestore(&at->ring_lock, flags);
	ret = copy_to_user(buffer, staging, queued) ? -EFAULT : queued;
	kfree(staging);
	return ret;
}

/* AP -> MD: userspace supplies the 0710 stream, the kernel supplies the header. */
static ssize_t at_write(struct file *file, const char __user *buffer, size_t count,
			loff_t *offset)
{
	struct ccci_md_at *at = file->private_data;
	u8 *staging;
	size_t sent = 0;
	int ret;

	if (!count)
		return 0;
	if (count > CCCI_AT_RING)
		return -EMSGSIZE;
	staging = memdup_user(buffer, count);
	if (IS_ERR(staging))
		return PTR_ERR(staging);
	while (sent < count) {
		u32 chunk = min_t(size_t, count - sent, CCCI_CCIF_AT_MAX);

		mutex_lock(&at->lock);
		ret = at->error;
		if (!ret && !at->hs2)
			ret = -ENOTCONN;
		if (!ret)
			ret = ccci_ccif_at_send(at->ccif, staging + sent, chunk, true);
		/*
		 * Backpressure and a not-yet-accepted HS2 are transient: latch only
		 * a real transport fault, otherwise the pipe would die before HS2.
		 */
		if (ret && ret != -EBUSY && ret != -EAGAIN && ret != -ENOTCONN)
			at->error = ret;
		mutex_unlock(&at->lock);
		if (ret == -EBUSY || ret == -EAGAIN) {
			/* Backpressure, not a fault: let the owner retry the rest. */
			ret = sent ? sent : -EAGAIN;
			goto out;
		}
		if (ret) {
			at->tx_error = ret;
			goto out;
		}
		mutex_lock(&at->lock);
		at->tx_frames++;
		at->tx_bytes += chunk;
		mutex_unlock(&at->lock);
		sent += chunk;
	}
	ret = sent;
out:
	kfree_sensitive(staging);
	return ret;
}

static __poll_t at_poll(struct file *file, poll_table *wait)
{
	struct ccci_md_at *at = file->private_data;
	unsigned long flags;
	__poll_t mask;

	poll_wait(file, &at->wait, wait);
	spin_lock_irqsave(&at->ring_lock, flags);
	if (at->error || at->removed)
		mask = EPOLLERR | EPOLLHUP;
	else if (at_queued(at))
		mask = EPOLLIN | EPOLLRDNORM;
	else
		mask = 0;
	spin_unlock_irqrestore(&at->ring_lock, flags);
	if (!mask && at->hs2)
		mask = EPOLLOUT | EPOLLWRNORM;
	return mask;
}

static const struct file_operations at_fops = {
	.owner = THIS_MODULE,
	.open = at_open,
	.release = at_release,
	.read = at_read,
	.write = at_write,
	.poll = at_poll,
	.llseek = noop_llseek,
};

/* Explicit size: this is appended after another line, so buf is not page aligned. */
ssize_t ccci_md_at_status(struct ccci_md_at *at, char *buf, size_t size)
{
	unsigned long flags;
	u32 queued;
	int error;
	bool hs2, stopped;

	if (!at || !buf || !size)
		return -EOPNOTSUPP;
	spin_lock_irqsave(&at->ring_lock, flags);
	queued = at_queued(at);
	error = at->error;
	hs2 = at->hs2;
	stopped = at->stopped;
	spin_unlock_irqrestore(&at->ring_lock, flags);
	return scnprintf(buf, size,
			 "open=%u hs2=%u stopped=%u error=%d queued=%u rx_frames=%u rx_bytes=%u rx_dropped=%u tx_frames=%u tx_bytes=%u tx_error=%d\n",
			 at->open_file ? 1 : 0, hs2, stopped, error, queued,
			 at->rx_frames, at->rx_bytes, at->rx_dropped,
			 at->tx_frames, at->tx_bytes, at->tx_error);
}

static void at_free(struct kref *refs)
{
	struct ccci_md_at *at = container_of(refs, struct ccci_md_at, refs);

	kvfree(at->ring);
	kfree(at->misc.name);
	kfree(at);
}

static void at_remove(void *context)
{
	struct ccci_md_at *at = context;
	unsigned long flags;

	misc_deregister(&at->misc);
	spin_lock_irqsave(&at->ring_lock, flags);
	at->removed = true;
	spin_unlock_irqrestore(&at->ring_lock, flags);
	wake_up_interruptible_all(&at->wait);
	kref_put(&at->refs, at_free);
}

struct ccci_md_at *ccci_md_at_create(struct device *dev, struct ccci_ccif *ccif)
{
	struct ccci_md_at *at;
	int ret;

	if (!dev || !ccif)
		return ERR_PTR(-EINVAL);
	at = kzalloc_obj(*at);
	if (!at)
		return ERR_PTR(-ENOMEM);
	kref_init(&at->refs);
	mutex_init(&at->lock);
	spin_lock_init(&at->ring_lock);
	init_waitqueue_head(&at->wait);
	init_completion(&at->closed);
	at->ring = kvmalloc(CCCI_AT_RING, GFP_KERNEL);
	if (!at->ring) {
		ret = -ENOMEM;
		goto free;
	}
	at->dev = dev;
	at->ccif = ccif;
	at->misc.name = kasprintf(GFP_KERNEL, "ccci-at-%s", dev_name(dev));
	if (!at->misc.name) {
		ret = -ENOMEM;
		goto free;
	}
	at->misc.minor = MISC_DYNAMIC_MINOR;
	at->misc.mode = 0600;
	at->misc.parent = dev;
	at->misc.fops = &at_fops;
	ret = misc_register(&at->misc);
	if (ret)
		goto free;
	ret = devm_add_action_or_reset(dev, at_remove, at);
	if (ret)
		return ERR_PTR(ret);
	return at;
free:
	kref_put(&at->refs, at_free);
	return ERR_PTR(ret);
}
