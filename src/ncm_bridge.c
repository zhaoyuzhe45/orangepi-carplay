#define _GNU_SOURCE
/*
 * ncm_bridge.c -- CDC-NCM NTB16 <-> dual-stack TUN bridge for CarPlay.
 *
 * The host's NcmUsbBridge sends one untagged Ethernet/IPv4 or IPv6 frame in one NTB16
 * block. The device-side FunctionFS bulk OUT endpoint receives the NCM writer's
 * bulk transfer, while the bulk IN endpoint carries frames produced by the TUN.
 */
#include "ncm_bridge.h"
#include "carplay_log_sampling.h"
#include "carplay_config.h"
#include "airplay_client.h"
#include "carplay_mdns.h"
#include "carplay_vehicle_state.h"
#include "carplay_audio_bridge.h"
#include "carplay_capabilities.h"
#include "carplay_display.h"
#include "usb_watchdog.h"
#include <arpa/inet.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <signal.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>
#include <poll.h>

#include <linux/if_tun.h>
#include <linux/if_addr.h>
#include <net/if.h>
#include <netinet/in.h>

#define NCM_TUN_DEVICE   "/dev/tun"
#define NCM_USB_BUFFER   (64u * 1024u)
#define NCM_ETH_HEADER   14u
#define NCM_IPV6_MIN     40u
#define NCM_IPV4_MIN     20u
#define NCM_NTH16_BYTES  12u
#define NCM_NDP16_BYTES  16u
#define NCM_NTH16_SIG    0x484d434eu
#define NCM_NDP16_SIG    0x304d434eu
#define NCM_DATAGRAM_OFF 28u
#define NCM_MAX_FRAME    65535u
#define NCM_MIN_BLOCK    (NCM_DATAGRAM_OFF + 1u)
#define AIRPLAY_CONNECT_RETRY_ATTEMPTS 40u
#define AIRPLAY_CONNECT_RETRY_DELAY_US 250000u
#define AIRPLAY_RECOVERY_POLL_MS 100u
#define AIRPLAY_RECOVERY_STABLE_MS 60000u
#define DISPLAY_PREPARATION_TIMEOUT_MS 250000u


/* dwc3 can leave a FunctionFS bulk transfer permanently queued after the
 * gadget rebuilds endpoints, so the writer parks inside write() and never
 * drains the TUN again (see docs/evidence/ncm-ndp16-length-bug-2026-10-23.md).
 * Signal the thread first; only re-enumerate when that does not help. */
#define NCM_UDC_PATH "/config/usb_gadget/g1/UDC"
#define NCM_UDC_SETTLE_MS 200u
#define NCM_WATCHDOG_WAKE_MS 3000u
#define NCM_WATCHDOG_REBIND_MS 15000u
#define NCM_WATCHDOG_COOLDOWN_MS 30000u
#define NCM_WATCHDOG_POLL_MS 200u

/* Kernel-side NCM (configfs "ncm.usb0"): the netdev the kernel creates for the
 * function, and how long to wait for it after the configuration is set.
 *
 * The name is NOT ours to choose. f_ncm.c exposes "ifname" as a read-only
 * attribute (show only, no store) and gether_setup_default() fixes the name to
 * "usb%d" when the directory is created, so writing to it silently fails and
 * the netdev is really called "usb0". The authoritative value is that very
 * attribute, which ncm_kernel_ifname_resolve() reads; NCM_KERNEL_IFNAME below
 * is only the last-resort guess when the attribute is unreadable. */
#define NCM_KERNEL_IFNAME "ncm0"
#define NCM_KERNEL_FUNC_DIR "/config/usb_gadget/g1/functions/ncm.usb0"
#define NCM_KERNEL_IFNAME_ATTR NCM_KERNEL_FUNC_DIR "/ifname"
#define NCM_NETDEV_WAIT_MS 5000u

extern void iap2_log(const char *fmt, ...);

static const uint8_t ncm_source_mac[6] = { 0x72, 0xf5, 0xc0, 0x9f, 0x8b, 0xe1 };

static pthread_mutex_t g_mu = PTHREAD_MUTEX_INITIALIZER;
static pthread_mutex_t g_usb_mu = PTHREAD_MUTEX_INITIALIZER;
static pthread_t g_usb_rx_thread;
static pthread_t g_usb_tx_thread;
static pthread_t g_watchdog_thread;
static struct usb_watchdog g_watchdog;
static atomic_int g_watchdog_running;
static int g_watchdog_started;
static atomic_int g_usb_rx_exited;
static atomic_int g_usb_tx_exited;
static int g_running;
static int g_usb_workers_running;
static unsigned long g_usb_generation;
static int g_usb_out_fd = -1;
static int g_usb_in_fd = -1;
static int g_tun_fd = -1;
static int g_tun_index;
/*
 * Kernel-side NCM.  See ncm_bridge_kernel_available() for why this exists:
 * in this mode the kernel owns interfaces 2/3 and the NTB framing, and all we
 * keep is the interface index plus the two addresses, which is exactly what the
 * connect path and the policy routes already consume.
 */
static int g_kernel_ncm;
/* Host-role cdc_ncm belongs to the car, independently of our gadget layout. */
static int g_host_netdev_ready;
static int g_host_auth_required;
static int g_host_authenticated;
static struct carplay_mdns *g_mdns;
static int g_mdns_enabled;
static char g_kernel_ifname[IFNAMSIZ] = NCM_KERNEL_IFNAME;
static uint8_t g_peer_mac[6];
static int g_peer_mac_valid;
static struct in6_addr g_peer_ipv6;
static int g_peer_ipv6_valid;
static uint16_t g_ntb_sequence;
static unsigned long g_rx_blocks;
static unsigned long g_tx_frames;
static unsigned long g_bad_frames;
static unsigned long g_rx_logged;
static unsigned long g_tx_logged;
static unsigned long g_usb_rx_chunks;
static unsigned long g_usb_rx_errors;
static unsigned long g_screen_data_rx_chunks;
static uint64_t g_screen_data_rx_bytes;
static pthread_cond_t g_connect_cv = PTHREAD_COND_INITIALIZER;
static int g_connect_thread_running;
static atomic_int g_connect_stop;
/* Retiring one connector does not revoke the enabled transport. */
static atomic_int g_connect_retiring;
static int g_transport_restarting;
static unsigned long g_transport_restart_epoch;
static int g_connect_sock = -1;
static int g_airplay_transport_restart_pending;
static int g_airplay_media_enabled = 1;
static int g_airplay_microphone_enabled = 1;
static int g_airplay_display_width;
static int g_airplay_display_height;
static int g_airplay_physical_width;
static int g_airplay_physical_height;
static int g_airplay_display_density;
static int g_airplay_video_fps = 30;
static int g_airplay_video_bitrate = CARPLAY_VIDEO_BITRATE_MAX;
static struct carplay_vehicle_state *g_vehicle_state;
static uint64_t g_vehicle_generation;
static struct ncm_airplay_status g_airplay_status;
static struct airplay_client *g_active_airplay;

struct ncm_connect_owner {
	unsigned long usb_generation;
	unsigned long restart_epoch;
	int tun_index;
	int kernel_ncm;
	int host_netdev_ready;
};
struct ncm_connect_request {
	struct ncm_connect_owner owner;
	char **addresses;
	/* Borrowed from addresses; set under g_mu after control TCP selection. */
	const char *selected_address;
	size_t count;
	uint16_t port;
	enum airplay_profile profile;
	int discovered;
	int media_audio;
	int microphone;
	int display_width;
	int display_height;
	int physical_width;
	int physical_height;
	int display_density;
	int video_fps;
	int video_bitrate;
	struct carplay_vehicle_state *vehicle_state;
	uint64_t vehicle_generation;
};
/* Borrowed from the single worker. Only read or changed under g_mu. */
static struct ncm_connect_request *g_connect_request;
static int g_connect_retrying;
static struct ncm_connect_request *g_pending_connect_request;
static int connect_owned_locked(const struct ncm_connect_request *request);
static int connect_ready_locked(void);
static int launch_connect_request_locked(struct ncm_connect_request *request);
static int queue_connect_endpoint(const char *const addresses[], size_t count,
				  uint16_t port, enum airplay_profile profile,
				  int discovered);
static int numeric_address(const char *address, char *literal, size_t capacity,
			   unsigned *scope);

static int connect_cancelled(void)
{
	return g_connect_stop || g_connect_retiring;
}

static void free_connect_request(struct ncm_connect_request *request)
{
	size_t i;

	if (!request)
		return;
	for (i = 0; i < request->count; i++)
		free(request->addresses[i]);
	free(request->addresses);
	free(request);
}
static uint16_t rd16_le(const uint8_t *p)
{
	return (uint16_t)((uint16_t)p[0] | ((uint16_t)p[1] << 8));
}

static uint16_t rd16_be(const uint8_t *p)
{
	return (uint16_t)(((uint16_t)p[0] << 8) | p[1]);
}

static uint32_t rd32_le(const uint8_t *p)
{
	return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
	       ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static void wr16_le(uint8_t *p, uint16_t value)
{
	p[0] = (uint8_t)value;
	p[1] = (uint8_t)(value >> 8);
}

static void wr32_le(uint8_t *p, uint32_t value)
{
	p[0] = (uint8_t)value;
	p[1] = (uint8_t)(value >> 8);
	p[2] = (uint8_t)(value >> 16);
	p[3] = (uint8_t)(value >> 24);
}

static int fd_is_open(int fd)
{
	return fd >= 0;
}

static uint64_t ncm_now_ms(void)
{
	struct timespec now;

	if (clock_gettime(CLOCK_MONOTONIC, &now) != 0)
		return 0;
	return (uint64_t)now.tv_sec * 1000u +
	       (uint64_t)(now.tv_nsec / 1000000);
}

static int run_ip_command(const char *what, const char *command)
{
	int rc = system(command);

	if (rc != 0)
		iap2_log("ncm: %s failed (status=%d): %s", what, rc, command);
	return rc == 0 ? 0 : -1;
}

static int ip_rule_present(int ipv6, int priority, const char *match)
{
	char command[64];
	char line[256];
	FILE *pipe;
	int found = 0;
	int parsed_priority;

	snprintf(command, sizeof(command), "ip -%d rule show 2>/dev/null",
		 ipv6 ? 6 : 4);
	pipe = popen(command, "r");
	if (!pipe)
		return 0;
	while (fgets(line, sizeof(line), pipe)) {
		if (sscanf(line, "%d:", &parsed_priority) == 1 &&
		    parsed_priority == priority && strstr(line, match)) {
			found = 1;
			break;
		}
	}
	pclose(pipe);
	return found;
}

/* Adding an IPv6 address does not make it immediately bindable: the kernel
 * first performs DAD. Keep DAD enabled and let the existing bounded connect
 * retries wait for the exact source on the exact NCM interface. */
static int ipv6_source_state(int index)
{
	FILE *file;
	char line[256];
	int result = 0;
	int saved_errno = EAGAIN;

	file = fopen("/proc/net/if_inet6", "r");
	if (!file)
		return -1;
	while (fgets(line, sizeof(line), file)) {
		char address[33], name[IFNAMSIZ];
		unsigned ifindex, prefix, scope, flags;

		if (sscanf(line, "%32s %x %x %x %x %15s", address,
			   &ifindex, &prefix, &scope, &flags, name) != 6 ||
		    ifindex != (unsigned)index ||
		    strcmp(address, "fe800000000000000000000000000001") != 0)
			continue;
		if (flags & IFA_F_DADFAILED) {
			result = -1;
			saved_errno = EADDRNOTAVAIL;
		} else if (!(flags & IFA_F_TENTATIVE)) {
			result = 1;
		}
		break;
	}
	if (ferror(file)) {
		result = -1;
		saved_errno = EIO;
	}
	fclose(file);
	if (result <= 0)
		errno = saved_errno;
	return result;
}

static int ensure_airplay_policy_routes(const char *reason)
{
	char ifname[IFNAMSIZ];
	char command[256];
	int added = 0;
	int failed = 0;
	int ipv6_state;

	if (g_tun_index <= 0 ||
	    !if_indextoname((unsigned)g_tun_index, ifname))
		return -1;

	snprintf(command, sizeof(command),
		 "ip -4 route replace 169.254.0.0/16 dev %s src 169.254.0.1 table 100</dev/null",
		 ifname);
	failed |= run_ip_command("refresh IPv4 AirPlay route", command) < 0;
	ipv6_state = ipv6_source_state(g_tun_index);
	if (ipv6_state > 0) {
		snprintf(command, sizeof(command),
			 "ip -6 route replace fe80::/64 dev %s src fe80::1 table 100</dev/null",
			 ifname);
		failed |= run_ip_command("refresh IPv6 AirPlay route", command) < 0;
	} else if (ipv6_state < 0) {
		iap2_log("ncm: IPv6 source readiness check failed on %s: %s",
			 ifname, strerror(errno));
		failed = 1;
	}

	if (!ip_rule_present(0, 38, "from 169.254.0.1 lookup 100")) {
		snprintf(command, sizeof(command),
			 "ip -4 rule del priority 38 >/dev/null 2>&1; "
			 "ip -4 rule add priority 38 from 169.254.0.1 lookup 100</dev/null");
		failed |= run_ip_command("restore IPv4 AirPlay source rule",
					 command) < 0;
		added = 1;
	}
	if (!ip_rule_present(1, 50, "from fe80::1 lookup 100")) {
		snprintf(command, sizeof(command),
			 "ip -6 rule del priority 50 >/dev/null 2>&1; "
			 "ip -6 rule add priority 50 from fe80::1 lookup 100</dev/null");
		failed |= run_ip_command("restore IPv6 AirPlay source rule",
					 command) < 0;
		added = 1;
	}
	if (added)
		iap2_log("ncm: refreshed AirPlay policy routing after %s", reason);
	return failed ? -1 : 0;
}

static int open_tun(const char *ifname)
{
	struct ifreq ifr;
	char command[256];
	int fd;

	fd = open(NCM_TUN_DEVICE, O_RDWR | O_NONBLOCK | O_CLOEXEC);
	if (fd < 0) {
		iap2_log("ncm: open %s failed: %s", NCM_TUN_DEVICE,
			 strerror(errno));
		return -1;
	}
	memset(&ifr, 0, sizeof(ifr));
	ifr.ifr_flags = IFF_TUN | IFF_NO_PI;
	snprintf(ifr.ifr_name, sizeof(ifr.ifr_name), "%s", ifname);
	if (ioctl(fd, TUNSETIFF, &ifr) < 0) {
		iap2_log("ncm: TUNSETIFF(%s) failed: %s", ifname,
			 strerror(errno));
		close(fd);
		return -1;
	}

	g_tun_index = (int)if_nametoindex(ifr.ifr_name);
	if (g_tun_index <= 0) {
		iap2_log("ncm: if_nametoindex(%s) failed: %s", ifr.ifr_name,
			 strerror(errno));
		close(fd);
		return -1;
	}

	snprintf(command, sizeof(command),
		 "ip link set dev %s mtu 1500 up</dev/null", ifr.ifr_name);
	run_ip_command("bring up TUN", command);
	snprintf(command, sizeof(command),
		 "ip -6 addr replace fe80::1/64 dev %s</dev/null", ifr.ifr_name);
	run_ip_command("assign IPv6 link-local", command);
	snprintf(command, sizeof(command),
		 "ip addr replace 169.254.0.1/16 dev %s</dev/null", ifr.ifr_name);
	run_ip_command("assign IPv4 link-local", command);
	ensure_airplay_policy_routes("TUN initialization");

	iap2_log("ncm: tun=%s index=%d addresses=169.254.0.1/16,fe80::1/64 mtu=1500",
		 ifr.ifr_name, g_tun_index);
	return fd;
}

int ncm_bridge_kernel_available(void)
{
	struct stat st;

	/*
	 * Probe the gadget layout, not a build flag: the same binary has to work
	 * on a flashed image (kernel emits interfaces 2/3) and on an unflashed
	 * one (we must describe them ourselves or the host sees no NCM at all).
	 * The supervisor creates this directory only when mkdir succeeds, which
	 * is precisely the test for CONFIG_USB_CONFIGFS_NCM.
	 */
	g_kernel_ncm = stat(NCM_KERNEL_FUNC_DIR, &st) == 0 && S_ISDIR(st.st_mode);
	if (g_kernel_ncm)
		iap2_log("ncm: kernel NCM function present (%s); interfaces 2/3 and their CS_INTERFACE descriptors come from the kernel",
			 NCM_KERNEL_FUNC_DIR);
	else
		iap2_log("ncm: no kernel NCM function; keeping the six-descriptor TUN layout (class descriptors unavailable)");
	return g_kernel_ncm;
}

/*
 * The kernel names this netdev, we do not: f_ncm.c's "ifname" attribute is
 * read-only and gether_setup_default() already fixed the name to "usb%d" when
 * the function directory was created. Reading that attribute back is the only
 * source that cannot drift; the directory scan covers a kernel without it.
 */
static int kernel_ifname_from_configfs(char *out, size_t len)
{
	FILE *fp;
	size_t n;

	fp = fopen(NCM_KERNEL_IFNAME_ATTR, "r");
	if (fp == NULL)
		return -1;
	if (fgets(out, (int)len, fp) == NULL) {
		fclose(fp);
		return -1;
	}
	fclose(fp);
	n = strlen(out);
	while (n > 0 && (out[n - 1] == '\n' || out[n - 1] == '\r' ||
			 out[n - 1] == ' ' || out[n - 1] == '\t'))
		out[--n] = '\0';
	return n > 0 ? 0 : -1;
}

static int kernel_ifname_from_scan(char *out, size_t len)
{
	DIR *dir;
	struct dirent *entry;
	int found = -1;

	dir = opendir("/sys/class/net");
	if (dir == NULL)
		return -1;
	while ((entry = readdir(dir)) != NULL) {
		/*
		 * gether registers NCM netdevs as "usb%d". Matching that prefix
		 * is deliberate: "any interface that is up" would be free to
		 * pick wlan0, which is a much worse answer than failing.
		 */
		if (strncmp(entry->d_name, "usb", 3) != 0)
			continue;
		if (if_nametoindex(entry->d_name) <= 0)
			continue;
		snprintf(out, len, "%s", entry->d_name);
		found = 0;
		break;
	}
	closedir(dir);
	return found;
}

int ncm_bridge_kernel_ifname(char *out, size_t len)
{
	if (out == NULL || len == 0)
		return -1;
	if (kernel_ifname_from_configfs(out, len) == 0)
		return 0;
	if (kernel_ifname_from_scan(out, len) == 0)
		return 0;
	snprintf(out, len, "%s", NCM_KERNEL_IFNAME);
	return -1;
}

/*
 * Bring the kernel's netdev up and give it the addresses the AirPlay path
 * binds to. Idempotent by construction (the ip commands are "replace"), so
 * BOUND can re-apply it after an unbind without checking anything first.
 */
/* Address plan shared by both netdev owners: our gadget's usb%d while we are
 * the device, and the head unit's cdc_ncm interface once we are the host.
 * Idempotent ("replace"), so a rebind can re-apply it blindly. */
static int netdev_configure(const char *name)
{
	char command[256];
	int index = (int)if_nametoindex(name);

	if (index <= 0)
		return -1;

	/* Same addresses as the TUN carried: the connect path binds its source to
	 * 169.254.0.1 / fe80::1 and scopes IPv6 by interface index, so keeping
	 * them identical is what makes this a drop-in replacement. On a real
	 * netdev the kernel also answers ARP/ND for them. */
	snprintf(command, sizeof(command),
		 "ip link set dev %s mtu 1500 up</dev/null", name);
	if (run_ip_command("bring up kernel NCM netdev", command) < 0)
		return -1;
	snprintf(command, sizeof(command),
		 "ip -6 addr replace fe80::1/64 dev %s</dev/null", name);
	if (run_ip_command("assign IPv6 link-local", command) < 0)
		return -1;
	snprintf(command, sizeof(command),
		 "ip addr replace 169.254.0.1/16 dev %s</dev/null", name);
	if (run_ip_command("assign IPv4 link-local", command) < 0)
		return -1;
	pthread_mutex_lock(&g_mu);
	g_tun_index = index;
	snprintf(g_kernel_ifname, sizeof(g_kernel_ifname), "%s", name);
	pthread_mutex_unlock(&g_mu);
	if (ensure_airplay_policy_routes("kernel NCM netdev") < 0) {
		pthread_mutex_lock(&g_mu);
		g_tun_index = 0;
		pthread_mutex_unlock(&g_mu);
		return -1;
	}

	iap2_log("ncm: netdev %s index=%d addresses=169.254.0.1/16,fe80::1/64 (kernel owns NTB framing)",
		 name, index);
	return 0;
}

/* "Wanted usb0, this box has [lo eth0 usb1]" is the difference between a
 * missing driver and a wrong name, and neither is visible without it. */
static void log_present_netdevs(const char *wanted, unsigned waited)
{
	char names[256];
	size_t used = 0;
	DIR *dir = opendir("/sys/class/net");
	struct dirent *entry;

	names[0] = '\0';
	while (dir && (entry = readdir(dir)) != NULL) {
		int written;

		if (entry->d_name[0] == '.')
			continue;
		written = snprintf(names + used, sizeof(names) - used, "%s%s",
				   used ? " " : "", entry->d_name);
		if (written < 0 || (size_t)written >= sizeof(names) - used)
			break;
		used += (size_t)written;
	}
	if (dir)
		closedir(dir);
	iap2_log("ncm: kernel netdev %s not up (waited %ums); present interfaces: [%s]",
		 wanted, waited, names);
}

static int netdev_apply(unsigned wait_ms)
{
	char name[IFNAMSIZ];
	unsigned waited = 0;
	int index;

	if (!g_kernel_ncm)
		return -1;

	if (ncm_bridge_kernel_ifname(name, sizeof(name)) < 0)
		iap2_log("ncm: cannot read %s; guessing %s",
			 NCM_KERNEL_IFNAME_ATTR, name);

	/*
	 * ncm_bind() registers the netdev when the UDC is bound, which happens
	 * before enumeration and therefore before FUNCTIONFS_BIND. A startup
	 * caller passes wait_ms=0 and takes whatever is already there; the
	 * BOUND path still tolerates a short window. There is no udev here.
	 */
	index = (int)if_nametoindex(name);
	while (index <= 0 && waited < wait_ms) {
		usleep(100000);
		waited += 100;
		index = (int)if_nametoindex(name);
	}
	if (index <= 0) {
		log_present_netdevs(name, wait_ms);
		return -1;
	}

	return netdev_configure(name);
}

/*
 * The netdev that belongs to the head unit.
 *
 * In host mode the car's own CDC-NCM function is taken by the kernel's
 * cdc_ncm driver, and that netdev has nothing to do with our gadget:
 * g_kernel_ncm and functions/ncm.usb0 describe what *we* offer while we are
 * the device.  netdev_apply() is written for that side and returns early when
 * our function directory is missing, so in host mode it reports "no
 * transport" even with the car's netdev sitting there fully up -- and it
 * would configure whichever interface the *name* usb0 happens to point at.
 *
 * The only identification that cannot be wrong is sysfs: keep the netdev
 * whose /sys/class/net/<if>/device resolves to an interface of the USB device
 * we just claimed, and whose driver is cdc_ncm.  This is also what keeps us
 * off eth0/wlan0 and off a stale usb0 left over from our own gadget.
 */
static int netdev_belongs_to(const char *ifname, const char *usb_dev)
{
	char link[256];
	char target[512];
	char driver[128];
	char prefix[64];
	const char *base;
	ssize_t n;

	snprintf(link, sizeof(link), "/sys/class/net/%s/device", ifname);
	n = readlink(link, target, sizeof(target) - 1);
	if (n <= 0)
		return 0;		/* lo, tun, wlan... not a USB device */
	target[n] = '\0';

	snprintf(prefix, sizeof(prefix), "%s/driver", link);
	n = readlink(prefix, driver, sizeof(driver) - 1);
	if (n <= 0)
		return 0;
	driver[n] = '\0';
	base = strrchr(driver, '/');
	base = base ? base + 1 : driver;
	if (strcmp(base, "cdc_ncm") != 0)
		return 0;

	if (!usb_dev || !*usb_dev)
		return 1;
	/* Interface sysfs names are "<device>:<config>.<interface>", e.g.
	 * "2-1:1.2", so the claimed device has to be their prefix. */
	snprintf(prefix, sizeof(prefix), "%s:", usb_dev);
	return strstr(target, prefix) != NULL;
}

static int car_netdev_name(const char *usb_dev, char *out, size_t len)
{
	struct dirent *entry;
	DIR *dir = opendir("/sys/class/net");

	if (!dir)
		return -1;
	while ((entry = readdir(dir)) != NULL) {
		if (entry->d_name[0] == '.')
			continue;
		if (!netdev_belongs_to(entry->d_name, usb_dev))
			continue;
		snprintf(out, len, "%s", entry->d_name);
		closedir(dir);
		return 0;
	}
	closedir(dir);
	return -1;
}

static void host_mdns_found(void *ctx, const char *address, uint16_t port)
{
	char literal[INET6_ADDRSTRLEN];
	unsigned scope;
	(void)ctx;
	if (!numeric_address(address, literal, sizeof(literal), &scope))
		return;
	/* The queue rechecks discovery eligibility under g_mu, so a callback
	 * already dispatched when stop begins cannot revive the connector. */
	(void)queue_connect_endpoint(&address, 1, port, AIRPLAY_PROFILE_LEGACY, 1);
}

/* Caller holds g_mu. Start does not join or wait for callbacks. */
static void host_mdns_start_locked(const char *name)
{
	if (g_mdns)
		return;
	g_mdns_enabled = 1;
	if (carplay_mdns_start(&g_mdns, name, host_mdns_found, NULL) < 0) {
		g_mdns_enabled = 0;
		iap2_log("ncm: interface discovery unavailable on %s: %s", name,
			 strerror(errno));
	}
}

int ncm_bridge_car_netdev_up(const char *usb_dev_name, unsigned wait_ms)
{
	char name[IFNAMSIZ];
	unsigned waited = 0;

	for (;;) {
		if (car_netdev_name(usb_dev_name, name, sizeof(name)) == 0) {
			if (netdev_configure(name) < 0)
				return -1;
			pthread_mutex_lock(&g_mu);
			g_host_auth_required = 1;
			g_host_netdev_ready = 1;
			/* An active worker being retired must observe stop until join. */
			if (!g_connect_thread_running && !g_transport_restarting)
				g_connect_stop = 0;
			if (connect_ready_locked() && !g_connect_thread_running && g_pending_connect_request) {
				struct ncm_connect_request *pending = g_pending_connect_request;
				g_pending_connect_request = NULL;
				if (launch_connect_request_locked(pending) < 0)
					g_pending_connect_request = pending;
			}
			host_mdns_start_locked(name);
			pthread_mutex_unlock(&g_mu);
			return 0;
		}
		if (waited >= wait_ms)
			break;
		usleep(100000);
		waited += 100;
	}
	iap2_log("ncm: no cdc_ncm netdev under %s after %ums; the head unit's network function did not bind",
		 usb_dev_name && *usb_dev_name ? usb_dev_name : "<any usb device>",
		 wait_ms);
	return -1;
}

int ncm_bridge_netdev_up(void)
{
	if (netdev_apply(NCM_NETDEV_WAIT_MS) < 0)
		return -1;
	pthread_mutex_lock(&g_mu);
	/* A new enabled gadget round follows stop/join. Kernel NCM has no
	 * USB workers whose start would normally clear the previous stop flag.
	 * Never revive a connector that is still retiring. */
	if (!g_connect_thread_running && !g_transport_restarting)
		g_connect_stop = 0;
	if (connect_ready_locked() && !g_connect_thread_running &&
	    g_pending_connect_request) {
		struct ncm_connect_request *pending = g_pending_connect_request;
		g_pending_connect_request = NULL;
		if (launch_connect_request_locked(pending) < 0)
			g_pending_connect_request = pending;
	}
	pthread_mutex_unlock(&g_mu);
	return 0;
}

int ncm_bridge_netdev_prepare(void)
{
	return netdev_apply(0u);
}

int ncm_bridge_open(const char *ifname)
{
	if (g_kernel_ncm) {
		/*
		 * Nothing to open: the netdev belongs to the kernel. ncm_bind()
		 * registers it as soon as the UDC is bound, which is before
		 * enumeration, so configure it right here instead of waiting for
		 * BOUND -- otherwise a box with no host never gets its addresses,
		 * and the first connect after plugging in races enumeration.
		 * Failing is not fatal: ncm_bridge_netdev_up() re-applies the
		 * same idempotent commands at BOUND time. The ifname argument is
		 * ignored -- the kernel names the netdev, we do not.
		 */
		(void)ifname;
		ncm_bridge_netdev_prepare();
		return 0;
	}
	pthread_mutex_lock(&g_mu);
	if (g_tun_fd >= 0) {
		pthread_mutex_unlock(&g_mu);
		return 0;
	}
	g_tun_fd = open_tun(ifname && *ifname ? ifname : "iap2ncm0");
	pthread_mutex_unlock(&g_mu);
	return g_tun_fd >= 0 ? 0 : -1;
}

static int hex_nibble(char c)
{
	if (c >= '0' && c <= '9')
		return c - '0';
	if (c >= 'a' && c <= 'f')
		return c - 'a' + 10;
	if (c >= 'A' && c <= 'F')
		return c - 'A' + 10;
	return -1;
}

/* NCM reserves 02:00:00:00:00:02 as the default host address. A head unit that
 * has not brought its NCM interface up yet sources its frames from that value,
 * so latching onto it makes every unicast frame we send get filtered by the
 * real NIC; the connect then just times out with no RST and no ICMP error. */
static int is_ncm_default_host_mac(const uint8_t *mac)
{
	static const uint8_t placeholder[6] = { 0x02, 0x00, 0x00, 0x00, 0x00, 0x02 };

	return memcmp(mac, placeholder, sizeof(placeholder)) == 0;
}

static void store_peer_mac(const uint8_t mac[6], const char *origin)
{
	if (g_peer_mac_valid && memcmp(g_peer_mac, mac, sizeof(g_peer_mac)) == 0)
		return;
	memcpy(g_peer_mac, mac, sizeof(g_peer_mac));
	g_peer_mac_valid = 1;
	iap2_log("ncm: peer MAC %02x:%02x:%02x:%02x:%02x:%02x from %s",
		 mac[0], mac[1], mac[2], mac[3], mac[4], mac[5], origin);
}

int ncm_bridge_set_peer_mac(const char *identifier)
{
	uint8_t mac[6];
	const char *p;
	size_t i;

	if (!identifier || strlen(identifier) < 17)
		return -1;
	p = identifier;
	for (i = 0; i < sizeof(mac); i++) {
		int hi = hex_nibble(p[0]);
		int lo = hi < 0 ? -1 : hex_nibble(p[1]);

		if (hi < 0 || lo < 0)
			return -1;
		mac[i] = (uint8_t)((hi << 4) | lo);
		p += 2;
		if (i + 1 < sizeof(mac)) {
			if (*p != ':' && *p != '-')
				return -1;
			p++;
		}
	}
	if (*p != '\0')
		return -1;
	if ((mac[0] & 1u) || is_ncm_default_host_mac(mac) ||
	    memcmp(mac, ncm_source_mac, sizeof(mac)) == 0)
		return -1;
	/* deviceIdentifier is authoritative: it describes the host interface the
	 * head unit will actually answer on, whereas a frame only tells us who
	 * sent it at that instant. */
	store_peer_mac(mac, "0x4301 deviceIdentifier");
	return 0;
}

static void learn_peer_mac(const uint8_t *frame, size_t len)
{
	if (len < NCM_ETH_HEADER)
		return;
	/* Judge the *source* address, not the destination: the head unit keeps
	 * sending router solicitations (33:33:00:00:00:02) and MLD reports
	 * (33:33:00:00:00:16) before anything unicast, and those are often the
	 * only frames we get before the first AirPlay connect. Rejecting every
	 * frame with a multicast destination left g_peer_mac invalid, so the SYN
	 * was addressed to our own MAC and the head unit silently dropped it
	 * (Connection timed out, no RST, no ICMPv6 unreachable). */
	if ((frame[6] & 1u) || memcmp(frame + 6, ncm_source_mac, 6) == 0)
		return;
	/* Keep a real address over the NCM default placeholder; a later frame or
	 * 0x4301 deviceIdentifier will replace it. */
	if (is_ncm_default_host_mac(frame + 6) && g_peer_mac_valid)
		return;
	store_peer_mac(frame + 6, "NCM frame");
}

static void learn_peer_ipv6(const uint8_t *packet, size_t len)
{
	struct in6_addr source;

	if (!packet || len < NCM_IPV6_MIN || (packet[0] >> 4) != 6)
		return;
	memcpy(&source, packet + 8, sizeof(source));
	if (IN6_IS_ADDR_UNSPECIFIED(&source) ||
	    IN6_IS_ADDR_MULTICAST(&source) ||
	    IN6_IS_ADDR_LOOPBACK(&source))
		return;
	pthread_mutex_lock(&g_mu);
	if (!g_peer_ipv6_valid || memcmp(&g_peer_ipv6, &source, sizeof(source)) != 0) {
		char text[INET6_ADDRSTRLEN];
		memcpy(&g_peer_ipv6, &source, sizeof(source));
		g_peer_ipv6_valid = 1;
		pthread_mutex_unlock(&g_mu);
		if (inet_ntop(AF_INET6, &source, text, sizeof(text)))
			iap2_log("ncm: learned peer IPv6 %s from NCM traffic", text);
		return;
	}
	pthread_mutex_unlock(&g_mu);
}

/* Snapshot the peer address learned from NCM traffic. Returns 1 and fills out
 * with the textual form when a usable address is known, 0 otherwise. The
 * AirPlay listener runs on the accessory's own tunnel address, so this is only
 * used to describe the link state when a connection cannot be established. */
static int copy_learned_peer_ipv6(char *out, size_t out_size)
{
	struct in6_addr peer;
	int valid;

	if (!out || out_size == 0)
		return 0;
	pthread_mutex_lock(&g_mu);
	valid = g_peer_ipv6_valid;
	if (valid)
		memcpy(&peer, &g_peer_ipv6, sizeof(peer));
	pthread_mutex_unlock(&g_mu);
	if (!valid)
		return 0;
	return inet_ntop(AF_INET6, &peer, out, out_size) ? 1 : 0;
}

static void log_tun_packet(const char *direction, const uint8_t *packet,
			   size_t len)
{
	unsigned long *logged = strcmp(direction, "RX") == 0 ? &g_rx_logged :
			       &g_tx_logged;
	uint8_t version;
	char src[INET6_ADDRSTRLEN];
	char dst[INET6_ADDRSTRLEN];
	size_t src_off;
	size_t dst_off;
	size_t tcp_off;
	uint8_t next;
	uint16_t source_port = 0;
	uint16_t destination_port = 0;
	uint8_t tcp_flags = 0;

	if (*logged >= 16 && *logged % 100 != 0)
		return;
	(*logged)++;
	if (len < NCM_IPV4_MIN)
		return;
	version = (uint8_t)(packet[0] >> 4);
	if (version == 4) {
		size_t header_len = (size_t)(packet[0] & 0x0fu) * 4u;
		if (header_len < NCM_IPV4_MIN || header_len > len)
			return;
		src_off = 12;
		dst_off = 16;
		next = packet[9];
		tcp_off = header_len;
		if (next == 6 && tcp_off + 20 <= len) {
			source_port = rd16_be(packet + tcp_off);
			destination_port = rd16_be(packet + tcp_off + 2);
			tcp_flags = packet[tcp_off + 13];
		}
	} else if (version == 6) {
		if (len < NCM_IPV6_MIN)
			return;
		src_off = 8;
		dst_off = 24;
		next = packet[6];
		tcp_off = NCM_IPV6_MIN;
		if (next == 6 && tcp_off + 20 <= len) {
			source_port = rd16_be(packet + tcp_off);
			destination_port = rd16_be(packet + tcp_off + 2);
			tcp_flags = packet[tcp_off + 13];
		}
	} else {
		return;
	}
	inet_ntop(version == 4 ? AF_INET : AF_INET6, packet + src_off, src,
		  sizeof(src));
	inet_ntop(version == 4 ? AF_INET : AF_INET6, packet + dst_off, dst,
		  sizeof(dst));
	if (next == 6) {
		iap2_log("ncm %s IPv%u tcp=%u->%u flags=0x%02x bytes=%zu %s -> %s",
			 direction, version, source_port, destination_port,
			 tcp_flags, len, src, dst);
	} else {
		iap2_log("ncm %s IPv%u next=%u bytes=%zu %s -> %s",
			 direction, version, next, len, src, dst);
	}
}

static void deliver_ethernet(const uint8_t *frame, size_t len)
{
	uint16_t ethertype;
	size_t payload_len;
	ssize_t n;
	int fd;

	if (len < NCM_ETH_HEADER) {
		g_bad_frames++;
		return;
	}
	ethertype = rd16_be(frame + 12);
	payload_len = len - NCM_ETH_HEADER;
	if ((ethertype != 0x0800 || payload_len < NCM_IPV4_MIN) &&
	    (ethertype != 0x86dd || payload_len < NCM_IPV6_MIN)) {
		if (g_bad_frames++ < 8)
			iap2_log("ncm: ignoring unsupported Ethernet frame bytes=%zu ethertype=0x%04x",
				 len, ethertype);
		return;
	}
	learn_peer_mac(frame, len);
	if (ethertype == 0x86dd)
		learn_peer_ipv6(frame + NCM_ETH_HEADER, payload_len);
	log_tun_packet("RX", frame + NCM_ETH_HEADER, payload_len);
	pthread_mutex_lock(&g_mu);
	fd = g_tun_fd;
	pthread_mutex_unlock(&g_mu);
	if (fd < 0)
		return;
	do {
		n = write(fd, frame + NCM_ETH_HEADER, payload_len);
	} while (n < 0 && (errno == EINTR || errno == EAGAIN) && g_running);
	if (n < 0)
		iap2_log("ncm: TUN write failed: %s", strerror(errno));
	else if ((size_t)n != payload_len)
		iap2_log("ncm: TUN short write %zd of %zu", n, payload_len);
}
static void process_ntb(const uint8_t *block, size_t len)
{
	uint16_t ndp_off;
	uint16_t ndp_len;
	int hops = 0;

	if (len < NCM_NTH16_BYTES || rd32_le(block) != NCM_NTH16_SIG) {
		g_bad_frames++;
		return;
	}
	ndp_off = rd16_le(block + 10);
	while (ndp_off && hops++ < 64) {
		size_t p = ndp_off;
		size_t end;
		uint16_t next;

		if (p + NCM_NDP16_BYTES > len ||
		    (rd32_le(block + p) & 0x00ffffffu) !=
		    (NCM_NDP16_SIG & 0x00ffffffu))
			break;
		ndp_len = rd16_le(block + p + 4);
		next = rd16_le(block + p + 6);
		end = p + ndp_len;
		if (end > len)
			end = len;
		for (p += 8; p + 4 <= end; p += 4) {
			uint16_t index = rd16_le(block + p);
			uint16_t length = rd16_le(block + p + 2);

			if (!index || !length)
				break;
			if ((size_t)index + length <= len)
				deliver_ethernet(block + index, length);
			else
				g_bad_frames++;
		}
		ndp_off = next;
	}
	g_rx_blocks++;
}

static void ntb_consume(uint8_t *buf, size_t *used)
{
	size_t off = 0;

	while (*used - off >= NCM_NTH16_BYTES) {
		uint16_t block_len;
		size_t start = off;

		while (start + 4 <= *used &&
		       rd32_le(buf + start) != NCM_NTH16_SIG)
			start++;
		if (start >= *used - 3)
			break;
		if (start > off) {
			memmove(buf, buf + start, *used - start);
			*used -= start;
			off = 0;
		}
		if (*used < NCM_NTH16_BYTES)
			break;
		block_len = rd16_le(buf + 8);
		if (block_len < NCM_MIN_BLOCK || block_len > *used) {
			if (block_len < NCM_MIN_BLOCK) {
				memmove(buf, buf + 1, *used - 1);
				(*used)--;
				continue;
			}
			break;
		}
		process_ntb(buf, block_len);
		memmove(buf, buf + block_len, *used - block_len);
		*used -= block_len;
		off = 0;
		/* A USB short-packet marker produced by the sender follows some blocks. */
		if (*used > 0 && buf[0] == 0 &&
		    (*used < 4 || rd32_le(buf) != NCM_NTH16_SIG)) {
			memmove(buf, buf + 1, *used - 1);
			(*used)--;
		}
	}
	if (off && off < *used) {
		memmove(buf, buf + off, *used - off);
		*used -= off;
	}
}

static void *usb_rx_main(void *arg)
{
	uint8_t *buf;
	size_t used = 0;
	int fd;
	unsigned long generation = (unsigned long)(uintptr_t)arg;

	buf = malloc(NCM_USB_BUFFER);
	if (!buf) {
		atomic_store(&g_usb_rx_exited, 1);
		return NULL;
	}
	pthread_mutex_lock(&g_mu);
	fd = g_usb_out_fd;
	pthread_mutex_unlock(&g_mu);
	iap2_log("ncm: USB OUT reader started fd=%d", fd);

	while (1) {
		ssize_t n;

		pthread_mutex_lock(&g_mu);
		if (!g_running || !g_usb_workers_running ||
		    g_usb_generation != generation || g_usb_out_fd != fd) {
			pthread_mutex_unlock(&g_mu);
			break;
		}
		pthread_mutex_unlock(&g_mu);
		if (used == NCM_USB_BUFFER) {
			iap2_log("ncm: incomplete NTB overflow, resetting");
			used = 0;
		}
		struct pollfd pfd;

		pfd.fd = fd;
		pfd.events = POLLIN;
		pfd.revents = 0;
		n = poll(&pfd, 1, 20);
		if (n < 0 && errno == EINTR)
			continue;
		if (n <= 0)
			continue;
		usb_watchdog_begin(&g_watchdog, USB_WATCHDOG_RX, ncm_now_ms());
		n = read(fd, buf + used, NCM_USB_BUFFER - used);
		usb_watchdog_end(&g_watchdog, USB_WATCHDOG_RX);
		if (n < 0 && (errno == EINTR || errno == EAGAIN))
			continue;
		if (n <= 0) {
			if (g_usb_rx_errors++ < 8)
				iap2_log("ncm: USB OUT read returned %zd: %s",
					 n, n == 0 ? "eof" : strerror(errno));
			usleep(20000);
			continue;
		}
		g_usb_rx_chunks++;
		usb_watchdog_progress(&g_watchdog, USB_WATCHDOG_RX);
		if (g_usb_rx_chunks <= 12 || g_usb_rx_chunks % 100 == 0)
			iap2_log("ncm: USB OUT read chunk=%zd used=%zu sig=%02x%02x%02x%02x",
				 n, used, n > 0 ? buf[used] : 0,
				 n > 1 ? buf[used + 1] : 0,
				 n > 2 ? buf[used + 2] : 0,
				 n > 3 ? buf[used + 3] : 0);
		used += (size_t)n;
		ntb_consume(buf, &used);
	}
	free(buf);
	iap2_log("ncm: USB OUT reader stopped blocks=%lu", g_rx_blocks);
	atomic_store(&g_usb_rx_exited, 1);
	return NULL;
}

static int build_ntb(uint8_t *out, size_t cap, const uint8_t *frame,
		     size_t frame_len, uint16_t sequence, size_t *out_len)
{
	size_t total = NCM_DATAGRAM_OFF + frame_len;

	if (!frame_len || total > 0xffffu || total > cap)
		return -1;
	memset(out, 0, NCM_DATAGRAM_OFF);
	wr32_le(out, NCM_NTH16_SIG);
	wr16_le(out + 4, NCM_NTH16_BYTES);
	wr16_le(out + 6, sequence);
	wr16_le(out + 8, (uint16_t)total);
	wr16_le(out + 10, NCM_NTH16_BYTES);
	wr32_le(out + 12, NCM_NDP16_SIG);
	wr16_le(out + 16, NCM_NDP16_BYTES);
	wr16_le(out + 18, 0);
	/* ndp16_entry is exactly { le16 wDatagramIndex; le16 wDatagramLength; }
	 * (CDC-NCM 1.0 section 3.3.1). Both fields are 16 bit and the pair
	 * repeats every 4 bytes until a {0,0} terminator. Widening the index to
	 * 32 bits pushes the length into the next entry's index slot and leaves
	 * entry[0].wDatagramLength == 0, which makes any host parser stop before
	 * the first datagram. */
	wr16_le(out + 20, NCM_DATAGRAM_OFF);
	wr16_le(out + 22, (uint16_t)frame_len);
	wr16_le(out + 24, 0);
	wr16_le(out + 26, 0);
	memcpy(out + NCM_DATAGRAM_OFF, frame, frame_len);
	*out_len = total;
	return 0;
}

/* No peer MAC learned yet. Sending to our own MAC (the old behaviour) makes
 * the head unit's NIC drop the frame before its IP stack can answer, which
 * looks exactly like a dead peer. Broadcast is accepted by any NIC, so the
 * first unicast attempt still reaches the head unit while its own multicast
 * frames (router solicitation / MLD report) teach us the real address. */
static void fill_unknown_dest(uint8_t *dest)
{
	static int logged;

	memset(dest, 0xff, 6);
	if (!logged) {
		logged = 1;
		iap2_log("ncm: no peer MAC learned yet; unicast frames use broadcast destination");
	}
}

static size_t make_ethernet(uint8_t *frame, size_t cap, const uint8_t *packet,
			    size_t len)
{
	uint8_t dest[6];
	uint8_t version;
	uint16_t ethertype;

	if (len < NCM_IPV4_MIN || NCM_ETH_HEADER + len > cap)
		return 0;
	version = (uint8_t)(packet[0] >> 4);
	if (version == 4) {
		if (len < NCM_IPV4_MIN)
			return 0;
		ethertype = 0x0800;
		if ((packet[16] & 0xf0u) == 0xe0u) {
			dest[0] = 0x01;
			dest[1] = 0x00;
			dest[2] = 0x5e;
			dest[3] = packet[17] & 0x7f;
			dest[4] = packet[18];
			dest[5] = packet[19];
		} else if (packet[16] == 255 && packet[17] == 255 &&
			   packet[18] == 255 && packet[19] == 255) {
			memset(dest, 0xff, sizeof(dest));
		} else if (g_peer_mac_valid) {
			memcpy(dest, g_peer_mac, sizeof(dest));
		} else {
			fill_unknown_dest(dest);
		}
	} else if (version == 6) {
		if (len < NCM_IPV6_MIN)
			return 0;
		ethertype = 0x86dd;
		if (packet[24] == 0xff) {
			dest[0] = 0x33;
			dest[1] = 0x33;
			memcpy(dest + 2, packet + 36, 4);
		} else if (g_peer_mac_valid) {
			memcpy(dest, g_peer_mac, sizeof(dest));
		} else {
			fill_unknown_dest(dest);
		}
	} else {
		return 0;
	}
	memcpy(frame, dest, sizeof(dest));
	memcpy(frame + 6, ncm_source_mac, sizeof(dest));
	frame[12] = (uint8_t)(ethertype >> 8);
	frame[13] = (uint8_t)ethertype;
	memcpy(frame + NCM_ETH_HEADER, packet, len);
	return NCM_ETH_HEADER + len;
}
static void *usb_tx_main(void *arg)
{
	uint8_t packet[65536];
	uint8_t frame[NCM_MAX_FRAME];
	uint8_t block[NCM_MAX_FRAME];
	int fd;
	unsigned long failures = 0;
	unsigned long generation = (unsigned long)(uintptr_t)arg;

	pthread_mutex_lock(&g_mu);
	fd = g_usb_in_fd;
	pthread_mutex_unlock(&g_mu);
	iap2_log("ncm: TUN/USB IN writer started fd=%d", fd);

	while (1) {
		size_t frame_len;
		size_t block_len;
		ssize_t n;
		int tun_fd;

		pthread_mutex_lock(&g_mu);
		if (!g_running || !g_usb_workers_running ||
		    g_usb_generation != generation || g_usb_in_fd != fd) {
			pthread_mutex_unlock(&g_mu);
			break;
		}
		tun_fd = g_tun_fd;
		pthread_mutex_unlock(&g_mu);
		if (tun_fd < 0) {
			usleep(20000);
			continue;
		}
		n = read(tun_fd, packet, sizeof(packet));
		if (n < 0 && (errno == EINTR || errno == EAGAIN)) {
			usleep(2000);
			continue;
		}
		if (n <= 0) {
			iap2_log("ncm: TUN read stopped: %s",
				 n == 0 ? "eof" : strerror(errno));
			usleep(20000);
			continue;
		}
		log_tun_packet("TX", packet, (size_t)n);
		frame_len = make_ethernet(frame, sizeof(frame), packet, (size_t)n);
		if (!frame_len)
			continue;
		if (build_ntb(block, sizeof(block), frame, frame_len,
			      g_ntb_sequence++, &block_len) < 0)
			continue;
		usb_watchdog_begin(&g_watchdog, USB_WATCHDOG_TX, ncm_now_ms());
		n = write(fd, block, block_len);
		usb_watchdog_end(&g_watchdog, USB_WATCHDOG_TX);
		if (n < 0) {
			if (errno == EINTR || errno == EAGAIN) {
				usleep(2000);
				continue;
			}
			if (failures++ < 12)
				iap2_log("ncm: USB IN write failed: %s",
					 strerror(errno));
			usleep(20000);
			continue;
		}
		if ((size_t)n != block_len) {
			if (failures++ < 12)
				iap2_log("ncm: USB IN short write %zd of %zu",
					 n, block_len);
			continue;
		}
		failures = 0;
		usb_watchdog_progress(&g_watchdog, USB_WATCHDOG_TX);
		g_tx_frames++;
		if (g_tx_frames <= 12 || g_tx_frames % 100 == 0)
			iap2_log("ncm: TX frame=%zu ntb=%zu payload=%zu seq=%u",
				 frame_len, block_len, block_len - NCM_DATAGRAM_OFF,
				 (unsigned)(g_ntb_sequence - 1));
	}
	iap2_log("ncm: TUN/USB IN writer stopped frames=%lu", g_tx_frames);
	atomic_store(&g_usb_tx_exited, 1);
	return NULL;
}

static void usb_worker_join(pthread_t thread, atomic_int *exited)
{
	/* RK3399 FunctionFS synchronous I/O can wait for a USB transfer despite
	 * O_NONBLOCK. SIGUSR1 uses the daemon's non-restarting wakeup handler.
	 * Repeat until exit to cover a signal arriving just before read/write. */
	while (!atomic_load(exited)) {
		(void)pthread_kill(thread, SIGUSR1);
		usleep(10000);
	}
	(void)pthread_join(thread, NULL);
}

static void *ncm_watchdog_main(void *arg)
{
	(void)arg;
	while (atomic_load(&g_watchdog_running)) {
		unsigned actions = usb_watchdog_tick(&g_watchdog, ncm_now_ms());

		if (actions & USB_WATCHDOG_ACTION_WAKE(USB_WATCHDOG_TX)) {
			iap2_log("ncm: USB IN write still blocked after %ums; signalling writer thread",
				 (unsigned)g_watchdog.policy.wake_ms);
			(void)pthread_kill(g_usb_tx_thread, SIGUSR1);
		}
		if (actions & USB_WATCHDOG_ACTION_WAKE(USB_WATCHDOG_RX)) {
			iap2_log("ncm: USB OUT read still blocked after %ums; signalling reader thread",
				 (unsigned)g_watchdog.policy.wake_ms);
			(void)pthread_kill(g_usb_rx_thread, SIGUSR1);
		}
		if (actions & USB_WATCHDOG_ACTION_REBIND) {
			struct usb_watchdog_rebind_report report;
			int rc;

			iap2_log("ncm: endpoint stall persisted %ums; re-enumerating UDC %s",
				 (unsigned)g_watchdog.policy.rebind_ms,
				 NCM_UDC_PATH);
			rc = usb_watchdog_rebind_udc(NCM_UDC_PATH, NCM_UDC_SETTLE_MS,
						     &report);
			if (rc == 0)
				iap2_log("ncm: UDC re-enumerated udc=%s unbind=%d rebind=%d",
					 report.udc, report.unbound,
					 report.rebound);
			else
				iap2_log("ncm: UDC re-enumeration failed path=%s; will retry after cooldown",
					 NCM_UDC_PATH);
		}
		usleep(NCM_WATCHDOG_POLL_MS * 1000u);
	}
	return NULL;
}

static int ncm_watchdog_start(void)
{
	struct usb_watchdog_policy policy;

	if (g_watchdog_started)
		return 0;
	policy.wake_ms = NCM_WATCHDOG_WAKE_MS;
	policy.rebind_ms = NCM_WATCHDOG_REBIND_MS;
	policy.cooldown_ms = NCM_WATCHDOG_COOLDOWN_MS;
	usb_watchdog_init(&g_watchdog, &policy);
	atomic_store(&g_watchdog_running, 1);
	if (pthread_create(&g_watchdog_thread, NULL, ncm_watchdog_main, NULL) !=
	    0) {
		atomic_store(&g_watchdog_running, 0);
		return -1;
	}
	g_watchdog_started = 1;
	iap2_log("ncm: USB endpoint watchdog started wake=%ums rebind=%ums cooldown=%ums",
		 NCM_WATCHDOG_WAKE_MS, NCM_WATCHDOG_REBIND_MS,
		 NCM_WATCHDOG_COOLDOWN_MS);
	return 0;
}

static void ncm_watchdog_stop(void)
{
	if (!g_watchdog_started)
		return;
	atomic_store(&g_watchdog_running, 0);
	(void)pthread_join(g_watchdog_thread, NULL);
	g_watchdog_started = 0;
}

static int connect_io_owned(void)
{
	int current;
	pthread_mutex_lock(&g_mu);
	current = g_connect_request && connect_owned_locked(g_connect_request);
	pthread_mutex_unlock(&g_mu);
	return current;
}

static int connect_wait(int fd, int timeout_ms)
{
	struct pollfd pfd;
	int elapsed = 0;

	pfd.fd = fd;
	pfd.events = POLLOUT;
	pfd.revents = 0;
	while (elapsed < timeout_ms) {
		int wait_ms = timeout_ms - elapsed;
		int n;
		int error = 0;
		socklen_t error_len = sizeof(error);

		if (!connect_io_owned()) {
			errno = ECANCELED;
			return -1;
		}
		if (wait_ms > (int)AIRPLAY_RECOVERY_POLL_MS)
			wait_ms = AIRPLAY_RECOVERY_POLL_MS;
		pfd.revents = 0;
		n = poll(&pfd, 1, wait_ms);
		elapsed += wait_ms;
		if (n < 0 && errno == EINTR)
			continue;
		if (n < 0)
			return -1;
		if (n == 0)
			continue;
		if (getsockopt(fd, SOL_SOCKET, SO_ERROR, &error, &error_len) < 0)
			return -1;
		if (error) {
			errno = error;
			return -1;
		}
		return 0;
	}
	errno = ETIMEDOUT;
	return -1;
}

/*
 * Is this address on the point-to-point link the head unit gives us?
 *
 * It matters because of what we do to the socket: on that link the only
 * usable source is our own 169.254.0.1 / fe80::1 on the NCM interface, and
 * binding wrongly makes every connect fail with an error that blames the
 * peer.  Anything else -- a routable address a head unit might advertise, or
 * a bench endpoint on the LAN -- has to go out through the normal route, so
 * SO_BINDTODEVICE and the source bind must be skipped rather than attempted.
 */
static int numeric_address(const char *address, char *literal, size_t capacity,
			   unsigned *scope)
{
	char buffer[INET6_ADDRSTRLEN + IFNAMSIZ + 4];
	char *zone;
	struct in_addr v4;
	struct in6_addr v6;
	size_t length;

	if (!address || (length = strlen(address)) >= sizeof(buffer))
		return 0;
	memcpy(buffer, address, length + 1);
	if (buffer[0] == '[') {
		if (length < 3 || buffer[length - 1] != ']')
			return 0;
		buffer[length - 1] = '\0';
		memmove(buffer, buffer + 1, length - 1);
	}
	*scope = 0;
	zone = strchr(buffer, '%');
	if (zone) {
		char *end;
		unsigned long index;
		*zone++ = '\0';
		if (!*zone)
			return 0;
		index = strtoul(zone, &end, 10);
		*scope = *end ? if_nametoindex(zone) :
			(index > 0 && index <= UINT32_MAX ? (unsigned)index : 0);
		if (!*scope)
			return 0;
	}
	if (strlen(buffer) >= capacity)
		return 0;
	strcpy(literal, buffer);
	if (!zone && inet_pton(AF_INET, literal, &v4) == 1)
		return AF_INET;
	return inet_pton(AF_INET6, literal, &v6) == 1 ? AF_INET6 : 0;
}

static int address_is_link_local(const char *address, int want_ipv4)
{
	char literal[INET6_ADDRSTRLEN];
	unsigned scope;
	if (numeric_address(address, literal, sizeof(literal), &scope) !=
	    (want_ipv4 ? AF_INET : AF_INET6))
		return 0;
	if (want_ipv4) {
		struct in_addr v4;

		if (inet_pton(AF_INET, literal, &v4) != 1)
			return 0;
		return (ntohl(v4.s_addr) & 0xFFFF0000u) == 0xA9FE0000u;
	}
	{
		struct in6_addr v6;

		if (inet_pton(AF_INET6, literal, &v6) != 1)
			return 0;
		return v6.s6_addr[0] == 0xfe &&
		       (v6.s6_addr[1] & 0xc0) == 0x80;
	}
}

static int connect_one(const char *address, uint16_t port, int want_ipv4,
		       char *resolved, size_t resolved_size, int verbose,
		       int restrict_to_ncm)
{
	struct sockaddr_storage target;
	struct sockaddr_storage source;
	socklen_t target_len = 0;
	socklen_t source_len = 0;
	struct timeval timeout;
	char interface_name[IFNAMSIZ];
	char literal[INET6_ADDRSTRLEN];
	unsigned scope;
	int on_ncm_link;
	int fd = -1;
	int flags;
	int rc;

	interface_name[0] = '\0';
	if (!port || numeric_address(address, literal, sizeof(literal), &scope) !=
	    (want_ipv4 ? AF_INET : AF_INET6) ||
	    (scope && scope != (unsigned)g_tun_index)) {
		errno = EINVAL;
		return -1;
	}
	on_ncm_link = address_is_link_local(literal, want_ipv4);

	memset(&target, 0, sizeof(target));
	memset(&source, 0, sizeof(source));
	if (want_ipv4) {
		struct sockaddr_in *target4 = (struct sockaddr_in *)&target;
		struct sockaddr_in *source4 = (struct sockaddr_in *)&source;

		target4->sin_family = AF_INET;
		target4->sin_port = htons(port);
		source4->sin_family = AF_INET;
		if (inet_pton(AF_INET, literal, &target4->sin_addr) != 1 ||
		    inet_pton(AF_INET, "169.254.0.1", &source4->sin_addr) != 1) {
			errno = EINVAL;
			return -1;
		}
		target_len = sizeof(*target4);
		source_len = sizeof(*source4);
	} else {
		struct sockaddr_in6 *target6 = (struct sockaddr_in6 *)&target;
		struct sockaddr_in6 *source6 = (struct sockaddr_in6 *)&source;
		target6->sin6_family = AF_INET6;
		target6->sin6_port = htons(port);
		target6->sin6_scope_id = on_ncm_link ? (unsigned)g_tun_index : 0;
		source6->sin6_family = AF_INET6;
		source6->sin6_scope_id = (unsigned)g_tun_index;
		if (inet_pton(AF_INET6, literal, &target6->sin6_addr) != 1 ||
		    inet_pton(AF_INET6, "fe80::1", &source6->sin6_addr) != 1) {
			errno = EINVAL;
			return -1;
		}
		target_len = sizeof(*target6);
		source_len = sizeof(*source6);
	}

	if (on_ncm_link && !want_ipv4) {
		int state = ipv6_source_state(g_tun_index);

		if (state <= 0) {
			int saved_errno = errno;
			if (verbose) {
				if (state == 0)
					iap2_log("ncm: waiting for IPv6 source fe80::1 on NCM interface (address absent or DAD pending)");
				else
					iap2_log("ncm: IPv6 source unavailable: %s", strerror(saved_errno));
			}
			errno = saved_errno;
			return -1;
		}
		/* DAD may have completed since the previous route refresh. Install
		 * the IPv6 source route before the first usable connection attempt. */
		if (ensure_airplay_policy_routes("IPv6 source ready") < 0)
			return -1;
	}
	fd = socket(want_ipv4 ? AF_INET : AF_INET6,
		    SOCK_STREAM | SOCK_CLOEXEC, 0);
	if (fd < 0)
		return -1;
	if (on_ncm_link || restrict_to_ncm) {
		if (!if_indextoname((unsigned)g_tun_index, interface_name)) {
			iap2_log("ncm: cannot resolve TUN name for index %d: %s",
				 g_tun_index, strerror(errno));
			close(fd);
			return -1;
		}
		if (setsockopt(fd, SOL_SOCKET, SO_BINDTODEVICE, interface_name,
			       strlen(interface_name) + 1) < 0) {
			iap2_log("ncm: bind endpoint socket to %s failed: %s",
				 interface_name, strerror(errno));
			close(fd);
			return -1;
		}
		if (on_ncm_link && bind(fd, (struct sockaddr *)&source, source_len) < 0) {
			iap2_log("ncm: bind AirPlay source %s failed: %s",
				 want_ipv4 ? "169.254.0.1" : "fe80::1",
				 strerror(errno));
			close(fd);
			return -1;
		}
	} else if (verbose) {
		iap2_log("ncm: %s is not on the NCM link; connecting through the normal route",
			 address);
	}
	flags = fcntl(fd, F_GETFL, 0);
	if (flags < 0 || fcntl(fd, F_SETFL, flags | O_NONBLOCK) < 0) {
		close(fd);
		return -1;
	}
	timeout.tv_sec = 0;
	timeout.tv_usec = 0;
	setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout));

	if (want_ipv4 || !on_ncm_link) {
		snprintf(resolved, resolved_size, "%s", literal);
	} else {
		snprintf(resolved, resolved_size, "[%s%%%s]", literal,
			 interface_name);
	}
	if (verbose)
		iap2_log("ncm: AirPlay TCP connecting family=IPv%u endpoint=%s port=%u",
			 want_ipv4 ? 4u : 6u, resolved, port);
	rc = connect(fd, (struct sockaddr *)&target, target_len);
	if (rc < 0 && errno != EINPROGRESS) {
		if (verbose)
			iap2_log("ncm: AirPlay TCP connect %s failed: %s",
				 resolved, strerror(errno));
		close(fd);
		return -1;
	}
	if (rc < 0 && connect_wait(fd, 5000) < 0) {
		if (verbose && errno != ECANCELED)
			iap2_log("ncm: AirPlay TCP connect %s failed: %s",
				 resolved, strerror(errno));
		close(fd);
		return -1;
	}
	return fd;
}

static int address_is_ipv4(const char *address);

static int connect_advertised_port(const struct ncm_connect_request *request,
				   uint16_t port,
				   char *resolved, size_t resolved_size)
{
	int pass;

	ensure_airplay_policy_routes("advertised endpoint connection");

	for (pass = 0; pass < 2 && connect_io_owned(); pass++) {
		size_t i;
		int want_ipv4 = pass == 0;

		for (i = 0; i < request->count && connect_io_owned(); i++) {
			int fd;

			if (!request->addresses[i] || !*request->addresses[i] ||
			    address_is_ipv4(request->addresses[i]) != want_ipv4)
				continue;
			fd = connect_one(request->addresses[i], port, want_ipv4,
					 resolved, resolved_size, 1, request->discovered);
			if (fd >= 0)
				return fd;
		}
	}
	/* Some DiPlay builds keep advertising fe80::2 in 0x4301 while the
	 * NCM interface uses a privacy link-local address. Try the actual peer
	 * learned from NCM traffic only after the protocol endpoint fails. */
	if (connect_io_owned()) {
		char address[INET6_ADDRSTRLEN];

		if (copy_learned_peer_ipv6(address, sizeof(address))) {
			int fd = connect_one(address, port, 0, resolved, resolved_size, 1,
					     request->discovered);
			if (fd >= 0) {
				iap2_log("ncm: AirPlay endpoint fallback connected to learned peer=%s", address);
				return fd;
			}
		}
	}
	return -1;
}
static int address_is_ipv4(const char *address)
{
	struct in_addr ignored;

	return inet_pton(AF_INET, address, &ignored) == 1;
}

static void publish_airplay_status(struct airplay_client *client,
				   const struct ncm_connect_request *request)
{
	struct ncm_airplay_status status;
	struct airplay_runtime_status runtime;
	struct carplay_audio_status audio;
	const struct carplay_session_capabilities *caps;
	memset(&status, 0, sizeof(status));
	memset(&runtime, 0, sizeof(runtime));
	memset(&audio, 0, sizeof(audio));
	if (client) {
		airplay_client_get_runtime_status(client, &runtime);
		airplay_client_get_audio_status(client, &audio);
		caps = airplay_client_capabilities(client);
		status.active = 1;
		status.info_complete = caps && caps->display.present ? 1 : 0;
		if (caps) {
			status.display_width = runtime.display_width > 0 ?
				runtime.display_width : caps->display.width_pixels;
			status.display_height = runtime.display_height > 0 ?
				runtime.display_height : caps->display.height_pixels;
			status.touch = caps->has_touch;
			status.keyboard = caps->has_keyboard;
			status.oem_icon_present = caps->oem_icon_present;
			status.request_car_ui_supported =
				caps->request_car_ui_supported;
		}
		status.car_ui_ready = airplay_client_car_ui_available(client);
		status.requested_width = runtime.requested_width;
		status.requested_height = runtime.requested_height;
		status.physical_width = runtime.physical_width;
		status.physical_height = runtime.physical_height;
		status.display_density = runtime.display_density;
		status.requested_fps = runtime.requested_fps;
		status.actual_fps = runtime.actual_fps;
		status.native_size = runtime.native_size;
		snprintf(status.display_error, sizeof(status.display_error), "%s",
			 runtime.display_error);
		status.audio_streams = airplay_client_audio_stream_count(client);
		status.media_audio = audio.media_active;
		status.microphone = audio.microphone_active;
		status.media_available = audio.media_available;
		status.microphone_available = audio.microphone_available;
		status.microphone_ready = audio.microphone_ready;
		status.microphone_demand = audio.microphone_demand;
		status.microphone_packets_received = audio.microphone_packets_received;
		status.streaming = airplay_client_record_accepted(client);
	}
	pthread_mutex_lock(&g_mu);
	if (!client || connect_owned_locked(request))
		g_airplay_status = status;
	pthread_mutex_unlock(&g_mu);
}

/*
 * Whether an AirPlay transport can exist at this instant.
 *
 * g_running keeps its original meaning: "our USB worker threads are running",
 * which is how the TUN layout knows its byte path is alive. With the kernel
 * owning NCM there are no workers at all, so g_running never becomes 1 and
 * every AirPlay connect was deferred until a "NCM restart" that this mode never
 * performs (ncm_bridge_start() returns early before its resume block). The
 * result was a session that reached 0x4301 CarPlayStartSession and then froze:
 * "deferred AirPlay TCP endpoint connect until NCM restart".
 *
 * In kernel mode the byte path is the kernel's netdev, whose index is cached in
 * g_tun_index once we have applied the addresses. That is the readiness signal.
 * Callers hold g_mu.
 */
static int transport_ready_locked(void)
{
	if (g_connect_stop || g_connect_retiring || g_transport_restarting)
		return 0;
	if (g_running)
		return 1;
	if (!g_kernel_ncm && !g_host_netdev_ready)
		return 0;
	return g_tun_index > 0;
}

/* Physical readiness and current-session authorization are independent. */
static int connect_ready_locked(void)
{
	return transport_ready_locked() &&
		(!g_host_auth_required ||
		 (g_host_netdev_ready && g_host_authenticated));
}

int ncm_bridge_host_authentication(int authenticated)
{
	int revoked;
	pthread_mutex_lock(&g_mu);
	revoked = g_host_authenticated && !authenticated;
	g_host_auth_required = 1;
	if (g_host_authenticated != !!authenticated)
		iap2_log("ncm: Host AirPlay authorization=%d netdev_ready=%d",
			 !!authenticated, g_host_netdev_ready);
	g_host_authenticated = !!authenticated;
	if (revoked) {
		/* The owner must end this USB round and join workers before reuse. */
		g_transport_restart_epoch++;
		g_airplay_transport_restart_pending = 0;
		g_connect_stop = 1;
		if (g_connect_sock >= 0)
			shutdown(g_connect_sock, SHUT_RDWR);
		free_connect_request(g_pending_connect_request);
		g_pending_connect_request = NULL;
	} else if (connect_ready_locked() && !g_connect_thread_running &&
		   g_pending_connect_request) {
		struct ncm_connect_request *pending = g_pending_connect_request;
		g_pending_connect_request = NULL;
		if (launch_connect_request_locked(pending) < 0)
			g_pending_connect_request = pending;
	}
	pthread_mutex_unlock(&g_mu);
	return revoked;
}

/* Caller holds g_mu. A lease is captured before the thread is launched so
 * a late worker cannot adopt the USB generation that replaced it. */
static int connect_owned_locked(const struct ncm_connect_request *request)
{
	const struct ncm_connect_owner *owner = &request->owner;

	return g_connect_request == request && connect_ready_locked() &&
		!g_airplay_transport_restart_pending && !g_pending_connect_request &&
		g_transport_restart_epoch == owner->restart_epoch &&
		g_usb_generation == owner->usb_generation &&
		g_tun_index == owner->tun_index &&
		g_kernel_ncm == owner->kernel_ncm &&
		g_host_netdev_ready == owner->host_netdev_ready &&
		g_vehicle_state == request->vehicle_state &&
		g_vehicle_generation == request->vehicle_generation;
}

static int connect_owned(const struct ncm_connect_request *request)
{
	int current;
	pthread_mutex_lock(&g_mu);
	current = connect_owned_locked(request);
	pthread_mutex_unlock(&g_mu);
	return current;
}

static int wait_connect_retry(const struct ncm_connect_request *request,
			      unsigned remaining_us)
{
	while (remaining_us && connect_owned(request)) {
		unsigned slice_us = AIRPLAY_RECOVERY_POLL_MS * 1000u;
		if (slice_us > remaining_us)
			slice_us = remaining_us;
		if (usleep(slice_us) == 0 || errno != EINTR)
			remaining_us -= slice_us;
	}
	return connect_owned(request);
}

/* No RTSP socket remains open here. The connector owns this bounded wait;
 * stop is observed every 100ms and never kills the switch/rollback process. */
static int wait_display_preparation(const struct ncm_connect_request *request,
	const char *request_id, const struct carplay_display_plan *plan)
{
	uint64_t deadline = ncm_now_ms() + DISPLAY_PREPARATION_TIMEOUT_MS;
	unsigned slices = 0;
	for (;;) {
		int result;
		if (!connect_owned(request)) {
			iap2_log("ncm: display preparation owner changed/stopped; detached transaction retained id=%s", request_id);
			return -1;
		}
		/* Slice count also bounds the wait if a monotonic clock read fails. */
		if (ncm_now_ms() >= deadline ||
		    slices >= DISPLAY_PREPARATION_TIMEOUT_MS / AIRPLAY_RECOVERY_POLL_MS) {
			iap2_log("ncm: display preparation timed out id=%s", request_id);
			return -1;
		}
		result = carplay_display_preparation_result(request_id,
			plan->physical_width, plan->physical_height, plan->physical_fps);
		if (result) {
			if (result < 0) {
				iap2_log("ncm: display preparation failed id=%s", request_id);
				return -1;
			}
			return connect_owned(request) ? 1 : -1;
		}
		usleep(AIRPLAY_RECOVERY_POLL_MS * 1000u);
		slices++;
	}
}

static void *connect_endpoint_main(void *arg)
{
	struct ncm_connect_request *request = arg;
	char resolved[INET6_ADDRSTRLEN + IFNAMSIZ + 4];
	unsigned int attempt;
	int fd = -1;
	int event_fd = -1;
	int screen_fd = -1;
	int record_sent;
	int screen_setup_sent;
	int live_started;
	int fallback_used = 0;
	int fallback_requested;
	int preparation_retry_used = 0;
	int preparation_requested;
	struct carplay_display_plan preparation_plan = {0};
	static const unsigned recovery_delays_s[] = {1, 2, 4, 8, 15};
	unsigned recovery_delay_index = 0;
	unsigned long recovery_waits = 0;
	int recovering = 0;
	int screen_attached;
	int video_established;
	uint64_t established_ms;
	unsigned int attempt_limit;
	char *selected_address = NULL;
	struct ncm_connect_request selected = *request;
	enum airplay_profile profile = request->profile;
	struct airplay_client *airplay = NULL;

connect_profile:
	record_sent = screen_setup_sent = screen_attached = live_started = 0;
	video_established = 0;
	established_ms = 0;
	attempt_limit = recovering ? 1 : AIRPLAY_CONNECT_RETRY_ATTEMPTS;
	for (attempt = 0; attempt < attempt_limit && fd < 0 &&
	     connect_owned(request); attempt++) {
		int pass;
		int verbose = recovering ? carplay_log_should_sample(recovery_waits) :
			attempt == 0 || attempt % 10 == 0;

		if (attempt == 0 || verbose)
			ensure_airplay_policy_routes("AirPlay reconnect");
		for (pass = 0; pass < 2 && fd < 0; pass++) {
			size_t i;
			int want_ipv4 = pass == 0;

			for (i = 0; i < request->count && fd < 0; i++) {
				if (!request->addresses[i] || !*request->addresses[i])
					continue;
				if ((recovering || fallback_used || preparation_retry_used) &&
				    request->addresses[i] != selected_address)
					continue;
				if (address_is_ipv4(request->addresses[i]) != want_ipv4)
					continue;
				if (!connect_owned(request))
					goto out;
				fd = connect_one(request->addresses[i], request->port,
						 want_ipv4, resolved, sizeof(resolved),
						 verbose, request->discovered);
				if (fd >= 0)
					selected_address = request->addresses[i];
			}
		}
		if (fd < 0 && attempt + 1 < attempt_limit &&
		    !wait_connect_retry(request, AIRPLAY_CONNECT_RETRY_DELAY_US))
			goto out;
	}
	if (fd < 0) {
		if (!recovering && connect_owned(request)) {
			char learned[INET6_ADDRSTRLEN];

			/* DiPlay serves AirPlay on the tunnel address it advertises, so a
			 * learned peer that differs from it is only diagnostic: it tells
			 * whether the accessory answered anything on the NCM link at all. */
			if (copy_learned_peer_ipv6(learned, sizeof(learned)))
				iap2_log("ncm: AirPlay TCP failed for every advertised endpoint (learned peer=%s never answered port=%u)",
					 learned, request->port);
			else
				iap2_log("ncm: AirPlay TCP failed for every advertised endpoint (no peer address learned from NCM traffic)");
		}
		goto recover;
	}
	pthread_mutex_lock(&g_mu);
	if (!connect_owned_locked(request)) {
		pthread_mutex_unlock(&g_mu);
		close(fd);
		fd = -1;
		goto out;
	}
	request->selected_address = selected_address;
	g_connect_sock = fd;
	pthread_mutex_unlock(&g_mu);
	selected.addresses = &selected_address;
	selected.count = 1;
	iap2_log("ncm: AirPlay TCP CONNECTED endpoint=%s port=%u selected_by=IPv4-first",
		 resolved, request->port);
	airplay = airplay_client_new_with_audio(fd, request->media_audio,
						 request->microphone);
	if (airplay)
		airplay_client_set_profile(airplay, profile);
	if (airplay)
		airplay_client_set_video_options(airplay, request->display_width,
					 request->display_height,
					 request->physical_width,
					 request->physical_height,
					 request->display_density,
					 request->video_fps,
					 request->video_bitrate);
	pthread_mutex_lock(&g_mu);
	if (!connect_owned_locked(request)) {
		pthread_mutex_unlock(&g_mu);
		goto cleanup_airplay;
	}
	if (airplay && request->vehicle_state)
		airplay_client_set_vehicle_state(airplay, request->vehicle_state,
						 request->vehicle_generation);
	g_active_airplay = airplay;
	pthread_mutex_unlock(&g_mu);
	publish_airplay_status(airplay, request);
	if (!airplay || airplay_client_start(airplay) < 0) {
		iap2_log("ncm: failed to start AirPlay control client");
		goto cleanup_airplay;
	}
	/* Preserve a usable live kernel socket across USB re-enable until a new
	 * endpoint retires it. A stale owner cannot finish setup or recover. */
	while (!connect_cancelled() && (live_started || connect_owned(request))) {
		/* Tick runs even when poll times out: silent handshakes and
		 * pending feedback requests must not retain the socket forever. */
		if (airplay_client_tick(airplay) < 0)
			break;
		publish_airplay_status(airplay, request);
		/*
		 * A refused handshake leaves nothing queued: the ladder only advances
		 * from the successful arm of the previous step.  Without this the
		 * session keeps a live socket, polls for 200 ms at a time and never
		 * sends another byte -- which reads as "the car went quiet" when the
		 * car in fact answered.
		 */
		if (airplay_client_failed(airplay)) {
			iap2_log("ncm: AirPlay session aborted: %s",
				 airplay_client_failure(airplay));
			break;
		}
		struct pollfd pfd[3] = {
			{ .fd = fd, .events = POLLIN },
			{ .fd = event_fd, .events = POLLIN },
			{ .fd = screen_fd, .events = POLLIN }
		};
		nfds_t poll_count = screen_fd >= 0 ? 3 :
				    (event_fd >= 0 ? 2 : 1);
		char data[16384];
		ssize_t n = poll(pfd, poll_count, live_started ? 200 : AIRPLAY_RECOVERY_POLL_MS);
		size_t i;
		if (n < 0 && errno != EINTR)
			break;

		if (n > 0) {
			for (i = 0; i < poll_count; i++) {
				const char *channel = i == 0 ? "control TCP" :
					i == 1 ? "event TCP" : "screen data TCP";
				if (!(pfd[i].revents &
				      (POLLIN | POLLHUP | POLLERR)))
					continue;
				n = recv(pfd[i].fd, data, sizeof(data),
					 MSG_DONTWAIT);
				if (n > 0) {
					if (i == 0)
						airplay_client_feed(
							airplay,
							(const uint8_t *)data,
							(size_t)n);
					else if (i == 1)
						airplay_client_feed_event(
							airplay,
							(const uint8_t *)data,
							(size_t)n);
					else {
						g_screen_data_rx_bytes += (uint64_t)n;
						if (carplay_log_should_sample(
							    ++g_screen_data_rx_chunks))
							iap2_log("ncm: AirPlay screen data rx bytes=%zd chunks=%lu total_bytes=%llu",
								 n,
								 g_screen_data_rx_chunks,
								 (unsigned long long)g_screen_data_rx_bytes);
					}
					continue;
				}
				if (n == 0) {
					iap2_log("ncm: AirPlay %s closed by peer",
						 channel);
					goto airplay_loop_done;
				}
				if (errno != EAGAIN && errno != EINTR) {
					iap2_log("ncm: AirPlay %s recv failed: %s",
						 channel, strerror(errno));
					goto airplay_loop_done;
				}
			}
		}
		if (connect_cancelled() || (!live_started && !connect_owned(request)) ||
		    airplay_client_failed(airplay))
			break;

		if (event_fd < 0 &&
		    airplay_client_setup_complete(airplay)) {
			uint16_t event_port = airplay_client_event_port(airplay);

			event_fd = connect_advertised_port(
				&selected, event_port, resolved, sizeof(resolved));
			if (event_fd < 0) {
				iap2_log("ncm: AirPlay event connect failed port=%u",
					 event_port);
				break;
			}
			iap2_log("ncm: AirPlay event CONNECTED endpoint=%s port=%u selected_by=IPv4-first",
				 resolved, event_port);
			pthread_mutex_lock(&g_mu);
			int attached = connect_owned_locked(request) ?
				airplay_client_attach_event(airplay, event_fd) : -1;
			pthread_mutex_unlock(&g_mu);
			if (attached < 0) {
				iap2_log("ncm: failed to attach AirPlay event channel");
				break;
			}
			if (profile == AIRPLAY_PROFILE_LEGACY) {
				if (airplay_client_send_record(airplay) < 0)
					break;
				record_sent = 1;
			}
		}
		if (!screen_setup_sent && event_fd >= 0 &&
		    (profile == AIRPLAY_PROFILE_HAP || airplay_client_record_accepted(airplay))) {
			int setup_result = airplay_client_send_screen_setup(airplay);
			if (setup_result < 0)
				break;
			/* Positive means the receiver currently owns the screen. Retry
			 * after modes/Unborrow is processed instead of consuming SETUP. */
			if (setup_result == 0)
				screen_setup_sent = 1;
		}

		if (screen_fd < 0 &&
		    airplay_client_screen_setup_complete(airplay)) {
			uint16_t data_port =
				airplay_client_screen_data_port(airplay);

			/* Legacy accepts RECORD before stream SETUP. A subsequent
			 * screen TCP failure must still recover this negotiated stream. */
			if (airplay_client_record_accepted(airplay)) {
				video_established = 1;
				established_ms = ncm_now_ms();
			}
			screen_fd = connect_advertised_port(
				&selected, data_port, resolved, sizeof(resolved));
			if (screen_fd < 0) {
				iap2_log("ncm: AirPlay screen data connect failed port=%u",
					 data_port);
				break;
			}
			iap2_log("ncm: AirPlay screen data CONNECTED endpoint=%s port=%u selected_by=IPv4-first",
				 resolved, data_port);
			pthread_mutex_lock(&g_mu);
			screen_attached = connect_owned_locked(request) &&
				airplay_client_attach_screen_data(airplay, screen_fd) == 0;
			pthread_mutex_unlock(&g_mu);
			if (!screen_attached) {
				iap2_log("ncm: failed to attach screen data");
				break;
			}
			if (airplay_client_start_audio(airplay, resolved) < 0)
				iap2_log("ncm: one or more audio streams unavailable; continuing video");
			if (!record_sent && airplay_client_send_record(airplay) < 0) {
				iap2_log("ncm: failed to send RECORD");
				break;
			}
			record_sent = 1;
		}
		if (!live_started && screen_attached &&
		    airplay_client_record_accepted(airplay)) {
			pthread_mutex_lock(&g_mu);
			if (!connect_owned_locked(request)) {
				pthread_mutex_unlock(&g_mu);
				break;
			}
			g_connect_retrying = 0;
			pthread_mutex_unlock(&g_mu);
			live_started = 1;
			if (!video_established) {
				video_established = 1;
				established_ms = ncm_now_ms();
			}
			if (recovering && carplay_log_should_sample(recovery_waits))
				iap2_log("ncm: AirPlay video re-established after recovery attempt %lu",
					 recovery_waits);
			if (airplay_client_send_test_screen(airplay) < 0) {
				iap2_log("ncm: failed to send test screen frames");
				break;
			}
			if (airplay_client_start_live_screen(airplay) < 0)
				iap2_log("ncm: live screen capture unavailable; test picture remains active");
		}
	}
airplay_loop_done:
cleanup_airplay:
	/* RECORD may have been accepted just before another channel closed in
	 * the same poll batch. Recovery does not require an encoder frame. */
	if (video_established || (airplay && airplay_client_screen_setup_complete(airplay) &&
				  airplay_client_record_accepted(airplay))) {
		uint64_t ended_ms = ncm_now_ms();
		recovering = 1;
		if (established_ms && ended_ms >= established_ms &&
		    ended_ms - established_ms >= AIRPLAY_RECOVERY_STABLE_MS) {
			if (recovery_delay_index)
				iap2_log("ncm: AirPlay video was stable; resetting recovery backoff");
			recovery_delay_index = 0;
			recovery_waits = 0;
		}
	}
	preparation_requested = airplay &&
		airplay_client_get_display_preparation(airplay, &preparation_plan) == 1;
	if (preparation_requested)
		iap2_log("ncm: closing AirPlay session for physical display preparation profile=%s preset=%ux%u canvas=%ux%u; receiver may restart USB before reconnect",
			profile == AIRPLAY_PROFILE_LEGACY ? "legacy" : "HAP",
			preparation_plan.physical_width, preparation_plan.physical_height,
			preparation_plan.width, preparation_plan.height);
	fallback_requested = airplay && profile == AIRPLAY_PROFILE_LEGACY &&
		!fallback_used && !preparation_requested && connect_owned(request) &&
		airplay_client_wants_hap_fallback(airplay);
	pthread_mutex_lock(&g_mu);
	if (g_active_airplay == airplay)
		g_active_airplay = NULL;
	if (g_connect_sock == fd)
		g_connect_sock = -1;
	if ((preparation_requested || recovering) && connect_owned_locked(request))
		g_connect_retrying = 1;
	pthread_mutex_unlock(&g_mu);
	publish_airplay_status(NULL, request);
	airplay_client_free(airplay);
	airplay = NULL;
	if (screen_fd >= 0) {
		shutdown(screen_fd, SHUT_RDWR);
		close(screen_fd);
	}
	if (event_fd >= 0) {
		shutdown(event_fd, SHUT_RDWR);
		close(event_fd);
	}
	shutdown(fd, SHUT_RDWR);
	close(fd);
	fd = event_fd = screen_fd = -1;
	if (preparation_requested) {
		char request_id[64] = {0};
		int dispatched = -1;
		/* Socket/client teardown is complete before any graphics switch.
		 * Hold the owner lock across dispatch to prevent a newer request
		 * from entering between the final ownership check and launch. */
		pthread_mutex_lock(&g_mu);
		if (!preparation_retry_used && connect_owned_locked(request))
			dispatched = carplay_display_start_preparation(&preparation_plan,
				request_id, sizeof(request_id));
		pthread_mutex_unlock(&g_mu);
		if (dispatched != 0 || !request_id[0])
			iap2_log("ncm: display preparation not dispatched (retry_used=%d); connector ending", preparation_retry_used);
		if (dispatched == 0 && request_id[0] &&
		    wait_display_preparation(request, request_id, &preparation_plan) == 1 &&
		    connect_owned(request)) {
			preparation_retry_used = 1;
			iap2_log("ncm: verified display preparation id=%s; reconnecting selected endpoint/profile once", request_id);
			goto connect_profile;
		}
		/* Failure, timeout, a superseding owner, or a second preparation
		 * request ends this connector. Never restart or fall back from it. */
		goto out;
	}
	if (fallback_requested && connect_owned(request)) {
		/* Never mix plaintext legacy and HAP on one TCP session. All
		 * old sockets/client are retired before one fresh connection to
		 * precisely the control endpoint that requested fallback. */
		iap2_log("ncm: legacy receiver requested HAP; reconnecting once on a fresh socket");
		fallback_used = 1;
		profile = AIRPLAY_PROFILE_HAP;
		goto connect_profile;
	}
recover:
	if (recovering && connect_owned(request)) {
		unsigned delay_s = recovery_delays_s[recovery_delay_index];
		if (recovery_delay_index + 1 < sizeof(recovery_delays_s) / sizeof(*recovery_delays_s))
			recovery_delay_index++;
		recovery_waits++;
		if (carplay_log_should_sample(recovery_waits))
			iap2_log("ncm: AirPlay video recovery attempt %lu in %us endpoint=%s profile=%s",
				 recovery_waits, delay_s, selected_address,
				 profile == AIRPLAY_PROFILE_LEGACY ? "legacy" : "HAP");
		if (wait_connect_retry(request, delay_s * 1000000u))
			goto connect_profile;
	}
out:
	if (recovering && !connect_owned(request))
		iap2_log("ncm: AirPlay video recovery canceled; endpoint or transport owner changed");
	struct ncm_connect_request *replacement = NULL;
	int replacement_queued = 0;

	pthread_mutex_lock(&g_mu);
	g_connect_request = NULL;
	g_connect_retrying = 0;
	free_connect_request(request);
	g_connect_thread_running = 0;
	g_connect_sock = -1;
	g_connect_retiring = 0;
	if (connect_ready_locked() && g_pending_connect_request) {
		replacement = g_pending_connect_request;
		g_pending_connect_request = NULL;
		if (launch_connect_request_locked(replacement) == 0) {
			replacement_queued = 1;
		} else {
			g_pending_connect_request = replacement;
		}
	}
	pthread_cond_broadcast(&g_connect_cv);
	pthread_mutex_unlock(&g_mu);
	if (replacement_queued)
		iap2_log("ncm: stale AirPlay transport retired; replacement endpoint connect started");
	return NULL;
}

static int launch_connect_request_locked(
	struct ncm_connect_request *request)
{
	pthread_t thread;
	pthread_attr_t attr;
	int create_error;

	if (!request || !connect_ready_locked() || g_connect_thread_running)
		return -1;
	g_airplay_transport_restart_pending = 0;
	g_connect_stop = 0;
	g_connect_sock = -1;
	g_connect_thread_running = 1;
	if (pthread_attr_init(&attr) != 0) {
		g_connect_thread_running = 0;
		return -1;
	}
	(void)pthread_attr_setdetachstate(
		&attr, PTHREAD_CREATE_DETACHED);
	request->owner = (struct ncm_connect_owner) {
		.usb_generation = g_usb_generation,
		.restart_epoch = g_transport_restart_epoch,
		.tun_index = g_tun_index,
		.kernel_ncm = g_kernel_ncm,
		.host_netdev_ready = g_host_netdev_ready,
	};
	g_connect_request = request;
	create_error = pthread_create(
		&thread, &attr, connect_endpoint_main, request);
	pthread_attr_destroy(&attr);
	if (create_error != 0) {
		g_connect_request = NULL;
		g_connect_thread_running = 0;
		return -1;
	}
	return 0;
}

/* mDNS and iAP2 can advertise the same endpoint with different profiles.
 * Once the active request selected its address, unused alternatives must
 * not prevent an advertisement from replacing it during recovery. */
static int same_connect_endpoint(const struct ncm_connect_request *left,
				 const struct ncm_connect_request *right)
{
	if (!left || !right || left->port != right->port)
		return 0;
	for (size_t i = 0; i < left->count; i++) {
		if (right->selected_address) {
			if (!strcmp(left->addresses[i], right->selected_address))
				return 1;
			continue;
		}
		for (size_t j = 0; j < right->count; j++)
			if (*left->addresses[i] && !strcmp(left->addresses[i], right->addresses[j]))
				return 1;
	}
	return 0;
}

static int queue_connect_endpoint(const char *const addresses[], size_t count,
				  uint16_t port, enum airplay_profile profile,
				  int discovered)
{
	struct ncm_connect_request *request;

	if (!addresses || !count || !port)
		return -1;
	request = calloc(1, sizeof(*request));
	if (!request)
		return -1;
	request->addresses = calloc(count, sizeof(*request->addresses));
	if (!request->addresses) {
		free(request);
		return -1;
	}
	request->count = count;
	request->port = port;
	request->profile = profile;
	request->discovered = discovered;
	pthread_mutex_lock(&g_mu);
	request->media_audio = g_airplay_media_enabled;
	request->microphone = g_airplay_microphone_enabled;
	request->display_width = g_airplay_display_width;
	request->display_height = g_airplay_display_height;
	request->physical_width = g_airplay_physical_width;
	request->physical_height = g_airplay_physical_height;
	request->display_density = g_airplay_display_density;
	request->video_fps = g_airplay_video_fps;
	request->video_bitrate = g_airplay_video_bitrate;
	request->vehicle_state = g_vehicle_state;
	request->vehicle_generation = g_vehicle_generation;
	pthread_mutex_unlock(&g_mu);
	for (size_t i = 0; i < count; i++) {
		request->addresses[i] = strdup(addresses[i] ? addresses[i] : "");
		if (!request->addresses[i]) {
			for (size_t j = 0; j < i; j++)
				free(request->addresses[j]);
			free(request->addresses);
			free(request);
			return -1;
		}
	}
	pthread_mutex_lock(&g_mu);
	if ((discovered && (!g_mdns_enabled || !g_host_netdev_ready || g_connect_stop ||
			    (g_pending_connect_request && !g_connect_retrying &&
			     !g_airplay_transport_restart_pending))) ||
	    ((discovered || g_connect_thread_running) &&
	     same_connect_endpoint(request, g_pending_connect_request)) ||
	    (g_connect_thread_running && !g_airplay_transport_restart_pending &&
	     (!g_connect_retrying ||
	      (same_connect_endpoint(request, g_connect_request) &&
	       (g_pending_connect_request || connect_owned_locked(g_connect_request)))))) {
		pthread_mutex_unlock(&g_mu);
		free_connect_request(request);
		return 0;
	}
	if (!connect_ready_locked()) {
		struct ncm_connect_request *previous =
			g_pending_connect_request;

		g_pending_connect_request = request;
		pthread_mutex_unlock(&g_mu);
		free_connect_request(previous);
		iap2_log("ncm: deferred AirPlay TCP endpoint connect until transport/Host authentication ready addresses=%zu port=%u",
			 count, port);
		return 0;
	}
	if (g_connect_thread_running) {
		struct ncm_connect_request *previous = g_pending_connect_request;

		g_pending_connect_request = request;
		g_connect_retiring = 1;
		if (g_connect_sock >= 0)
			shutdown(g_connect_sock, SHUT_RDWR);
		pthread_mutex_unlock(&g_mu);
		free_connect_request(previous);
		iap2_log("ncm: queued replacement AirPlay endpoint addresses=%zu port=%u",
			 count, port);
		return 0;
	}
	if (launch_connect_request_locked(request) < 0) {
		pthread_mutex_unlock(&g_mu);
		free_connect_request(request);
		return -1;
	}
	pthread_mutex_unlock(&g_mu);
	iap2_log("ncm: queued AirPlay TCP endpoint connect addresses=%zu port=%u priority=IPv4-first",
		 count, port);
	return 0;
}

int ncm_bridge_connect_endpoint(const char *const addresses[], size_t count,
				uint16_t port)
{
	return queue_connect_endpoint(addresses, count, port, AIRPLAY_PROFILE_HAP, 0);
}

/*
 * Bench: run the AirPlay sender against a fixed endpoint, with no USB at all.
 *
 * Everything below the socket is the production path -- the same RTSP client,
 * binary-plist encoder, AES-128-CTR screen channel and RTP audio -- so a
 * simulated head unit on another machine exercises the real code.  What is
 * skipped is precisely the part that needs a car: the 0x51 role switch, iAP2,
 * the NCM link and the 0x4301 address advertisement.
 *
 * The only state that has to be faked is g_running: transport_ready_locked()
 * is what the connect thread consults before it keeps the socket, and with no
 * USB transport there is nothing else to ask.  The caller asked for this
 * deliberately, so the log says so too -- a box left in bench mode answers
 * nothing on the bus.
 */
int ncm_bridge_bench_start(const char *address, uint16_t port)
{
	int rc;
	const char *choice = getenv("CARPLAY_AIRPLAY_PROFILE");
	enum airplay_profile profile = choice && strcmp(choice, "legacy") == 0 ?
		AIRPLAY_PROFILE_LEGACY : AIRPLAY_PROFILE_HAP;

	if (!address || !*address)
		return -1;
	pthread_mutex_lock(&g_mu);
	if (g_connect_thread_running) {
		pthread_mutex_unlock(&g_mu);
		return -1;
	}
	g_running = 1;
	g_connect_stop = 0;
	g_connect_sock = -1;
	pthread_mutex_unlock(&g_mu);

	iap2_log("ncm: BENCH MODE -- AirPlay sender dials %s:%u with no USB transport; "
		 "this box will not enumerate as a phone", address, port);
	rc = queue_connect_endpoint(&address, 1, port, profile, 0);
	if (rc < 0)
		iap2_log("ncm: bench endpoint %s:%u was refused", address, port);
	return rc;
}

static void usb_workers_start_failed_locked(void)
{
	g_usb_generation++;
	g_usb_workers_running = 0;
	g_usb_out_fd = -1;
	g_usb_in_fd = -1;
}

static int usb_workers_start(int usb_out_fd, int usb_in_fd)
{
	pthread_t rx_thread;
	pthread_t tx_thread;
	unsigned long generation;
	int flags;

	pthread_mutex_lock(&g_usb_mu);
	pthread_mutex_lock(&g_mu);
	if (!g_running) {
		pthread_mutex_unlock(&g_mu);
		pthread_mutex_unlock(&g_usb_mu);
		return -1;
	}
	if (g_usb_workers_running || usb_out_fd < 0 || usb_in_fd < 0) {
		pthread_mutex_unlock(&g_mu);
		pthread_mutex_unlock(&g_usb_mu);
		return -1;
	}
	flags = fcntl(usb_out_fd, F_GETFL, 0);
	if (flags >= 0)
		(void)fcntl(usb_out_fd, F_SETFL, flags | O_NONBLOCK);
	flags = fcntl(usb_in_fd, F_GETFL, 0);
	if (flags >= 0)
		(void)fcntl(usb_in_fd, F_SETFL, flags | O_NONBLOCK);
	generation = ++g_usb_generation;
	if (!generation)
		generation = ++g_usb_generation;
	g_usb_out_fd = usb_out_fd;
	g_usb_in_fd = usb_in_fd;
	g_usb_workers_running = 1;
	atomic_store(&g_usb_rx_exited, 0);
	atomic_store(&g_usb_tx_exited, 0);
	pthread_mutex_unlock(&g_mu);

	if (pthread_create(&rx_thread, NULL, usb_rx_main,
			   (void *)(uintptr_t)generation) != 0) {
		pthread_mutex_lock(&g_mu);
		usb_workers_start_failed_locked();
		pthread_mutex_unlock(&g_mu);
		pthread_mutex_unlock(&g_usb_mu);
		return -1;
	}
	if (pthread_create(&tx_thread, NULL, usb_tx_main,
			   (void *)(uintptr_t)generation) != 0) {
		pthread_mutex_lock(&g_mu);
		usb_workers_start_failed_locked();
		pthread_mutex_unlock(&g_mu);
		usb_worker_join(rx_thread, &g_usb_rx_exited);
		pthread_mutex_unlock(&g_usb_mu);
		return -1;
	}
	g_usb_rx_thread = rx_thread;
	g_usb_tx_thread = tx_thread;
	pthread_mutex_unlock(&g_usb_mu);
	if (ncm_watchdog_start() < 0)
		iap2_log("ncm: USB endpoint watchdog unavailable; a wedged endpoint needs a manual UDC rebind");
	return 0;
}

static void usb_workers_stop(void)
{
	int running;

	pthread_mutex_lock(&g_usb_mu);
	pthread_mutex_lock(&g_mu);
	running = g_usb_workers_running;
	g_usb_generation++;
	g_usb_workers_running = 0;
	g_usb_out_fd = -1;
	g_usb_in_fd = -1;
	pthread_mutex_unlock(&g_mu);
	if (running) {
		usb_worker_join(g_usb_rx_thread, &g_usb_rx_exited);
		usb_worker_join(g_usb_tx_thread, &g_usb_tx_exited);
	}
	/* Kept alive while joining: if that join is itself stuck inside a
	 * FunctionFS syscall the watchdog is what unblocks it. */
	ncm_watchdog_stop();
	pthread_mutex_unlock(&g_usb_mu);
}

int ncm_bridge_start(int usb_out_fd, int usb_in_fd)
{
	if (g_kernel_ncm) {
		/*
		 * No endpoint workers in netdev mode: the kernel terminates the
		 * NTB stream and the netdev carries plain Ethernet. Kept as a
		 * working entry point so a caller that does not branch still ends
		 * up with a usable interface.
		 */
		return ncm_bridge_netdev_up();
	}
	pthread_mutex_lock(&g_mu);
	if (g_running) {
		pthread_mutex_unlock(&g_mu);
		return 0;
	}
	if (g_tun_fd < 0 || usb_out_fd < 0 || usb_in_fd < 0) {
		pthread_mutex_unlock(&g_mu);
		return -1;
	}
	g_running = 1;
	g_peer_mac_valid = 0;
	g_peer_ipv6_valid = 0;
	g_rx_blocks = 0;
	g_tx_frames = 0;
	g_bad_frames = 0;
	g_usb_rx_chunks = 0;
	g_usb_rx_errors = 0;
	g_rx_logged = 0;
	g_tx_logged = 0;
	g_screen_data_rx_chunks = 0;
	g_screen_data_rx_bytes = 0;
	g_connect_stop = 0;
	g_connect_sock = -1;
	g_airplay_transport_restart_pending = 0;
	pthread_mutex_unlock(&g_mu);

	if (usb_workers_start(usb_out_fd, usb_in_fd) < 0) {
		pthread_mutex_lock(&g_mu);
		g_running = 0;
		pthread_mutex_unlock(&g_mu);
		iap2_log("ncm: failed to start USB workers");
		return -1;
	}

	pthread_mutex_lock(&g_mu);
	if (g_pending_connect_request) {
		struct ncm_connect_request *pending =
			g_pending_connect_request;

		g_pending_connect_request = NULL;
		iap2_log("ncm: resuming deferred AirPlay endpoint connect addresses=%zu port=%u",
			 pending->count, pending->port);
		if (launch_connect_request_locked(pending) < 0) {
			g_pending_connect_request = pending;
			iap2_log("ncm: deferred AirPlay endpoint could not start after NCM restart");
		}
	}
	pthread_mutex_unlock(&g_mu);
	iap2_log("ncm: started usb_out_fd=%d usb_in_fd=%d",
		 usb_out_fd, usb_in_fd);
	return 0;
}

/* The endpoint owner serializes enable/disable. A stop or auth revocation
 * during OS work invalidates this completion, even if the netdev survives. */
static void complete_transport_restart(unsigned long epoch, int result)
{
    pthread_mutex_lock(&g_mu);
    if (epoch == g_transport_restart_epoch && !g_connect_stop && result == 0) {
        g_transport_restarting = 0;
        if (g_connect_thread_running && g_pending_connect_request) {
            g_connect_retiring = 1;
            if (g_connect_sock >= 0) shutdown(g_connect_sock, SHUT_RDWR);
        }
        if (!g_connect_thread_running) {
            g_connect_retiring = 0;
            if (connect_ready_locked() && g_pending_connect_request) {
                struct ncm_connect_request *pending = g_pending_connect_request;
                g_pending_connect_request = NULL;
                if (launch_connect_request_locked(pending) < 0)
                    g_pending_connect_request = pending;
            }
        }
    }
    pthread_mutex_unlock(&g_mu);
}

int ncm_bridge_restart_usb(int usb_out_fd, int usb_in_fd)
{
    int kernel, result;
    unsigned long epoch;
    pthread_mutex_lock(&g_mu);
    kernel = g_kernel_ncm;
    if (g_connect_stop || (!kernel &&
        (!g_running || g_tun_fd < 0 || usb_out_fd < 0 || usb_in_fd < 0))) {
        pthread_mutex_unlock(&g_mu);
        return -1;
    }
    epoch = ++g_transport_restart_epoch;
    g_transport_restarting = 1;
    g_airplay_transport_restart_pending = 1;
    if (g_connect_thread_running && !kernel) {
        g_connect_retiring = 1;
        /* Retain fd ownership while shutting it down; retirement may close
         * and reuse the descriptor as soon as g_mu is released. */
        if (g_connect_sock >= 0) shutdown(g_connect_sock, SHUT_RDWR);
    }
    pthread_mutex_unlock(&g_mu);
    if (kernel) {
        /* Only our epoch-checked completion may re-enable connectors.
         * netdev_up also clears stop for a NEW USB round, which could revive
         * a transport stopped while this OS operation was in flight. */
        result = netdev_apply(NCM_NETDEV_WAIT_MS);
    } else {
        usb_workers_stop();
        result = usb_workers_start(usb_out_fd, usb_in_fd);
    }
    complete_transport_restart(epoch, result);
    iap2_log("ncm: %s transport re-enable result=%d; replacement waits for current endpoint",
             kernel ? "kernel" : "TUN", result);
    return result;
}

void ncm_bridge_stop(void)
{
	int running;
	int connect_sock;
	struct carplay_mdns *mdns;

	pthread_mutex_lock(&g_mu);
	running = g_running;
	g_running = 0;
	g_host_netdev_ready = 0;
	g_host_auth_required = 0;
	g_host_authenticated = 0;
	g_transport_restart_epoch++;
	g_transport_restarting = 0;
	g_airplay_transport_restart_pending = 0;
	g_connect_stop = 1;
	g_mdns_enabled = 0;
	mdns = g_mdns;
	g_mdns = NULL;
	connect_sock = g_connect_sock;
	/* Connector detaches this fd under the same lock before closing it. */
	if (connect_sock >= 0)
		shutdown(connect_sock, SHUT_RDWR);
	pthread_mutex_unlock(&g_mu);
	carplay_mdns_stop(mdns);
	iap2_log("ncm: stop begin running=%d connect_sock=%d", running,
		 connect_sock);
	/* No workers exist in netdev mode; stopping them would join threads that
	 * were never created. */
	if (!g_kernel_ncm)
		usb_workers_stop();
	if (running)
		iap2_log("ncm: stopped blocks=%lu tx=%lu bad=%lu",
			 g_rx_blocks, g_tx_frames, g_bad_frames);
	pthread_mutex_lock(&g_mu);
	iap2_log("ncm: AirPlay connect wait begin running=%d",
		 g_connect_thread_running);
	while (g_connect_thread_running)
		pthread_cond_wait(&g_connect_cv, &g_mu);
	iap2_log("ncm: AirPlay connect wait complete");
	g_connect_sock = -1;
	g_airplay_transport_restart_pending = 0;
	free_connect_request(g_pending_connect_request);
	g_pending_connect_request = NULL;
	pthread_mutex_unlock(&g_mu);
}
void ncm_bridge_close(void)
{
	int fd;

	ncm_bridge_stop();
	pthread_mutex_lock(&g_mu);
	g_tun_index = 0;
	g_peer_mac_valid = 0;
	g_peer_ipv6_valid = 0;
	pthread_mutex_unlock(&g_mu);
	if (g_kernel_ncm) {
		/*
		 * The netdev belongs to the kernel; drop only the cached index so a
		 * later connect cannot scope IPv6 to an interface that is gone.
		 */
		pthread_mutex_lock(&g_mu);
		g_tun_index = 0;
		pthread_mutex_unlock(&g_mu);
		return;
	}
	pthread_mutex_lock(&g_mu);
	fd = g_tun_fd;
	g_tun_fd = -1;
	pthread_mutex_unlock(&g_mu);
	if (fd_is_open(fd))
		close(fd);
}

void ncm_bridge_set_airplay_options(int enable_media, int enable_microphone,
                                    int display_width, int display_height,
                                    int physical_width, int physical_height,
                                    int display_density, int video_fps,
                                    int video_bitrate)
{
	pthread_mutex_lock(&g_mu);
	g_airplay_media_enabled = !!enable_media;
	g_airplay_microphone_enabled = !!enable_microphone;
	g_airplay_display_width = display_width;
	g_airplay_display_height = display_height;
	g_airplay_physical_width = physical_width;
	g_airplay_physical_height = physical_height;
	g_airplay_display_density = display_density;
	g_airplay_video_fps = video_fps;
	g_airplay_video_bitrate = video_bitrate > CARPLAY_VIDEO_BITRATE_MAX ?
		CARPLAY_VIDEO_BITRATE_MAX : video_bitrate;
	pthread_mutex_unlock(&g_mu);
}

void ncm_bridge_set_vehicle_state(struct carplay_vehicle_state *state,
                                  uint64_t generation)
{
	pthread_mutex_lock(&g_mu);
	g_vehicle_state = state;
	g_vehicle_generation = generation;
	pthread_mutex_unlock(&g_mu);
}

void ncm_bridge_get_airplay_status(struct ncm_airplay_status *status)
{
	if (!status)
		return;
	pthread_mutex_lock(&g_mu);
	*status = g_airplay_status;
	pthread_mutex_unlock(&g_mu);
}

int ncm_bridge_is_running(void)
{
	if (g_kernel_ncm) {
		char name[IFNAMSIZ];

		/*
		 * Resolve instead of trusting the cached name: the question here
		 * is "is there a netdev to send through", and a stale "ncm0"
		 * would answer no even while usb0 is up and addressed.
		 */
		if (ncm_bridge_kernel_ifname(name, sizeof(name)) < 0)
			return 0;
		return if_nametoindex(name) > 0;
	}
	return g_running;
}

int ncm_bridge_request_car_ui(void)
{
	int result;
	pthread_mutex_lock(&g_mu);
	if (!g_active_airplay) {
		pthread_mutex_unlock(&g_mu);
		return 1;
	}
	result = airplay_client_request_car_ui(g_active_airplay);
	pthread_mutex_unlock(&g_mu);
	return result;
}
