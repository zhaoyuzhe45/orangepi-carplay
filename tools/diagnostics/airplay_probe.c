#define _GNU_SOURCE
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <net/if.h>
#include <netinet/in.h>
#include <poll.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>

static int wait_connect(int fd, int timeout_ms)
{
    struct pollfd pfd = { .fd = fd, .events = POLLOUT };
    int n;
    socklen_t sl = sizeof(int);
    int err = 0;

    for (;;) {
        n = poll(&pfd, 1, timeout_ms);
        if (n < 0 && errno == EINTR)
            continue;
        break;
    }
    if (n == 0) {
        fprintf(stderr, "probe: timeout after %d ms\n", timeout_ms);
        return -1;
    }
    if (n < 0) {
        fprintf(stderr, "probe: poll: %s\n", strerror(errno));
        return -1;
    }
    if (getsockopt(fd, SOL_SOCKET, SO_ERROR, &err, &sl) < 0) {
        fprintf(stderr, "probe: SO_ERROR: %s\n", strerror(errno));
        return -1;
    }
    if (err) {
        fprintf(stderr, "probe: connect: %s\n", strerror(err));
        return -1;
    }
    return 0;
}

static int probe_ipv6(const char *ifname, const char *src, const char *dst, int port)
{
    struct sockaddr_in6 a;
    struct timeval tv = { .tv_sec = 8, .tv_usec = 0 };
    char buf[1024];
    unsigned idx = if_nametoindex(ifname);
    int fd, flags, n;

    fd = socket(AF_INET6, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (fd < 0) { perror("socket6"); return -1; }
    if (!idx) { perror("if_nametoindex"); close(fd); return -1; }
    if (setsockopt(fd, SOL_SOCKET, SO_BINDTODEVICE, ifname, strlen(ifname) + 1) < 0)
        perror("SO_BINDTODEVICE");
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));

    memset(&a, 0, sizeof(a));
    a.sin6_family = AF_INET6;
    a.sin6_port = 0;
    a.sin6_scope_id = idx;
    if (inet_pton(AF_INET6, src, &a.sin6_addr) != 1) return -1;
    if (bind(fd, (struct sockaddr *)&a, sizeof(a)) < 0) {
        perror("bind6"); close(fd); return -1;
    }
    memset(&a, 0, sizeof(a));
    a.sin6_family = AF_INET6;
    a.sin6_port = htons((uint16_t)port);
    a.sin6_scope_id = idx;
    if (inet_pton(AF_INET6, dst, &a.sin6_addr) != 1) return -1;

    flags = fcntl(fd, F_GETFL, 0);
    fcntl(fd, F_SETFL, flags | O_NONBLOCK);
    fprintf(stderr, "probe6: connecting src=[%s%%%s] dst=[%s%%%s]:%d\n", src, ifname, dst, ifname, port);
    n = connect(fd, (struct sockaddr *)&a, sizeof(a));
    if (n < 0 && errno != EINPROGRESS) { perror("connect6"); close(fd); return -1; }
    if (wait_connect(fd, 12000) < 0) { close(fd); return -1; }
    fprintf(stderr, "probe6: CONNECTED\n");
    n = recv(fd, buf, sizeof(buf), 0);
    if (n > 0) {
        fprintf(stderr, "probe6: first RX bytes=%d hex=", n);
        for (int i = 0; i < n && i < 32; i++) fprintf(stderr, "%02x", (unsigned char)buf[i]);
        fprintf(stderr, "\n");
    } else {
        fprintf(stderr, "probe6: connected socket, recv=%d errno=%s\n", n, strerror(errno));
    }
    sleep(2);
    close(fd);
    return 0;
}

static int probe_ipv4(const char *ifname, const char *src, const char *dst, int port)
{
    struct sockaddr_in a;
    struct timeval tv = { .tv_sec = 8, .tv_usec = 0 };
    char buf[1024];
    int fd, flags, n;

    fd = socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (fd < 0) { perror("socket4"); return -1; }
    if (setsockopt(fd, SOL_SOCKET, SO_BINDTODEVICE, ifname, strlen(ifname) + 1) < 0)
        perror("SO_BINDTODEVICE");
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));

    memset(&a, 0, sizeof(a));
    a.sin_family = AF_INET;
    a.sin_port = 0;
    if (inet_pton(AF_INET, src, &a.sin_addr) != 1) return -1;
    if (bind(fd, (struct sockaddr *)&a, sizeof(a)) < 0) { perror("bind4"); close(fd); return -1; }

    memset(&a, 0, sizeof(a));
    a.sin_family = AF_INET;
    a.sin_port = htons((uint16_t)port);
    if (inet_pton(AF_INET, dst, &a.sin_addr) != 1) return -1;
    flags = fcntl(fd, F_GETFL, 0);
    fcntl(fd, F_SETFL, flags | O_NONBLOCK);
    fprintf(stderr, "probe4: connecting src=%s@%s dst=%s:%d\n", src, ifname, dst, port);
    n = connect(fd, (struct sockaddr *)&a, sizeof(a));
    if (n < 0 && errno != EINPROGRESS) { perror("connect4"); close(fd); return -1; }
    if (wait_connect(fd, 12000) < 0) { close(fd); return -1; }
    fprintf(stderr, "probe4: CONNECTED\n");
    n = recv(fd, buf, sizeof(buf), 0);
    fprintf(stderr, "probe4: connected socket, recv=%d errno=%s\n", n, strerror(errno));
    sleep(2);
    close(fd);
    return 0;
}

int main(int argc, char **argv)
{
    int port = 7000;
    if (argc < 2) {
        fprintf(stderr, "usage: %s 4 [src dst] | 6 [src dst] [ifname]\n", argv[0]);
        return 2;
    }
    if (!strcmp(argv[1], "4")) {
        const char *src = argc > 2 ? argv[2] : "169.254.0.1";
        const char *dst = argc > 3 ? argv[3] : "169.254.0.2";
        return probe_ipv4(argc > 4 ? argv[4] : "iap2ncm0", src, dst, port) == 0 ? 0 : 1;
    }
    if (!strcmp(argv[1], "6")) {
        const char *src = argc > 2 ? argv[2] : "fe80::1";
        const char *dst = argc > 3 ? argv[3] : "fe80::2";
        return probe_ipv6(argc > 4 ? argv[4] : "iap2ncm0", src, dst, port) == 0 ? 0 : 1;
    }
    return 2;
}
