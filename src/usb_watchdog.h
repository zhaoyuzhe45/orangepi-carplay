#ifndef CARPLAY_USB_WATCHDOG_H
#define CARPLAY_USB_WATCHDOG_H

#include <stdint.h>

/*
 * FunctionFS bulk endpoints can park a synchronous read()/write() forever
 * after the gadget rebuilds its endpoints: dwc3 drops the cancelled request
 * and never completes it, so the thread stops draining the TUN and the whole
 * link dies silently. Two recoveries exist, cheapest first:
 *
 *   1. deliver a signal to the blocked thread (a read restarts safely),
 *   2. unbind/bind the UDC, which re-enumerates the gadget and unblocks the
 *      syscall with -ESHUTDOWN.
 *
 * The policy below is deliberately free of clocks and sysfs so the timing
 * rules can be unit tested; the caller supplies monotonic milliseconds and
 * performs the actions.
 */

enum usb_watchdog_channel {
	USB_WATCHDOG_TX = 0, /* bulk IN: TUN -> host */
	USB_WATCHDOG_RX = 1, /* bulk OUT: host -> TUN */
	USB_WATCHDOG_CHANNELS
};

#define USB_WATCHDOG_ACTION_WAKE(channel) (1u << (unsigned)(channel))
#define USB_WATCHDOG_ACTION_REBIND 0x100u

#define USB_WATCHDOG_DEFAULT_WAKE_MS 3000u
#define USB_WATCHDOG_DEFAULT_REBIND_MS 15000u
#define USB_WATCHDOG_DEFAULT_COOLDOWN_MS 30000u

struct usb_watchdog_policy {
	uint32_t wake_ms;     /* stuck this long -> signal the blocked thread */
	uint32_t rebind_ms;   /* still stuck -> re-enumerate the gadget */
	uint32_t cooldown_ms; /* minimum gap between two re-enumerations */
};

struct usb_watchdog {
	struct usb_watchdog_policy policy;
	/* Written by the endpoint threads, read by the watchdog thread. */
	_Atomic uint64_t started_ms[USB_WATCHDOG_CHANNELS];
	_Atomic int active[USB_WATCHDOG_CHANNELS];
	_Atomic int warned[USB_WATCHDOG_CHANNELS];
	_Atomic int seen_traffic[USB_WATCHDOG_CHANNELS];
	_Atomic uint64_t last_rebind_ms;
	_Atomic int has_rebound;
	_Atomic unsigned wakes;
	_Atomic unsigned rebinds;
};

#define USB_WATCHDOG_UDC_NAME_MAX 64

struct usb_watchdog_rebind_report {
	char udc[USB_WATCHDOG_UDC_NAME_MAX];
	int unbound;
	int rebound;
};

void usb_watchdog_init(struct usb_watchdog *watchdog,
		       const struct usb_watchdog_policy *policy);

/* Called around a blocking FunctionFS syscall. begin() records the monotonic
 * start time; end() marks the channel healthy again. */
void usb_watchdog_begin(struct usb_watchdog *watchdog,
			enum usb_watchdog_channel channel, uint64_t now_ms);
void usb_watchdog_end(struct usb_watchdog *watchdog,
		      enum usb_watchdog_channel channel);

/* Marks that the channel carried at least one datagram. Until then the
 * watchdog stays silent for that channel: a host that never drives the bulk
 * pipe (no NCM driver, still enumerating, or a head unit that only speaks
 * iAP2) is idle, not wedged. Treating that as a stall made the daemon poke the
 * thread every 3 s and re-enumerate the gadget every 15 s, which a car head
 * unit would see as an endless replug. */
void usb_watchdog_progress(struct usb_watchdog *watchdog,
			   enum usb_watchdog_channel channel);

/* Returns a bitmask of USB_WATCHDOG_ACTION_*; performs no side effects. */
unsigned usb_watchdog_tick(struct usb_watchdog *watchdog, uint64_t now_ms);

unsigned usb_watchdog_wake_count(struct usb_watchdog *watchdog);
unsigned usb_watchdog_rebind_count(struct usb_watchdog *watchdog);

/* Rename-free rebind: read the bound controller name, write an empty line to
 * unbind, then write the name back after settle_ms. Returns 0 on success. */
int usb_watchdog_rebind_udc(const char *udc_path, unsigned settle_ms,
			    struct usb_watchdog_rebind_report *report);

#endif /* CARPLAY_USB_WATCHDOG_H */
