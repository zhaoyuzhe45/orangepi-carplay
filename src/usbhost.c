/*
 * usbhost.c -- usbfs transport for the host half of a wired CarPlay link.
 *
 * Enumeration is done through sysfs rather than libusb: the box has no
 * libusb dependency, sysfs gives us busnum/devnum without parsing
 * descriptors by hand, and the only thing we need from the device is one
 * vendor interface with a bulk pair.
 */
#define _GNU_SOURCE
#include "usbhost.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <linux/usbdevice_fs.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <unistd.h>

#define SYS_USB		"/sys/bus/usb/devices"
#define DEV_USB		"/dev/bus/usb"
#define GADGET_UDC	"/config/usb_gadget/g1/UDC"
#define PATH_BUF	512

/*
 * True while our own gadget is still attached to the UDC.
 *
 * This decides whether the 05ac:12a8 filter below is meaningful.  In host
 * mode the gadget has been unbound, so nothing on the bus is us -- and a head
 * unit is perfectly free to present the same IDs (forging them is trivial,
 * which is the whole basis of this project).  Filtering by ID then would hide
 * the very device we are looking for.
 */
static int gadget_is_bound(void)
{
	char buf[64];
	ssize_t n;
	int fd;

	fd = open(GADGET_UDC, O_RDONLY | O_CLOEXEC);
	if (fd < 0)
		return 0;
	n = read(fd, buf, sizeof(buf) - 1);
	close(fd);
	if (n <= 0)
		return 0;
	return buf[0] != '\n' && buf[0] != '\0';
}

static int read_uint_file(const char *path, int base, unsigned *out)
{
	char buf[32];
	char *end;
	unsigned long value;
	ssize_t n;
	int fd;

	fd = open(path, O_RDONLY | O_CLOEXEC);
	if (fd < 0)
		return -1;
	n = read(fd, buf, sizeof(buf) - 1);
	close(fd);
	if (n <= 0)
		return -1;
	buf[n] = '\0';

	errno = 0;
	value = strtoul(buf, &end, base);
	if (errno != 0 || end == buf)
		return -1;
	*out = (unsigned)value;
	return 0;
}

static int sysfs_uint(const char *device, const char *iface, const char *attr,
		      int base, unsigned *out)
{
	char path[PATH_BUF];
	int n;

	if (iface)
		n = snprintf(path, sizeof(path), SYS_USB "/%s/%s/%s", device,
			     iface, attr);
	else
		n = snprintf(path, sizeof(path), SYS_USB "/%s/%s", device, attr);
	if (n < 0 || (size_t)n >= sizeof(path))
		return -1;
	return read_uint_file(path, base, out);
}

struct endpoints {
	unsigned in;
	unsigned out;
	unsigned in_max;
	unsigned out_max;
};

/*
 * Collects the bulk pair of one interface.  Endpoint addresses come from the
 * directory name (ep_85) because that is what usbfs wants for USBDEVFS_BULK;
 * bmAttributes is read to skip interrupt endpoints that share the interface.
 */
static int collect_endpoints(const char *device, const char *iface,
			     struct endpoints *eps)
{
	char dir_path[PATH_BUF];
	struct dirent *entry;
	DIR *dir;
	int n;

	n = snprintf(dir_path, sizeof(dir_path), SYS_USB "/%s/%s", device, iface);
	if (n < 0 || (size_t)n >= sizeof(dir_path))
		return -1;

	dir = opendir(dir_path);
	if (!dir)
		return -1;

	memset(eps, 0, sizeof(*eps));
	while ((entry = readdir(dir)) != NULL) {
		char attr_path[PATH_BUF];
		char *end;
		unsigned address;
		unsigned attributes;
		unsigned max_packet;

		if (strncmp(entry->d_name, "ep_", 3) != 0)
			continue;

		errno = 0;
		address = (unsigned)strtoul(entry->d_name + 3, &end, 16);
		if (errno != 0 || end == entry->d_name + 3)
			continue;

		snprintf(attr_path, sizeof(attr_path),
			 SYS_USB "/%s/%s/%s/bmAttributes", device, iface,
			 entry->d_name);
		if (read_uint_file(attr_path, 16, &attributes) < 0)
			continue;
		/* bits 1:0 == 2 is bulk; anything else (interrupt on 0x83,
		 * isochronous on a capture path) is not our channel. */
		if ((attributes & 0x03u) != 0x02u)
			continue;
		snprintf(attr_path, sizeof(attr_path),
			 SYS_USB "/%s/%s/%s/wMaxPacketSize", device, iface,
			 entry->d_name);
		/* usbcore exports wMaxPacketSize with %04x, not decimal. */
		if (read_uint_file(attr_path, 16, &max_packet) < 0)
			max_packet = 512;

		if (address & 0x80u) {
			eps->in = address;
			eps->in_max = max_packet;
		} else {
			eps->out = address;
			eps->out_max = max_packet;
		}
	}
	closedir(dir);

	return (eps->in && eps->out) ? 0 : -1;
}

/*
 * Tries one interface directory.  Accepts the two spellings of an Apple iAP2
 * interface: a real iPhone advertises ff/f0/00, our own box ff/fe/02, and a
 * head unit copying either layout is equally valid.
 *
 * Class/subclass/protocol only: the endpoint pair is collected *after* the
 * interface has been switched to a usable alternate setting, because sysfs
 * only ever lists the endpoints of the setting that is currently active.
 */
static int match_interface(const char *device, const char *iface,
			   unsigned *number)
{
	unsigned interface_class;
	unsigned subclass;
	unsigned protocol;
	unsigned ifnum;

	if (sysfs_uint(device, iface, "bInterfaceClass", 16, &interface_class) < 0)
		return -1;
	if (interface_class != USBHOST_IAP2_CLASS)
		return -1;

	if (sysfs_uint(device, iface, "bInterfaceSubClass", 16, &subclass) < 0)
		return -1;
	if (subclass != USBHOST_IAP2_SUBCLASS_MFI &&
	    subclass != USBHOST_IAP2_SUBCLASS_FFS)
		return -1;

	if (sysfs_uint(device, iface, "bInterfaceNumber", 16, &ifnum) < 0)
		return -1;
	/* Protocol 0 (MFi) is what iAP2 runs on; 2 is our own relay layout.
	 * Anything else on this subclass is a different Apple function. */
	if (sysfs_uint(device, iface, "bInterfaceProtocol", 16, &protocol) < 0 ||
	    (subclass == USBHOST_IAP2_SUBCLASS_MFI && protocol != 0) ||
	    (subclass == USBHOST_IAP2_SUBCLASS_FFS && protocol != 2))
		return -1;

	*number = ifnum;
	return 0;
}

/*
 * Put the interface into its bulk-bearing alternate setting.
 *
 * The MFi spec (EA Native Transport, p.507) requires iAP2 to be exposed as
 * two alternate settings: setting 0 is zero-bandwidth with no endpoints, and
 * setting 1 carries the bulk pair.  A head unit is free to power up in
 * setting 0, and sysfs will then show no endpoints at all -- which looks
 * exactly like "this interface is not iAP2".  Selecting setting 1 first is
 * what turns that into a usable channel.
 *
 * Devices with a single setting reject the ioctl; that is not an error, it
 * just means the current setting already is the only one.
 */
static void select_bulk_alt(int fd, unsigned interface)
{
	struct usbdevfs_setinterface set;

	memset(&set, 0, sizeof(set));
	set.interface = interface;
	set.altsetting = 1;
	(void)ioctl(fd, USBDEVFS_SETINTERFACE, &set);
}

/*
 * Which USB bus belongs to the controller we just re-roled.
 *
 * A head unit is only reachable through the OTG controller the gadget was
 * bound to; every other bus on this board (two EHCI/OHCI pairs and the
 * always-present USB3 host controller) carries unrelated hardware.  Scanning
 * them all means a USB stick or a 4G dongle with a vendor interface and a
 * bulk pair can be claimed as "the car" milliseconds after the switch, and
 * the real head unit -- arriving a few hundred ms later -- is then ignored
 * (orangecar lost a whole session to exactly that: docs/protocol.md 2.2).
 *
 * The controller name comes from CARPLAY_OTG_UDC, set by the supervisor from
 * /sys/class/udc right before it flips the role.  Empty means "not known, do
 * not filter" -- the previous behaviour, kept so an unset environment can
 * never make the box blind.
 */
static int bus_is_role_switched(int busnum)
{
	char path[PATH_BUF];
	char target[PATH_BUF];
	const char *udc_name = getenv("CARPLAY_OTG_UDC");
	ssize_t n;

	if (!udc_name || !*udc_name)
		return 1;

	snprintf(path, sizeof(path), SYS_USB "/usb%d", busnum);
	n = readlink(path, target, sizeof(target) - 1);
	if (n <= 0)
		return 0;
	target[n] = '\0';
	/* Match a whole component, never another controller sharing a prefix. */
	{
		const char *match = target;
		size_t len = strlen(udc_name);
		while ((match = strstr(match, udc_name)) != NULL) {
			if ((match == target || match[-1] == '/') &&
			    (match[len] == '/' || match[len] == '\0'))
				return 1;
			match++;
		}
	}
	return 0;
}

/* Enumerates every device sysfs knows about; "1-1" style names only, since
 * "1-1:1.0" entries are interfaces of the same device. */
int usbhost_open(struct usbhost *host, uint16_t exclude_vid,
		 uint16_t exclude_pid)
{
	struct dirent *device_entry;
	DIR *devices;
	int last_error = 0;

	if (!host)
		return -1;
	memset(host, 0, sizeof(*host));
	host->fd = -1;

	devices = opendir(SYS_USB);
	if (!devices)
		return -1;

	while ((device_entry = readdir(devices)) != NULL) {
		struct dirent *iface_entry;
		struct endpoints eps;
		unsigned busnum;
		unsigned devnum;
		unsigned vid;
		unsigned pid;
		unsigned interface_number;
		char device_dir[PATH_BUF];
		DIR *interfaces;
		int claimed = 0;

		if (strchr(device_entry->d_name, ':') != NULL)
			continue;
		if (strchr(device_entry->d_name, '-') == NULL)
			continue;	/* root hubs */

		if (sysfs_uint(device_entry->d_name, NULL, "busnum", 10, &busnum) < 0)
			continue;
		if (!bus_is_role_switched((int)busnum))
			continue;	/* some other controller's port */
		if (sysfs_uint(device_entry->d_name, NULL, "devnum", 10, &devnum) < 0)
			continue;
		if (sysfs_uint(device_entry->d_name, NULL, "idVendor", 16, &vid) < 0)
			continue;
		if (sysfs_uint(device_entry->d_name, NULL, "idProduct", 16, &pid) < 0)
			continue;

		if (gadget_is_bound() && vid == exclude_vid && pid == exclude_pid)
			continue;	/* our own gadget, still visible */

		/* Interface directories live next to the device directory. */
		interfaces = opendir(SYS_USB);
		if (!interfaces)
			break;

		snprintf(device_dir, sizeof(device_dir), "%s:",
			 device_entry->d_name);

		while ((iface_entry = readdir(interfaces)) != NULL) {
			char path[PATH_BUF];
			int fd;
			int ifnum;

			if (strncmp(iface_entry->d_name, device_dir,
				    strlen(device_dir)) != 0)
				continue;
			if (match_interface(device_entry->d_name,
					    iface_entry->d_name,
					    &interface_number) < 0)
				continue;

			snprintf(path, sizeof(path), DEV_USB "/%03u/%03u",
				 busnum, devnum);
			fd = open(path, O_RDWR | O_CLOEXEC);
			if (fd < 0) {
				last_error = errno;
				continue;
			}

			ifnum = (int)interface_number;
			if (ioctl(fd, USBDEVFS_CLAIMINTERFACE, &ifnum) < 0) {
				last_error = errno;
				close(fd);
				continue;
			}

			/*
			 * Read the endpoints of the setting the device is
			 * already in.  sysfs only lists the active setting, so
			 * this is also the cheapest way to learn whether a
			 * SET_INTERFACE is needed at all -- and on a gadget that
			 * has just been configured, an unnecessary one can take
			 * the peer off the bus with -71.
			 */
			if (collect_endpoints(device_entry->d_name,
					      iface_entry->d_name, &eps) < 0) {
				/*
				 * Setting 0 is allowed to be zero-bandwidth
				 * (MFi native transport, p.507), so ask for the
				 * bulk-bearing setting and look again.
				 */
				select_bulk_alt(fd, interface_number);
				if (collect_endpoints(device_entry->d_name,
						      iface_entry->d_name,
						      &eps) < 0) {
					(void)ioctl(fd,
						    USBDEVFS_RELEASEINTERFACE,
						    &ifnum);
					close(fd);
					continue;
				}
			}

			host->fd = fd;
			host->busnum = (int)busnum;
			host->devnum = (int)devnum;
			host->interface = ifnum;
			host->ep_in = (int)eps.in;
			host->ep_out = (int)eps.out;
			host->ep_in_max = (int)eps.in_max;
			host->ep_out_max = (int)eps.out_max;
			host->vid = (uint16_t)vid;
			host->pid = (uint16_t)pid;
			snprintf(host->sysfs_name, sizeof(host->sysfs_name), "%.31s",
				 device_entry->d_name);
			/*
			 * Keep the head unit awake.  A head unit that autosuspends
			 * mid-handshake looks identical to one that walked away, and
			 * every later bulk transfer fails with -EPIPE.
			 */
			{
				char ctl[PATH_BUF];
				int ctl_fd;

				snprintf(ctl, sizeof(ctl),
					 SYS_USB "/%s/power/control",
					 device_entry->d_name);
				ctl_fd = open(ctl, O_WRONLY | O_CLOEXEC);
				if (ctl_fd >= 0) {
					/* Best effort: an old kernel without
					 * power management has no such file. */
					(void)write(ctl_fd, "on", 2);
					close(ctl_fd);
				}
			}
			claimed = 1;
			break;
		}
		closedir(interfaces);
		if (claimed)
			break;
	}
	closedir(devices);

	if (host->fd >= 0)
		return 0;
	if (last_error)
		errno = last_error;
	return last_error ? -2 : -1;
}

void usbhost_close(struct usbhost *host)
{
	if (!host || host->fd < 0)
		return;
	/* Best effort: the kernel releases the claim when the fd closes, but
	 * an explicit release keeps the interface usable if we ever reopen. */
	ioctl(host->fd, USBDEVFS_RELEASEINTERFACE, &host->interface);
	close(host->fd);
	host->fd = -1;
}

int usbhost_present(void)
{
	struct usbhost probe;
	int rc = usbhost_open(&probe, 0x05ac, 0x12a8);

	if (rc == 0)
		usbhost_close(&probe);
	return rc == 0;
}

int usbhost_role_bus_count(void)
{
	struct dirent *entry;
	DIR *devices;
	int count = 0;

	devices = opendir(SYS_USB);
	if (!devices)
		return -1;

	while ((entry = readdir(devices)) != NULL) {
		char *end;
		long busnum;

		if (strncmp(entry->d_name, "usb", 3) != 0)
			continue;
		busnum = strtol(entry->d_name + 3, &end, 10);
		if (end == entry->d_name + 3 || *end != '\0' ||
		    busnum < 0 || busnum > 2147483647L)
			continue;
		if (bus_is_role_switched((int)busnum))
			count++;
	}
	closedir(devices);
	return count;
}

/*
 * One line per device and per interface currently on the bus.
 *
 * Without this, "no CarPlay head unit on the bus yet" covers two very
 * different worlds: an empty bus, and a peer that enumerated fine but does
 * not look like anything we accept.  The 08:16 attempt against the A4L spent
 * ten seconds in the second world and told us nothing (2026-10-29).  Cheap
 * enough to call once per round: it is a tmpfs walk, no USB traffic.
 */
static int append(char *buf, size_t len, size_t *used, const char *fmt, ...)
{
	va_list ap;
	int n;

	if (*used >= len)
		return -1;
	va_start(ap, fmt);
	n = vsnprintf(buf + *used, len - *used, fmt, ap);
	va_end(ap);
	if (n < 0)
		return -1;
	if ((size_t)n >= len - *used) {
		*used = len - 1;
		return -1;
	}
	*used += (size_t)n;
	return 0;
}

int usbhost_snapshot(char *buf, size_t len)
{
	struct dirent *device_entry;
	DIR *devices;
	size_t used = 0;
	int count = 0;

	if (!buf || len < 64)
		return -1;
	buf[0] = '\0';

	devices = opendir(SYS_USB);
	if (!devices)
		return -1;

	while ((device_entry = readdir(devices)) != NULL) {
		unsigned vid = 0, pid = 0, speed = 0, busnum;
		char iface_prefix[64];
		struct dirent *iface_entry;
		DIR *interfaces;

		if (strchr(device_entry->d_name, ':') != NULL)
			continue;
		if (strchr(device_entry->d_name, '-') == NULL)
			continue;	/* root hubs */

		if (sysfs_uint(device_entry->d_name, NULL, "busnum", 10, &busnum) < 0 ||
		    !bus_is_role_switched((int)busnum))
			continue;

		if (sysfs_uint(device_entry->d_name, NULL, "idVendor", 16, &vid) < 0)
			continue;
		if (sysfs_uint(device_entry->d_name, NULL, "idProduct", 16, &pid) < 0)
			continue;
		(void)sysfs_uint(device_entry->d_name, NULL, "speed", 10, &speed);

		count++;
		append(buf, len, &used, "%s %04x:%04x %uMbps", device_entry->d_name,
		       vid, pid, speed);

		snprintf(iface_prefix, sizeof(iface_prefix), "%.62s:",
			 device_entry->d_name);
		interfaces = opendir(SYS_USB);
		if (!interfaces) {
			append(buf, len, &used, "\n");
			continue;
		}
		while ((iface_entry = readdir(interfaces)) != NULL) {
			unsigned cls = 0, sub = 0, proto = 0, num = 0;
			char link[PATH_BUF];
			char driver[64] = "-";
			ssize_t n;

			if (strncmp(iface_entry->d_name, iface_prefix,
				    strlen(iface_prefix)) != 0)
				continue;
			(void)sysfs_uint(device_entry->d_name, iface_entry->d_name,
					 "bInterfaceClass", 16, &cls);
			(void)sysfs_uint(device_entry->d_name, iface_entry->d_name,
					 "bInterfaceSubClass", 16, &sub);
			(void)sysfs_uint(device_entry->d_name, iface_entry->d_name,
					 "bInterfaceProtocol", 16, &proto);
			(void)sysfs_uint(device_entry->d_name, iface_entry->d_name,
					 "bInterfaceNumber", 16, &num);

			snprintf(link, sizeof(link), SYS_USB "/%s/driver",
				 iface_entry->d_name);
			n = readlink(link, driver, sizeof(driver) - 1);
			if (n > 0) {
				driver[n] = '\0';
				if (strrchr(driver, '/'))
					memmove(driver, strrchr(driver, '/') + 1,
						strlen(strrchr(driver, '/') + 1) + 1);
			} else {
				strcpy(driver, "-");
			}
			append(buf, len, &used, "\n  if%u %02x/%02x/%02x drv=%s",
			       num, cls, sub, proto, driver);
		}
		closedir(interfaces);
		append(buf, len, &used, "\n");
	}
	closedir(devices);
	return count;
}

int usbhost_read(struct usbhost *host, void *buf, size_t len, int timeout_ms)
{
	struct usbdevfs_bulktransfer bulk;
	int transferred;
	unsigned recoveries = 0;

	if (!host || host->fd < 0 || !buf || len == 0) {
		errno = EINVAL;
		return -1;
	}

	memset(&bulk, 0, sizeof(bulk));
	bulk.ep = (unsigned int)host->ep_in;
	bulk.len = (unsigned int)len;
	bulk.timeout = (unsigned int)timeout_ms;
	bulk.data = buf;

	for (;;) {
		transferred = ioctl(host->fd, USBDEVFS_BULK, &bulk);
		if (transferred >= 0 || errno != EPIPE || recoveries == 2)
			return transferred;
		/* Only clear the stalled endpoint; never reset the device or bus.
		 * A permanently stalled peer must still terminate this round. */
		recoveries++;
		if (ioctl(host->fd, USBDEVFS_CLEAR_HALT, &bulk.ep) < 0)
			return -1;
	}
}

int usbhost_write(struct usbhost *host, const void *buf, size_t len,
		  int timeout_ms)
{
	struct usbdevfs_bulktransfer bulk;
	int transferred;

	if (!host || host->fd < 0 || !buf || len == 0) {
		errno = EINVAL;
		return -1;
	}

	memset(&bulk, 0, sizeof(bulk));
	bulk.ep = (unsigned int)host->ep_out;
	bulk.len = (unsigned int)len;
	bulk.timeout = (unsigned int)timeout_ms;
	bulk.data = (void *)buf;

	transferred = ioctl(host->fd, USBDEVFS_BULK, &bulk);
	if (transferred < 0 && errno == EPIPE) {
		/* usbfs hides actual_length on a failed synchronous OUT URB.
		 * Clear the halt once, but never replay an uncertain prefix. The
		 * caller must retire the damaged byte stream even if this succeeds. */
		if (ioctl(host->fd, USBDEVFS_CLEAR_HALT, &bulk.ep) < 0)
			return -1;
		errno = EPIPE;
	}
	return transferred;
}

int usbhost_control_in(struct usbhost *host, uint8_t request, uint16_t value,
		       uint16_t index, void *buf, size_t len, int timeout_ms)
{
	struct usbdevfs_ctrltransfer ctrl;
	int transferred;

	if (!host || host->fd < 0 || !buf) {
		errno = EINVAL;
		return -1;
	}

	memset(&ctrl, 0, sizeof(ctrl));
	ctrl.bRequestType = 0xc0;	/* device-to-host, vendor, device */
	ctrl.bRequest = request;
	ctrl.wValue = value;
	ctrl.wIndex = index;
	ctrl.wLength = (uint16_t)len;
	ctrl.timeout = (unsigned int)timeout_ms;
	ctrl.data = buf;

	transferred = ioctl(host->fd, USBDEVFS_CONTROL, &ctrl);
	if (transferred < 0)
		return -1;
	return transferred;
}

int usbhost_control_out(struct usbhost *host, uint8_t request, uint16_t value,
			uint16_t index, const void *buf, size_t len,
			int timeout_ms)
{
	struct usbdevfs_ctrltransfer ctrl;
	int transferred;

	if (!host || host->fd < 0) {
		errno = EINVAL;
		return -1;
	}

	memset(&ctrl, 0, sizeof(ctrl));
	ctrl.bRequestType = 0x40;	/* host-to-device, vendor, device */
	ctrl.bRequest = request;
	ctrl.wValue = value;
	ctrl.wIndex = index;
	ctrl.wLength = (uint16_t)len;
	ctrl.timeout = (unsigned int)timeout_ms;
	ctrl.data = (void *)buf;

	transferred = ioctl(host->fd, USBDEVFS_CONTROL, &ctrl);
	if (transferred < 0)
		return -1;
	return transferred;
}
