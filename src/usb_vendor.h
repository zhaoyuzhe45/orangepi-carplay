#ifndef USB_VENDOR_H
#define USB_VENDOR_H

#include <stddef.h>
#include <stdint.h>

/* Vendor control requests an Apple host uses while bringing up CarPlay.
 *
 * A real head unit walks these in order before it will accept the accessory:
 *   0xC0/0x53  CarPlay capability probe       -> 4 bytes, 0x00000001 = supported
 *   0xC0/0x45  usbmuxd mode read              -> 4 bytes, 3:3:3:0 = unswitched
 *   0x40/0x52  switch to CarPlay configuration-> 1 byte
 *   0x40/0x51  switch the accessory to host   -> **4-byte OUT data stage**
 * Without 0x53 answering "supported" the host stops right there and never
 * sends iAP2, so an accessory that only implements 0x52 looks dead on real
 * hardware even though it works against a lenient test peer.
 *
 * 0x51 is deliberately absent from usb_vendor_response(): its data stage has
 * to be consumed, so iap2d handles it in handle_setup() before the table is
 * consulted.  Answering it here would ACK a request whose payload is still
 * sitting in the endpoint -- the host then times out after five seconds.
 */
struct usb_vendor_request {
	uint8_t bmRequestType;
	uint8_t bRequest;
	uint16_t wValue;
	uint16_t wIndex;
	uint16_t wLength;
};

/* Fills `response` and returns the number of bytes the host expects back.
 * Returns 0 for a request that has no data stage (the caller only has to let
 * FunctionFS finish the status stage), and -1 when the request is not one of
 * ours and must be left alone. */
int usb_vendor_response(const struct usb_vendor_request *request,
			uint8_t *response, size_t capacity);

/* Short human-readable name for logging. Never returns NULL. */
const char *usb_vendor_request_name(uint8_t bRequest);

#endif /* USB_VENDOR_H */
