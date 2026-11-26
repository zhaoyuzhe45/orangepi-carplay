#define _POSIX_C_SOURCE 200809L
#include "airplay_timing.h"

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

struct airplay_timing {
    int fd;
    uint16_t port;
    struct sockaddr_storage peer;
    pthread_t thread;
    atomic_int stop;
    atomic_int error;
};

int airplay_timing_ntp_from_unix(int64_t seconds, long nanoseconds,
                               uint64_t *ntp)
{
    if (!ntp || nanoseconds < 0 || nanoseconds >= 1000000000L) {
        errno = EINVAL;
        return -1;
    }
    /* The wire seconds field wraps at the NTP era boundary in 2036. */
    *ntp = (((uint64_t)seconds + UINT64_C(2208988800)) << 32) |
        (((uint64_t)nanoseconds << 32) / UINT64_C(1000000000));
    return 0;
}

int airplay_timing_now(uint64_t *ntp)
{
    struct timespec now;
    if (clock_gettime(CLOCK_REALTIME, &now) < 0)
        return -1;
    return airplay_timing_ntp_from_unix((int64_t)now.tv_sec, now.tv_nsec, ntp);
}

static void write64(uint8_t *out, uint64_t value)
{
    for (int i = 7; i >= 0; i--) {
        out[i] = (uint8_t)value;
        value >>= 8;
    }
}

int airplay_timing_reply(const uint8_t *request, size_t request_len,
                         uint64_t received, uint64_t transmitted,
                         uint8_t reply[AIRPLAY_TIMING_PACKET_BYTES])
{
    if (!request || !reply || request_len != AIRPLAY_TIMING_PACKET_BYTES ||
        request[0] != 0x80 || request[1] != 0xd2 ||
        request[2] != 0 || request[3] != 7)
        return -1;
    memset(reply, 0, AIRPLAY_TIMING_PACKET_BYTES);
    reply[0] = 0x80;
    reply[1] = 0xd3;
    reply[3] = 7;
    memcpy(reply + 8, request + 24, 8);
    write64(reply + 16, received);
    write64(reply + 24, transmitted);
    return 0;
}

static int peer_matches(const struct sockaddr_storage *expected,
                         const struct sockaddr_storage *sender)
{
    if (expected->ss_family != sender->ss_family)
        return 0;
    if (expected->ss_family == AF_INET) {
        const struct sockaddr_in *a = (const struct sockaddr_in *)expected;
        const struct sockaddr_in *b = (const struct sockaddr_in *)sender;
        return a->sin_addr.s_addr == b->sin_addr.s_addr;
    }
    if (expected->ss_family == AF_INET6) {
        const struct sockaddr_in6 *a = (const struct sockaddr_in6 *)expected;
        const struct sockaddr_in6 *b = (const struct sockaddr_in6 *)sender;
        return memcmp(&a->sin6_addr, &b->sin6_addr, sizeof(a->sin6_addr)) == 0 &&
            (!IN6_IS_ADDR_LINKLOCAL(&a->sin6_addr) ||
             a->sin6_scope_id == b->sin6_scope_id);
    }
    return 0;
}

static void *timing_main(void *opaque)
{
    struct airplay_timing *timing = opaque;
    while (!atomic_load(&timing->stop)) {
        struct pollfd pfd = { .fd = timing->fd, .events = POLLIN };
        struct sockaddr_storage sender;
        socklen_t sender_len = sizeof(sender);
        uint8_t request[AIRPLAY_TIMING_PACKET_BYTES + 1];
        uint8_t reply[AIRPLAY_TIMING_PACKET_BYTES];
        uint64_t received, transmitted;
        ssize_t n;
        int ready = poll(&pfd, 1, 200);
        if (ready < 0 && errno == EINTR)
            continue;
        if (ready < 0 || (ready > 0 && (pfd.revents & (POLLERR | POLLHUP | POLLNVAL))))
            break;
        if (!ready || atomic_load(&timing->stop))
            continue;
        memset(&sender, 0, sizeof(sender));
        n = recvfrom(timing->fd, request, sizeof(request), 0,
                     (struct sockaddr *)&sender, &sender_len);
        if (n < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR)
                continue;
            break;
        }
        if (airplay_timing_now(&received) < 0)
            break;
        if (!peer_matches(&timing->peer, &sender))
            continue;
        if (airplay_timing_now(&transmitted) < 0)
            break;
        if (airplay_timing_reply(request, (size_t)n, received, transmitted, reply) < 0)
            continue;
        n = sendto(timing->fd, reply, sizeof(reply), 0,
                    (struct sockaddr *)&sender, sender_len);
        if (n != (ssize_t)sizeof(reply)) {
            if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR))
                continue;
            break;
        }
    }
    if (!atomic_load(&timing->stop))
        atomic_store(&timing->error, errno ? errno : EIO);
    return NULL;
}

int airplay_timing_start(struct airplay_timing **out, int control_fd)
{
    struct airplay_timing *timing;
    struct sockaddr_storage local;
    socklen_t local_len = sizeof(local), peer_len;
    int error, flags;
    uint64_t now;
    if (!out || *out) {
        errno = EINVAL;
        return -1;
    }
    timing = calloc(1, sizeof(*timing));
    if (!timing)
        return -1;
    timing->fd = -1;
    atomic_init(&timing->stop, 0);
    atomic_init(&timing->error, 0);
    peer_len = sizeof(timing->peer);
    memset(&local, 0, sizeof(local));
    if (getsockname(control_fd, (struct sockaddr *)&local, &local_len) < 0 ||
        getpeername(control_fd, (struct sockaddr *)&timing->peer, &peer_len) < 0 ||
        airplay_timing_now(&now) < 0)
        goto fail;
    if (local.ss_family != timing->peer.ss_family ||
        (local.ss_family != AF_INET && local.ss_family != AF_INET6)) {
        errno = EAFNOSUPPORT;
        goto fail;
    }
    if (local.ss_family == AF_INET) {
        ((struct sockaddr_in *)&local)->sin_port = 0;
    } else {
        struct sockaddr_in6 *a = (struct sockaddr_in6 *)&local;
        struct sockaddr_in6 *b = (struct sockaddr_in6 *)&timing->peer;
        a->sin6_port = 0;
        if (IN6_IS_ADDR_LINKLOCAL(&a->sin6_addr) && !a->sin6_scope_id)
            a->sin6_scope_id = b->sin6_scope_id;
        if (IN6_IS_ADDR_LINKLOCAL(&b->sin6_addr) && !b->sin6_scope_id)
            b->sin6_scope_id = a->sin6_scope_id;
        if ((IN6_IS_ADDR_LINKLOCAL(&a->sin6_addr) && !a->sin6_scope_id) ||
            (IN6_IS_ADDR_LINKLOCAL(&b->sin6_addr) && !b->sin6_scope_id)) {
            errno = EINVAL;
            goto fail;
        }
    }
    timing->fd = socket(local.ss_family, SOCK_DGRAM, 0);
    if (timing->fd < 0)
        goto fail;
    flags = fcntl(timing->fd, F_GETFL, 0);
    if (flags < 0 || fcntl(timing->fd, F_SETFL, flags | O_NONBLOCK) < 0 ||
        fcntl(timing->fd, F_SETFD, FD_CLOEXEC) < 0 ||
        bind(timing->fd, (struct sockaddr *)&local, local_len) < 0 ||
        getsockname(timing->fd, (struct sockaddr *)&local, &local_len) < 0)
        goto fail;
    timing->port = ntohs(local.ss_family == AF_INET ?
        ((struct sockaddr_in *)&local)->sin_port :
        ((struct sockaddr_in6 *)&local)->sin6_port);
    if (!timing->port) {
        errno = EADDRNOTAVAIL;
        goto fail;
    }
    error = pthread_create(&timing->thread, NULL, timing_main, timing);
    if (error) {
        errno = error;
        goto fail;
    }
    *out = timing;
    return 0;
fail:
    error = errno;
    if (timing->fd >= 0)
        close(timing->fd);
    free(timing);
    errno = error;
    return -1;
}

uint16_t airplay_timing_port(const struct airplay_timing *timing)
{
    return timing ? timing->port : 0;
}

int airplay_timing_error(const struct airplay_timing *timing)
{
    return timing ? atomic_load(&timing->error) : 0;
}

void airplay_timing_stop(struct airplay_timing **out)
{
    struct airplay_timing *timing;
    if (!out || !*out)
        return;
    timing = *out;
    atomic_store(&timing->stop, 1);
    pthread_join(timing->thread, NULL);
    close(timing->fd);
    free(timing);
    *out = NULL;
}
