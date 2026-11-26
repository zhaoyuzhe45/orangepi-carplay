/*
 * box_role_test.c -- role decision and the daemon -> supervisor hand-off.
 *
 * The interesting cases are the ones that must *not* switch: an IN-direction
 * read of 0x51, a 0x52 configuration switch, or a 0x51 with an unexpected
 * wValue.  Getting any of those wrong would flip the box to host in the
 * middle of a session that was working fine.
 */
#define _GNU_SOURCE
#include "box_role.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static int failures;

#define EXPECT(condition, message) do { \
    if (!(condition)) { \
        fprintf(stderr, "FAIL: %s (%s:%d)\n", message, __FILE__, __LINE__); \
        failures++; \
    } \
} while (0)

static int make_test_dir(char *path, size_t capacity)
{
    const char *base = getenv("TMPDIR");
    int n = snprintf(path, capacity, "%s/box-role-%ld",
                     base ? base : "/data/local/tmp", (long)getpid());

    if (n < 0 || (size_t)n >= capacity)
        return -1;
    if (mkdir(path, 0700) < 0 && errno != EEXIST)
        return -1;
    return 0;
}

static void test_request_mapping(void)
{
    /* The swap: device-directed OUT, wValue 1 means "become the host". */
    EXPECT(box_role_from_request(0x40, 0x51, 1) == BOX_ROLE_HOST,
           "0x40/0x51 wValue=1 asks for host");
    EXPECT(box_role_from_request(0x40, 0x51, 0) == BOX_ROLE_DEVICE,
           "0x40/0x51 wValue=0 asks for device");

    /*
     * 0xc0/0x51 is the *read* form of the same request number.  Treating it
     * as a swap would flip roles every time a host polls the current mode.
     */
    EXPECT(box_role_from_request(0xC0, 0x51, 1) < 0,
           "the IN form of 0x51 is not a role swap");
    EXPECT(box_role_from_request(0x41, 0x51, 1) < 0,
           "interface-directed vendor request must not switch the device");
    EXPECT(box_role_from_request(0x42, 0x51, 1) < 0,
           "endpoint-directed vendor request must not switch the device");

    /* Neighbouring vendor requests must not be mistaken for the swap. */
    EXPECT(box_role_from_request(0x40, 0x53, 0) < 0, "0x53 is not a role swap");
    EXPECT(box_role_from_request(0x40, 0x52, 4) < 0, "0x52 is not a role swap");
    EXPECT(box_role_from_request(0x40, 0x45, 0) < 0, "0x45 is not a role swap");
    EXPECT(box_role_from_request(0x40, 0x40, 0) < 0, "0x40 is not a role swap");

    /* An unknown wValue is not a decision we are entitled to make. */
    EXPECT(box_role_from_request(0x40, 0x51, 2) < 0,
           "unknown wValue leaves the role untouched");

    EXPECT(strcmp(box_role_name(BOX_ROLE_HOST), "host") == 0, "host name");
    EXPECT(strcmp(box_role_name(BOX_ROLE_DEVICE), "device") == 0, "device name");
}

static void test_handoff_roundtrip(const char *dir)
{
    char path[256];
    char buf[32];
    int fd;
    ssize_t n;

    snprintf(path, sizeof(path), "%s/role_request", dir);

    /* Nothing pending: -1, not "device".  A stale default would switch the
     * box on every supervisor tick. */
    EXPECT(box_role_request_take(path) < 0, "no request yields -1");

    EXPECT(box_role_request_write(path, BOX_ROLE_HOST) == 0,
           "write a host request");

    /* The supervisor reads it with the shell, so the format has to be one
     * word plus a newline. */
    fd = open(path, O_RDONLY);
    EXPECT(fd >= 0, "request file exists after write");
    if (fd >= 0) {
        n = read(fd, buf, sizeof(buf) - 1);
        close(fd);
        if (n < 0)
            n = 0;
        buf[n] = '\0';
        EXPECT(strcmp(buf, "host\n") == 0, "on-disk format is 'host\\n'");
    }

    EXPECT(box_role_request_take(path) == BOX_ROLE_HOST, "take returns host");
    /* Consume-on-read: the supervisor must not act twice on one hand-off. */
    EXPECT(access(path, F_OK) != 0, "take removes the request");
    EXPECT(box_role_request_take(path) < 0, "second take yields -1");
}

static void test_handoff_device_and_garbage(const char *dir)
{
    char path[256];
    FILE *f;

    snprintf(path, sizeof(path), "%s/role_request2", dir);

    EXPECT(box_role_request_write(path, BOX_ROLE_DEVICE) == 0,
           "write a device request");
    EXPECT(box_role_request_take(path) == BOX_ROLE_DEVICE,
           "take returns device");

    /* A truncated or corrupted file must not be read as "host". */
    f = fopen(path, "w");
    EXPECT(f != NULL, "create a corrupt request");
    if (f) {
        fputs("hos", f);
        fclose(f);
        EXPECT(box_role_request_take(path) < 0,
               "a truncated role is rejected");
    }

    /* Write must survive being pointed at a directory that cannot be
     * written, without leaving a temp file behind. */
    EXPECT(box_role_request_write("/nonexistent-dir/role_request",
                                  BOX_ROLE_HOST) < 0,
           "unwritable path fails cleanly");
}

int main(void)
{
    char dir[128];

    test_request_mapping();

    if (make_test_dir(dir, sizeof(dir)) != 0) {
        fprintf(stderr, "FAIL: cannot create test dir\n");
        return 1;
    }
    test_handoff_roundtrip(dir);
    test_handoff_device_and_garbage(dir);
    {
        char path[256];
        const char *invalid[] = { "hostile\n", "device-junk\n", "host\nextra", "host\0extra" };
        size_t sizes[] = { 8, 12, 10, 10 };
        size_t i;
        snprintf(path, sizeof(path), "%s/invalid", dir);
        for (i = 0; i < sizeof(invalid) / sizeof(invalid[0]); i++) {
            FILE *f = fopen(path, "wb");
            EXPECT(f != NULL, "create invalid role");
            if (f) {
                fwrite(invalid[i], 1, sizes[i], f);
                fclose(f);
                EXPECT(box_role_request_take(path) < 0, "reject trailing bytes in role request");
            }
        }
        EXPECT(box_role_request_write(path, (enum box_role)99) < 0,
               "invalid enum cannot silently request device role");
        unlink(path);
    }

    /* Leave nothing behind: this runs on the device under /data/local/tmp. */
    {
        char path[256];

        snprintf(path, sizeof(path), "%s/role_request", dir);
        unlink(path);
        snprintf(path, sizeof(path), "%s/role_request2", dir);
        unlink(path);
        snprintf(path, sizeof(path), "%s/role_request.tmp", dir);
        unlink(path);
        rmdir(dir);
    }

    if (failures) {
        fprintf(stderr, "%d failure(s)\n", failures);
        return 1;
    }
    printf("box_role_test: all checks passed\n");
    return 0;
}
