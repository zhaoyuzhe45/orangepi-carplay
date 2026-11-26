/*
 * usbhost.h -- drive a head unit over usbfs instead of being driven by it.
 *
 * In the normal (device) shape this box is a gadget: iap2d reads endpoint
 * files under /dev/usb-ffs and the peer enumerates us.  After a 0x51 role
 * swap the bus belongs to us, so the same iAP2/AirPlay code has to talk to
 * endpoints that belong to somebody else.  That is what this module wraps:
 * find the newly enumerated device, claim its iAP2 vendor interface, and
 * expose bulk reads/writes with roughly the same shape as an endpoint fd.
 *
 * Only the iAP2 channel lives here.  The NCM data path is handed to the
 * kernel: once cdc_ncm binds the head unit's network function we get a
 * normal netdev and configure it the same way we configure ncm.usb0 today.
 */
#ifndef USBHOST_H
#define USBHOST_H

#include <stddef.h>
#include <stdint.h>
#include <errno.h>

/* Apple's iAP2 interface: vendor specific class, MFi accessory subclass.
 * 0xff/0xf0/0x00 is what a real iPhone exposes; our own box uses
 * 0xff/0xfe/0x02.  Both are accepted because a head unit is free to copy
 * either layout, and the bulk pair is what actually matters. */
#define USBHOST_IAP2_CLASS	0xff
#define USBHOST_IAP2_SUBCLASS_MFI	0xf0
#define USBHOST_IAP2_SUBCLASS_FFS	0xfe

struct usbhost {
	int fd;		/* /dev/bus/usb/BBB/DDD, -1 when closed */
	int busnum;
	int devnum;
	int interface;	/* claimed bInterfaceNumber */
	int ep_in;	/* bEndpointAddress, e.g. 0x85 */
	int ep_out;	/* bEndpointAddress, e.g. 0x04 */
	int ep_in_max;	/* wMaxPacketSize of the IN endpoint */
	int ep_out_max;
	uint16_t vid;
	uint16_t pid;
	/* sysfs device name, e.g. "1-1".  Kept because the NCM netdev has to be
	 * matched back to this device: /sys/class/net/<if>/device points at the
	 * interface ("1-1:1.2"), whose name starts with this one.  busnum/devnum
	 * cannot be used for that -- they are usbfs addressing, not sysfs. */
	char sysfs_name[32];
};

/* Looks for a device exposing an iAP2 vendor interface and claims it.
 *
 * `exclude_vid`/`exclude_pid` skip our own gadget (the box still shows up as
 * 05ac:12a8 while the gadget is bound).  The filter is *only* applied while
 * our UDC is actually bound: once the gadget is unbound nothing on the bus is
 * us, and a head unit is free to present the same IDs -- they are trivial to
 * forge, which is what this box itself does.
 *
 * The endpoints are read from whatever alternate setting the device is
 * currently in, because sysfs only lists the endpoints of the active setting.
 * SET_INTERFACE(1) is issued only when that read finds no bulk pair: iAP2 is
 * allowed to idle on a zero-bandwidth setting 0, but on this class of gadget
 * a SET_INTERFACE that lands right after SET_CONFIGURATION is answered with
 * -71 and the peer drops off the bus (orangecar, docs/protocol.md 4.1).  So
 * the harmless default is "look first, ask the device only if we must".
 *
 * Returns 0 on success, -1 when nothing matches, -2 when a candidate was
 * found but could not be opened or claimed (errno is set). */
int usbhost_open(struct usbhost *host, uint16_t exclude_vid,
		 uint16_t exclude_pid);

/* Whether a failed host USB round should re-enumerate the peer. ENODEV,
 * ESHUTDOWN and ENXIO always describe loss of the target. After successful
 * iAP2 authentication, give EPROTO/EPIPE a bounded chance to re-enumerate.
 * Those errors alone do not prove that the car intentionally reconfigured
 * USB; the field capture already had a working NCM interface before EPROTO.
 * Before authentication they remain fatal policy errors so a bad peer cannot
 * create a reconnect loop. */
static inline int usbhost_reconnect_candidate(int error, int post_auth)
{
	return error == ENODEV || error == ESHUTDOWN || error == ENXIO ||
	       (post_auth && (error == EPROTO || error == EPIPE));
}

void usbhost_close(struct usbhost *host);

/* Bulk transfers.  Both return the byte count, or -1 on error (errno set;
 * ETIMEDOUT for a NAK that outlived `timeout_ms`). Read retries EPIPE after
 * endpoint clear-halt at most twice. Write clears an OUT halt once but does
 * not retry any error: usbfs does not report partial output on failure, so
 * the caller must retire that byte stream instead of replaying the buffer.
 * Positive short writes report exactly the prefix accepted by usbfs. */
int usbhost_read(struct usbhost *host, void *buf, size_t len, int timeout_ms);
int usbhost_write(struct usbhost *host, const void *buf, size_t len,
		  int timeout_ms);

/* Vendor control transfers, used for the opening handshake when we are the
 * host: 0xc0/0x53 capability probe, 0xc0/0x45 mode read, 0x40/0x52 CarPlay
 * configuration switch.  Returns the transferred byte count or -1. */
int usbhost_control_in(struct usbhost *host, uint8_t request, uint16_t value,
		       uint16_t index, void *buf, size_t len, int timeout_ms);
int usbhost_control_out(struct usbhost *host, uint8_t request, uint16_t value,
			uint16_t index, const void *buf, size_t len,
			int timeout_ms);

/* True once the device has an iAP2 interface we can claim. */
int usbhost_present(void);

/* Returns the number of root buses belonging to the role-switched
 * controller.  A positive value means xHCI has registered the host bus;
 * it does not imply that a downstream device has enumerated yet. */
int usbhost_role_bus_count(void);

/* Renders devices on the selected controller into `buf`, one device
 * per line with its interfaces (class/subclass/protocol) and the kernel
 * driver that owns each one. Uses the same bus filter as usbhost_open();
 * an unset CARPLAY_OTG_UDC includes all buses. Returns the device count, or -1. Used to tell
 * "the bus is empty" apart from "a peer is there and we do not accept it". */
int usbhost_snapshot(char *buf, size_t len);

#endif /* USBHOST_H */
