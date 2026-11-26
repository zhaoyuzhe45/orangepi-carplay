#define _GNU_SOURCE
#include "carplay_time_sync.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>

static int read_now(int64_t *seconds, char *detail, size_t detail_capacity)
{
    struct timespec now;

    if (clock_gettime(CLOCK_REALTIME, &now) < 0) {
        snprintf(detail, detail_capacity, "clock_gettime errno=%d", errno);
        return -1;
    }
    *seconds = (int64_t)now.tv_sec;
    return 0;
}

int main(int argc, char **argv)
{
    char *end = NULL;
    long long requested;
    char detail[128];
    int rc;
    int64_t now_seconds;

    if (argc != 2) {
        fprintf(stderr, "usage: %s UNIX_UTC_SECONDS\n", argv[0]);
        return 2;
    }
    errno = 0;
    requested = strtoll(argv[1], &end, 10);
    if (errno || !end || *end) {
        fprintf(stderr, "invalid Unix time: %s\n", argv[1]);
        return 2;
    }

    rc = carplay_time_sync_apply_epoch(requested - 3, detail,
                                       sizeof(detail));
    printf("stale offset: rc=%d detail=%s\n", rc, detail);
    if (rc <= 0)
        return 1;
    if (read_now(&now_seconds, detail, sizeof(detail)) < 0) {
        fprintf(stderr, "%s\n", detail);
        return 1;
    }

    rc = carplay_time_sync_apply_epoch(requested, detail, sizeof(detail));
    printf("restore target: rc=%d detail=%s\n", rc, detail);
    if (rc <= 0)
        return 1;
    if (read_now(&now_seconds, detail, sizeof(detail)) < 0) {
        fprintf(stderr, "%s\n", detail);
        return 1;
    }
    if (now_seconds < requested - 2 || now_seconds > requested + 2) {
        fprintf(stderr, "verification failed: current=%lld target=%lld\n",
                (long long)now_seconds, requested);
        return 1;
    }
    printf("time sync device probe passed target=%lld current=%lld\n",
           requested, (long long)now_seconds);
    return 0;
}
