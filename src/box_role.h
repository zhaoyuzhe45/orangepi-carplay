/*
 * box_role.h -- which side of the USB link we are supposed to be.
 *
 * Wired CarPlay starts with the head unit as host and the accessory as
 * device, then the head unit sends a vendor request (0x40/0x51) asking the
 * accessory to take over as host.  Everything after that -- iAP2, NCM,
 * AirPlay -- runs with the roles reversed.
 *
 * This box supports both shapes on purpose:
 *
 *   BOX_ROLE_DEVICE  we stay a gadget and let the peer drive us.  This is
 *                    the shape DiPlay uses (Android host), and it stays the
 *                    default so the existing regression bench keeps working.
 *   BOX_ROLE_HOST    we take the bus after a 0x51, enumerate the head unit
 *                    and drive it the way an iPhone would.
 *
 * The daemon sees 0x51 on ep0 but must not touch dwc3: switching roles means
 * unbinding the gadget the daemon is using.  So the decision is persisted to
 * a small file and the supervisor owns the actual switch.
 */
#ifndef BOX_ROLE_H
#define BOX_ROLE_H

#include <stddef.h>

enum box_role {
	BOX_ROLE_DEVICE = 0,
	BOX_ROLE_HOST = 1,
};

/* Maps an Apple vendor control request to the role it asks for.
 *
 * Returns BOX_ROLE_HOST for 0x40/0x51 wValue=1, BOX_ROLE_DEVICE for
 * 0x40/0x51 wValue=0, and -1 for anything else (nothing to do). */
int box_role_from_request(unsigned bm_request_type, unsigned b_request,
			  unsigned w_value);

const char *box_role_name(enum box_role role);

/* Persisted hand-off between iap2d (sees 0x51 on ep0) and the supervisor
 * (owns dwc3_mode).  Writes atomically -- temp file plus rename -- so a
 * crash never leaves a half-written line for the supervisor to misread. */
int box_role_request_write(const char *path, enum box_role role);

/* Reads a pending request and removes it.  Returns the role, or -1 when
 * there is nothing pending or the file cannot be parsed. */
int box_role_request_take(const char *path);

#endif /* BOX_ROLE_H */
