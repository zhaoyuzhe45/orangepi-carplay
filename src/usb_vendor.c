#include "usb_vendor.h"

#define REQ_SET_CHARGE_CURRENT 0x40
#define REQ_GET_USB_MODE       0x45
#define REQ_SET_CARPLAY_MODE   0x51
#define REQ_CARPLAY_CONFIG     0x52
#define REQ_CARPLAY_SUPPORT    0x53

#define REQTYPE_VENDOR_IN      0xC0
#define REQTYPE_VENDOR_OUT     0x40

static void put_le32(uint8_t *out, uint32_t value)
{
	out[0] = (uint8_t)value;
	out[1] = (uint8_t)(value >> 8);
	out[2] = (uint8_t)(value >> 16);
	out[3] = (uint8_t)(value >> 24);
}

int usb_vendor_response(const struct usb_vendor_request *request,
			uint8_t *response, size_t capacity)
{
	if (!request || !response)
		return -1;

	if (request->bmRequestType == REQTYPE_VENDOR_IN) {
		switch (request->bRequest) {
		case REQ_CARPLAY_SUPPORT:
			if (capacity < 4)
				return -1;
			put_le32(response, 0x00000001u);
			return 4;
		case REQ_GET_USB_MODE:
			if (capacity < 4)
				return -1;
			/* 3:3:3:0 is usbmuxd's initial, unswitched mode. */
			response[0] = 0x03;
			response[1] = 0x03;
			response[2] = 0x03;
			response[3] = 0x00;
			return 4;
		case REQ_CARPLAY_CONFIG:
			if (capacity < 1)
				return -1;
			response[0] = 0x00;
			return 1;
		default:
			return -1;
		}
	}

	if (request->bmRequestType == REQTYPE_VENDOR_OUT) {
		switch (request->bRequest) {
		case REQ_SET_CARPLAY_MODE:
			/*
			 * 0x40/0x51 is the USB role swap and it *does* carry a
			 * 4-byte OUT data stage.  It cannot be answered from this
			 * table -- returning 0 here would leave the payload
			 * unconsumed and hang the control pipe for five seconds.
			 * handle_setup() intercepts it ahead of us and reads the
			 * data stage; anything reaching this point is a caller
			 * that has not been taught about it yet.
			 */
			return -1;
		case REQ_SET_CHARGE_CURRENT:
			/* No data stage: letting FunctionFS run the status stage
			 * is the whole answer. */
			return 0;
		default:
			return -1;
		}
	}

	return -1;
}

const char *usb_vendor_request_name(uint8_t bRequest)
{
	switch (bRequest) {
	case REQ_SET_CHARGE_CURRENT:
		return "set-charging-current";
	case REQ_GET_USB_MODE:
		return "get-usb-mode";
	case REQ_SET_CARPLAY_MODE:
		return "set-carplay-mode";
	case REQ_CARPLAY_CONFIG:
		return "carplay-configuration";
	case REQ_CARPLAY_SUPPORT:
		return "carplay-support";
	default:
		return "unknown";
	}
}
