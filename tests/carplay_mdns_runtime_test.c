#ifdef MDNS_RUNTIME_BASELINE
#define carplay_mdns_start implemented_carplay_mdns_start
#define carplay_mdns_stop implemented_carplay_mdns_stop
#define mdns_build_announcement implemented_mdns_build_announcement
#define mdns_packet_allowed implemented_mdns_packet_allowed
#endif
#define main parser_suite_main
#include "carplay_mdns_test.c"
#undef main
#include <dirent.h>

#ifdef MDNS_RUNTIME_BASELINE
#undef carplay_mdns_start
#undef carplay_mdns_stop
#undef mdns_build_announcement
#undef mdns_packet_allowed
int carplay_mdns_start(struct carplay_mdns **out, const char *ifname,
                      void (*cb)(void *, const char *, uint16_t), void *ctx)
{ (void)ifname; (void)cb; (void)ctx; *out = NULL; errno = ENOSYS; return -1; }
void carplay_mdns_stop(struct carplay_mdns *m) { (void)m; }
static size_t mdns_build_announcement(struct carplay_mdns *m, uint8_t *p, size_t cap, uint32_t ttl)
{ (void)m; (void)p; (void)cap; (void)ttl; return 0; }
static bool mdns_packet_allowed(const struct carplay_mdns *m, unsigned index, int hop, unsigned port)
{ (void)m; (void)index; (void)hop; (void)port; return false; }
#endif

static atomic_uint callback_count;
static atomic_bool slow_callback, callback_inflight;
static void runtime_found(void *ctx, const char *address, uint16_t port)
{
    (void)ctx;
    assert(strcmp(address, "192.168.20.1") == 0 && port == 7000);
    atomic_fetch_add(&callback_count, 1);
    if (atomic_load(&slow_callback)) {
        atomic_store(&callback_inflight, true);
        struct timespec delay = {0, 80000000}; nanosleep(&delay, NULL);
        atomic_store(&callback_inflight, false);
    }
}
static void pause_ms(unsigned ms)
{ struct timespec ts = {ms / 1000, (long)(ms % 1000) * 1000000}; nanosleep(&ts, NULL); }
static int control_connect(uint16_t port)
{
    int fd = socket(AF_INET, SOCK_STREAM, 0); assert(fd >= 0);
    struct sockaddr_in a = {.sin_family = AF_INET, .sin_port = htons(port)};
    assert(inet_pton(AF_INET, "127.0.0.1", &a.sin_addr) == 1);
    assert(connect(fd, (struct sockaddr *)&a, sizeof(a)) == 0);
    return fd;
}
static void expect_status(int fd, const char *status)
{
    char response[256] = {0};
    struct pollfd p = {.fd = fd, .events = POLLIN}; assert(poll(&p, 1, 2000) == 1);
    size_t used = 0; ssize_t n;
    while ((n = recv(fd, response + used, sizeof(response) - 1 - used, 0)) > 0) {
        used += (size_t)n; assert(used < sizeof(response) - 1);
    }
    assert(n == 0 && used > 0);
    assert(strstr(response, status));
    if (strstr(status, "200 OK")) assert(!strcmp(response, "HTTP/1.1 200 OK\r\n\r\n"));
    else assert(strstr(response, "Content-Length: 0\r\n"));
    close(fd);
}
static void check_bound(int fd)
{
    if (fd < 0) return;
    char name[IFNAMSIZ] = {0}; socklen_t len = sizeof(name);
    assert(getsockopt(fd, SOL_SOCKET, SO_BINDTODEVICE, name, &len) == 0);
    assert(strcmp(name, "lo") == 0);
}
static void check_advertisement(struct carplay_mdns *m)
{
    uint8_t p[MDNS_PACKET]; size_t n = mdns_build_announcement(m, p, sizeof(p), 120);
    assert(n > 12 && dns_u16(p + 2) == 0x8400 && dns_u16(p + 4) == 0);
    unsigned count = dns_u16(p + 6) + dns_u16(p + 10), addresses = 0, srv_count = 0, nsec = 0;
    size_t pos = 12;
    for (unsigned i = 0; i < count; i++) {
        struct dns_name name; assert(dns_name_read(p, n, &pos, &name) == 0);
        assert(pos + 10 <= n);
        unsigned type = dns_u16(p + pos), size = dns_u16(p + pos + 8); pos += 10;
        assert(pos + size <= n);
        if (type == 33) { assert(dns_u16(p + pos + 4) == m->ctrl_port); srv_count++; }
        if (type == 16) {
            const char *expected_id = "id=0A:0B:0C:0D:0E:0F", *expected_version = "srcvers=409.16";
            bool id = false, version = false; size_t t = pos;
            while (t < pos + size) {
                unsigned bytes = p[t++]; assert(bytes <= pos + size - t);
                if (bytes == strlen(expected_id) && !memcmp(p + t, expected_id, bytes)) id = true;
                if (bytes == strlen(expected_version) && !memcmp(p + t, expected_version, bytes)) version = true;
                t += bytes;
            }
            assert(id && version);
        }
        if (type == 1 || type == 28) {
            bool actual = false;
            for (size_t j = 0; j < m->address_count; j++)
                if (m->addresses[j].family == (type == 1 ? AF_INET : AF_INET6) &&
                    size == (type == 1 ? 4 : 16) && !memcmp(m->addresses[j].b, p + pos, size)) actual = true;
            assert(actual); addresses++;
        }
        if (type == 47) nsec++;
        pos += size;
    }
    assert(pos == n && addresses > 0 && srv_count == 1 && nsec >= 2);
}
static unsigned descriptor_count(void)
{
    unsigned n = 0; DIR *dir = opendir("/proc/self/fd"); assert(dir);
    while (readdir(dir)) n++;
    closedir(dir); return n;
}
int main(void)
{
    unsigned before_fds = descriptor_count();
    struct carplay_mdns *m = NULL;
    assert(carplay_mdns_start(&m, "not-an-iface", runtime_found, NULL) < 0 && m == NULL);
    assert(carplay_mdns_start(&m, "lo", runtime_found, NULL) == 0);
    assert(m && m->udp4 >= 0 && m->tcp4 >= 0 && m->ctrl_port);
    check_bound(m->udp4); check_bound(m->udp6); check_bound(m->tcp4); check_bound(m->tcp6);
    assert(mdns_packet_allowed(m, m->ifindex, 255, 5353));
    assert(!mdns_packet_allowed(m, m->ifindex + 1, 255, 5353));
    assert(!mdns_packet_allowed(m, m->ifindex, 254, 5353));
    assert(!mdns_packet_allowed(m, m->ifindex, 255, 7000));
    check_advertisement(m);
    int fd = control_connect(m->ctrl_port);
    const char *a = "GET /ctrl-int/1/connect HTTP/1.1\r\nAirPlay-Receiver-Device-ID: aa:bb";
    assert(send(fd, a, strlen(a), 0) == (ssize_t)strlen(a));
    struct pollfd check = {.fd = fd, .events = POLLIN}; assert(poll(&check, 1, 30) == 0);
    assert(send(fd, "\r\n\r\n", 4, 0) == 4); expect_status(fd, "HTTP/1.1 200 OK");
    fd = control_connect(m->ctrl_port);
    a = "GET /ctrl-int/1/connect HTTP/1.1\r\n\r\n";
    assert(send(fd, a, strlen(a), 0) == (ssize_t)strlen(a)); expect_status(fd, "404 Not Found");
    assert(m->tcp6 >= 0);
    fd = socket(AF_INET6, SOCK_STREAM, 0); assert(fd >= 0);
    struct sockaddr_in6 v6 = {.sin6_family = AF_INET6, .sin6_port = htons(m->ctrl_port)};
    assert(inet_pton(AF_INET6, "::1", &v6.sin6_addr) == 1);
    assert(connect(fd, (struct sockaddr *)&v6, sizeof(v6)) == 0);
    a = "GET /ctrl-int/1/connect HTTP/1.1\r\nAirPlay-Receiver-Device-ID: v6-peer\r\n\r\n";
    assert(send(fd, a, strlen(a), 0) == (ssize_t)strlen(a)); expect_status(fd, "200 OK");

    int injector = socket(AF_INET, SOCK_DGRAM, 0); assert(injector >= 0);
    int yes = 1, ttl = 255;
    assert(setsockopt(injector, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes)) == 0);
    assert(setsockopt(injector, SOL_SOCKET, SO_BINDTODEVICE, "lo", 3) == 0);
    struct sockaddr_in source = {.sin_family = AF_INET, .sin_port = htons(5353)};
    assert(bind(injector, (struct sockaddr *)&source, sizeof(source)) == 0);
    struct in_addr loop; inet_pton(AF_INET, "127.0.0.1", &loop);
    assert(setsockopt(injector, IPPROTO_IP, IP_MULTICAST_IF, &loop, sizeof(loop)) == 0);
    assert(setsockopt(injector, IPPROTO_IP, IP_MULTICAST_TTL, &ttl, sizeof(ttl)) == 0);
    struct sockaddr_in dst = {.sin_family = AF_INET, .sin_port = htons(5353)};
    inet_pton(AF_INET, "224.0.0.251", &dst.sin_addr);
    struct packet packet; init_packet(&packet);
    ptr(&packet, "_airplay._tcp.local", "Dash._airplay._tcp.local", 120);
    srv(&packet, "Dash._airplay._tcp.local", "head.local", 7000, 120); addr(&packet, "head.local", 0, 120);
    assert(sendto(injector, packet.b, packet.n, 0, (struct sockaddr *)&dst, sizeof(dst)) == (ssize_t)packet.n);
    for (unsigned i = 0; i < 100 && atomic_load(&callback_count) == 0; i++) pause_ms(10);
    assert(atomic_load(&callback_count) == 1);
    for (unsigned i = 0; i < 20; i++)
        assert(sendto(injector, packet.b, packet.n, 0, (struct sockaddr *)&dst, sizeof(dst)) == (ssize_t)packet.n);
    pause_ms(50); assert(atomic_load(&callback_count) == 1);
    /* A partial control client must not delay stop or survive its join. */
    fd = control_connect(m->ctrl_port); assert(send(fd, "GET /", 5, 0) == 5);
    atomic_store(&slow_callback, true);
    struct packet second; init_packet(&second);
    ptr(&second, "_carplay._tcp.local", "Another._carplay._tcp.local", 120);
    srv(&second, "Another._carplay._tcp.local", "head.local", 7000, 120);
    assert(sendto(injector, second.b, second.n, 0, (struct sockaddr *)&dst, sizeof(dst)) == (ssize_t)second.n);
    for (unsigned i = 0; i < 1000 && !atomic_load(&callback_inflight); i++) pause_ms(1);
    assert(atomic_load(&callback_inflight));
    carplay_mdns_stop(m); close(fd); assert(!atomic_load(&callback_inflight));
    assert(atomic_load(&callback_count) == 2); atomic_store(&slow_callback, false);
    assert(sendto(injector, packet.b, packet.n, 0, (struct sockaddr *)&dst, sizeof(dst)) == (ssize_t)packet.n);
    pause_ms(50); assert(atomic_load(&callback_count) == 2); close(injector);
    for (unsigned i = 0; i < 8; i++) {
        assert(carplay_mdns_start(&m, "lo", runtime_found, NULL) == 0); carplay_mdns_stop(m);
    }
    /* A busy conventional port must not leave an advertised closed port. */
    int occupied = socket(AF_INET6, SOCK_STREAM, 0); assert(occupied >= 0);
    int reuse = 1, dual = 0;
    assert(setsockopt(occupied, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse)) == 0);
    assert(setsockopt(occupied, IPPROTO_IPV6, IPV6_V6ONLY, &dual, sizeof(dual)) == 0);
    struct sockaddr_in6 busy = {.sin6_family = AF_INET6, .sin6_port = htons(7100)};
    assert(bind(occupied, (struct sockaddr *)&busy, sizeof(busy)) == 0); assert(listen(occupied, 1) == 0);
    assert(carplay_mdns_start(&m, "lo", runtime_found, NULL) == 0 && m->ctrl_port != 7100);
    check_advertisement(m); fd = control_connect(m->ctrl_port);
    a = "GET /ctrl-int/1/connect HTTP/1.1\r\nAirPlay-Receiver-Device-ID: test\r\n\r\n";
    assert(send(fd, a, strlen(a), 0) == (ssize_t)strlen(a)); expect_status(fd, "200 OK");
    carplay_mdns_stop(m); close(occupied);
    carplay_mdns_stop(NULL);
    assert(descriptor_count() == before_fds);
    puts("PASS: mDNS socket interface binding, incoming validation, actual-address advertisement, fragmented HTTP control, live discovery de-duplication, stop/join, restart");
    return 0;
}
