/*
 * iap2d.c -- CarPlay box side, milestone B: present an iPhone-like USB
 *            composite through FunctionFS.
 *            Orange Pi 4 LTS (RK3399, Android 8.1), kernel 4.4.
 *
 * Interface layout presented to the host -- copied from a real iPhone 8 in
 * wired-CarPlay mode (see docs/evidence/iphone8-carplay-working-descriptors-
 * 2026-10-27.md):
 *
 *   0/0:06.01.01 x3   PTP                    (kernel f_ptp, linked as f0)
 *   1/0:ff.fe.02 x2   Apple USBMUX / Mobile Device
 *   2/0:ff.fd.01 x0   Apple USB Ethernet (ipheth alt 0)
 *   ...               CDC-NCM control + data
 *
 * Two things here are deliberate and were wrong in earlier revisions:
 *
 *   * There is no 0xFF/0x42/0x01 interface.  It was invented to label a bare
 *     iAP2 bulk pair; a head unit looking for Apple's Mobile Device function
 *     (ff/fe/02, interface 1, immediately behind PTP) never finds one, which
 *     matches the A4L: it configures us and then issues no transfer at all.
 *     The bare iAP2 link and the USBMUX link now share the single ff/fe/02
 *     pair and are demultiplexed by frame header, exactly like a real phone:
 *     iAP2 link frames start 0x40 0x40, USBMUX frames 0xFE 0xED 0xFA 0xCE.
 *   * 0xFF/0xFD/0x01 is present but carries no endpoints and no protocol.
 *     Hosts use its presence as "this configuration is a CarPlay
 *     configuration" -- DiPlay's IphoneCarPlayConfiguration prefers exactly
 *     (usbmux && ncm && apple-ethernet).  Without it the configuration looks
 *     like "charging + PTP", so the head unit has no reason to open iAP2.
 *
 * The iAP2 stack therefore still has two transports, but they are no longer
 * two endpoints: whichever frame type shows up first owns the session for
 * that round (see g_native_active).
 *
 * Why there are no CDC functional descriptors (header/union/ethernet/NCM):
 * f_fs's descriptor parser only accepts INTERFACE, ENDPOINT, STRING, HID, OTG,
 * IAD and SS-companion descriptors; everything else, including CS_INTERFACE
 * (0x24), hits `default: return -EINVAL` and the whole write is rejected.
 * A userspace host (Android's UsbDevice, which is what DiPlay uses) parses only
 * interface and endpoint descriptors, so the functional descriptors are not
 * needed there.  A Linux host running cdc_ncm still needs the kernel NCM
 * function rather than this FunctionFS representation.
 *
 * Two FunctionFS blocks must be written: descriptors, then strings.  Writing
 * only the first leaves ffs_dev->desc_ready false and every bind attempt fails
 * with -ENODEV ("failed to start g1: -19").
 */
#define _GNU_SOURCE
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <linux/types.h>
#include <linux/usb/ch9.h>
#include <linux/usb/functionfs.h>
#include <poll.h>
#include <signal.h>
#include <stdatomic.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/eventfd.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>

#include "iap2link.h"
#include "carplay_log_sampling.h"
#include "iap2session.h"
#include "box_role.h"
#include "carplay_config.h"
#include "carplay_control.h"
#include "carplay_log.h"
#include "carplay_vehicle_state.h"
#include "lockdown.h"
#include "lockdown_tls.h"
#include "ncm_bridge.h"
#include "usbmux.h"
#include "usb_vendor.h"
#include "usbhost.h"

static void reset_session(void);

#define FFS_ROOT "/dev/usb-ffs/iap2"
#define EP0_PATH FFS_ROOT "/ep0"
/*
 * Where the daemon leaves a role-swap decision for the supervisor.  The
 * daemon must not touch dwc3 itself: switching roles means unbinding the
 * gadget whose endpoints this process is reading from.
 */
#define ROLE_REQUEST_PATH "/data/carplay/role_request"
/*
 * When the 0x51 hand-off happened, in CLOCK_MONOTONIC milliseconds.
 *
 * The hand-off crosses a process boundary (we exit, the supervisor switches
 * dwc3, a new daemon starts), so an in-memory timestamp cannot survive it.
 * CLOCK_MONOTONIC does survive it as long as the box is not rebooted, which
 * is exactly the window we care about.  One file, one number, measured once
 * per hand-off -- this is what turns "probably a few hundred ms" into a
 * number from the first real head unit we plug into.
 */
#define ROLE_SWAP_STAMP_PATH "/data/carplay/role-swap.stamp"
/*
 * How long to wait for the head unit to show up after we took the bus.  The
 * accessory spec gives the head unit 1 s to be enumerated by the new host and
 * lets it fall back to host after 200 ms of bus inactivity, so a peer that is
 * really there appears almost immediately.  Ten seconds is the point where we
 * conclude the switch was a mistake (a host that cannot become a device, or a
 * stale 0x51) and hand the role back instead of leaving the port dead.
 *
 * 400 rounds x 50 ms = 20 s.  The polling granularity is part of the budget,
 * not an implementation detail: a 500 ms sleep would spend half the head
 * unit's 1 s patience doing nothing.  The 20 s ceiling is for the *give-up*
 * decision only: the A4L's own fall-back took 13 s after our last attempt
 * (measured 2026-10-29 08:16), so a 10 s window was retiring the round while
 * the head unit was still retrying addresses.
 */
#define HOST_ENUM_GIVE_UP_ROUNDS 400
#define HOST_ENUM_POLL_US (50 * 1000)
#define HOST_RECONNECT_ROUNDS 3u
#define HOST_RECONNECT_GRACE_MS 2000u
#define HOST_POST_AUTH_RECONNECT_GRACE_MS 10000u
#define HOST_RECONNECT_SETTLE_US 500000u
/*
 * How long to wait for the head unit's cdc_ncm netdev after claiming iAP2.
 *
 * cdc_ncm registers the netdev *last*, after it has switched the data
 * interface's alternate setting, so the netdev's presence is proof that the
 * alt switch is done.  Starting iAP2 before that puts our first control
 * message inside the car's NCM bring-up, which serialises against iAP2 bulk
 * traffic and stalls it for 0.4-1.4 s (orangecar, docs/protocol.md 4.3).
 * 1.5 s is their measured window.
 */
#define HOST_NETDEV_WAIT_MS 1500u
#define MAX_EPS 12

/*
 * In-process role switch.  The daemon itself unbinds the gadget and flips
 * dwc3 to host, because the supervisor round-trip (exit 76 -> script ->
 * re-exec) spends several hundred milliseconds the head unit does not grant:
 * the accessory spec lets a device-side peer fall back after 200 ms of bus
 * inactivity.  Enumeration itself is kernel-driven once xHCI is up, so the
 * daemon only has to be quick about getting out of the kernel's way.
 *
 * Both nodes are writable from the daemon's context (u:r:su:s0, SELinux
 * permissive on this build), which retires the old "iap2d cannot touch
 * configfs" assumption -- that one dates from an enforcing policy.
 */
#define DWC3_MODE_PATH		"/sys/devices/platform/usb0/dwc3_mode"
#define GADGET_UDC_PATH		"/config/usb_gadget/g1/UDC"
#define UDC_NAME		"fe800000.dwc3"
#define ROLE_STATE_PATH		"/data/carplay/usb-role.state"
#define ROLE_FAILURES_PATH	"/data/carplay/role-switch.failures"
/*
 * Remote policy switch, read on every 0x51.  "switch" arms the host takeover
 * for experiments; anything else (or no file) keeps the default, which is to
 * acknowledge the request and stay a device.  A file beats a rebuild when the
 * only way to find out is another trip to the car.
 */
#define ROLE_POLICY_PATH	"/data/carplay/role-switch.policy"
#define ROLE_SWITCH_MAX_ATTEMPTS 3
#define ROLE_MODE_TRIES		40	/* 40 * 50 ms = 2 s settle budget */
#define ROLE_MODE_POLL_US	(50 * 1000)
#define ROLE_HOST_BUS_TRIES	40	/* xHCI registration gets the same 2 s budget */
#define SELFTEST_TRIGGER_DELAY_S 3

static uint64_t now_ms(void)
{
	struct timespec ts;

	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (uint64_t)ts.tv_sec * 1000u + (uint64_t)(ts.tv_nsec / 1000000);
}

/* Both are best-effort: losing the stamp only costs us the measurement. */
static void role_swap_stamp_write(uint64_t ms)
{
	char buf[32];
	int n;
	int fd;

	n = snprintf(buf, sizeof(buf), "%llu\n", (unsigned long long)ms);
	if (n <= 0)
		return;
	fd = open(ROLE_SWAP_STAMP_PATH, O_WRONLY | O_CREAT | O_TRUNC, 0600);
	if (fd < 0)
		return;
	(void)write(fd, buf, (size_t)n);
	close(fd);
}

static int role_swap_stamp_take(uint64_t *ms)
{
	char buf[32];
	ssize_t n;
	int fd;

	fd = open(ROLE_SWAP_STAMP_PATH, O_RDONLY | O_CLOEXEC);
	if (fd < 0)
		return -1;
	n = read(fd, buf, sizeof(buf) - 1);
	close(fd);
	unlink(ROLE_SWAP_STAMP_PATH);
	if (n <= 0)
		return -1;
	buf[n] = '\0';
	*ms = strtoull(buf, NULL, 10);
	return 0;
}

/* Dump the first bytes of outgoing traffic so the framing can be checked
 * against DiPlay's parser offline.  Bounded per bind to avoid log spam. */
static int tx_log_budget;

static void log_tx(const uint8_t *p, size_t n)
{
	char hex[3 * 64 + 4];
	size_t i;
	int off = 0;

	if (tx_log_budget <= 0)
		return;
	tx_log_budget--;
	for (i = 0; i < n && i < 64; i++)
		off += snprintf(hex + off, sizeof(hex) - (size_t)off, "%02x ",
				p[i]);
	iap2_log("TX RAW %zu bytes: %s", n, hex);
}

/*
 * A write to a FunctionFS IN endpoint blocks until the host has actually read
 * the request.  A host that never drains the iAP2 interface (Windows' Apple
 * driver, for instance) would wedge the entire daemon inside write(), stopping
 * retransmissions and receive processing.  Everything outgoing therefore goes
 * through this queue and a dedicated writer thread.
 */
struct txq {
	pthread_mutex_t mu;
	pthread_cond_t cv;
	uint8_t buf[65536];
	size_t head;
	size_t tail;
	int fd; /* -1 while no endpoint is bound */
	/*
	 * Set instead of fd when we own the bus: the same queue then drains
	 * through USBDEVFS_BULK into the head unit's IN endpoint.  Keeping both
	 * in one queue means the iAP2 writer never has to know which shape it
	 * is running in.
	 */
	struct usbhost *host;
	int framed;
	unsigned trace_left;
	int running;
};

static struct txq g_tx;
static pthread_t g_writer;
static pthread_t g_reader;
static pthread_t g_eth_reader;
static pthread_t g_ep0thread;
static int g_writer_started;
static int g_reader_started;
static int g_eth_reader_started;
static int g_ep0_started;
static volatile sig_atomic_t g_reader_exited;
static volatile sig_atomic_t g_eth_reader_exited;
static volatile sig_atomic_t g_ep0_exited;
static int g_tx_initialized;
static atomic_int g_endpoint_round_stop;
static atomic_uint_fast64_t g_endpoint_enable_generation;
static atomic_uint_fast64_t g_endpoint_reconfiguration_since;
static atomic_int g_ncm_restart_requested;
/*
 * Set once at startup from the gadget layout (see ncm_bridge_kernel_available):
 * when the kernel's ncm.usb0 function is linked into our configuration it owns
 * interfaces 2/3, so our descriptors must stop at interface 1 and ep0 only hands
 * us four endpoint files.
 */
static int g_kernel_ncm;
static struct usbmux g_mux_obj;
static struct usbmux *g_mux = &g_mux_obj;
static volatile sig_atomic_t g_stop;
/* Unlike the device-loop unwind for 0x51, this is never reset. */
static volatile sig_atomic_t g_external_stop;
/* Sticky wakeup for device-side waits; never drained during the handoff.
 * Created before worker threads/signals and kept open until process exit. */
static int g_device_stop_fd = -1;

static void wake_device_loop(void)
{
	uint64_t one = 1;
	int saved_errno = errno;

	if (g_device_stop_fd >= 0)
		(void)write(g_device_stop_fd, &one, sizeof(one));
	errno = saved_errno;
}

static void device_wait_ms(unsigned delay_ms)
{
	uint64_t deadline = now_ms() + delay_ms;

	while (!g_stop) {
		uint64_t now = now_ms();
		unsigned left;

		if (now >= deadline)
			return;
		left = (unsigned)(deadline - now);
		if (g_device_stop_fd >= 0) {
			struct pollfd pfd = { .fd = g_device_stop_fd, .events = POLLIN };
			int rc = poll(&pfd, 1, (int)left);

			if (rc >= 0)
				return;
			if (errno == EINTR)
				continue;
		}
		/* Eventfd creation/poll failure must not restore a 100 ms
		 * shutdown penalty. The normal path sleeps once in poll(). */
		usleep((left < 5u ? left : 5u) * 1000u);
	}
}
static volatile sig_atomic_t g_reload_requested;
/*
 * Set when 0x51 asked for the host role.  Two hand-off shapes share this
 * flag: the in-process switch (preferred -- the daemon closes its own ffs
 * endpoints, unbinds the gadget and flips dwc3 itself) and the supervisor
 * hand-off (fallback -- exit 76 and let the script own the switch).  Which
 * one runs is decided where the main loop unwinds.
 */
static volatile sig_atomic_t g_role_handoff;
/*
 * BOX_ROLE_HOST once a 0x51 (or the self-test trigger) asked for the bus and
 * the policy allows switching.  The device main loop unwinds on g_stop and
 * consults this before tearing anything down for good.
 */
static volatile sig_atomic_t g_switch_pending;
/* Arrival time of the switch trigger, in CLOCK_MONOTONIC ms.  Every later
 * step logs its offset from this, so one look at the log says where the
 * 1 s / 200 ms budget went. */
static uint64_t g_role_t0;
/*
 * Host shape. Reader and writer share g_host for one round; both are joined
 * before it is closed or overwritten by the next enumeration.
 */
static struct usbhost g_host;
static atomic_int g_host_active;
static atomic_int g_host_round_stop;
static atomic_int g_host_failed;
static int g_mux_selected;
static int g_native_prefix_pending;
static char g_status_error[192];

static void txq_init(void)
{
	/* The in-process role handoff reuses this object after joining TX. */
	if (g_tx_initialized) {
		pthread_mutex_destroy(&g_tx.mu);
		pthread_cond_destroy(&g_tx.cv);
	}
	memset(&g_tx, 0, sizeof(g_tx));
	pthread_mutex_init(&g_tx.mu, NULL);
	pthread_cond_init(&g_tx.cv, NULL);
	g_tx.fd = -1;
	g_tx.running = 1;
	g_tx_initialized = 1;
}

static void txq_push(const uint8_t *p, size_t n)
{
	pthread_mutex_lock(&g_tx.mu);
	if (!g_tx.running || (g_tx.fd < 0 && !g_tx.host)) {
		pthread_mutex_unlock(&g_tx.mu);
		return;
	}
	if (n > sizeof(g_tx.buf) - g_tx.tail && g_tx.head) {
		/* The writer owns a private copy of its in-flight prefix. Preserve
		 * that prefix until its completion advances the compacted head. */
		size_t pending = g_tx.tail - g_tx.head;
		memmove(g_tx.buf, g_tx.buf + g_tx.head, pending);
		g_tx.head = 0;
		g_tx.tail = pending;
	}
	if (n > sizeof(g_tx.buf) - g_tx.tail) {
		/* Dropping arbitrary stream bytes cannot be recovered safely by
		 * appending the next frame. End the round without resetting indices
		 * underneath a USB transfer that is still using them. */
		iap2_log("tx queue overflow pending=%zu incoming=%zu; ending round",
			 g_tx.tail - g_tx.head, n);
		g_tx.running = 0;
		if (g_tx.host)
			atomic_store(&g_host_failed, EOVERFLOW);
		else
			atomic_store(&g_endpoint_round_stop, 1);
		pthread_cond_signal(&g_tx.cv);
		pthread_mutex_unlock(&g_tx.mu);
		return;
	}
	memcpy(g_tx.buf + g_tx.tail, p, n);
	g_tx.tail += n;
	pthread_cond_signal(&g_tx.cv);
	pthread_mutex_unlock(&g_tx.mu);
}

static void txq_set_fd(int fd)
{
	pthread_mutex_lock(&g_tx.mu);
	g_tx.fd = fd;
	g_tx.host = NULL;
	g_tx.framed = 0;
	g_tx.head = g_tx.tail = 0;
	pthread_cond_signal(&g_tx.cv);
	pthread_mutex_unlock(&g_tx.mu);
}

/* Host shape: no endpoint fd exists, so the queue drains through usbfs. */
static void txq_set_host(struct usbhost *host)
{
	pthread_mutex_lock(&g_tx.mu);
	g_tx.host = host;
	g_tx.fd = -1;
	g_tx.framed = 1;
	g_tx.trace_left = 96;
	g_tx.head = g_tx.tail = 0;
	pthread_cond_signal(&g_tx.cv);
	pthread_mutex_unlock(&g_tx.mu);
}

static void *writer_main(void *arg)
{
	(void)arg;
	for (;;) {
		uint8_t chunk[65536];
		size_t n;
		ssize_t written;
		struct usbhost *host;
		int fd;
		int write_error;
		int framed, trace;
		uint64_t started_ms;

		pthread_mutex_lock(&g_tx.mu);
		while (g_tx.running && !g_stop &&
		       ((g_tx.fd < 0 && !g_tx.host) ||
			g_tx.head == g_tx.tail))
			pthread_cond_wait(&g_tx.cv, &g_tx.mu);
		if (!g_tx.running || g_stop) {
			pthread_mutex_unlock(&g_tx.mu);
			break;
		}
		fd = g_tx.fd;
		host = g_tx.host;
		if (host && atomic_load(&g_host_failed)) {
			pthread_mutex_unlock(&g_tx.mu);
			break;
		}
		n = g_tx.tail - g_tx.head;
		framed = host && g_tx.framed;
		if (framed) {
			/* The native link emits complete frames, but pump chunks and queue
			 * coalescing lose their boundaries. Reassemble before USB submission:
			 * one detect/ACK/data frame per transfer, as in the reference sender.
			 * In particular, never append CSM behind a short standalone ACK. */
			const uint8_t *p = g_tx.buf + g_tx.head;
			size_t frame = 0;
			int invalid = 0;
			if (n >= 2) {
				if (p[0] != 0xff) invalid = 1;
				else if (p[1] == 0x55) {
					static const uint8_t detect[] = {0xff,0x55,2,0,0xee,0x10};
					frame = sizeof(detect);
					if (n >= frame && memcmp(p, detect, frame)) invalid = 1;
				} else if (p[1] == 0x5a) {
					if (n >= 9) {
						uint8_t sum = 0;
						for (size_t i = 0; i < 9; i++) sum += p[i];
						frame = ((size_t)p[2] << 8) | p[3];
						if (sum || frame < 9) invalid = 1;
					}
				} else invalid = 1;
			}
			if (invalid) {
				iap2_log("host TX invalid queued framing; ending round");
				atomic_store(&g_host_failed, EPROTO);
				pthread_mutex_unlock(&g_tx.mu);
				break;
			}
			if (!frame || n < frame) {
				pthread_cond_wait(&g_tx.cv, &g_tx.mu);
				pthread_mutex_unlock(&g_tx.mu);
				continue;
			}
			n = frame;
		} else if (n > 2048) {
			n = 2048;
		}
		trace = host && g_tx.trace_left > 0;
		if (trace) g_tx.trace_left--;
		memcpy(chunk, g_tx.buf + g_tx.head, n);
		pthread_mutex_unlock(&g_tx.mu);
		started_ms = now_ms();
		if (trace) {
			int link_frame = n >= 9 && chunk[0] == 0xff && chunk[1] == 0x5a;
			iap2_log("host TX submit mono_ms=%llu bytes=%zu ctl=%d seq=%d ack=%d sess=%d csm=0x%04x",
				 (unsigned long long)started_ms, n,
				 link_frame ? chunk[4] : -1, link_frame ? chunk[5] : -1,
				 link_frame ? chunk[6] : -1, link_frame ? chunk[7] : -1,
				 link_frame && n >= 16 && chunk[9] == 0x40 && chunk[10] == 0x40
				 ? ((unsigned)chunk[13] << 8) | chunk[14] : 0);
		}

		/*
		 * In host shape the transfer has its own timeout: a head unit that
		 * stops draining its IN endpoint must not wedge this thread the way
		 * a blocking FunctionFS write would.
		 */
		written = host ? usbhost_write(host, chunk, n, 1000)
			       : write(fd, chunk, n);
		write_error = written < 0 ? (errno ? errno : EIO) : EIO;
		if (host && (trace || written != (ssize_t)n))
			iap2_log("host TX complete mono_ms=%llu elapsed_ms=%llu requested=%zu written=%zd errno=%d (USB completion, not peer ACK)",
				 (unsigned long long)now_ms(),
				 (unsigned long long)(now_ms() - started_ms), n, written,
				 written < 0 ? write_error : 0);
		if (framed && written >= 0 && written != (ssize_t)n) {
			/* A short transfer has already ended this USB packet. Do not
			 * submit its suffix as a fresh packet or replay an accepted prefix. */
			iap2_log("host TX short frame; ending round without suffix/replay");
			written = -1;
			write_error = EIO;
		}
		if (written < 0 && !host && write_error == EINTR)
			continue;
		if (written < 0 && !host &&
		    (write_error == EAGAIN || write_error == EWOULDBLOCK)) {
			usleep(2000);
			continue;
		}
		pthread_mutex_lock(&g_tx.mu);
		if (g_tx.fd == fd && g_tx.host == host) {
			if (written > 0) {
				g_tx.head += (size_t)written;
				if (g_tx.head == g_tx.tail)
					g_tx.head = g_tx.tail = 0;
			}
			if (written < 0 || (host && written == 0)) {
				iap2_log("tx writer: %s write failed: %s",
					 host ? "host bulk" : "endpoint",
					 strerror(write_error));
				if (!host)
					g_tx.fd = -1;
				else {
					iap2_log("host bulk OUT accepted length unknown; ending round without replay");
					atomic_store(&g_host_failed, write_error);
				}
				g_tx.head = g_tx.tail = 0;
			}
		}
		pthread_mutex_unlock(&g_tx.mu);
		if (host && written <= 0)
			break;
	}
	return NULL;
}

/* Endpoint index in descriptor order.  Every working sample -- iPhone 6,
 * iPhone 8 (iOS 12 and 16) and DDBox010 -- leads a CarPlay configuration with
 * PTP at interface 0, puts iAP2 at interface 1 and Apple ethernet at 2.  The
 * order below follows that, so a head unit that indexes interfaces instead of
 * matching class codes still finds what it expects.
 *
 *   intf 0  PTP   06/01/01   2 ep   (bulk IN 0x81 / bulk OUT 0x04)
 *   intf 1  iAP2  ff/fe/02   2 ep   (0x83 / 0x03) -- iAP2 + USBMUX
 *   intf 2  eth   ff/fd/01   alt1: 2 ep (0x86 / 0x07)
 *   intf 3/4 NCM  (kernel function, or described here in the TUN layout)
 *
 * PTP carries two endpoints rather than Apple's three: the interrupt endpoint
 * only delivers MTP events and nothing in a CarPlay session reads it, while
 * the dwc3 core has 8 IN / 8 OUT and the standby configuration's kernel ptp
 * already holds two INs for the whole gadget lifetime.  Dropping it keeps the
 * whole gadget at 7 IN / 5 OUT instead of pinning both directions at the cap.
 */
#define EP_PTP_IN     0 /* 06/01/01 IN  -- Apple device identity */
#define EP_PTP_OUT    1 /* 06/01/01 OUT */
#define EP_MUX_IN     2 /* ff/fe/02 IN  -- bare iAP2 + USBMUX */
#define EP_MUX_OUT    3 /* ff/fe/02 OUT -- bare iAP2 + USBMUX */
#define EP_ETH_IN     4 /* ff/fd/01 alt1 IN  -- ipheth data */
#define EP_ETH_OUT    5 /* ff/fd/01 alt1 OUT -- ipheth data */
#define EP_NCM_IRQ    6
#define EP_NCM_IN     7
#define EP_NCM_OUT    8
#define EP_COUNT      9

static int g_stderr_enabled = -1;

static void log_to_stderr(const char *fmt, va_list ap)
{
	va_list copy;
	struct timespec ts;

	if (g_stderr_enabled < 0)
		g_stderr_enabled = isatty(STDERR_FILENO);
	if (!g_stderr_enabled)
		return;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	fprintf(stderr, "[%ld.%03ld] ", (long)ts.tv_sec, ts.tv_nsec / 1000000);
	va_copy(copy, ap);
	vfprintf(stderr, fmt, copy);
	va_end(copy);
	fputc('\n', stderr);
	fflush(stderr);
}

static void emit_log(const char *fmt, va_list ap)
{
	va_list file_ap;

	log_to_stderr(fmt, ap);
	va_copy(file_ap, ap);
	carplay_log_vwrite(CARPLAY_LOG_INFO, fmt, file_ap);
	va_end(file_ap);
}

static void logmsg(const char *fmt, ...)
{
	va_list ap;

	va_start(ap, fmt);
	emit_log(fmt, ap);
	va_end(ap);
}

/* Shared with iap2link.c so both modules log through one channel. */
void iap2_log(const char *fmt, ...)
{
	va_list ap;

	va_start(ap, fmt);
	emit_log(fmt, ap);
	va_end(ap);
}

/*
 * init owns the USB gadget and SIGKILLs adbd's service cgroup whenever the
 * gadget is reconfigured; everything started from `adb shell` lives there.
 * Escape the process group and the per-service memory cgroup (only the memory
 * controller is per-service on this board) so ep0 stays open across rebinds.
 */
static void escape_adbd_cgroup(void)
{
	int fd;
	char pid[32];
	int len;

	if (setsid() < 0)
		logmsg("setsid: %s (continuing)", strerror(errno));
	else
		logmsg("setsid ok, pgid=%d", (int)getpgrp());

	len = snprintf(pid, sizeof(pid), "%d", (int)getpid());
	fd = open("/dev/memcg/tasks", O_WRONLY | O_CLOEXEC);
	if (fd < 0) {
		logmsg("open /dev/memcg/tasks: %s (continuing)", strerror(errno));
		return;
	}
	if (write(fd, pid, (size_t)len) != len)
		logmsg("memcg move failed: %s", strerror(errno));
	else
		logmsg("moved to root memory cgroup");
	close(fd);
}

static uint8_t *put_intf(uint8_t *p, uint8_t num, uint8_t alt, uint8_t neps,
			 uint8_t cls, uint8_t sub, uint8_t proto,
			 uint8_t name_index)
{
	*p++ = 9;
	*p++ = USB_DT_INTERFACE;
	*p++ = num;
	*p++ = alt;
	*p++ = neps;
	*p++ = cls;
	*p++ = sub;
	*p++ = proto;
	/* 1-based position in g_intf_strings; f_fs rewrites it to the real
	 * string id at bind time.  Zero means "no name", which is what the
	 * kernel NCM function reports for interfaces 2/3. */
	*p++ = name_index;
	return p;
}

static uint8_t *put_ep(uint8_t *p, uint8_t addr, uint8_t attr, uint16_t mps,
		       uint8_t interval)
{
	*p++ = 7;
	*p++ = USB_DT_ENDPOINT;
	*p++ = addr;
	*p++ = attr;
	*p++ = (uint8_t)(mps & 0xFF);
	*p++ = (uint8_t)((mps >> 8) & 0xFF);
	*p++ = interval;
	return p;
}

/* Interface names, position-indexed from 1.  A descriptor's iInterface is the
 * POSITION in this table, not a global string id: f_fs rewrites it at bind
 * time (__ffs_func_bind_do_nums -> FFS_STRING -> stringtabs[0]->strings[i-1].id),
 * which is why the numbers in build_speed_block and the order here must stay
 * in step.  A real iPhone names every vendor interface ("Apple USB
 * Multiplexor" on ff/fe/02, "Apple USB Ethernet" on ff/fd/01, "NCM
 * Control"/"NCM Data" on the CDC pair); ours used to be anonymous, which a
 * strict host can see in the descriptor alone. */
static const char *const g_intf_strings[] = {
	"Apple USB Multiplexor",	/* intf 0: ff/fe/02, iAP2 + USBMUX */
	"Apple USB Ethernet",		/* intf 1: ff/fd/01, CarPlay marker */
	"NCM Control",			/* intf 2: CDC-NCM control (TUN only) */
	"NCM Data",			/* intf 3: CDC-NCM data (TUN only) */
};
#define INTF_STRING_COUNT \
	((unsigned)(sizeof(g_intf_strings) / sizeof(g_intf_strings[0])))

/* One speed's worth of descriptors.  FS and HS must list the same endpoints in
 * the same order with the same addresses; only wMaxPacketSize differs. */
static int g_eth_alt = 1;

static int build_speed_block(uint8_t *p, int hs)
{
	uint16_t bulk = hs ? 512 : 64;
	uint16_t intr = 16;
	uint8_t *s = p;

	/* intf 0: PTP 06/01/01.
	 *
	 * Every sample that works -- iPhone 6, iPhone 8 on iOS 12 and 16, and
	 * DDBox010 -- leads its CarPlay configuration with this interface, and a
	 * head unit that finds no PTP at interface 0 treats the box as a USB
	 * stick with a battery: it enumerates, accepts a configuration and then
	 * never opens iAP2.  The standby configuration (b.1) offers the kernel's
	 * three-endpoint ptp; this one is ours and carries the two bulk endpoints
	 * only, because the interrupt endpoint exists for MTP events that no
	 * CarPlay session reads and the dwc3 core has no IN to spare.
	 *
	 * Leave iInterface at 0: Apple's PTP interface is anonymous on every
	 * device we have captured. */
	s = put_intf(s, 0, 0, 2, 0x06, 0x01, 0x01, 0);
	s = put_ep(s, 0x81, USB_ENDPOINT_XFER_BULK, bulk, 0);
	s = put_ep(s, 0x04, USB_ENDPOINT_XFER_BULK, bulk, 0);

	/* intf 1: Apple's Mobile Device / USBMUX function, 0xFF/0xFE/0x02 with
	 * one bulk pair.  This is the interface an OEM head unit opens -- it
	 * sits right behind PTP, exactly where a real iPhone puts it.  Bare
	 * iAP2 link frames and USBMUX frames both arrive here; dispatch_out_frame
	 * splits them by frame header.  Endpoint addresses follow the iPhone's
	 * 0x8x/0x0x bulk convention; f_fs may renumber them, which is harmless
	 * because the host reads whatever the descriptors say. */
	s = put_intf(s, 1, 0, 2, 0xFF, 0xFE, 0x02, 1);
	s = put_ep(s, 0x83, USB_ENDPOINT_XFER_BULK, bulk, 0);
	s = put_ep(s, 0x03, USB_ENDPOINT_XFER_BULK, bulk, 0);

	/* intf 2: Apple USB Ethernet, 0xFF/0xFD/0x01.
	 *
	 * DDBox010 -- the one Android-based box known to work on this head unit
	 * -- exposes it as alt 0 (no endpoints) plus alt 1 and alt 2, each
	 * carrying a bulk pair; ipheth selects alt 1 and moves ethernet frames
	 * over it.  Declaring only the alt 0 marker (what this file did until
	 * now) makes the configuration look like CarPlay without ever giving
	 * the host a data path, which is the difference between "recognised"
	 * and "usable".
	 *
	 * Alt settings are the one descriptor shape older FunctionFS builds are
	 * known to refuse, so this is a runtime choice rather than a constant:
	 * write_descriptors() clears g_eth_alt and rewrites the block in the
	 * marker-only form if ep0 rejects it.  The endpoint pair is 0x86/0x07
	 * rather than DDBox's 0x86/0x05 because 0x05 is the TUN layout's NCM
	 * data OUT and f_fs matches endpoints by address.
	 *
	 * Descriptors alone are not enough: for years 4.4's f_fs exported no
	 * ->get_alt, and composite_setup() reads that as "altsetting zero only",
	 * STALLing every non-zero SET_INTERFACE.  The host then reported
	 * `setInterface iface=2/1 reported failure` and never opened the pair.
	 * The kernel patch (f_fs: ffs_func_get_alt) fixes that side; g_eth_alt
	 * stays as the fallback for a kernel without it. */
	s = put_intf(s, 2, 0, 0, 0xFF, 0xFD, 0x01, 2);
	if (g_eth_alt) {
		s = put_intf(s, 2, 1, 2, 0xFF, 0xFD, 0x01, 2);
		s = put_ep(s, 0x86, USB_ENDPOINT_XFER_BULK, bulk, 0);
		s = put_ep(s, 0x07, USB_ENDPOINT_XFER_BULK, bulk, 0);
	}

	/* intf 3/4: CDC-NCM control, notification interrupt IN.
	 *
	 * A real iPhone also carries four CS_INTERFACE functional descriptors
	 * (Header/Union/Ethernet/NCM) here.  FunctionFS on this 4.4 kernel
	 * refuses any descriptor type other than INTERFACE/ENDPOINT: adding
	 * them makes the ep0 write fail with EINVAL and no gadget comes up.
	 *
	 * So there are two layouts, chosen at startup:
	 *   - kernel NCM (configfs ncm.usb0 linked into b.2): interfaces 3/4
	 *     belong to the kernel function, which emits the class descriptors.
	 *     We must NOT describe them again -- doing so fails the bind.
	 *   - otherwise: describe them here, without the functional descriptors,
	 *     which is enough for an Android host (DiPlay) but not for a Linux
	 *     host running cdc_ncm.
	 */
	if (!g_kernel_ncm) {
		s = put_intf(s, 3, 0, 1, 0x02, 0x0D, 0x00, 3);
		s = put_ep(s, 0x84, USB_ENDPOINT_XFER_INT, intr, 8);

		/* intf 4: CDC-NCM data carrying the bulk pair.
		 *
		 * bInterfaceProtocol is 0x01, not the 0x00 that Linux's f_ncm.c
		 * writes: a real iPhone advertises 0x0A/0x00/0x01 on this
		 * interface and strict hosts compare the full triple.  This only
		 * takes effect in the TUN layout -- when ncm.usb0 owns the
		 * interface the value comes from f_ncm.c and cannot be changed
		 * from userspace. */
		s = put_intf(s, 4, 0, 2, 0x0A, 0x00, 0x01, 4);
		s = put_ep(s, 0x85, USB_ENDPOINT_XFER_BULK, bulk, 0);
		s = put_ep(s, 0x05, USB_ENDPOINT_XFER_BULK, bulk, 0);
	}

	return (int)(s - p);
}

/* Worst case: 6 interfaces (PTP, ff/fe/02, ff/fd/01 alt0, ff/fd/01 alt1, NCM
 * control, NCM data) and 9 endpoints, identical for FS and HS. */
#define DESC_PER_SPEED (6 * 9 + 9 * 7)
/* Interface/endpoint counts per layout, without the optional ff/fd/01 alt1
 * block; DESC_IFACES_ETH_ALT / DESC_EPS_ETH_ALT are added when g_eth_alt. */
#define DESC_IFACES_KERNEL_NCM 3
#define DESC_EPS_KERNEL_NCM    4
#define DESC_IFACES_TUN        5
#define DESC_EPS_TUN           7
#define DESC_IFACES_ETH_ALT    1
#define DESC_EPS_ETH_ALT       2

/* One descriptor write attempt, with the counts that match the current
 * g_eth_alt setting.  Split out so the caller can retry in marker-only form
 * after ep0 refuses alt settings. */
static ssize_t write_descriptors_once(int ep0)
{
	uint8_t buf[sizeof(struct usb_functionfs_descs_head) + 2 * DESC_PER_SPEED];
	struct usb_functionfs_descs_head *h =
		(struct usb_functionfs_descs_head *)buf;
	uint8_t *p = buf + sizeof(*h);
	int ifaces = g_kernel_ncm ? DESC_IFACES_KERNEL_NCM : DESC_IFACES_TUN;
	int eps = g_kernel_ncm ? DESC_EPS_KERNEL_NCM : DESC_EPS_TUN;

	if (g_eth_alt) {
		ifaces += DESC_IFACES_ETH_ALT;
		eps += DESC_EPS_ETH_ALT;
	}

	p += build_speed_block(p, 0);
	p += build_speed_block(p, 1);

	h->magic = FUNCTIONFS_DESCRIPTORS_MAGIC;
	h->length = (uint32_t)(p - buf);
	h->fs_count = (uint16_t)(ifaces + eps);
	h->hs_count = h->fs_count;

	logmsg("writing %u bytes of descriptors (fs_count=%u hs_count=%u ncm=%s eth=%s)",
	       h->length, h->fs_count, h->hs_count,
	       g_kernel_ncm ? "kernel" : "tun",
	       g_eth_alt ? "alt1 pair" : "marker only");
	return write(ep0, buf, h->length);
}

static int write_descriptors(int ep0)
{
	ssize_t n = write_descriptors_once(ep0);

	if (n < 0 && g_eth_alt) {
		logmsg("descriptor write failed: %s -- retrying without the ff/fd/01 bulk pair",
		       strerror(errno));
		g_eth_alt = 0;
		n = write_descriptors_once(ep0);
	}
	if (n < 0) {
		logmsg("descriptor write failed: %s", strerror(errno));
		return -1;
	}
	logmsg("descriptors accepted (%zd bytes)", n);
	return 0;
}

/* Mandatory second block: ffs_ready() (which sets desc_ready) runs only after
 * this write.  Indices here are positional: entry i answers iInterface == i+1.
 * A rejected block falls back to the empty one (str_count == lang_count == 0,
 * which is legal) -- a gadget with anonymous interfaces still enumerates, a
 * gadget whose string table was refused does not. */
static int write_strings(int ep0)
{
	uint8_t buf[sizeof(struct usb_functionfs_strings_head) + 128];
	struct usb_functionfs_strings_head *sh =
		(struct usb_functionfs_strings_head *)buf;
	uint8_t *p = buf + sizeof(*sh);
	unsigned i;
	ssize_t n;

	/* One language block: 0x0409 (en-US) then each string with its own NUL
	 * terminator (__ffs_data_got_strings walks them with strnlen). */
	*p++ = 0x09;
	*p++ = 0x04;
	for (i = 0; i < INTF_STRING_COUNT; i++) {
		size_t len = strlen(g_intf_strings[i]) + 1;

		memcpy(p, g_intf_strings[i], len);
		p += len;
	}
	sh->magic = FUNCTIONFS_STRINGS_MAGIC;
	sh->length = (uint32_t)(p - buf);
	sh->str_count = INTF_STRING_COUNT;
	sh->lang_count = 1;

	n = write(ep0, buf, sh->length);
	if (n >= 0) {
		logmsg("strings accepted (%zd bytes, %u interface names) -- ffs is now ACTIVE",
		       n, INTF_STRING_COUNT);
		return 0;
	}
	logmsg("strings write failed: %s -- retrying without interface names",
	       strerror(errno));

	memset(buf, 0, sizeof(*sh));
	sh->magic = FUNCTIONFS_STRINGS_MAGIC;
	sh->length = sizeof(*sh);
	sh->str_count = 0;
	sh->lang_count = 0;
	n = write(ep0, sh, sizeof(*sh));
	if (n < 0) {
		logmsg("strings write failed: %s", strerror(errno));
		return -1;
	}
	logmsg("strings accepted (%zd bytes) -- ffs is now ACTIVE", n);
	return 0;
}

/*
 * Vendor control requests.  A real head unit probes 0xC0/0x53 first and only
 * keeps going when four bytes of 0x00000001 come back; it then switches the
 * iPhone to host mode with 0x40/0x51, asks for the CarPlay configuration with
 * 0xC0/0x52 (one byte back), and tops up the charging current with 0x40/0x40.
 * 0x45 is usbmuxd's GET_USB_MODE, read as 4 bytes where 3:3:3:0 is the initial
 * (unswitched) mode.  The whole table lives in usb_vendor.c so it can be
 * tested without dragging in the daemon's main().
 */
/*
 * Consume the data stage of a control-OUT request.
 *
 * FunctionFS leaves a wLength != 0 OUT request pending until user space reads
 * it, and that read is what completes the status stage.  Skipping it -- or
 * stalling the request -- is what makes the host time out after five seconds,
 * which is exactly how DDBox behaves when probed.
 *
 * wLength == 0 has no data stage; the status stage is completed by queueing a
 * zero-length ep0 request, and one read() on ep0 while the setup is pending
 * does exactly that.  A zero-length write() would be exactly wrong: while an
 * OUT setup is pending, ffs_ep0_write() stalls every request whose
 * bRequestType lacks USB_DIR_IN.  The A4L head unit sends 0x51 with
 * wLength = 0, so this is the path that runs in the car.
 */
static int consume_control_out(int ep0, uint16_t w_length)
{
	uint8_t buf[64];
	uint16_t left = w_length;

	if (left == 0)
		return read(ep0, buf, sizeof(buf)) < 0 ? -1 : 0;

	while (left > 0) {
		size_t want = left < sizeof(buf) ? (size_t)left : sizeof(buf);
		struct pollfd pfd = { .fd = ep0, .events = POLLIN };
		ssize_t n;

		if (poll(&pfd, 1, 200) <= 0)
			return -1;
		n = read(ep0, buf, want);
		if (n < 0) {
			if (errno == EINTR || errno == EAGAIN)
				continue;
			return -1;
		}
		if (n == 0)
			break;
		left = (uint16_t)(left - (uint16_t)n);
	}
	return 0;
}

/* Logs `what` with its offset from the switch trigger.  The whole point of
 * the timestamp chain is that one trip to a real head unit answers "where
 * did the 1 s go" without any further instrumentation. */
static void role_ts(const char *what)
{
	iap2_log("role-switch t=+%llu ms: %s",
		 (unsigned long long)(now_ms() - g_role_t0), what);
}

/* One line, no trailing newline needed by either node: sysfs and configfs
 * both accept the value with or without one. */
static int sysfs_write_line(const char *path, const char *value)
{
	int fd = open(path, O_WRONLY);
	size_t len = strlen(value);
	ssize_t n;

	if (fd < 0)
		return -1;
	n = write(fd, value, len);
	close(fd);
	return n == (ssize_t)len ? 0 : -1;
}

static int sysfs_read_trimmed(const char *path, char *buf, size_t len)
{
	int fd = open(path, O_RDONLY);
	ssize_t n;
	size_t end;

	if (fd < 0)
		return -1;
	n = read(fd, buf, len - 1);
	close(fd);
	if (n < 0)
		return -1;
	buf[n] = '\0';
	end = strlen(buf);
	while (end > 0 && (buf[end - 1] == '\n' || buf[end - 1] == '\r' ||
			   buf[end - 1] == ' ' || buf[end - 1] == '\t'))
		buf[--end] = '\0';
	return 0;
}

static int gadget_is_attached(void)
{
	char udc[64];

	if (sysfs_read_trimmed(GADGET_UDC_PATH, udc, sizeof(udc)) < 0)
		return 0;
	return udc[0] != '\0';
}

/*
 * "addressed" vs "configured" is the one fact that separates "the head unit
 * is still deciding" from "the head unit selected a configuration and we are
 * supposed to be talking iAP2 now".  The kernel tracks it per UDC, so read it
 * instead of guessing from our own endpoint state.
 */
static const char *udc_state(void)
{
	static char state[32];

	if (sysfs_read_trimmed("/sys/class/udc/" UDC_NAME "/state", state,
			       sizeof(state)) < 0)
		return "unknown";
	return state[0] ? state : "unknown";
}

/*
 * The supervisor counts failed host rounds in this file and stops honouring
 * the switch after three -- the guard against a head unit that keeps sending
 * 0x51 while the port cannot complete a host round.  The in-process path
 * must honour the same budget, or it would bypass the guard entirely.
 * A garbled file counts as zero, same as the supervisor's rule.
 */
static int role_switch_failures(void)
{
	char buf[16];
	const char *p;
	int n = 0;

	if (sysfs_read_trimmed(ROLE_FAILURES_PATH, buf, sizeof(buf)) < 0)
		return 0;
	if (buf[0] == '\0')
		return 0;
	for (p = buf; *p >= '0' && *p <= '9'; p++)
		n = n * 10 + (*p - '0');
	if (*p != '\0')
		return 0;
	return n;
}

/* Same one-line format the supervisor persists with set_role_state(). */
static void role_state_write(const char *role)
{
	int fd = open(ROLE_STATE_PATH, O_WRONLY | O_CREAT | O_TRUNC, 0644);

	if (fd < 0)
		return;
	(void)write(fd, role, strlen(role));
	(void)write(fd, "\n", 1);
	close(fd);
}

static int wait_dwc3_mode(const char *want)
{
	int i;
	char mode[32];

	for (i = 0; i < ROLE_MODE_TRIES; i++) {
		if (sysfs_read_trimmed(DWC3_MODE_PATH, mode, sizeof(mode)) == 0 &&
		    strcmp(mode, want) == 0)
			return 0;
		usleep(ROLE_MODE_POLL_US);
	}
	return -1;
}

static int wait_role_host_bus(void)
{
	int i;

	for (i = 0; i < ROLE_HOST_BUS_TRIES; i++) {
		int buses = usbhost_role_bus_count();

		if (buses > 0)
			return buses;
		usleep(ROLE_MODE_POLL_US);
	}
	return -1;
}

/*
 * The half of the switch that has a deadline.  Order matters: the gadget
 * must be unbound before dwc3 can leave peripheral mode, and closing our ffs
 * endpoints first is what lets the function release the UDC cleanly.
 *
 * Returns 0 when the kernel reports host mode, -1 on the first step that
 * refuses (caller falls back to the supervisor hand-off, exit 76).
 */
static int role_switch_to_host(void)
{
	char mode[32];

	role_ts("stopping runtime; closing ffs endpoints");
	if (gadget_is_attached()) {
		if (sysfs_write_line(GADGET_UDC_PATH, "") < 0) {
			role_ts("cannot unbind the UDC");
			return -1;
		}
		/* The unbind is synchronous from configfs' side; give the
		 * gadgetfs layer a moment to report the empty UDC. */
		{
			int i;

			for (i = 0; i < ROLE_MODE_TRIES && gadget_is_attached();
			     i++)
				usleep(ROLE_MODE_POLL_US);
			if (gadget_is_attached()) {
				role_ts("UDC stayed bound");
				return -1;
			}
		}
		role_ts("gadget unbound");
	} else {
		role_ts("gadget already unbound");
	}

	role_ts("writing dwc3 host mode");
	if (sysfs_write_line(DWC3_MODE_PATH, "host") < 0) {
		role_ts("dwc3 refused the mode write (dr_mode is not otg?)");
		return -1;
	}
	role_ts("dwc3 host mode write returned");
	if (wait_dwc3_mode("host") < 0) {
		(void)sysfs_read_trimmed(DWC3_MODE_PATH, mode, sizeof(mode));
		role_ts("mode did not become host");
		return -1;
	}
	if (wait_role_host_bus() < 0) {
		role_ts("host mode reported, but xhci root bus did not register");
		return -1;
	}
	role_ts("host mode active; the kernel enumerates the bus from here");
	/* The supervisor's failure budget keys off this file: without it the
	 * give-up below would never be counted and a peer that keeps sending
	 * 0x51 would flip the port forever. */
	role_state_write("host");
	return 0;
}

/*
 * Is a host takeover actually being asked for?
 *
 * The answer is in wValue, and only wValue.  Four independent sources agree
 * (2026-10-29):
 *
 *   MediaTek's out-of-tree carplay.c, written for exactly this hand-off,
 *     issues  usb_control_msg(..., 0x51, USB_TYPE_VENDOR, 1, 0, NULL, 0)
 *     and calls it "Apple Device to Host Mode Switch": wValue 1, wLength 0.
 *   wiomoc/iap2 documents the same request: "Value: 1 for CarPlay mode, 0 for
 *     standard mode.  Index 0.  Length 0."
 *   The wired-CarPlay walk-throughs (table 3.5 in the leaked flow doc, and
 *     the reverse-engineering write-up) show the identical 8-byte sequence
 *     40 51 01 00 00 00 00 00 and label it "iPhone USB role switch", wValue
 *     0x01 meaning "switch to host".
 *   orangecar, an accessory that impersonates an iPhone against real cars,
 *     reads wValue=0 as "the user chose the car's own phone-app mode instead
 *     of CarPlay" and wValue=1 as the CarPlay hand-off.
 *
 * So wLength == 0 is the *normal* shape; it has never meant "no payload, so
 * nothing is being handed over".  The 2026-10-29 note that claimed otherwise
 * was inferred backwards from a failed run and is refuted here.
 *
 * What actually went wrong on 08:16 is a separate question: the switch was
 * taken, the head unit did come back as a device, and enumeration on our bus
 * never completed (SET_ADDRESS retries in dmesg, "no head unit" in the log).
 * That is a host-side enumeration problem, not a reason to ignore 0x51.
 *
 * Keeping the escape hatch: CARPLAY_ROLE_SWITCH=off (or role-switch.policy
 * reading "stay") pins us to the device role, because a car attempt costs a
 * head-unit power cycle when it fails.
 */
static int role_switch_armed(uint16_t w_value, uint16_t w_length)
{
	const char *policy = getenv("CARPLAY_ROLE_SWITCH");
	char buf[24];

	if (policy && (strcmp(policy, "force") == 0 || strcmp(policy, "on") == 0 ||
		       strcmp(policy, "1") == 0))
		return 1;
	if (policy && (strcmp(policy, "off") == 0 || strcmp(policy, "0") == 0))
		return 0;			/* the DiPlay regression bench */
	if (sysfs_read_trimmed(ROLE_POLICY_PATH, buf, sizeof(buf)) == 0) {
		if (strcmp(buf, "switch") == 0)
			return 1;
		if (strcmp(buf, "stay") == 0)
			return 0;
	}
	(void)w_length;			/* 0 in both shapes; says nothing */
	return w_value == 1;
}

/*
 * 0x40/0x51 -- "switch the accessory to host", or "back to standard mode".
 *
 * wValue decides which:
 *   1  CarPlay: we take the bus, the head unit becomes a device and is
 *      enumerated by us.  This is what every reference implementation does
 *      and what the A4L asked for at 08:16:48.
 *   0  The car's own phone-app mode: stay a device.
 *
 * Answer it properly first: an accessory that stalls here is the reason the
 * A4L used to retry forever and never configure the box.
 */
static void handle_role_swap(int ep0, const struct usb_ctrlrequest *setup)
{
	int role = box_role_from_request(setup->bRequestType, setup->bRequest,
					 setup->wValue);
	int failed;
	uint64_t ack_ms;

	/*
	 * The ACK matters more than any switch: a head unit that gets a stall
	 * here gives up on the whole CarPlay handshake.
	 */
	failed = consume_control_out(ep0, setup->wLength);
	ack_ms = now_ms();
	iap2_log("ep0: 0x51 -> %s; wValue=%u wLength=%u data stage %s (udc=%s)",
		 box_role_name(role), setup->wValue, setup->wLength,
		 failed ? "NOT consumed" : "acked", udc_state());
	/* Half the value of this line is that it survives the next few
	 * seconds: 0x51 is exactly where this box used to lose power. */
	carplay_log_sync();

	if (failed) {
		iap2_log("ep0: role switch cancelled because control acknowledgement failed");
		return;
	}
	if (role != BOX_ROLE_HOST)
		return;
	if (!role_switch_armed(setup->wValue, setup->wLength)) {
		iap2_log("ep0: 0x51 takeover suppressed by policy (wValue=%u); "
			 "staying a device", setup->wValue);
		return;
	}
	if (role_switch_failures() >= ROLE_SWITCH_MAX_ATTEMPTS) {
		iap2_log("ep0: host switch suppressed: %d failures since boot "
			 "(budget %d); staying a device",
			 role_switch_failures(), ROLE_SWITCH_MAX_ATTEMPTS);
		return;
	}

	/* A fast head unit can send 0x51 during the supervisor's post-bind
	 * check. Publish intent before closing ep0/unbinding, so an expected
	 * detach cannot be mistaken for attach failure and killed with SIGTERM.
	 * The host loop replaces this request with "device" when it gives up. */
	if (box_role_request_write(ROLE_REQUEST_PATH, BOX_ROLE_HOST) < 0) {
		iap2_log("ep0: cannot publish host intent; staying a device");
		return;
	}
	/* Include log sync and intent publication in ACK-to-Host timing. */
	g_role_t0 = ack_ms;
	role_swap_stamp_write(g_role_t0);
	role_ts("0x51 acked; unwinding the device loop for the in-process switch");
	/*
	 * The ep0 thread cannot do the switch itself -- it would have to join
	 * the very thread it is running on.  Instead it stops the world and
	 * lets the main thread (which owns the runtime) do it.
	 */
	g_switch_pending = BOX_ROLE_HOST;
	g_stop = 1;
	wake_device_loop();
}

static void handle_setup(int ep0, const struct usb_ctrlrequest *setup)
{
	struct usb_vendor_request request;
	uint8_t response[4];
	uint16_t wLength = setup->wLength;
	int len;
	ssize_t n;

	iap2_log("ep0 SETUP bmRequestType=0x%02x bRequest=0x%02x (%s) "
		 "wValue=0x%04x wIndex=0x%04x wLength=%u",
		 setup->bRequestType, setup->bRequest,
		 usb_vendor_request_name(setup->bRequest),
		 setup->wValue, setup->wIndex, wLength);

	/*
	 * Role swap is the one vendor request we must answer ourselves rather
	 * than from the static table: it carries a data stage, and the answer is
	 * a policy decision (switch now, or stay a device and let the head unit
	 * fall back on its own).
	 */
	if (box_role_from_request(setup->bRequestType, setup->bRequest,
				  setup->wValue) >= 0) {
		handle_role_swap(ep0, setup);
		return;
	}

	request.bmRequestType = setup->bRequestType;
	request.bRequest = setup->bRequest;
	request.wValue = setup->wValue;
	request.wIndex = setup->wIndex;
	request.wLength = setup->wLength;

	len = usb_vendor_response(&request, response, sizeof(response));
	if (len < 0) {
		/* Not ours. FunctionFS still owns the status stage, so just
		 * say so and leave the endpoint alone. */
		iap2_log("ep0: unhandled vendor request 0x%02x", setup->bRequest);
		return;
	}
	if (len == 0) {
		/*
		 * Vendor-OUT with nothing to say back (0x40 charge-current
		 * top-up).  ffs hands the setup over in delayed-status, so
		 * the transfer only completes once user space queues an ep0
		 * request: consume whatever data stage was announced --
		 * read() for wLength > 0, zero-length queue for wLength
		 * == 0.  Returning here without it is what hangs the
		 * control pipe for five seconds.
		 *
		 * IN requests are excluded: for one of those the status stage
		 * belongs to the IN transfer, and reading ep0 would steal the
		 * next event instead of answering it.
		 */
		if (setup->bRequestType & USB_DIR_IN) {
			iap2_log("ep0: 0x%02x has no reply of its own; leaving "
				 "the IN status stage to ffs",
				 setup->bRequest);
			return;
		}
		if (consume_control_out(ep0, wLength) < 0)
			iap2_log("ep0: 0x%02x data stage consume failed",
				 setup->bRequest);
		else
			iap2_log("ep0: 0x%02x acknowledged (no data stage)",
				 setup->bRequest);
		return;
	}

	if (len > wLength)
		len = wLength;
	n = write(ep0, response, (size_t)len);
	if (n < 0)
		iap2_log("ep0: response write failed: %s", strerror(errno));
	else
		iap2_log("ep0: %zd bytes answered", n);
}

static void *ep0_main(void *arg)
{
	int ep0 = (int)(intptr_t)arg;
	struct usb_functionfs_event ev;

	for (;;) {
		ssize_t n = read(ep0, &ev, sizeof(ev));

		if (n < 1) {
			if (g_stop)
				break;
			if (errno == EAGAIN || errno == EWOULDBLOCK) {
				usleep(20000);
				continue;
			}
			if (errno == EINTR)
				continue;
			iap2_log("ep0: event read stopped: %s", strerror(errno));
			break;
		}

		switch (ev.type) {
		case FUNCTIONFS_ENABLE:
			atomic_fetch_add(&g_endpoint_enable_generation, 1);
			iap2_log("ep0: FUNCTIONFS_ENABLE generation=%llu",
				 (unsigned long long)atomic_load(&g_endpoint_enable_generation));
			if (atomic_exchange(
				    &g_endpoint_reconfiguration_since, 0)) {
				/* The host commonly selects the CarPlay configuration
				 * again while opening its USBMUX pipe. FunctionFS closes
				 * the old endpoint requests and re-enables the same
				 * endpoint files. Reset protocol state without tearing
				 * down the round or losing requests from the new host
				 * session. */
				iap2_log("ep0: endpoint re-enable recovered; "
					 "preserving usbmux and iAP2 session state");
				usbmux_endpoint_reconfigured(g_mux);
				atomic_store(&g_ncm_restart_requested, 1);
			}
			break;
		case FUNCTIONFS_DISABLE:
			iap2_log("ep0: FUNCTIONFS_DISABLE; ending current endpoint round");
			atomic_store(&g_endpoint_round_stop, 1);
			break;
		case FUNCTIONFS_BIND:
			iap2_log("ep0: FUNCTIONFS_BIND (new USB session)");
			reset_session();
			break;
		case FUNCTIONFS_UNBIND:
			iap2_log("ep0: FUNCTIONFS_UNBIND");
			break;
		case FUNCTIONFS_SETUP:
			handle_setup(ep0, &ev.u.setup);
			break;
		case FUNCTIONFS_SUSPEND:
			iap2_log("ep0: FUNCTIONFS_SUSPEND");
			break;
		case FUNCTIONFS_RESUME:
			iap2_log("ep0: FUNCTIONFS_RESUME");
			break;
		default:
			iap2_log("ep0: event type %u", ev.type);
			break;
		}
	}
	g_ep0_exited = 1;
	return NULL;
}

static int ep_index(const char *name)
{
	const char *d = name + 2;

	if (*d == '\0')
		return -1;
	return (int)strtol(d, NULL, 16);
}

static int collect_epfiles(char names[][16], int max)
{
	DIR *d;
	struct dirent *de;
	int n = 0;

	d = opendir(FFS_ROOT);
	if (!d)
		return -1;

	while ((de = readdir(d)) != NULL) {
		if (strncmp(de->d_name, "ep", 2) != 0)
			continue;
		if (strcmp(de->d_name, "ep0") == 0)
			continue;
		if (n < max) {
			strncpy(names[n], de->d_name, 15);
			names[n][15] = '\0';
			n++;
		}
	}
	closedir(d);
	return n;
}

static void sort_names(char names[][16], int n)
{
	int i, j;

	for (i = 1; i < n; i++) {
		char key[16];

		strcpy(key, names[i]);
		j = i - 1;
		while (j >= 0 && ep_index(names[j]) > ep_index(key)) {
			strcpy(names[j + 1], names[j]);
			j--;
		}
		strcpy(names[j + 1], key);
	}
}

static int wait_epfiles(char names[][16], int max, int timeout_ms)
{
	int waited = 0;

	while (!g_stop && waited < timeout_ms) {
		int n = collect_epfiles(names, max);

		if (n > 0)
			return n;
		device_wait_ms(100);
		waited += 100;
	}
	return 0;
}

static int wait_bound(int fds[], int n, int timeout_ms)
{
	int waited = 0;

	while (!g_stop && waited < timeout_ms) {
		struct usb_endpoint_descriptor d;
		int i, ok = 0;

		for (i = 0; i < n; i++) {
			memset(&d, 0, sizeof(d));
			if (ioctl(fds[i], FUNCTIONFS_ENDPOINT_DESC, &d) == 0)
				ok++;
		}
		if (ok == n)
			return 0;
		device_wait_ms(100);
		waited += 100;
	}
	return -1;
}

/*
 * True when the supervisor advertised a second configuration (b.2) that holds
 * this function set. In that layout b.1 is a PTP-only standby and the host is
 * *expected* to sit on it for an arbitrary time before it switches to b.2 --
 * so "no endpoint enabled yet" is the normal state, not a dead session.
 *
 * The single-configuration layout is the opposite: the host selected the
 * configuration this function lives in, and if it still never enabled an
 * endpoint something is wrong, so the 60 s timeout must keep its original
 * meaning there.
 */
static int g_standby_config;

static int carplay_config_is_second(void)
{
	return g_standby_config;
}

static const char *ep_role(int idx)
{
	switch (idx) {
	case EP_PTP_IN:     return "PTP  IN    ";
	case EP_PTP_OUT:    return "PTP  OUT   ";
	case EP_MUX_IN:     return "iAP2/MUX IN ";
	case EP_MUX_OUT:    return "iAP2/MUX OUT";
	case EP_ETH_IN:     return "eth  IN    ";
	case EP_ETH_OUT:    return "eth  OUT   ";
	case EP_NCM_IRQ:    return "NCM irq  IN ";
	case EP_NCM_IN:     return "NCM data IN ";
	case EP_NCM_OUT:    return "NCM data OUT";
	default:          return "unknown     ";
	}
}

/* ------------------------------------------------------------------ *
 * Function endpooint I/O lives in two threads.
 *
 * f_fs epfiles implement no ->poll(), so poll() always reports the file
 * as ready and a blocking read() then parks the caller forever -- which
 * used to freeze the whole daemon (no retransmissions, no timers) as
 * soon as the host stayed silent.  Writes have the same shape: a write
 * to a bulk IN endpoint blocks until the host actually reads it.
 *
 * So: a writer thread drains a byte queue, a reader thread blocks on the
 * OUT endpoint, and the timer loop below only touches the link object
 * while holding g_link_mu.
 * ------------------------------------------------------------------ */
static pthread_mutex_t g_link_mu = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t g_link_cv = PTHREAD_COND_INITIALIZER;
static struct iap2_link g_link_obj;
static struct iap2_session g_session_obj;
static struct lockdown g_lockdown_obj;
static struct iap2_link *g_link = &g_link_obj;
static struct iap2_session *g_session = &g_session_obj;
static struct carplay_vehicle_state g_vehicle_state;
static struct carplay_config g_config;
static uint64_t g_session_generation;
static struct lockdown *g_lockdown = &g_lockdown_obj;
static struct lockdown_tls *g_tls;
static uint32_t g_lockdown_connection;
static uint32_t g_carkit_connection;
static unsigned long g_raw_rx_events;
static int g_out_fd = -1;
/* ff/fd/01 alt1 pair.  Read-only for now: the frames are logged and dropped
 * until the AirPlay bridge replaces the kernel NCM netdev. */
static int g_eth_out_fd = -1;

/*
 * One ff/fe/02 bulk pair, two possible transports.  An OEM head unit drives
 * bare iAP2 link frames over it; a PC or DiPlay drives USBMUX instead.  Both
 * arrive on the same OUT endpoint, so the reader splits them by frame header
 * and the first transport that shows traffic owns the round.  Keeping the
 * choice sticky avoids interleaving two independent byte streams into one
 * link object.  g_native_in_fd is the endpoint that link answers on -- the
 * same file as g_out_fd's peer.
 */
static int g_native_in_fd = -1;
static int g_native_active;

static void publish_control_status(const char *state, int usb_bound)
{
	struct carplay_control_status status;
	struct ncm_airplay_status airplay;
	int night_known = 0;
	int night = 0;

	memset(&status, 0, sizeof(status));
	memset(&airplay, 0, sizeof(airplay));
	ncm_bridge_get_airplay_status(&airplay);
	if (state && strcmp(state, "carplay") == 0) {
		if (airplay.streaming)
			state = "carplay";
		else if (airplay.active)
			state = "airplay";
		else
			state = "usb-bound";
	}
	carplay_vehicle_state_get_night(&g_vehicle_state, &night_known, &night);
	snprintf(status.state, sizeof(status.state), "%s",
		 state ? state : "unknown");
	status.generation = g_session_generation;
	status.usb_bound = !!usb_bound;
	status.display_width = airplay.display_width > 0 ? airplay.display_width :
		g_config.fallback_width;
	status.display_height = airplay.display_height > 0 ? airplay.display_height :
		g_config.fallback_height;
	status.requested_width = airplay.requested_width;
	status.requested_height = airplay.requested_height;
	status.physical_width = airplay.physical_width;
	status.physical_height = airplay.physical_height;
	status.display_density = airplay.display_density ?
		airplay.display_density : g_config.display_density;
	status.requested_fps = g_config.video_fps;
	status.actual_fps = airplay.actual_fps;
	status.native_size = airplay.native_size;
	snprintf(status.display_error, sizeof(status.display_error), "%s",
		 airplay.display_error);
	status.touch = airplay.touch;
	status.keyboard = airplay.keyboard;
	status.media_audio = airplay.active ? airplay.media_audio : 0;
	status.microphone = airplay.active ? airplay.microphone : 0;
	status.gps = g_config.car_gps;
	carplay_vehicle_state_snapshot(&g_vehicle_state, now_ms(), &status.vehicle);
	status.appearance_known = night_known;
	status.night_mode = night;
	status.audio_streams = airplay.audio_streams;
	status.media_available = airplay.media_available;
	status.microphone_available = airplay.microphone_available;
	status.microphone_ready = airplay.microphone_ready;
	status.microphone_demand = airplay.microphone_demand;
	status.microphone_packets_received = airplay.microphone_packets_received;
	status.oem_icon_present = airplay.oem_icon_present;
	status.request_car_ui_supported = airplay.request_car_ui_supported;
	status.car_ui_ready = airplay.car_ui_ready;
	snprintf(status.last_error, sizeof(status.last_error), "%s",
		 g_status_error);
	carplay_control_set_status(&status);
}

static int control_car_ui_requested(void *context)
{
	int result;
	(void)context;
	result = ncm_bridge_request_car_ui();
	if (result == 0)
		logmsg("car UI requested; keeping CarPlay, audio and USB active");
	else if (result == 1)
		logmsg("car UI request rejected: AirPlay event channel is not ready");
	else if (result == 2)
		logmsg("car UI request rejected: car lacks enhancedRequestCarUI");
	else
		logmsg("car UI request failed while sending requestUI");
	return result;
}

static void control_config_changed(const struct carplay_config *config,
                                   void *context)
{
	(void)context;
	if (config) {
		snprintf(g_status_error, sizeof(g_status_error),
			 "configuration saved; automatic restart required");
		logmsg("configuration saved; requesting supervised restart");
	}
	g_reload_requested = 1;
	g_stop = 1;
}

static void begin_vehicle_session(void)
{
	g_session_generation++;
	if (!g_session_generation)
		g_session_generation = 1;
	carplay_vehicle_state_begin_session(&g_vehicle_state,
					    g_session_generation);
	ncm_bridge_set_vehicle_state(&g_vehicle_state, g_session_generation);
	if (g_config.car_gps)
		iap2_session_set_vehicle_state(g_session, &g_vehicle_state,
					      g_session_generation);
}

/* A new USB enumeration starts a fresh usbmux session and iAP2 link. */
static void reset_session(void)
{
	pthread_mutex_lock(&g_link_mu);
	g_mux_selected = 0;
	g_native_prefix_pending = 0;
	iap2_link_init(g_link, now_ms());
	iap2_session_init(g_session);
	g_raw_rx_events = 0;
	begin_vehicle_session();
	publish_control_status("usb-wait", 0);
	usbmux_init(g_mux);
	lockdown_init(g_lockdown);
	if (g_tls) {
		lockdown_tls_destroy(g_tls);
		g_tls = lockdown_tls_create();
	}
	g_lockdown_connection = 0;
	g_carkit_connection = 0;
	pthread_mutex_unlock(&g_link_mu);
}

static void log_bytes(const char *tag, const uint8_t *p, size_t n)
{
	char hex[3 * 32 + 4];
	size_t i;
	int off = 0;

	for (i = 0; i < n && i < 32; i++)
		off += snprintf(hex + off, sizeof(hex) - (size_t)off, "%02x ",
				p[i]);
	iap2_log("%s %zu bytes: %s", tag, n, hex);
}

static void mux_send_cb(void *ctx, const uint8_t *data, size_t len)
{
	struct usbmux *mux = ctx;

	if (mux && usbmux_connected(mux))
		usbmux_send(mux, data, len);
}

static void lockdown_send_cb(void *ctx, const uint8_t *data, size_t len)
{
	struct usbmux *mux = ctx;

	if (!mux || !usbmux_connected(mux))
		return;
	if (g_tls && lockdown_tls_handshake_done(g_tls)) {
		iap2_log("lockdown: TX secure plist frame %zu bytes", len);
		lockdown_tls_write(g_tls, data, len, mux_send_cb, mux);
	} else {
		iap2_log("lockdown: TX plist frame %zu bytes", len);
		mux_send_cb(mux, data, len);
	}
}

static void secure_plist_recv(void *ctx, const uint8_t *data, size_t len)
{
	struct lockdown *l = ctx;

	if (!l)
		return;
	if (lockdown_feed(l, data, len, lockdown_send_cb, g_mux) < 0)
		iap2_log("lockdown: malformed secure plist rejected");
}

/*
 * Port 62078 carries length-prefixed lockdown plists. Port 62079 is the
 * syslog relay and remains passive. Port 62080 is com.apple.carkit.service,
 * where the iAP2 link starts only after that service connection is up.
 * Caller holds g_link_mu.
 */
static void pump_locked(void)
{
	static uint8_t buf[4096];
	size_t n;

	if (!g_mux || !g_link || !g_lockdown)
		return;

	/*
	 * Bare iAP2 on ff/fe/02.  No framing layer sits between the bulk
	 * pair and the link, so frames move in both directions untouched.  The
	 * link is started by the reader the moment the host touches the pair.
	 */
	if (g_native_active) {
		while ((n = iap2_link_take_control(g_link, buf, sizeof(buf))) > 0)
			iap2_session_feed(g_session, g_link, buf, n, now_ms());
		iap2_session_tick(g_session, g_link, now_ms());
		if (g_host_active && ncm_bridge_host_authentication(
			    iap2_session_authentication_ready(g_session, g_link))) {
			iap2_log("host: current authentication lost; retiring AirPlay and USB round");
			atomic_store(&g_host_failed, EPROTO);
		}
		while ((n = iap2_link_take(g_link, buf, sizeof(buf))) > 0) {
			log_tx(buf, n);
			txq_push(buf, n);
		}
		return;
	}

	while ((n = usbmux_take_in(g_mux, buf, sizeof(buf))) > 0) {
		if (g_mux->destination_port == USBMUX_LOCAL_PORT) {
			if (g_mux->connection_id != g_lockdown_connection) {
				g_lockdown_connection = g_mux->connection_id;
				lockdown_init(g_lockdown);
				if (g_tls) {
					lockdown_tls_destroy(g_tls);
					g_tls = lockdown_tls_create();
				}
				iap2_log("lockdown: new TCP connection %u",
					 g_lockdown_connection);
			}
			if (g_lockdown->tls_requested) {
				if (!g_tls)
					g_tls = lockdown_tls_create();
				if (lockdown_tls_start(g_tls) < 0) {
					iap2_log("tls: server start failed");
				} else if (lockdown_tls_feed(g_tls, buf, n,
							 mux_send_cb, g_mux,
							 secure_plist_recv,
							 g_lockdown) < 0) {
					iap2_log("tls: encrypted input failed");
				}
			} else {
				if (lockdown_feed(g_lockdown, buf, n,
						  lockdown_send_cb, g_mux) < 0)
					iap2_log("lockdown: malformed input rejected");
				if (g_lockdown->tls_requested) {
					if (!g_tls)
						g_tls = lockdown_tls_create();
					if (lockdown_tls_start(g_tls) < 0)
						iap2_log("tls: server start failed");
				}
			}
		} else if (g_mux->destination_port == 62080) {
			if (g_mux->connection_id != g_carkit_connection) {
				g_carkit_connection = g_mux->connection_id;
				iap2_link_init(g_link, now_ms());
				iap2_session_init(g_session);
				begin_vehicle_session();
				publish_control_status("iap2", 1);
				iap2_log("carkit: new TCP connection %u",
					 g_carkit_connection);
				iap2_link_start(g_link, now_ms());
			}
			iap2_link_feed(g_link, buf, n, now_ms());
			while ((n = iap2_link_take_control(
					g_link, buf, sizeof(buf))) > 0)
				iap2_session_feed(g_session, g_link, buf, n,
						  now_ms());
		} else {
			iap2_log("relay: dropping %zu bytes for passive port %u",
				 n, g_mux->destination_port);
		}
	}

	if (usbmux_connected(g_mux) && g_mux->destination_port == 62080) {
		if (g_link->state == IAP2_IDLE) {
			g_carkit_connection = g_mux->connection_id;
			iap2_session_init(g_session);
			if (g_config.car_gps)
				iap2_session_set_vehicle_state(g_session, &g_vehicle_state,
						      g_session_generation);
			iap2_log("pump: carkit service up, starting the iAP2 link");
			iap2_link_start(g_link, now_ms());
		}
		while ((n = iap2_link_take_control(
				g_link, buf, sizeof(buf))) > 0)
			iap2_session_feed(g_session, g_link, buf, n, now_ms());
		iap2_session_tick(g_session, g_link, now_ms());
		while ((n = iap2_link_take(g_link, buf, sizeof(buf))) > 0)
			usbmux_send(g_mux, buf, n);
	}

	while ((n = usbmux_take(g_mux, buf, sizeof(buf))) > 0) {
		log_tx(buf, n);
		txq_push(buf, n);
	}
}

static void endpoint_reader_failed(int fd, const char *operation, int error_code)
{
	int is_current;

	pthread_mutex_lock(&g_link_mu);
	is_current = g_out_fd == fd;
	pthread_mutex_unlock(&g_link_mu);
	if (!is_current)
		return;
	if (error_code == ESHUTDOWN) {
		uint64_t expected = 0;

		if (atomic_compare_exchange_strong(
			    &g_endpoint_reconfiguration_since, &expected,
			    now_ms()))
			iap2_log("round endpoint %s transiently configured; "
				 "waiting for endpoint re-enable", operation);
		return;
	}
	if (atomic_exchange(&g_endpoint_round_stop, 1) == 0) {
		if (error_code)
			iap2_log("round endpoint %s failed: %s; reconnecting",
				 operation, strerror(error_code));
		else
			iap2_log("round endpoint %s reached EOF; reconnecting", operation);
	}
}

/* LSP starts FF 5A, detection starts FF 55; 40 40 belongs to the CSM
 * payload inside LSP. Keep the selected transport for continuation bytes. */
#define IAP2_FRAME_MAGIC 0xFF
#define USBMUX_FRAME_MAGIC 0xFE

static int frame_is_iap2(const uint8_t *buf, size_t n)
{
	if (g_native_active)
		return 1;
	if (g_mux_selected)
		return 0;
	if (n >= 2 && buf[0] == IAP2_FRAME_MAGIC &&
	    (buf[1] == 0x5A || buf[1] == 0x55))
		return 1;
	if (n >= 1 && buf[0] == USBMUX_FRAME_MAGIC)
		return 0;
	/* Split frame: keep feeding the transport that is running. */
	return g_native_active;
}

/*
 * Per-frame dispatch.  The caller has already returned from read(); the shared
 * link state is taken here so the timer loop and the reader serialise.
 *
 * The transport is chosen by frame header, not by endpoint: a head unit opens
 * ff/fe/02 and speaks iAP2 link frames on it, while DiPlay claims the same
 * interface and speaks USBMUX.  Selecting on the endpoint (as the two-pair
 * layout used to) cannot work any more -- and on a real phone it never did.
 */
static void dispatch_out_frame(const uint8_t *buf, size_t n)
{
	uint8_t prefix[2];
	int native;
	pthread_mutex_lock(&g_link_mu);
	if (g_stop || n == 0) {
		pthread_mutex_unlock(&g_link_mu);
		return;
	}
	if (g_link) {
		if (!g_native_active && !g_mux_selected &&
		    !g_native_prefix_pending && n == 1 && buf[0] == 0xFF) {
			g_native_prefix_pending = 1;
			pthread_mutex_unlock(&g_link_mu);
			return;
		}
		prefix[0] = 0xFF;
		prefix[1] = buf[0];
		native = g_native_prefix_pending ? frame_is_iap2(prefix, 2) :
			 frame_is_iap2(buf, n);
		if (native) {
			/*
			 * The head unit is driving bare iAP2. Start the link
			 * before feeding so the first frame lands in a running
			 * link.
			 */
			if (!g_native_active) {
				g_native_active = 1;
				if (g_host_active)
					txq_set_host(&g_host);
				else
					txq_set_fd(g_native_in_fd);
				iap2_session_init(g_session);
				if (g_config.car_gps)
					iap2_session_set_vehicle_state(g_session, &g_vehicle_state,
							      g_session_generation);
				iap2_link_start(g_link, now_ms());
				iap2_log("native: host is driving bare iAP2 on "
					 "ff/fe/02; this round uses the native "
					 "link");
			}
			if (carplay_log_should_sample(++g_raw_rx_events))
				log_bytes("RX IAP2", buf, n);
			if (g_native_prefix_pending)
				iap2_link_feed(g_link, prefix, 1, now_ms());
			iap2_link_feed(g_link, buf, n, now_ms());
		} else {
			g_mux_selected = 1;
			if (carplay_log_should_sample(++g_raw_rx_events))
				log_bytes("RX RAW", buf, n);
			if (g_native_prefix_pending)
				usbmux_feed(g_mux, prefix, 1);
			usbmux_feed(g_mux, buf, n);
		}
		g_native_prefix_pending = 0;
		pump_locked();
	}
	pthread_mutex_unlock(&g_link_mu);
}

/*
 * One reader thread on the single blocking OUT endpoint.
 *
 * f_fs epfiles implement no ->poll(), so poll() reports every endpoint ready
 * whether or not data exists, and a synchronous read() always waits for a
 * transfer to complete (f_fs.c: ffs_epfile_io -> wait_for_completion_
 * interruptible) regardless of O_NONBLOCK.  Two threads used to read two
 * pairs and race for the same link object; now that bare iAP2 and USBMUX
 * share ff/fe/02 there is exactly one OUT endpoint to block on and the split
 * happens in dispatch_out_frame().
 */
static void *endpoint_reader_main(void *arg)
{
	uint8_t buf[4096];

	(void)arg;
	for (;;) {
		int fd;
		ssize_t n;

		pthread_mutex_lock(&g_link_mu);
		while (!g_stop && g_out_fd < 0)
			pthread_cond_wait(&g_link_cv, &g_link_mu);
		fd = g_out_fd;
		if (g_stop) {
			pthread_mutex_unlock(&g_link_mu);
			break;
		}
		pthread_mutex_unlock(&g_link_mu);

		n = read(fd, buf, sizeof(buf));
		if (n <= 0) {
			int read_error = n < 0 ? errno : 0;
			int current;

			if (read_error == EAGAIN || read_error == EWOULDBLOCK ||
			    read_error == EINTR) {
				/*
				 * EAGAIN: nothing queued yet. EINTR: the
				 * round moved on and the teardown or shutdown
				 * interrupted us. Either way re-read the round
				 * state at the top instead of blaming the
				 * endpoint.
				 */
				usleep(2000);
				continue;
			}
			pthread_mutex_lock(&g_link_mu);
			current = g_out_fd;
			pthread_mutex_unlock(&g_link_mu);
			if (fd != current) {
				/* This round already replaced the endpoint. */
				continue;
			}
			endpoint_reader_failed(fd, "read", read_error);
			usleep(2000);
			continue;
		}

		dispatch_out_frame(buf, (size_t)n);
	}

	g_reader_exited = 1;
	return NULL;
}

/*
 * ipheth reader for the ff/fd/01 alt1 bulk pair.
 *
 * The pair exists so a host classifies this configuration as CarPlay and can
 * bring the link up, but the bridge that turns those frames into a netdev is
 * not written yet.  Reading it anyway is deliberate: an OUT endpoint nobody
 * services NAKs every transfer, so the host's URB queue fills and the link
 * looks dead on arrival with no trace on our side.  Consuming the frames costs
 * nothing and leaves the length and first bytes of each packet in the log --
 * which is the only local evidence that a head unit opened alt 1 at all.
 */
static void *eth_reader_main(void *arg)
{
	uint8_t buf[4096];
	unsigned long total = 0;
	unsigned long frames = 0;

	(void)arg;
	for (;;) {
		int fd;
		ssize_t n;

		pthread_mutex_lock(&g_link_mu);
		while (!g_stop && g_eth_out_fd < 0)
			pthread_cond_wait(&g_link_cv, &g_link_mu);
		fd = g_eth_out_fd;
		if (g_stop) {
			pthread_mutex_unlock(&g_link_mu);
			break;
		}
		pthread_mutex_unlock(&g_link_mu);

		n = read(fd, buf, sizeof(buf));
		if (n <= 0) {
			int read_error = n < 0 ? errno : 0;
			int current;

			if (read_error == EAGAIN || read_error == EWOULDBLOCK ||
			    read_error == EINTR) {
				usleep(2000);
				continue;
			}
			pthread_mutex_lock(&g_link_mu);
			current = g_eth_out_fd;
			pthread_mutex_unlock(&g_link_mu);
			if (fd != current)
				continue;
			logmsg("eth: read failed: %s", strerror(read_error));
			usleep(2000);
			continue;
		}

		total += (unsigned long)n;
		frames++;
		/* First few frames verbatim, then every 64th, so a chatty link
		 * cannot flood the log. */
		if (frames <= 8 || (frames % 64) == 0)
			logmsg("eth: RX %zd bytes (frames=%lu total=%lu) hdr=%02x %02x %02x %02x %02x %02x",
			       n, frames, total,
			       n > 0 ? buf[0] : 0, n > 1 ? buf[1] : 0,
			       n > 2 ? buf[2] : 0, n > 3 ? buf[3] : 0,
			       n > 4 ? buf[4] : 0, n > 5 ? buf[5] : 0);
	}

	g_eth_reader_exited = 1;
	return NULL;
}

static int serve_round(int round)
{
	char names[MAX_EPS][16];
	int fds[MAX_EPS];
	int in_fd = -1, out_fd = -1;
	int n, i;
	/*
	 * How many endpoint files ep0 must hand us for the index map below to be
	 * valid. With the kernel owning NCM, our descriptors stop at ff/fd/01
	 * (which has no endpoints), so a two-file set is complete rather than
	 * truncated.
	 */
	const int expected_eps = g_kernel_ncm ? (g_eth_alt ? 6 : 4) : EP_COUNT;

	atomic_store(&g_endpoint_round_stop, 0);
	atomic_store(&g_endpoint_reconfiguration_since, 0);
	atomic_store(&g_ncm_restart_requested, 0);
	logmsg("round %d: waiting for endpoint files", round);
	n = wait_epfiles(names, MAX_EPS, 20000);
	if (n <= 0) {
		logmsg("round %d: no endpoint files appeared", round);
		return -1;
	}
	sort_names(names, n);
	logmsg("round %d: %d endpoint files present", round, n);

	for (i = 0; i < n; i++) {
		char path[64];

		snprintf(path, sizeof(path), FFS_ROOT "/%s", names[i]);
		fds[i] = open(path, O_RDWR);
		if (fds[i] < 0) {
			int opened;
			logmsg("  open %s failed: %s", path, strerror(errno));
			for (opened = 0; opened < i; opened++)
				close(fds[opened]);
			return -1;
		}
	}

	logmsg("round %d: waiting for bind", round);
	if (carplay_config_is_second()) {
		/*
		 * Standby layout: b.1 (PTP only) is what the host selects first and
		 * b.2 is us. Only FUNCTIONFS_ENABLE means it actually switched, and
		 * it may take the host a minute of probing before it does. Reopening
		 * the endpoint files on a timeout would push that switch up to the
		 * reconnect backoff (30 s at the cap) and re-arm the descriptors
		 * underneath a host that is about to use them, so wait indefinitely
		 * instead and say so once a minute.
		 */
		while (!g_stop && !atomic_load(&g_endpoint_round_stop) &&
		       wait_bound(fds, n, 60000) < 0)
			logmsg("round %d: host has not selected configuration 2 yet; "
			       "still waiting (udc=%s)", round, udc_state());
		if (g_stop || atomic_load(&g_endpoint_round_stop)) {
			unsigned long long generation =
				atomic_load(&g_endpoint_enable_generation);
			int waited = 0;

			logmsg("round %d: endpoint set was disabled before becoming active",
			       round);
			for (i = 0; i < n; i++)
				close(fds[i]);
			/*
			 * A host switching to the CarPlay configuration shows up
			 * as DISABLE immediately followed by ENABLE for the new
			 * endpoint set.  Wait that out and report a clean round
			 * instead of a failure: the head unit selects
			 * configuration 2 the instant the 0x51 ACK lands, and a
			 * 1 s backoff here would open the iAP2 link a full second
			 * late -- right when the head unit starts its own
			 * SYNCHRONIZE.
			 */
			while (!g_stop && waited < 600 &&
			       atomic_load(&g_endpoint_enable_generation) ==
				       generation) {
				device_wait_ms(20);
				waited += 20;
			}
			if (!g_stop &&
			    atomic_load(&g_endpoint_enable_generation) !=
				    generation) {
				logmsg("round %d: host selected the CarPlay "
				       "configuration; reopening the endpoint set "
				       "immediately", round);
				carplay_log_sync();
				return 0;
			}
			return -1;
		}
		logmsg("round %d: host selected configuration 2; endpoints enabled", round);
	} else if (wait_bound(fds, n, 60000) < 0) {
		logmsg("round %d: function never bound", round);
		for (i = 0; i < n; i++)
			close(fds[i]);
		return -1;
	}
	if (atomic_load(&g_endpoint_round_stop)) {
		logmsg("round %d: endpoint set was disabled before becoming active",
		       round);
		for (i = 0; i < n; i++)
			close(fds[i]);
		return -1;
	}

	for (i = 0; i < n; i++) {
		struct usb_endpoint_descriptor d;

		memset(&d, 0, sizeof(d));
		if (ioctl(fds[i], FUNCTIONFS_ENDPOINT_DESC, &d) < 0)
			continue;
		logmsg("  [%d] %s %-4s addr=0x%02x attr=0x%02x mps=%u", i,
		       names[i], ep_role(i), d.bEndpointAddress,
		       d.bmAttributes, d.wMaxPacketSize);
	}

	/*
	 * The endpoint index map is tied to the descriptor plan above, so a
	 * partial set must never be mapped through it: with 5 files present,
	 * index 3 would be an NCM endpoint.  Either the whole set the current
	 * layout promises is there, or this daemon has nothing safe to do.
	 */
	g_native_in_fd = -1;
	g_native_active = 0;
	pthread_mutex_lock(&g_link_mu);
	g_mux_selected = 0;
	g_native_prefix_pending = 0;
	g_eth_out_fd = -1;
	pthread_mutex_unlock(&g_link_mu);
	if (n >= expected_eps) {
		in_fd = fds[EP_MUX_IN];
		out_fd = fds[EP_MUX_OUT];
		/* Bare iAP2 answers on the same IN endpoint USBMUX uses. */
		g_native_in_fd = in_fd;
		if (g_eth_alt && n > EP_ETH_OUT) {
			int flags = fcntl(fds[EP_ETH_OUT], F_GETFL, 0);
			if (flags >= 0)
				(void)fcntl(fds[EP_ETH_OUT], F_SETFL,
					    flags | O_NONBLOCK);
			pthread_mutex_lock(&g_link_mu);
			g_eth_out_fd = fds[EP_ETH_OUT];
			pthread_cond_broadcast(&g_link_cv);
			pthread_mutex_unlock(&g_link_mu);
		}
	}
	if (in_fd >= 0 && out_fd >= 0) {
		int flags = fcntl(in_fd, F_GETFL, 0);
		if (flags >= 0)
			(void)fcntl(in_fd, F_SETFL, flags | O_NONBLOCK);
		flags = fcntl(out_fd, F_GETFL, 0);
		if (flags >= 0)
			(void)fcntl(out_fd, F_SETFL, flags | O_NONBLOCK);
	}
	if (in_fd < 0 || out_fd < 0) {
		logmsg("round %d: not enough endpoints (%d files, need %d)", round,
		       n, expected_eps);
		g_native_active = 0;
		g_native_in_fd = -1;
		for (i = 0; i < n; i++)
			close(fds[i]);
		return -1;
	}

	logmsg("round %d: BOUND (%d endpoints). ff/fe/02 carries iAP2 and USBMUX, starting NCM bridge",
	       round, n);
	publish_control_status("usb-bound", 1);
	if (g_kernel_ncm) {
		/*
		 * Interfaces 2/3 are the kernel's. Wait for the netdev it registered
		 * and hand it the addresses the AirPlay path expects; nothing here
		 * touches an endpoint.
		 */
		if (ncm_bridge_netdev_up() == 0)
			logmsg("round %d: kernel NCM netdev ready", round);
		else
			logmsg("round %d: kernel NCM netdev not ready; AirPlay has no transport",
			       round);
	} else if (n > EP_NCM_OUT) {
		if (ncm_bridge_start(fds[EP_NCM_OUT], fds[EP_NCM_IN]) == 0)
			logmsg("round %d: NCM bulk IN/OUT bridge active", round);
		else
			logmsg("round %d: NCM bridge failed to start", round);
	} else {
		logmsg("round %d: missing NCM endpoints", round);
	}

	tx_log_budget = 24;

	pthread_mutex_lock(&g_link_mu);
	txq_set_fd(in_fd);
	pump_locked();
	g_out_fd = out_fd;
	/* Broadcast, not signal: the reader may be parked on this condition. */
	pthread_cond_broadcast(&g_link_cv);
	pthread_mutex_unlock(&g_link_mu);

	/* Timer loop: retransmissions keep running even if the host is silent
	 * and the reader thread is parked inside read(). */
	for (;;) {
		uint64_t reconfiguration_since;

		device_wait_ms(100);
		if (g_stop)
			break;
		reconfiguration_since = atomic_load(
			&g_endpoint_reconfiguration_since);
		if (reconfiguration_since &&
		    now_ms() - reconfiguration_since >= 1000) {
			iap2_log("round: endpoint re-enable did not recover "
				 "within 1000ms; reconnecting");
			atomic_store(&g_endpoint_round_stop, 1);
		}
		if (atomic_exchange(&g_ncm_restart_requested, 0)) {
			int restart_rc;

			if (g_kernel_ncm)
				iap2_log("round: re-applying kernel NCM netdev after endpoint re-enable; AirPlay transport will reconnect");
			else
				iap2_log("round: restarting NCM USB workers after endpoint re-enable; preserving TUN/iAP2 and rebuilding AirPlay transport");
			/* Only one branch is evaluated, so the NCM endpoint indices
			 * are never read when they do not exist. */
			restart_rc = g_kernel_ncm
				? ncm_bridge_restart_usb(-1, -1)
				: ncm_bridge_restart_usb(fds[EP_NCM_OUT],
							 fds[EP_NCM_IN]);
			if (restart_rc < 0)
				iap2_log("round: NCM restart failed");
		}
		if (atomic_load(&g_endpoint_round_stop) || g_stop) {
			pthread_mutex_lock(&g_link_mu);
			g_out_fd = -1;
			g_native_active = 0;
			pthread_cond_broadcast(&g_link_cv);
			pthread_mutex_unlock(&g_link_mu);
			break;
		}
		pthread_mutex_lock(&g_link_mu);
		if (g_out_fd < 0 || g_stop) {
			pthread_mutex_unlock(&g_link_mu);
			break;
		}
		iap2_link_tick(g_link, now_ms());
		if (g_link->state == IAP2_DEAD) {
			logmsg("round %d: iAP2 link exhausted or reset; reconnecting", round);
			atomic_store(&g_endpoint_round_stop, 1);
		}
		pump_locked();
		publish_control_status("carplay", 1);
		pthread_mutex_unlock(&g_link_mu);
	}

	publish_control_status("teardown", 1);
	pthread_mutex_lock(&g_link_mu);
	iap2_session_send_location_stop(g_session, g_link, now_ms());
	carplay_vehicle_state_end_session(&g_vehicle_state, g_session_generation);
	publish_control_status("teardown", 0);
	logmsg("round %d: link torn down (tx=%u rx=%u frames, usbmux rx=%u tx=%u)",
	       round, g_link->tx_frames, g_link->rx_frames,
	       g_mux ? g_mux->frames_in : 0, g_mux ? g_mux->frames_out : 0);
	g_out_fd = -1;
	g_native_active = 0;
	g_native_in_fd = -1;
	g_eth_out_fd = -1;
	pthread_mutex_unlock(&g_link_mu);

	/*
	 * Wake the readers out of read() before the descriptor files go away.
	 * Closing a file does not interrupt a read that is already blocked in
	 * it, and a reader left parked on a stale file would silently swallow
	 * the next round's traffic on that channel.
	 */
	if (g_reader_started && !g_reader_exited)
		(void)pthread_kill(g_reader, SIGUSR1);
	if (g_eth_reader_started && !g_eth_reader_exited)
		(void)pthread_kill(g_eth_reader, SIGUSR1);

	ncm_bridge_stop();
	txq_set_fd(-1);
	for (i = 0; i < n; i++)
		close(fds[i]);
	return 0;
}

static void handle_stop_signal(int signal_number)
{
	(void)signal_number;
	g_external_stop = 1;
	g_stop = 1;
	wake_device_loop();
}

/* Device workers have already joined. Clear only the internal handoff's
 * unwind, and recheck sticky stop causes after the store to cover a signal
 * or management reload racing with it. Host reconnect never calls this. */
static int host_handoff_resume(void)
{
	if (g_external_stop || g_reload_requested)
		return 0;
	g_stop = 0;
	if (g_external_stop || g_reload_requested) {
		g_stop = 1;
		return 0;
	}
	return 1;
}

static void handle_shutdown_wakeup(int signal_number)
{
	(void)signal_number;
}

/*
 * Tears the transport down.  `final` selects how far: 1 ends the process
 * (management service, vehicle state and file log go too), 0 keeps those
 * alive because the in-process role switch hands over to the host loop in
 * the same process and the management port must survive the trip.
 */
static void stop_runtime(int ep0, int final)
{
	int i;

	logmsg("shutdown: stop_runtime enter ep0=%d", ep0);
	g_stop = 1;
	if (g_tx_initialized) {
		pthread_mutex_lock(&g_tx.mu);
		g_tx.running = 0;
		g_tx.fd = -1;
		pthread_cond_broadcast(&g_tx.cv);
		pthread_mutex_unlock(&g_tx.mu);
	}
	pthread_mutex_lock(&g_link_mu);
	g_out_fd = -1;
	g_native_active = 0;
	atomic_store(&g_endpoint_round_stop, 1);
	pthread_cond_broadcast(&g_link_cv);
	pthread_mutex_unlock(&g_link_mu);
	logmsg("shutdown: bridge close begin");
	ncm_bridge_close();
	logmsg("shutdown: bridge close complete");
	/* Wake the FunctionFS event reader before waiting for its thread. */
	if (ep0 >= 0)
		close(ep0);
	logmsg("shutdown: ep0 closed");
	/* Closing an endpoint does not reliably wake a blocked FunctionFS
	 * read owned by another thread. A dedicated signal interrupts those
	 * syscalls so the normal shutdown path can join both readers. */
	/* Closing an endpoint does not reliably wake a blocked FunctionFS
	 * read owned by another thread. Keep delivering the wake-up until each
	 * reader has returned, covering the small signal-before-read race. */
	for (i = 0; i < 200; i++) {
		int readers_running =
			(g_reader_started && !g_reader_exited) ||
			(g_eth_reader_started && !g_eth_reader_exited) ||
			(g_ep0_started && !g_ep0_exited);
		if (!readers_running)
			break;
		if (g_reader_started && !g_reader_exited)
			(void)pthread_kill(g_reader, SIGUSR1);
		if (g_eth_reader_started && !g_eth_reader_exited)
			(void)pthread_kill(g_eth_reader, SIGUSR1);
		if (g_ep0_started && !g_ep0_exited)
			(void)pthread_kill(g_ep0thread, SIGUSR1);
		usleep(10000);
	}
	logmsg("shutdown: FunctionFS readers wake sent reader_exited=%d eth_exited=%d ep0_exited=%d",
	       g_reader_exited, g_eth_reader_exited, g_ep0_exited);
	if (g_writer_started) {
		logmsg("shutdown: writer join begin");
		(void)pthread_join(g_writer, NULL);
		g_writer_started = 0;
		logmsg("shutdown: writer join complete");
	}
	if (g_reader_started) {
		logmsg("shutdown: reader join begin");
		(void)pthread_join(g_reader, NULL);
		g_reader_started = 0;
		logmsg("shutdown: reader join complete");
	}
	if (g_eth_reader_started) {
		logmsg("shutdown: eth reader join begin");
		(void)pthread_join(g_eth_reader, NULL);
		g_eth_reader_started = 0;
		logmsg("shutdown: eth reader join complete");
	}
	if (g_ep0_started) {
		logmsg("shutdown: ep0 join begin");
		(void)pthread_join(g_ep0thread, NULL);
		g_ep0_started = 0;
		logmsg("shutdown: ep0 join complete");
	}
	logmsg("shutdown: control stop begin");
	if (final) {
		carplay_control_stop();
		logmsg("shutdown: control stop complete");
		carplay_vehicle_state_destroy(&g_vehicle_state);
		carplay_log_shutdown();
	} else {
		logmsg("shutdown: control service kept alive for the role switch");
	}
}

/*
 * Reader for the host shape: pull iAP2 frames out of the head unit's IN
 * endpoint.  Separate from endpoint_reader_main() because usbfs has no file
 * to park in -- USBDEVFS_BULK blocks for its own timeout and reports
 * ETIMEDOUT when the device has nothing queued, which is the idle state and
 * not a failure.
 */
static void *host_reader_main(void *arg)
{
	uint8_t buf[4096];

	(void)arg;
	for (;;) {
		int n;

		if (g_stop || atomic_load(&g_host_round_stop) ||
		    atomic_load(&g_host_failed))
			break;
		if (!g_host_active) {
			usleep(50000);
			continue;
		}
		n = usbhost_read(&g_host, buf, sizeof(buf), 200);
		if (n < 0) {
			if (errno == ETIMEDOUT || errno == EINTR)
				continue;
			if (errno == EAGAIN || errno == EWOULDBLOCK) {
				usleep(2000);
				continue;
			}
			atomic_store(&g_host_failed, errno ? errno : EIO);
			iap2_log("host reader: %s; ending host round", strerror(errno));
			break;
		}
		if (n > 0 && !g_stop && !atomic_load(&g_host_round_stop))
			dispatch_out_frame(buf, (size_t)n);
	}
	g_reader_exited = 1;
	return NULL;
}

/*
 * What is actually on the bus, logged line by line so every line keeps its
 * own timestamp.  Called while a host round is waiting and once more when it
 * gives up: the difference between "nothing attached" and "attached, but not
 * shaped like iAP2" is the whole answer and we cannot see it from the daemon
 * otherwise.
 */
static void host_bus_report(const char *tag)
{
	char snap[4096];
	char *line;
	char *save = NULL;
	int buses = usbhost_role_bus_count();
	int devices = usbhost_snapshot(snap, sizeof(snap));

	if (devices <= 0) {
		if (devices < 0)
			logmsg("host bus (%s): sysfs unreadable; xhci root buses=%d",
			       tag, buses);
		else if (buses < 0)
			logmsg("host bus (%s): no downstream devices; USB sysfs unreadable",
			       tag);
		else if (buses == 0)
			logmsg("host bus (%s): no xhci root bus for selected UDC",
			       tag);
		else
			logmsg("host bus (%s): xhci root buses=%d; no downstream devices enumerated",
			       tag, buses);
		carplay_log_sync();
		return;
	}
	logmsg("host bus (%s): xhci root buses=%d; %d device(s) visible",
	       tag, buses, devices);
	for (line = strtok_r(snap, "\n", &save); line;
	     line = strtok_r(NULL, "\n", &save))
		logmsg("host bus | %s", line);
	carplay_log_sync();
}

/* Enumeration success is not link success; a silent peer must not park us
 * in host mode forever. This is a local recovery policy, not a wire timer. */
static int host_link_timed_out(uint64_t deadline, uint64_t now)
{
	return deadline != 0 && now >= deadline;
}

/* Reap one USB round without changing the process's sticky stop request.
 * Both usbfs I/O workers use bounded transfers. Their handles remain valid
 * until both joins complete, including a signal racing with re-enumeration. */
static void host_round_stop(void)
{
	carplay_vehicle_state_end_session(&g_vehicle_state, g_session_generation);
	publish_control_status("usb-reconnecting", 0);
	atomic_store(&g_host_round_stop, 1);
	if (g_tx_initialized) {
		pthread_mutex_lock(&g_tx.mu);
		g_tx.running = 0;
		g_tx.host = NULL;
		g_tx.head = g_tx.tail = 0;
		pthread_cond_broadcast(&g_tx.cv);
		pthread_mutex_unlock(&g_tx.mu);
	}
	pthread_mutex_lock(&g_link_mu);
	g_host_active = 0;
	g_native_active = 0;
	g_out_fd = -1;
	pthread_cond_broadcast(&g_link_cv);
	pthread_mutex_unlock(&g_link_mu);
	if (g_writer_started) {
		(void)pthread_join(g_writer, NULL);
		g_writer_started = 0;
	}
	if (g_reader_started) {
		(void)pthread_join(g_reader, NULL);
		g_reader_started = 0;
	}
	/* Stop discovery, connection and stream workers before clearing the
	 * host interface. Configured AirPlay options survive bridge close. */
	ncm_bridge_close();
}

static int host_round_start(void)
{
	txq_init();
	atomic_store(&g_host_round_stop, 0);
	atomic_store(&g_host_failed, 0);
	g_reader_exited = 0;
	if (pthread_create(&g_writer, NULL, writer_main, NULL) != 0)
		return -1;
	g_writer_started = 1;
	if (pthread_create(&g_reader, NULL, host_reader_main, NULL) != 0)
		return -1;
	g_reader_started = 1;
	return 0;
}


/*
 * Host main loop.  Enumerate the head unit, then run exactly the same link
 * timers the device path runs -- iAP2 link, sessions and AirPlay are shared,
 * which is the whole point of splitting the role from the transport.
 */
static int run_host_mode(const struct carplay_config *config, int force_attach)
{
	int round = 0;
	int wait_rounds = 0;
	int stamp_reported = 0;
	unsigned reconnects = 0;
	int restore_device = 0;
	int auth_handoff = 0;
	uint64_t link_deadline = 0;
	uint64_t enum_deadline = now_ms() +
		HOST_ENUM_GIVE_UP_ROUNDS * (HOST_ENUM_POLL_US / 1000u);

	(void)config;
	if (!force_attach)
		logmsg("host mode: no gadget bound; supervisor already switched dwc3");

	if (!g_tls)
		g_tls = lockdown_tls_create();

	while (!g_stop) {
		int post_auth = auth_handoff;

		round++;
		if (usbhost_open(&g_host, 0x05ac, 0x12a8) != 0) {
			if (g_stop)
				break;
			wait_rounds++;
			if (now_ms() >= enum_deadline) {
				/*
				 * Nothing came up.  Hand the role back rather than sit
				 * in host mode forever: on this port that means no
				 * gadget, no host controller traffic and a head unit
				 * staring at a dead bus.
				 */
				host_bus_report("give-up");
				logmsg("host: no head unit after %d ms; returning the device role",
				       wait_rounds * (HOST_ENUM_POLL_US / 1000));
				restore_device = 1;
				break;
			}
			/* Rounds 1-2 and a slow heartbeat carry the same
			 * information as 200 identical lines, with the bus
			 * contents attached. */
			if (round <= 2 || round == 10 || round == 20 ||
			    round % 100 == 0)
				host_bus_report("waiting");
			/* Other ports may carry Apple devices too. Never drop the
			 * controller constraint just because the car is delayed. */
			usleep(HOST_ENUM_POLL_US);
			continue;
		}
		if (g_stop || now_ms() >= enum_deadline) {
			usbhost_close(&g_host);
			restore_device = !g_stop;
			break;
		}
		wait_rounds = 0;
		if (!stamp_reported) {
			uint64_t stamp;

			stamp_reported = 1;
			if (g_role_t0 && now_ms() > g_role_t0) {
				/* In-process switch: the trigger time is
				 * already in this address space. */
				role_ts("first device seen on the bus "
					"(spec budget: 1000 ms to enumeration "
					"traffic)");
			} else if (role_swap_stamp_take(&stamp) == 0 &&
				   now_ms() > stamp) {
				logmsg("host: enumerated %llu ms after the 0x51 "
				       "hand-off (spec budget: 1000 ms to first "
				       "enumeration traffic)",
				       (unsigned long long)(now_ms() - stamp));
			}
		}
		iap2_log("host %d: driving %04x:%04x bus=%03d dev=%03d iface=%d "
			 "ep_in=0x%02x(%d) ep_out=0x%02x(%d)",
			 round, g_host.vid, g_host.pid, g_host.busnum,
			 g_host.devnum, g_host.interface, g_host.ep_in,
			 g_host.ep_in_max, g_host.ep_out, g_host.ep_out_max);

		(void)ncm_bridge_host_authentication(0);
		if (host_round_start() != 0) {
			logmsg("host: cannot start both I/O workers; restoring device role");
			host_round_stop();
			usbhost_close(&g_host);
			restore_device = 1;
			break;
		}

		g_mux_selected = 0;
		g_native_prefix_pending = 0;
		iap2_link_init(g_link, now_ms());
		g_link->usb_out_max_packet = g_host.ep_out_max;
		iap2_session_init(g_session);
		begin_vehicle_session();
		usbmux_init(g_mux);
		lockdown_init(g_lockdown);
		publish_control_status("usb-bound", 1);

		pthread_mutex_lock(&g_link_mu);
		g_native_active = 1;
		atomic_store(&g_host_failed, 0);
		txq_set_host(&g_host);
		iap2_link_start(g_link, now_ms());
		link_deadline = now_ms() + 15000u;
		g_host_active = 1;
		pump_locked();
		pthread_cond_broadcast(&g_link_cv);
		pthread_mutex_unlock(&g_link_mu);

		/* Initiate iAP2 before waiting for the independently bound NCM
		 * interface. Revisit a late interface while link timers keep running. */
		int netdev_ready = 0;
		uint64_t netdev_retry = now_ms();
		uint64_t netdev_deadline = netdev_retry + 15000u;
		while (!g_stop) {
			/* Service the session's 20 ms backpressure deadlines even when
			 * an ACK arrives just before its next eligible retry. */
			usleep(IAP2_SESSION_WINDOW_POLL_MS * 1000u);
			if (g_stop || atomic_load(&g_host_failed))
				break;
			if (!netdev_ready && now_ms() >= netdev_retry) {
				netdev_ready = ncm_bridge_car_netdev_up(g_host.sysfs_name, 0) == 0;
				netdev_retry = now_ms() + 500u;
				if (netdev_ready)
					logmsg("host %d: netdev ready for AirPlay", round);
				else if (now_ms() >= netdev_deadline) {
					logmsg("host: no usable cdc_ncm interface after 15000 ms");
					atomic_store(&g_host_failed, ENETDOWN);
				}
			}
			if (atomic_load(&g_host_failed))
				break;
			if (!g_host_active)
				break;
			pthread_mutex_lock(&g_link_mu);
			iap2_link_tick(g_link, now_ms());
			pump_locked();
			/* Live authentication is cleared on LINK reset; retain whether
			 * this USB round ever queued AuthenticationCompleted. */
			if (g_session->authentication_completed_once) {
				post_auth = 1;
				auth_handoff = 1;
			}
			if (g_link->state == IAP2_NORMAL)
				link_deadline = 0;
			if (host_link_timed_out(link_deadline, now_ms())) {
				logmsg("host: iAP2 negotiation timed out after 15000 ms");
				atomic_store(&g_host_failed, ETIMEDOUT);
			}
			if (g_link->state == IAP2_DEAD)
				atomic_store(&g_host_failed, EPROTO);
			publish_control_status("carplay", 1);
			pthread_mutex_unlock(&g_link_mu);
		}

		/* A read/write may still be inside usbfs. Stop and join both
		 * workers before closing the descriptor or reusing g_host. */
		host_round_stop();
		/* The reader can finish authentication and fail before the next
		 * timer iteration. After joining it, its round evidence is stable. */
		if (g_session->authentication_completed_once)
			post_auth = auth_handoff = 1;
		usbhost_close(&g_host);
		logmsg("host %d: link down (tx=%u rx=%u frames)", round,
		       g_link->tx_frames, g_link->rx_frames);
		if (g_stop)
			break;
		{
			int failure = atomic_load(&g_host_failed);
			int reconnect = usbhost_reconnect_candidate(failure, post_auth);

			if (!reconnect || reconnects >= HOST_RECONNECT_ROUNDS) {
				logmsg("host: returning device role after %s "
				       "(authenticated=%d reacquires=%u/%u)",
				       strerror(failure), post_auth, reconnects,
				       HOST_RECONNECT_ROUNDS);
				restore_device = 1;
				break;
			}
			reconnects++;
			enum_deadline = now_ms() +
				(post_auth ? HOST_POST_AUTH_RECONNECT_GRACE_MS :
					    HOST_RECONNECT_GRACE_MS);
			if (post_auth) {
				logmsg("host: authenticated USB failure (%s), cause unconfirmed; "
				       "staying host for reacquire %u/%u (%u ms grace)",
				       strerror(failure), reconnects,
				       HOST_RECONNECT_ROUNDS,
				       HOST_POST_AUTH_RECONNECT_GRACE_MS);
			} else {
				logmsg("host: USB disconnected; staying host for "
				       "reacquire %u/%u (%u ms grace)",
				       reconnects, HOST_RECONNECT_ROUNDS,
				       HOST_RECONNECT_GRACE_MS);
			}
			publish_control_status("usb-reconnecting", 0);
			/* EPROTO is observed just before the kernel removes the old
			 * usbfs node. Reopening in the same millisecond can claim that
			 * doomed node and fail again; wait past unregister/re-enumeration. */
			if (failure == EPROTO || failure == EPIPE)
				usleep(HOST_RECONNECT_SETTLE_US);
		}
	}
	if (g_tls) {
		lockdown_tls_destroy(g_tls);
		g_tls = NULL;
	}
	if (restore_device && !g_stop &&
	    box_role_request_write(ROLE_REQUEST_PATH, BOX_ROLE_DEVICE) == 0)
		g_role_handoff = 1;

	return 0;
}

int main(int argc, char **argv)
{
	int ep0 = -1;
	int round = 0;
	int foreground = 0;
	int management_only = 0;
	int force_attach = 0;
	int host_mode = 0;
	int selftest_role_switch = 0;
	char bench_endpoint[64] = "";
	int retry_delay_ms;
	int i;
	char config_error[192];
	enum carplay_config_load_result config_result;
	struct carplay_log_policy log_policy;

	for (i = 1; i < argc; i++) {
		if (strcmp(argv[i], "--fg") == 0)
			foreground = 1;
		else if (strcmp(argv[i], "--management-only") == 0)
			management_only = 1;
		else if (strcmp(argv[i], "--force-attach") == 0)
			force_attach = 1;
		else if (strcmp(argv[i], "--host") == 0)
			host_mode = 1;
		else if (strcmp(argv[i], "--selftest-role-switch") == 0)
			selftest_role_switch = 1;
		else if (strcmp(argv[i], "--airplay-endpoint") == 0 && i + 1 < argc)
			snprintf(bench_endpoint, sizeof(bench_endpoint), "%s",
				 argv[++i]);
		else {
			fprintf(stderr,
				"usage: %s [--fg] [--management-only] "
				"[--force-attach] [--host] "
				"[--airplay-endpoint HOST[:PORT]] "
				"[--selftest-role-switch]\n",
				argv[0]);
			return 64;
		}
	}

	setvbuf(stderr, NULL, _IOLBF, 0);
	/*
	 * The supervisor owns the gadget layout and tells us which one it built.
	 * Probing configfs from here looked simpler but does not work: iap2d runs
	 * under a SELinux domain that cannot stat /config/usb_gadget, so the probe
	 * silently reported "no second configuration" and every build behaved as
	 * the single-configuration layout.
	 */
	{
		const char *standby = getenv("CARPLAY_STANDBY_CONFIG");

		g_standby_config = (standby && standby[0] == '1') ? 1 : 0;
	}
	g_device_stop_fd = eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
	if (g_device_stop_fd < 0)
		logmsg("warning: device stop eventfd unavailable; using bounded wait: %s",
		       strerror(errno));
	signal(SIGINT, handle_stop_signal);
	signal(SIGTERM, handle_stop_signal);
	signal(SIGPIPE, SIG_IGN);
	{
		struct sigaction wakeup_action;
		memset(&wakeup_action, 0, sizeof(wakeup_action));
		wakeup_action.sa_handler = handle_shutdown_wakeup;
		sigemptyset(&wakeup_action.sa_mask);
		wakeup_action.sa_flags = 0;
		if (sigaction(SIGUSR1, &wakeup_action, NULL) < 0)
			logmsg("warning: cannot install FunctionFS wake-up signal: %s",
			       strerror(errno));
	}

	/* --fg leaves the process in the caller's process group so a test
	 * harness can wait() for it and report the exit status. */
	if (!foreground)
		escape_adbd_cgroup();

	carplay_config_defaults(&g_config);
	config_result = carplay_config_load(CARPLAY_CONFIG_PATH, &g_config,
					    config_error, sizeof(config_error));
	memset(&log_policy, 0, sizeof(log_policy));
	log_policy.directory = CARPLAY_LOG_DEFAULT_DIRECTORY;
	log_policy.file_bytes = g_config.log_file_bytes;
	log_policy.retention_days = (unsigned)g_config.log_retention_days;
	log_policy.total_bytes = g_config.log_total_bytes;
	log_policy.minimum_level = (enum carplay_log_level)g_config.log_level;
	if (carplay_log_init(&log_policy) < 0)
		logmsg("warning: rotating file log unavailable");
	if (config_result == CARPLAY_CONFIG_DEFAULTED_MISSING) {
		logmsg("configuration missing; writing validated defaults");
		(void)carplay_config_save(CARPLAY_CONFIG_PATH, &g_config, NULL, 0);
	} else if (config_result == CARPLAY_CONFIG_DEFAULTED_INVALID) {
		logmsg("configuration invalid: %s", config_error);
		(void)carplay_config_save(CARPLAY_CONFIG_PATH, &g_config, NULL, 0);
	} else {
		logmsg("configuration loaded enabled=%d media=%d microphone=%d gps=%d",
		       g_config.enabled, g_config.media_audio, g_config.microphone,
		       g_config.car_gps);
	}
	carplay_vehicle_state_init(&g_vehicle_state);
	ncm_bridge_set_airplay_options(g_config.media_audio,
					g_config.microphone,
					g_config.display_width,
					g_config.display_height,
					g_config.physical_width,
					g_config.physical_height,
					g_config.display_density,
					g_config.video_fps,
					g_config.video_bitrate);
	if (carplay_control_start(g_config.control_port, &g_config,
				  control_config_changed, NULL,
				  control_car_ui_requested, NULL) < 0)
		logmsg("warning: local management service could not start port=%d",
		       g_config.control_port);
	publish_control_status(g_config.enabled ? "waiting-usb" : "disabled", 0);
	/*
	 * Bench shape: dial a fixed AirPlay endpoint and run the sender against
	 * it.  No gadget, no iAP2, no NCM -- see ncm_bridge_bench_start().  It
	 * sits before every attach gate because it is not a USB mode at all.
	 */
	if (bench_endpoint[0]) {
		char host[64];
		char *colon;
		unsigned long port = 7000ul;

		snprintf(host, sizeof(host), "%s", bench_endpoint);
		colon = strrchr(host, ':');
		if (colon) {
			*colon = '\0';
			port = strtoul(colon + 1, NULL, 10);
			if (port == 0 || port > 65535ul) {
				logmsg("--airplay-endpoint: bad port in '%s'",
				       bench_endpoint);
				carplay_control_stop();
				carplay_vehicle_state_destroy(&g_vehicle_state);
				carplay_log_shutdown();
				return 64;
			}
		}
		begin_vehicle_session();
		if (ncm_bridge_bench_start(host, (uint16_t)port) < 0) {
			logmsg("--airplay-endpoint: could not start the sender for %s:%lu",
			       host, port);
			carplay_control_stop();
			carplay_vehicle_state_destroy(&g_vehicle_state);
			carplay_log_shutdown();
			return 1;
		}
		logmsg("bench: AirPlay sender running against %s:%lu; no USB transport",
		       host, port);
		while (!g_stop) {
			publish_control_status("carplay", 0);
			usleep(200000);
		}
		carplay_vehicle_state_end_session(&g_vehicle_state, g_session_generation);
		publish_control_status("teardown", 0);
		ncm_bridge_stop();
		carplay_control_stop();
		carplay_vehicle_state_destroy(&g_vehicle_state);
		carplay_log_shutdown();
		return g_reload_requested ? 75 : 0;
	}
	/*
	 * Host shape: the supervisor already switched dwc3 after a 0x51, so
	 * there is no gadget to prepare and no descriptors to write.  The
	 * bootStart / attach gates below belong to the device path only --
	 * here the daemon's job is to enumerate the head unit and drive iAP2
	 * from our side.
	 */
	if (host_mode) {
		if (!g_config.enabled) {
			logmsg("CarPlay disabled by configuration; host mode not started");
			carplay_control_stop();
			carplay_vehicle_state_destroy(&g_vehicle_state);
			carplay_log_shutdown();
			return 0;
		}
		/*
		 * The give-up path in run_host_mode sets g_role_handoff when
		 * it hands the role back to the supervisor.  Returning its
		 * bare 0 made the supervisor log "clean stop" and exit its
		 * whole loop (seen live 2026-10-28); only init restarting the
		 * service kept the box alive.  Map the hand-off to 76.
		 */
		run_host_mode(&g_config, force_attach);
		if (g_role_handoff)
			return 76;
		return g_reload_requested ? 75 : 0;
	}
	if (!g_config.enabled || management_only ||
	    (!g_config.boot_start && !force_attach)) {
		if (!g_config.enabled)
			logmsg("CarPlay disabled by configuration; management mode remains active");
		else if (management_only)
			logmsg("management-only mode requested; USB attach remains disabled");
		else
			logmsg("bootStart is disabled; management mode remains active");
		while (!g_stop)
			usleep(200000);
		carplay_control_stop();
		carplay_vehicle_state_destroy(&g_vehicle_state);
		carplay_log_shutdown();
		return g_reload_requested ? 75 : 0;
	}

	txq_init();
	/*
	 * Decide the NCM layout before a single descriptor is written: if the
	 * kernel function is linked into our configuration we must not describe
	 * interfaces 2/3 ourselves, and ep0 will only hand us four endpoints.
	 */
	g_kernel_ncm = ncm_bridge_kernel_available();
	if (ncm_bridge_open("iap2ncm0") != 0) {
		logmsg("warning: NCM dual-stack TUN unavailable");
	} else if (g_kernel_ncm) {
		logmsg("NCM ready: kernel netdev mode (class descriptors from configfs ncm.usb0)");
	} else {
		logmsg("NCM dual-stack TUN ready (IPv4 preferred)");
	}
	g_tls = lockdown_tls_create();
	if (pthread_create(&g_writer, NULL, writer_main, NULL) != 0)
		logmsg("warning: no writer thread, TX will be dropped");
	else
		g_writer_started = 1;
	g_reader_exited = 0;
	if (pthread_create(&g_reader, NULL, endpoint_reader_main,
			   (void *)(intptr_t)0) != 0)
		logmsg("warning: no reader thread, USBMUX RX will not be processed");
	else
		g_reader_started = 1;
	g_eth_reader_exited = 0;
	if (pthread_create(&g_eth_reader, NULL, eth_reader_main, NULL) != 0)
		logmsg("warning: no eth reader thread, ff/fd/01 frames will be NAKed");
	else
		g_eth_reader_started = 1;
	ep0 = open(EP0_PATH, O_RDWR | O_NONBLOCK);
	if (ep0 < 0) {
		logmsg("open %s: %s", EP0_PATH, strerror(errno));
		stop_runtime(-1, 1);
		return 1;
	}
	logmsg("ep0 opened");

	if (write_descriptors(ep0) < 0) {
		stop_runtime(ep0, 1);
		return 1;
	}
	if (write_strings(ep0) < 0) {
		stop_runtime(ep0, 1);
		return 1;
	}

	g_ep0_exited = 0;
	if (pthread_create(&g_ep0thread, NULL, ep0_main,
			   (void *)(intptr_t)ep0) != 0)
		logmsg("warning: no ep0 thread, vendor control requests unanswered");
	else
		g_ep0_started = 1;
	iap2_link_init(g_link, now_ms());
	iap2_session_init(g_session);
	begin_vehicle_session();
	usbmux_init(g_mux);
	lockdown_init(g_lockdown);

	/*
	 * Self-test: exercise the in-process role switch without a head unit
	 * sending 0x51.  The device runtime is up (gadget bound by the
	 * supervisor, descriptors written); wait a moment, then trigger the
	 * exact path a real 0x51 takes.  With nothing on the bus the host loop
	 * gives up after 10 s and the supervisor hand-off brings the box back
	 * to device mode -- the full cycle, all of it timestamped.
	 */
	if (selftest_role_switch) {
		int waited;

		waited = 0;
		while (waited < SELFTEST_TRIGGER_DELAY_S * 10 && !g_stop) {
			device_wait_ms(100);
			waited++;
		}
		if (g_stop) {
			/* Stopped while the self-test was still waiting: a
			 * plain service stop must not flip the port to host
			 * on its way out. */
			logmsg("selftest: interrupted before the trigger; leaving device mode");
			g_switch_pending = BOX_ROLE_DEVICE;
		} else {
			logmsg("selftest: triggering the in-process host switch now");
			g_role_t0 = now_ms();
			g_switch_pending = BOX_ROLE_HOST;
			g_stop = 1;
		}
	}

	retry_delay_ms = g_config.reconnect_initial_ms;
	while (!g_stop) {
		round++;
		if (serve_round(round) < 0) {
			unsigned delay_ms = (unsigned)retry_delay_ms;
			logmsg("USB reconnect backoff delay=%ums", delay_ms);
			while (!g_stop && delay_ms) {
				unsigned step = delay_ms > 200u ? 200u : delay_ms;
				device_wait_ms(step);
				delay_ms -= step;
			}
			if (retry_delay_ms < g_config.reconnect_max_ms)
				retry_delay_ms *= 2;
			if (retry_delay_ms > g_config.reconnect_max_ms)
				retry_delay_ms = g_config.reconnect_max_ms;
		} else {
			retry_delay_ms = g_config.reconnect_initial_ms;
			device_wait_ms(100);
		}
	}

	logmsg("shutdown requested");
	stop_runtime(ep0, 0);
	/*
	 * Preferred path: the switch happens right here, in this process, so
	 * the gap between "0x51 acked" and "host mode active" is a few
	 * milliseconds of sysfs writes instead of a supervisor round-trip.
	 * The return trip (host -> device after a give-up) keeps using the
	 * supervisor: there is no deadline on it -- the head unit already fell
	 * back to host per the spec's 1 s rule -- and the script owns the
	 * standby-config bookkeeping on re-attach.
	 */
	if (g_switch_pending == BOX_ROLE_HOST &&
	    !g_external_stop && !g_reload_requested) {
		if (role_switch_to_host() == 0) {
			/*
			 * The trigger set g_stop to unwind the device loop;
			 * run_host_mode parks on the same flag, so it must be
			 * cleared or the host loop would exit before its first
			 * enumeration round (seen live on 2026-10-28: the box
			 * switched to host in 46 ms and returned instantly).
			 */
			g_role_handoff = 0;
			if (host_handoff_resume())
				run_host_mode(&g_config, force_attach);
			/*
			 * Only a queued role request is a handoff. A signal or
			 * management reload remains a normal stop/reload exit.
			 */
			return g_role_handoff ? 76 : (g_reload_requested ? 75 : 0);
		}
		/*
		 * The kernel refused something.  Queue the old hand-off so
		 * the supervisor still gets us to host; better late than a
		 * dead bus.
		 */
		logmsg("in-process switch failed; falling back to the "
		       "supervisor hand-off");
		if (g_external_stop || g_reload_requested)
			return g_reload_requested ? 75 : 0;
		if (box_role_request_write(ROLE_REQUEST_PATH, BOX_ROLE_HOST) == 0)
			g_role_handoff = 1;
		return 76;
	}
	/*
	 * 76 is the role hand-off: the supervisor switches dwc3 to host and
	 * restarts us in host mode.  Keeping it distinct from 75 (config
	 * reload) and from a crash keeps the supervisor's backoff honest.
	 */
	if (g_role_handoff)
		return 76;
	return g_reload_requested ? 75 : 0;
}
