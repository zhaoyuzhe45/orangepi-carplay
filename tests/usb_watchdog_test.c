#define _GNU_SOURCE
#include <assert.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "../src/usb_watchdog.h"

static struct usb_watchdog_policy make_policy(uint32_t wake_ms,
					       uint32_t rebind_ms,
					       uint32_t cooldown_ms)
{
	struct usb_watchdog_policy policy;

	policy.wake_ms = wake_ms;
	policy.rebind_ms = rebind_ms;
	policy.cooldown_ms = cooldown_ms;
	return policy;
}

/* An idle channel must never produce an action, no matter how much time
 * passes: the watchdog may only fire for a syscall that is still in flight. */
static void test_idle_channel_is_silent(void)
{
	struct usb_watchdog wd;
	struct usb_watchdog_policy policy = make_policy(3000, 15000, 30000);

	usb_watchdog_init(&wd, &policy);
	assert(usb_watchdog_tick(&wd, 0) == 0);
	assert(usb_watchdog_tick(&wd, 1000) == 0);
	assert(usb_watchdog_tick(&wd, 600000) == 0);
	assert(usb_watchdog_wake_count(&wd) == 0);
	assert(usb_watchdog_rebind_count(&wd) == 0);
}

/* A pipe the host never drives must stay silent. Blocking on the first write
 * because the head unit has no NCM driver is not a wedge; re-enumerating on
 * that basis would look like an endless replug to the car. */
static void test_never_transferred_channel_is_silent(void)
{
	struct usb_watchdog wd;
	struct usb_watchdog_policy policy = make_policy(3000, 15000, 30000);

	usb_watchdog_init(&wd, &policy);
	usb_watchdog_begin(&wd, USB_WATCHDOG_TX, 1000);
	usb_watchdog_begin(&wd, USB_WATCHDOG_RX, 1000);
	assert(usb_watchdog_tick(&wd, 4000) == 0);
	assert(usb_watchdog_tick(&wd, 16000) == 0);
	assert(usb_watchdog_tick(&wd, 600000) == 0);
	assert(usb_watchdog_wake_count(&wd) == 0);
	assert(usb_watchdog_rebind_count(&wd) == 0);
}

/* Once data has moved, a later stall is a real wedge and must be recovered.
 * The wait before the first datagram must not carry over into the decision. */
static void test_progress_unlocks_detection(void)
{
	struct usb_watchdog wd;
	struct usb_watchdog_policy policy = make_policy(3000, 15000, 30000);

	usb_watchdog_init(&wd, &policy);
	usb_watchdog_begin(&wd, USB_WATCHDOG_TX, 1000);
	assert(usb_watchdog_tick(&wd, 100000) == 0);
	usb_watchdog_end(&wd, USB_WATCHDOG_TX);
	usb_watchdog_progress(&wd, USB_WATCHDOG_TX);
	usb_watchdog_begin(&wd, USB_WATCHDOG_TX, 200000);
	assert(usb_watchdog_tick(&wd, 203000) ==
	       USB_WATCHDOG_ACTION_WAKE(USB_WATCHDOG_TX));
	assert(usb_watchdog_tick(&wd, 215000) == USB_WATCHDOG_ACTION_REBIND);
}

/* A blocked write is poked exactly once when it crosses the wake threshold.
 * Repeating the signal on every poll would make the blocked syscall restart
 * and drop frames continuously. */
static void test_stall_wakes_once(void)
{
	struct usb_watchdog wd;
	struct usb_watchdog_policy policy = make_policy(3000, 15000, 30000);

	usb_watchdog_init(&wd, &policy);
	usb_watchdog_progress(&wd, USB_WATCHDOG_TX);
	usb_watchdog_begin(&wd, USB_WATCHDOG_TX, 1000);
	assert(usb_watchdog_tick(&wd, 3999) == 0);
	assert(usb_watchdog_tick(&wd, 4000) ==
	       USB_WATCHDOG_ACTION_WAKE(USB_WATCHDOG_TX));
	assert(usb_watchdog_wake_count(&wd) == 1);
	assert(usb_watchdog_tick(&wd, 4600) == 0);
	assert(usb_watchdog_tick(&wd, 12000) == 0);
	assert(usb_watchdog_wake_count(&wd) == 1);
}

/* The signal did not help: re-enumerating the gadget is the only recovery
 * left. It must happen once per cooldown window, not on every poll. */
static void test_stall_rebinds_after_threshold(void)
{
	struct usb_watchdog wd;
	struct usb_watchdog_policy policy = make_policy(3000, 15000, 30000);

	usb_watchdog_init(&wd, &policy);
	usb_watchdog_progress(&wd, USB_WATCHDOG_TX);
	usb_watchdog_begin(&wd, USB_WATCHDOG_TX, 1000);
	assert(usb_watchdog_tick(&wd, 4000) ==
	       USB_WATCHDOG_ACTION_WAKE(USB_WATCHDOG_TX));
	assert(usb_watchdog_tick(&wd, 15999) == 0);
	assert(usb_watchdog_tick(&wd, 16000) == USB_WATCHDOG_ACTION_REBIND);
	assert(usb_watchdog_rebind_count(&wd) == 1);
	/* Still stuck, but inside the cooldown window. */
	assert(usb_watchdog_tick(&wd, 16100) == 0);
	assert(usb_watchdog_tick(&wd, 45000) == 0);
	assert(usb_watchdog_rebind_count(&wd) == 1);
	/* Cooldown elapsed and the write is still blocked: retry. */
	assert(usb_watchdog_tick(&wd, 46000) == USB_WATCHDOG_ACTION_REBIND);
	assert(usb_watchdog_rebind_count(&wd) == 2);
}

/* Once the syscall returns, the channel is healthy again and a later stall
 * must be able to wake it a second time. */
static void test_end_clears_channel(void)
{
	struct usb_watchdog wd;
	struct usb_watchdog_policy policy = make_policy(3000, 15000, 30000);

	usb_watchdog_init(&wd, &policy);
	usb_watchdog_progress(&wd, USB_WATCHDOG_TX);
	usb_watchdog_begin(&wd, USB_WATCHDOG_TX, 1000);
	assert(usb_watchdog_tick(&wd, 5000) ==
	       USB_WATCHDOG_ACTION_WAKE(USB_WATCHDOG_TX));
	usb_watchdog_end(&wd, USB_WATCHDOG_TX);
	assert(usb_watchdog_tick(&wd, 60000) == 0);
	usb_watchdog_begin(&wd, USB_WATCHDOG_TX, 70000);
	assert(usb_watchdog_tick(&wd, 73000) ==
	       USB_WATCHDOG_ACTION_WAKE(USB_WATCHDOG_TX));
	assert(usb_watchdog_wake_count(&wd) == 2);
}

/* TX and RX recover differently (a signal restarts a read safely), so each
 * channel keeps its own age and warning state. */
static void test_channels_are_independent(void)
{
	struct usb_watchdog wd;
	struct usb_watchdog_policy policy = make_policy(3000, 15000, 30000);

	usb_watchdog_init(&wd, &policy);
	usb_watchdog_progress(&wd, USB_WATCHDOG_RX);
	usb_watchdog_progress(&wd, USB_WATCHDOG_TX);
	usb_watchdog_begin(&wd, USB_WATCHDOG_RX, 1000);
	assert(usb_watchdog_tick(&wd, 4000) ==
	       USB_WATCHDOG_ACTION_WAKE(USB_WATCHDOG_RX));
	usb_watchdog_end(&wd, USB_WATCHDOG_RX);
	usb_watchdog_begin(&wd, USB_WATCHDOG_TX, 5000);
	assert(usb_watchdog_tick(&wd, 8000) ==
	       USB_WATCHDOG_ACTION_WAKE(USB_WATCHDOG_TX));
}

static void write_text_file(const char *path, const char *text)
{
	int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0600);
	ssize_t length = (ssize_t)strlen(text);

	assert(fd >= 0);
	assert(write(fd, text, (size_t)length) == length);
	close(fd);
}

/* sysfs exposes the bound controller as text; a rebind means "write empty to
 * unbind, then write the controller name back". Verify both steps and that
 * the controller name survives a trailing newline. */
static void test_rebind_udc_round_trip(void)
{
	char path[] = "/data/local/tmp/usb-watchdog-udc-XXXXXX";
	struct usb_watchdog_rebind_report report;
	int fd;
	int rc;

	fd = mkstemp(path);
	assert(fd >= 0);
	close(fd);
	write_text_file(path, "fe800000.dwc3\n");
	rc = usb_watchdog_rebind_udc(path, 1, &report);
	assert(rc == 0);
	assert(report.unbound == 1);
	assert(report.rebound == 1);
	assert(strcmp(report.udc, "fe800000.dwc3") == 0);
	unlink(path);
}

static void test_rebind_udc_rejects_bad_state(void)
{
	struct usb_watchdog_rebind_report report;
	char path[] = "/data/local/tmp/usb-watchdog-empty-XXXXXX";
	int fd;

	assert(usb_watchdog_rebind_udc("/data/local/tmp/does-not-exist", 1,
				       &report) < 0);
	assert(report.unbound == 0);
	assert(report.rebound == 0);

	fd = mkstemp(path);
	assert(fd >= 0);
	close(fd);
	/* An empty UDC means nothing is bound, so there is no name to restore. */
	write_text_file(path, "\n");
	assert(usb_watchdog_rebind_udc(path, 1, &report) < 0);
	assert(report.rebound == 0);
	unlink(path);
}

int main(void)
{
	test_idle_channel_is_silent();
	test_never_transferred_channel_is_silent();
	test_progress_unlocks_detection();
	test_stall_wakes_once();
	test_stall_rebinds_after_threshold();
	test_end_clears_channel();
	test_channels_are_independent();
	test_rebind_udc_round_trip();
	test_rebind_udc_rejects_bad_state();
	puts("usb watchdog tests passed");
	return 0;
}
