#define _POSIX_C_SOURCE 200809L
#include "airplay_timing.h"
#include <arpa/inet.h>
#include <dirent.h>
#include <errno.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

#define CHECK(x) do { if (!(x)) { fprintf(stderr, "FAIL line %d: %s (errno=%d)\n", __LINE__, #x, errno); exit(1); } } while (0)

static uint64_t read64(const uint8_t *p)
{
    uint64_t n = 0;
    for (int i = 0; i < 8; i++) n = (n << 8) | p[i];
    return n;
}

static void make_ping(uint8_t ping[32])
{
    memset(ping, 0, 32);
    ping[0] = 0x80; ping[1] = 0xd2; ping[3] = 7;
    for (int i = 0; i < 8; i++) ping[24+i] = (uint8_t)(0x12+i);
}

static void test_wire(void)
{
    uint8_t ping[33], reply[32];
    uint64_t ntp = 0;
    make_ping(ping);
    CHECK(airplay_timing_reply(ping, 32, UINT64_C(0x123456789abcdef0),
        UINT64_C(0xfedcba9876543210), reply) == 0);
    CHECK(reply[0] == 0x80 && reply[1] == 0xd3 && reply[2] == 0 && reply[3] == 7);
    CHECK(read64(reply + 8) == read64(ping + 24));
    CHECK(read64(reply + 16) == UINT64_C(0x123456789abcdef0));
    CHECK(read64(reply + 24) == UINT64_C(0xfedcba9876543210));
    CHECK(reply[4] == 0 && reply[5] == 0 && reply[6] == 0 && reply[7] == 0);
    for (size_t n = 0; n < 32; n++) CHECK(airplay_timing_reply(ping, n, 0, 0, reply) < 0);
    CHECK(airplay_timing_reply(ping, 33, 0, 0, reply) < 0);
    CHECK(airplay_timing_reply(NULL, 32, 0, 0, reply) < 0);
    CHECK(airplay_timing_reply(ping, 32, 0, 0, NULL) < 0);
    for (int i = 0; i < 4; i++) {
        uint8_t old = ping[i]; ping[i] ^= 1;
        CHECK(airplay_timing_reply(ping, 32, 0, 0, reply) < 0);
        ping[i] = old;
    }
    CHECK(airplay_timing_ntp_from_unix(0, 0, &ntp) == 0);
    CHECK(ntp == UINT64_C(0x83aa7e8000000000));
    CHECK(airplay_timing_ntp_from_unix(1, 500000000, &ntp) == 0);
    CHECK(ntp == UINT64_C(0x83aa7e8180000000));
    CHECK(airplay_timing_ntp_from_unix(0, -1, &ntp) < 0);
    CHECK(airplay_timing_ntp_from_unix(0, 1000000000L, &ntp) < 0);
    CHECK(airplay_timing_ntp_from_unix(0, 0, NULL) < 0);
    CHECK(airplay_timing_ntp_from_unix(2085978496LL, 0, &ntp) == 0 && ntp == 0);
    CHECK(airplay_timing_now(&ntp) == 0 && ntp != 0);
    puts("PASS: timing wire, malformed requests, originate echo, NTP endian/fraction/era");
}

static int entries(const char *path)
{
    DIR *dir = opendir(path);
    struct dirent *entry;
    int n = 0;
    CHECK(dir != NULL);
    while ((entry = readdir(dir)) != NULL)
        if (entry->d_name[0] != '.') n++;
    closedir(dir);
    return n;
}

static int readable(int fd, int timeout)
{
    struct pollfd p = { .fd = fd, .events = POLLIN };
    int r;
    do { r = poll(&p, 1, timeout); } while (r < 0 && errno == EINTR);
    CHECK(r >= 0);
    return r > 0 && (p.revents & POLLIN);
}

static void test_loopback(int family)
{
    struct sockaddr_storage addr;
    socklen_t len;
    int listener, client, server, udp, impostor = -1;
    struct airplay_timing *timing = NULL;
    uint8_t ping[32], reply[32];
    uint64_t before, after, received, transmitted;
    int base_fds = entries("/proc/self/fd");
    int base_threads = entries("/proc/self/task");
    memset(&addr, 0, sizeof(addr));
    if (family == AF_INET) {
        struct sockaddr_in *a = (struct sockaddr_in *)&addr;
        a->sin_family = AF_INET; a->sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        len = sizeof(*a);
    } else {
        struct sockaddr_in6 *a = (struct sockaddr_in6 *)&addr;
        a->sin6_family = AF_INET6; a->sin6_addr = in6addr_loopback;
        len = sizeof(*a);
    }
    listener = socket(family, SOCK_STREAM, 0); CHECK(listener >= 0);
    CHECK(bind(listener, (struct sockaddr *)&addr, len) == 0);
    CHECK(listen(listener, 1) == 0);
    CHECK(getsockname(listener, (struct sockaddr *)&addr, &len) == 0);
    client = socket(family, SOCK_STREAM, 0); CHECK(client >= 0);
    CHECK(connect(client, (struct sockaddr *)&addr, len) == 0);
    server = accept(listener, NULL, NULL); CHECK(server >= 0);
    for (int cycle = 0; cycle < 5; cycle++) {
        CHECK(airplay_timing_start(&timing, server) == 0);
        CHECK(timing != NULL && airplay_timing_port(timing) != 0);
        CHECK(airplay_timing_error(timing) == 0);
        if (family == AF_INET) ((struct sockaddr_in *)&addr)->sin_port = htons(airplay_timing_port(timing));
        else ((struct sockaddr_in6 *)&addr)->sin6_port = htons(airplay_timing_port(timing));
        udp = socket(family, SOCK_DGRAM, 0); CHECK(udp >= 0);
        CHECK(connect(udp, (struct sockaddr *)&addr, len) == 0);
        make_ping(ping);
        CHECK(airplay_timing_now(&before) == 0);
        CHECK(send(udp, ping, sizeof(ping), 0) == 32);
        CHECK(readable(udp, 1000));
        CHECK(recv(udp, reply, sizeof(reply), 0) == 32);
        CHECK(airplay_timing_now(&after) == 0);
        received = read64(reply + 16); transmitted = read64(reply + 24);
        CHECK(reply[1] == 0xd3 && memcmp(reply + 8, ping + 24, 8) == 0);
        CHECK(received >= before && transmitted >= received && transmitted <= after);
        ping[1] = 0xd3;
        CHECK(send(udp, ping, sizeof(ping), 0) == 32 && !readable(udp, 250));
        if (family == AF_INET) {
            struct sockaddr_in wrong = { .sin_family = AF_INET };
            CHECK(inet_pton(AF_INET, "127.0.0.2", &wrong.sin_addr) == 1);
            impostor = socket(family, SOCK_DGRAM, 0); CHECK(impostor >= 0);
            CHECK(bind(impostor, (struct sockaddr *)&wrong, sizeof(wrong)) == 0);
            make_ping(ping);
            CHECK(sendto(impostor, ping, sizeof(ping), 0, (struct sockaddr *)&addr, len) == 32);
            CHECK(!readable(impostor, 250));
            close(impostor);
        }
        struct timespec a, b;
        CHECK(clock_gettime(CLOCK_MONOTONIC, &a) == 0);
        airplay_timing_stop(&timing);
        CHECK(clock_gettime(CLOCK_MONOTONIC, &b) == 0);
        CHECK(timing == NULL);
        CHECK((b.tv_sec-a.tv_sec)*1000+(b.tv_nsec-a.tv_nsec)/1000000 < 1000);
        airplay_timing_stop(&timing);
        close(udp);
    }
    close(server); close(client); close(listener);
    /* A joined pthread can briefly remain in /proc until the kernel reaps it. */
    for (int i = 0; i < 100 && entries("/proc/self/task") != base_threads; i++) {
        struct timespec pause = { .tv_nsec = 1000000 }; nanosleep(&pause, NULL);
    }
    CHECK(entries("/proc/self/fd") == base_fds);
    CHECK(entries("/proc/self/task") == base_threads);
    CHECK(airplay_timing_start(&timing, -1) < 0 && timing == NULL);
    CHECK(entries("/proc/self/fd") == base_fds);
    printf("PASS: IPv%d real UDP response/filter/stop/fd-thread cleanup (5 cycles)\n", family == AF_INET ? 4 : 6);
}

int main(int argc, char **argv)
{
    if (argc == 2 && strcmp(argv[1], "wire") == 0) test_wire();
    else if (argc == 2 && strcmp(argv[1], "ipv4") == 0) test_loopback(AF_INET);
    else if (argc == 2 && strcmp(argv[1], "ipv6") == 0) test_loopback(AF_INET6);
    else { test_wire(); test_loopback(AF_INET); test_loopback(AF_INET6); }
    return 0;
}
