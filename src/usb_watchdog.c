#define _GNU_SOURCE
/*
 * usb_watchdog.c -- turns "endpoint syscall never returns" into a bounded
 * recovery: signal the blocked thread, then re-enumerate the UDC.
 *
 * See src/usb_watchdog.h for why FunctionFS bulk endpoints can wedge after a
 * gadget endpoint rebuild.
 */
#include "usb_watchdog.h"

#include <fcntl.h>
#include <stdatomic.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

static uint32_t policy_or_default(uint32_t value, uint32_t fallback)
{
	return value ? value : fallback;
}

void usb_watchdog_init(struct usb_watchdog *watchdog,
		       const struct usb_watchdog_policy *policy)
{
	int channel;

	if (!watchdog)
		return;
	watchdog->policy.wake_ms = policy_or_default(
		policy ? policy->wake_ms : 0, USB_WATCHDOG_DEFAULT_WAKE_MS);
	watchdog->policy.rebind_ms = policy_or_default(
		policy ? policy->rebind_ms : 0, USB_WATCHDOG_DEFAULT_REBIND_MS);
	watchdog->policy.cooldown_ms = policy_or_default(
		policy ? policy->cooldown_ms : 0,
		USB_WATCHDOG_DEFAULT_COOLDOWN_MS);
	for (channel = 0; channel < USB_WATCHDOG_CHANNELS; channel++) {
		atomic_store_explicit(&watchdog->started_ms[channel], 0,
				      memory_order_relaxed);
		atomic_store_explicit(&watchdog->active[channel], 0,
				      memory_order_relaxed);
		atomic_store_explicit(&watchdog->warned[channel], 0,
				      memory_order_relaxed);
		atomic_store_explicit(&watchdog->seen_traffic[channel], 0,
				      memory_order_relaxed);
	}
	atomic_store_explicit(&watchdog->last_rebind_ms, 0,
			      memory_order_relaxed);
	atomic_store_explicit(&watchdog->has_rebound, 0, memory_order_relaxed);
	atomic_store_explicit(&watchdog->wakes, 0, memory_order_relaxed);
	atomic_store_explicit(&watchdog->rebinds, 0, memory_order_relaxed);
}

void usb_watchdog_begin(struct usb_watchdog *watchdog,
			enum usb_watchdog_channel channel, uint64_t now_ms)
{
	if (!watchdog || (unsigned)channel >= USB_WATCHDOG_CHANNELS)
		return;
	/* Publish the timestamp before the channel becomes visible as active so
	 * the watchdog never computes an age from a stale start time. */
	atomic_store_explicit(&watchdog->started_ms[channel], now_ms,
			      memory_order_relaxed);
	atomic_store_explicit(&watchdog->warned[channel], 0,
			      memory_order_relaxed);
	atomic_store_explicit(&watchdog->active[channel], 1,
			      memory_order_release);
}

void usb_watchdog_end(struct usb_watchdog *watchdog,
		      enum usb_watchdog_channel channel)
{
	if (!watchdog || (unsigned)channel >= USB_WATCHDOG_CHANNELS)
		return;
	atomic_store_explicit(&watchdog->active[channel], 0,
			      memory_order_release);
	atomic_store_explicit(&watchdog->warned[channel], 0,
			      memory_order_relaxed);
}

void usb_watchdog_progress(struct usb_watchdog *watchdog,
			   enum usb_watchdog_channel channel)
{
	if (!watchdog || (unsigned)channel >= USB_WATCHDOG_CHANNELS)
		return;
	atomic_store_explicit(&watchdog->seen_traffic[channel], 1,
			      memory_order_relaxed);
}

unsigned usb_watchdog_tick(struct usb_watchdog *watchdog, uint64_t now_ms)
{
	uint64_t oldest = 0;
	unsigned action = 0;
	int channel;

	if (!watchdog)
		return 0;
	for (channel = 0; channel < USB_WATCHDOG_CHANNELS; channel++) {
		uint64_t started;
		uint64_t age;

		if (!atomic_load_explicit(&watchdog->active[channel],
					  memory_order_acquire))
			continue;
		/* Never carried data: the host simply is not using this pipe. */
		if (!atomic_load_explicit(&watchdog->seen_traffic[channel],
					  memory_order_relaxed))
			continue;
		started = atomic_load_explicit(&watchdog->started_ms[channel],
					       memory_order_relaxed);
		age = now_ms - started; /* monotonic clock, no wrap in practice */
		if (age >= watchdog->policy.wake_ms &&
		    !atomic_load_explicit(&watchdog->warned[channel],
					  memory_order_relaxed)) {
			atomic_store_explicit(&watchdog->warned[channel], 1,
					      memory_order_relaxed);
			atomic_fetch_add_explicit(&watchdog->wakes, 1,
						  memory_order_relaxed);
			action |= USB_WATCHDOG_ACTION_WAKE(channel);
		}
		if (age >= watchdog->policy.rebind_ms &&
		    (!oldest || started < oldest))
			oldest = started;
	}
	if (oldest) {
		int has_rebound = atomic_load_explicit(&watchdog->has_rebound,
						       memory_order_relaxed);
		uint64_t last = atomic_load_explicit(&watchdog->last_rebind_ms,
						     memory_order_relaxed);

		if (!has_rebound || now_ms - last >= watchdog->policy.cooldown_ms) {
			atomic_store_explicit(&watchdog->has_rebound, 1,
					      memory_order_relaxed);
			atomic_store_explicit(&watchdog->last_rebind_ms, now_ms,
					      memory_order_relaxed);
			atomic_fetch_add_explicit(&watchdog->rebinds, 1,
						  memory_order_relaxed);
			action |= USB_WATCHDOG_ACTION_REBIND;
		}
	}
	return action;
}

unsigned usb_watchdog_wake_count(struct usb_watchdog *watchdog)
{
	if (!watchdog)
		return 0;
	return atomic_load_explicit(&watchdog->wakes, memory_order_relaxed);
}

unsigned usb_watchdog_rebind_count(struct usb_watchdog *watchdog)
{
	if (!watchdog)
		return 0;
	return atomic_load_explicit(&watchdog->rebinds, memory_order_relaxed);
}

static ssize_t read_bound_udc(const char *path, char *name, size_t cap)
{
	ssize_t length;
	size_t end;
	int fd;

	fd = open(path, O_RDONLY | O_CLOEXEC);
	if (fd < 0)
		return -1;
	length = read(fd, name, cap - 1);
	close(fd);
	if (length <= 0)
		return -1;
	name[length] = '\0';
	end = (size_t)length;
	while (end > 0 && (name[end - 1] == '\n' || name[end - 1] == '\r' ||
			   name[end - 1] == ' '))
		name[--end] = '\0';
	return (ssize_t)end;
}

static int write_all(const char *path, const char *text, size_t length)
{
	ssize_t written;
	int fd;

	fd = open(path, O_WRONLY | O_CLOEXEC);
	if (fd < 0)
		return -1;
	written = write(fd, text, length);
	close(fd);
	return written == (ssize_t)length ? 0 : -1;
}

int usb_watchdog_rebind_udc(const char *udc_path, unsigned settle_ms,
			    struct usb_watchdog_rebind_report *report)
{
	char name[USB_WATCHDOG_UDC_NAME_MAX];
	char bind_line[USB_WATCHDOG_UDC_NAME_MAX + 2];
	ssize_t length;
	int bind_length;

	if (report) {
		memset(report, 0, sizeof(*report));
		report->unbound = 0;
		report->rebound = 0;
	}
	if (!udc_path || !*udc_path)
		return -1;
	/* An already empty UDC gives no name to restore, so a rebind would
	 * leave the gadget permanently unbound. Let the caller retry later. */
	length = read_bound_udc(udc_path, name, sizeof(name));
	if (length <= 0)
		return -1;
	if (report)
		snprintf(report->udc, sizeof(report->udc), "%s", name);
	if (write_all(udc_path, "\n", 1) < 0)
		return -1;
	if (report)
		report->unbound = 1;
	if (settle_ms)
		usleep(settle_ms * 1000u);
	bind_length = snprintf(bind_line, sizeof(bind_line), "%s\n", name);
	if (bind_length <= 0 || (size_t)bind_length >= sizeof(bind_line))
		return -1;
	if (write_all(udc_path, bind_line, (size_t)bind_length) < 0)
		return -1;
	if (report)
		report->rebound = 1;
	return 0;
}
