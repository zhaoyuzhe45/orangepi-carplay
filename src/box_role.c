/*
 * box_role.c -- role decision and the daemon -> supervisor hand-off.
 */
#define _GNU_SOURCE
#include "box_role.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#define VENDOR_OUT	0x40
#define VENDOR_IN	0xC0
#define TYPE_MASK	0x60u
#define TYPE_VENDOR	0x40u
#define REQ_ROLE_SWAP	0x51

int box_role_from_request(unsigned bm_request_type, unsigned b_request,
			  unsigned w_value)
{
	/*
	 * Only the device-directed OUT form carries the swap.  A 0xc0 read of
	 * the same request number is a different thing (it reports the current
	 * mode), so the direction bit is part of the test, not a formality.
	 */
	if (bm_request_type != VENDOR_OUT)
		return -1;
	if (b_request != REQ_ROLE_SWAP)
		return -1;

	switch (w_value) {
	case 1:
		return BOX_ROLE_HOST;
	case 0:
		return BOX_ROLE_DEVICE;
	default:
		return -1;
	}
}

const char *box_role_name(enum box_role role)
{
	return role == BOX_ROLE_HOST ? "host" : "device";
}

int box_role_request_write(const char *path, enum box_role role)
{
	char tmp[256];
	int fd;
	int n;
	ssize_t written;
	size_t used = 0;
	char value[8];

	if (!path || !*path || (role != BOX_ROLE_HOST && role != BOX_ROLE_DEVICE))
		return -1;
	n = snprintf(tmp, sizeof(tmp), "%s.tmp", path);
	if (n < 0 || (size_t)n >= sizeof(tmp))
		return -1;

	fd = open(tmp, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0600);
	if (fd < 0)
		return -1;

	/*
	 * "host\n" / "device\n": a trailing newline keeps the file readable by
	 * the shell half (`read role < ...`), which is what the supervisor
	 * uses.  Written through a temp file so a reader never observes a
	 * partially written value.
	 */
	n = snprintf(value, sizeof(value), "%s\n", box_role_name(role));
	while (used < (size_t)n) {
		written = write(fd, value + used, (size_t)n - used);
		if (written < 0 && errno == EINTR)
			continue;
		if (written <= 0)
			break;
		used += (size_t)written;
	}
	close(fd);

	if (used != (size_t)n) {
		unlink(tmp);
		return -1;
	}
	if (rename(tmp, path) < 0) {
		unlink(tmp);
		return -1;
	}
	return 0;
}

int box_role_request_take(const char *path)
{
	char buf[16];
	ssize_t n;
	int fd;
	int role = -1;

	if (!path || !*path)
		return -1;

	fd = open(path, O_RDONLY | O_CLOEXEC);
	if (fd < 0)
		return -1;

	n = read(fd, buf, sizeof(buf) - 1);
	close(fd);
	/* Consume-on-read: a stale request must not fire twice. */
	unlink(path);

	if (n <= 0)
		return -1;
	buf[n] = '\0';

	if ((n == 4 && memcmp(buf, "host", 4) == 0) ||
	    (n == 5 && memcmp(buf, "host\n", 5) == 0))
		role = BOX_ROLE_HOST;
	else if ((n == 6 && memcmp(buf, "device", 6) == 0) ||
	         (n == 7 && memcmp(buf, "device\n", 7) == 0))
		role = BOX_ROLE_DEVICE;
	return role;
}
