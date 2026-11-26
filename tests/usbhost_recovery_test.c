#include "usbhost.h"

#include <assert.h>
#include <errno.h>
#include <stdio.h>

int main(void)
{
	assert(usbhost_reconnect_candidate(ENODEV, 0));
	assert(usbhost_reconnect_candidate(ESHUTDOWN, 0));
	assert(usbhost_reconnect_candidate(ENXIO, 0));
	assert(!usbhost_reconnect_candidate(EPROTO, 0));
	assert(usbhost_reconnect_candidate(EPROTO, 1));
	assert(usbhost_reconnect_candidate(EPIPE, 1));
	assert(!usbhost_reconnect_candidate(EPIPE, 0));
	assert(!usbhost_reconnect_candidate(ENETDOWN, 1));
	puts("usbhost recovery policy tests passed");
	return 0;
}
