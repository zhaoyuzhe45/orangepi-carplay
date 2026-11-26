#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include "carplay_mdns.h"
#include "carplay_identity.h"

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <net/if.h>
#include <netinet/in.h>
#include <poll.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

#define MDNS_PACKET 4096
#define MDNS_RECORDS 96
#define MDNS_ENDPOINTS 16
#define MDNS_ADDRESSES 8
#define MDNS_CLIENTS 4
#define MDNS_PORT 5353
#define CTRL_PORT 7100
#define CTRL_TTL 120

/* Canonical, uncompressed wire names retain label boundaries and arbitrary
 * octets: a dot or NUL within a label cannot masquerade as a service suffix. */
struct dns_name { uint8_t b[256]; uint16_t n; };
struct dns_record {
    struct dns_name name, target;
    uint16_t type, klass, port;
    uint32_t ttl;
    uint64_t expires, received;
    uint8_t address[16];
};
struct mdns_endpoint {
    struct dns_name instance;
    char address[INET6_ADDRSTRLEN + IFNAMSIZ + 1];
    uint16_t port;
    bool active, seen;
};
struct local_address { int family; uint8_t b[16]; };
struct ctrl_client { int fd; uint64_t deadline; size_t used; char request[4096]; };
struct carplay_mdns {
    char ifname[IFNAMSIZ];
    unsigned ifindex;
    void (*found)(void *, const char *, uint16_t);
    void *ctx;
    struct dns_record records[MDNS_RECORDS];
    struct mdns_endpoint endpoints[MDNS_ENDPOINTS];
    struct local_address addresses[MDNS_ADDRESSES];
    size_t address_count;
    struct dns_name host, instance;
    char device_id[18];
    int udp4, udp6, tcp4, tcp6, wake[2];
    uint16_t ctrl_port;
    struct ctrl_client clients[MDNS_CLIENTS];
    atomic_bool stopping;
    pthread_t thread;
};

static uint16_t dns_u16(const uint8_t *p) { return (uint16_t)((p[0] << 8) | p[1]); }
static uint32_t dns_u32(const uint8_t *p)
{ return ((uint32_t)dns_u16(p) << 16) | dns_u16(p + 2); }

static int dns_name_read(const uint8_t *p, size_t len, size_t *offset, struct dns_name *out)
{
    size_t pos = *offset, end = 0, used = 0;
    unsigned hops = 0;
    bool jumped = false;
    memset(out, 0, sizeof(*out));
    for (;;) {
        if (pos >= len || ++hops > 128) return -1;
        uint8_t n = p[pos++];
        if ((n & 0xc0) == 0xc0) {
            if (pos >= len) return -1;
            size_t target = ((size_t)(n & 63) << 8) | p[pos++];
            /* RFC 1035 compression refers to a prior occurrence. Reject
             * forward/self pointers as well as bounded expansion loops. */
            if (target >= pos - 2) return -1;
            if (!jumped) end = pos;
            jumped = true; pos = target; continue;
        }
        if (n & 0xc0 || n > 63 || pos + n > len || used + n + 1 > 255) return -1;
        out->b[used++] = n;
        for (unsigned i = 0; i < n; i++) {
            uint8_t c = p[pos++];
            out->b[used++] = (c >= 'A' && c <= 'Z') ? (uint8_t)(c + 32) : c;
        }
        if (!n) break;
    }
    out->n = (uint16_t)used;
    *offset = jumped ? end : pos;
    return 0;
}

static struct dns_name dns_literal(const char *s)
{
    struct dns_name name = {{0}, 0};
    size_t n = 0;
    while (*s) {
        const char *dot = strchr(s, '.');
        size_t part = dot ? (size_t)(dot - s) : strlen(s);
        if (!part || part > 63 || n + part + 2 > sizeof(name.b)) return (struct dns_name){{0},0};
        name.b[n++] = (uint8_t)part;
        for (size_t i = 0; i < part; i++) {
            unsigned char c = (unsigned char)s[i];
            name.b[n++] = (c >= 'A' && c <= 'Z') ? (uint8_t)(c + 32) : c;
        }
        if (!dot) break;
        s = dot + 1;
    }
    name.b[n++] = 0; name.n = (uint16_t)n;
    return name;
}
static bool dns_equal(const struct dns_name *a, const struct dns_name *b)
{ return a->n == b->n && a->n && !memcmp(a->b, b->b, a->n); }
static bool dns_is(const struct dns_name *a, const char *s)
{ struct dns_name b = dns_literal(s); return dns_equal(a, &b); }
static bool dns_instance_of(const struct dns_name *name, const struct dns_name *service)
{
    size_t prefix = (size_t)name->b[0] + 1;
    return name->b[0] && name->n == prefix + service->n &&
           !memcmp(name->b + prefix, service->b, service->n);
}
static bool dns_browse_service(const struct dns_name *name)
{ return dns_is(name, "_airplay._tcp.local") || dns_is(name, "_carplay._tcp.local"); }

static bool record_same(const struct dns_record *a, const struct dns_record *b)
{
    if (a->type != b->type || !dns_equal(&a->name, &b->name)) return false;
    if (a->type == 12) return dns_equal(&a->target, &b->target);
    if (a->type == 33) return a->port == b->port && dns_equal(&a->target, &b->target);
    return !memcmp(a->address, b->address, a->type == 1 ? 4 : 16);
}
static bool address_usable(const struct dns_record *r)
{
    if (r->type == 1) {
        uint32_t v; memcpy(&v, r->address, 4); v = ntohl(v);
        return v && v != UINT32_MAX && (v >> 24) < 224;
    }
    struct in6_addr a; memcpy(&a, r->address, 16);
    return !IN6_IS_ADDR_UNSPECIFIED(&a) && !IN6_IS_ADDR_MULTICAST(&a) && !IN6_IS_ADDR_V4MAPPED(&a);
}

/* Parse and validate the complete DNS message before committing any record.
 * Questions and unknown RRs are bounded and skipped structurally. */
static int mdns_ingest(struct carplay_mdns *m, const uint8_t *p, size_t len, uint64_t now)
{
    if (len < 12 || len > MDNS_PACKET || !(p[2] & 0x80) || (p[2] & 0x7a) || (p[3] & 15)) return -1;
    unsigned questions = dns_u16(p + 4);
    unsigned count = dns_u16(p + 6) + dns_u16(p + 8) + dns_u16(p + 10);
    if (questions > MDNS_RECORDS || count > MDNS_RECORDS) return -1;
    struct dns_record incoming[MDNS_RECORDS];
    size_t pos = 12, kept = 0;
    for (unsigned i = 0; i < questions; i++) {
        struct dns_name name;
        if (dns_name_read(p, len, &pos, &name) || pos + 4 > len) return -1;
        pos += 4;
    }
    for (unsigned i = 0; i < count; i++) {
        struct dns_record r; memset(&r, 0, sizeof(r));
        if (dns_name_read(p, len, &pos, &r.name) || pos + 10 > len) return -1;
        r.type = dns_u16(p + pos); r.klass = dns_u16(p + pos + 2); r.ttl = dns_u32(p + pos + 4);
        size_t size = dns_u16(p + pos + 8); pos += 10;
        if (size > len - pos) return -1;
        size_t end = pos + size, data = pos;
        bool supported = true;
        if (r.type == 12 || r.type == 33) {
            if (r.type == 33) {
                if (size < 7) return -1;
                r.port = dns_u16(p + pos + 4); data += 6;
            }
            if (dns_name_read(p, len, &data, &r.target) || data != end) return -1;
        } else if (r.type == 1 || r.type == 28) {
            if (size != (r.type == 1 ? 4u : 16u)) return -1;
            memcpy(r.address, p + pos, size);
        } else supported = false;
        pos = end;
        if (!supported || (r.klass & 0x7fff) != 1) continue;
        if (r.type == 12 && (!dns_browse_service(&r.name) || !dns_instance_of(&r.target, &r.name))) continue;
        if ((r.type == 1 || r.type == 28) && !address_usable(&r)) continue;
        r.received = now;
        r.expires = now + (uint64_t)(r.ttl > 86400 ? 86400 : r.ttl) * 1000;
        incoming[kept++] = r;
    }
    if (pos != len) return -1;
    for (size_t i = 0; i < kept; i++) {
        struct dns_record *r = &incoming[i];
        for (size_t j = 0; j < MDNS_RECORDS; j++) {
            struct dns_record *old = &m->records[j];
            if (old->expires <= now) { old->expires = 0; continue; }
            if (old->type != r->type || !dns_equal(&old->name, &r->name)) continue;
            if (!r->ttl && record_same(old, r)) old->expires = 0;
            /* SRV describes one service endpoint. A replacement (including
             * port 0 withdrawal) invalidates its previous target immediately. */
            else if (r->ttl && r->type == 33 && !record_same(old, r)) old->expires = 0;
            else if (r->ttl && (r->klass & 0x8000) && !record_same(old, r) &&
                     old->received + 1000 < now && old->expires > now + 1000) {
                bool in_packet = false;
                for (size_t k = 0; k < kept; k++)
                    if (incoming[k].ttl && record_same(old, &incoming[k])) in_packet = true;
                if (!in_packet) old->expires = now + 1000;
            }
        }
        if (!r->ttl) continue;
        size_t slot = MDNS_RECORDS, oldest = 0;
        for (size_t j = 0; j < MDNS_RECORDS; j++) {
            if (m->records[j].expires && record_same(&m->records[j], r)) { slot = j; break; }
            if (!m->records[j].expires && slot == MDNS_RECORDS) slot = j;
            if (m->records[j].expires < m->records[oldest].expires) oldest = j;
        }
        if (slot == MDNS_RECORDS) slot = oldest;
        m->records[slot] = *r;
    }
    return 0;
}

static bool endpoint_address(const struct carplay_mdns *m, const struct dns_record *r,
                             char *text, size_t size)
{
    int family = r->type == 1 ? AF_INET : AF_INET6;
    if (!inet_ntop(family, r->address, text, (socklen_t)size)) return false;
    if (family == AF_INET6) {
        struct in6_addr a; memcpy(&a, r->address, 16);
        if (IN6_IS_ADDR_LINKLOCAL(&a)) {
            size_t used = strlen(text);
            if (snprintf(text + used, size - used, "%%%s", m->ifname) >= (int)(size - used)) return false;
        }
    }
    return true;
}

static void mdns_resolve(struct carplay_mdns *m, uint64_t now)
{
    for (size_t i = 0; i < MDNS_RECORDS; i++) if (m->records[i].expires <= now) m->records[i].expires = 0;
    for (size_t i = 0; i < MDNS_ENDPOINTS; i++) m->endpoints[i].seen = false;
    for (size_t i = 0; i < MDNS_RECORDS; i++) {
        struct dns_record *ptr = &m->records[i];
        if (!ptr->expires || ptr->type != 12 || !dns_browse_service(&ptr->name)) continue;
        struct mdns_endpoint *previous = NULL, *empty = NULL;
        for (size_t e = 0; e < MDNS_ENDPOINTS; e++) {
            if (m->endpoints[e].active && dns_equal(&m->endpoints[e].instance, &ptr->target)) previous = &m->endpoints[e];
            if (!m->endpoints[e].active && !empty) empty = &m->endpoints[e];
        }
        char chosen[INET6_ADDRSTRLEN + IFNAMSIZ + 1] = {0};
        uint16_t chosen_port = 0; int rank = 0;
        for (size_t j = 0; j < MDNS_RECORDS; j++) {
            struct dns_record *srv = &m->records[j];
            if (!srv->expires || srv->type != 33 || !srv->port || !dns_equal(&srv->name, &ptr->target)) continue;
            for (size_t k = 0; k < MDNS_RECORDS; k++) {
                struct dns_record *a = &m->records[k];
                if (!a->expires || (a->type != 1 && a->type != 28) || !dns_equal(&a->name, &srv->target)) continue;
                char candidate[sizeof(chosen)];
                if (!endpoint_address(m, a, candidate, sizeof(candidate))) continue;
                int priority = a->type == 1 ? 2 : 1;
                if (previous && previous->port == srv->port && !strcmp(previous->address, candidate)) priority = 3;
                if (priority > rank) { strcpy(chosen, candidate); chosen_port = srv->port; rank = priority; }
            }
        }
        if (!chosen_port) continue;
        struct mdns_endpoint *ep = previous ? previous : empty;
        if (!ep) continue;
        bool changed = !ep->active || ep->port != chosen_port || strcmp(ep->address, chosen);
        ep->active = ep->seen = true; ep->instance = ptr->target; ep->port = chosen_port; strcpy(ep->address, chosen);
        if (changed && m->found && !atomic_load(&m->stopping)) m->found(m->ctx, chosen, chosen_port);
    }
    for (size_t e = 0; e < MDNS_ENDPOINTS; e++) if (!m->endpoints[e].seen) m->endpoints[e].active = false;
}

struct dns_writer { uint8_t *p; size_t n, cap; unsigned answers; bool failed; };
static void wire_bytes(struct dns_writer *w, const void *p, size_t n)
{
    if (n > w->cap - w->n) { w->failed = true; return; }
    memcpy(w->p + w->n, p, n); w->n += n;
}
static void wire16(struct dns_writer *w, unsigned n)
{ uint8_t p[2] = {(uint8_t)(n >> 8), (uint8_t)n}; wire_bytes(w, p, 2); }
static void wire32(struct dns_writer *w, uint32_t n)
{ wire16(w, n >> 16); wire16(w, n); }
static void wire_name(struct dns_writer *w, const struct dns_name *name)
{ if (!name->n) w->failed = true; else wire_bytes(w, name->b, name->n); }
static size_t wire_rr(struct dns_writer *w, const struct dns_name *name, unsigned type,
                      bool unique, uint32_t ttl)
{
    wire_name(w, name); wire16(w, type); wire16(w, unique ? 0x8001 : 1); wire32(w, ttl);
    size_t at = w->n; wire16(w, 0); w->answers++;
    return at;
}
static void wire_end_rr(struct dns_writer *w, size_t at)
{
    if (w->failed) return;
    size_t size = w->n - at - 2;
    w->p[at] = (uint8_t)(size >> 8); w->p[at + 1] = (uint8_t)size;
}
static void wire_ptr(struct dns_writer *w, const struct dns_name *owner,
                     const struct dns_name *target, bool unique, uint32_t ttl)
{ size_t at = wire_rr(w, owner, 12, unique, ttl); wire_name(w, target); wire_end_rr(w, at); }
static void wire_nsec(struct dns_writer *w, const struct dns_name *name,
                      const uint8_t *bitmap, size_t size, uint32_t ttl)
{
    size_t at = wire_rr(w, name, 47, true, ttl);
    wire_name(w, name); uint8_t header[2] = {0, (uint8_t)size};
    wire_bytes(w, header, 2); wire_bytes(w, bitmap, size); wire_end_rr(w, at);
}
static struct dns_name address_reverse(const struct local_address *a)
{
    char text[96];
    if (a->family == AF_INET)
        snprintf(text, sizeof(text), "%u.%u.%u.%u.in-addr.arpa", a->b[3], a->b[2], a->b[1], a->b[0]);
    else {
        size_t n = 0; static const char hex[] = "0123456789abcdef";
        for (int i = 15; i >= 0; i--) {
            text[n++] = hex[a->b[i] & 15]; text[n++] = '.';
            text[n++] = hex[a->b[i] >> 4]; text[n++] = '.';
        }
        strcpy(text + n, "ip6.arpa");
    }
    return dns_literal(text);
}
static size_t mdns_build_announcement(struct carplay_mdns *m, uint8_t *p, size_t cap, uint32_t ttl)
{
    if (cap < 12 || !m->ctrl_port || !m->address_count) return 0;
    struct dns_writer w = {.p = p, .n = 12, .cap = cap}; memset(p, 0, 12); p[2] = 0x84;
    struct dns_name service = dns_literal("_carplay-ctrl._tcp.local");
    struct dns_name services = dns_literal("_services._dns-sd._udp.local");
    wire_ptr(&w, &service, &m->instance, false, ttl);
    size_t at = wire_rr(&w, &m->instance, 33, true, ttl);
    wire16(&w, 0); wire16(&w, 0); wire16(&w, m->ctrl_port); wire_name(&w, &m->host); wire_end_rr(&w, at);
    at = wire_rr(&w, &m->instance, 16, true, ttl);
    char id[32]; int idlen = snprintf(id + 1, sizeof(id) - 1, "id=%s", m->device_id); id[0] = (char)idlen;
    wire_bytes(&w, id, (size_t)idlen + 1);
    char version[64]; int version_length = snprintf(version + 1, sizeof(version) - 1,
                                                   "srcvers=%s", CARPLAY_SOURCE_VERSION);
    if (version_length < 0 || version_length >= (int)sizeof(version) - 1) return 0;
    version[0] = (char)version_length;
    wire_bytes(&w, version, (size_t)version_length + 1); wire_end_rr(&w, at);
    wire_ptr(&w, &services, &service, false, ttl);
    uint8_t host_bitmap[4] = {0};
    for (size_t i = 0; i < m->address_count; i++) {
        const struct local_address *a = &m->addresses[i];
        unsigned type = a->family == AF_INET ? 1 : 28;
        at = wire_rr(&w, &m->host, type, true, ttl);
        wire_bytes(&w, a->b, type == 1 ? 4 : 16); wire_end_rr(&w, at);
        host_bitmap[type / 8] |= (uint8_t)(0x80 >> (type % 8));
        struct dns_name reverse = address_reverse(a); wire_ptr(&w, &reverse, &m->host, true, ttl);
    }
    unsigned answer_count = w.answers;
    static const uint8_t instance_bitmap[] = {0,0,0x80,0,0x40}, ptr_bitmap[] = {0,0x08};
    wire_nsec(&w, &m->instance, instance_bitmap, sizeof(instance_bitmap), ttl);
    wire_nsec(&w, &m->host, host_bitmap, host_bitmap[3] ? 4 : 1, ttl);
    for (size_t i = 0; i < m->address_count; i++) {
        struct dns_name reverse = address_reverse(&m->addresses[i]);
        wire_nsec(&w, &reverse, ptr_bitmap, sizeof(ptr_bitmap), ttl);
    }
    if (w.failed) return 0;
    p[6] = (uint8_t)(answer_count >> 8); p[7] = (uint8_t)answer_count;
    unsigned additional = w.answers - answer_count; p[10] = (uint8_t)(additional >> 8); p[11] = (uint8_t)additional;
    return w.n;
}

static uint64_t mdns_now(void)
{ struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts); return (uint64_t)ts.tv_sec * 1000 + (uint64_t)ts.tv_nsec / 1000000; }
static int fd_nonblock(int fd)
{
    int fl = fcntl(fd, F_GETFL, 0), fdfl = fcntl(fd, F_GETFD, 0);
    if (fl < 0 || fdfl < 0 || fcntl(fd, F_SETFL, fl | O_NONBLOCK) < 0 || fcntl(fd, F_SETFD, fdfl | FD_CLOEXEC) < 0) return -1;
    return 0;
}
static int bound_socket(const struct carplay_mdns *m, int family, int type)
{
    int fd = socket(family, type, 0), yes = 1;
    if (fd < 0) return -1;
    if (fd_nonblock(fd) < 0 ||
        setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes)) < 0 ||
        setsockopt(fd, SOL_SOCKET, SO_BINDTODEVICE, m->ifname, (socklen_t)strlen(m->ifname) + 1) < 0 ||
        (family == AF_INET6 && setsockopt(fd, IPPROTO_IPV6, IPV6_V6ONLY, &yes, sizeof(yes)) < 0)) {
        int err = errno; close(fd); errno = err; return -1;
    }
    return fd;
}
static int socket_bind_port(int fd, int family, uint16_t port)
{
    if (family == AF_INET) {
        struct sockaddr_in a = {.sin_family = AF_INET, .sin_port = htons(port)};
        return bind(fd, (struct sockaddr *)&a, sizeof(a));
    }
    struct sockaddr_in6 a = {.sin6_family = AF_INET6, .sin6_port = htons(port)};
    return bind(fd, (struct sockaddr *)&a, sizeof(a));
}
static int mdns_udp_socket(struct carplay_mdns *m, int family)
{
    int fd = bound_socket(m, family, SOCK_DGRAM), yes = 1, hops = 255, no = 0;
    if (fd < 0) return -1;
    if (socket_bind_port(fd, family, MDNS_PORT) < 0) goto fail;
    if (family == AF_INET) {
        struct ip_mreqn group; memset(&group, 0, sizeof(group));
        inet_pton(AF_INET, "224.0.0.251", &group.imr_multiaddr); group.imr_ifindex = (int)m->ifindex;
        if (setsockopt(fd, IPPROTO_IP, IP_PKTINFO, &yes, sizeof(yes)) < 0 ||
            setsockopt(fd, IPPROTO_IP, IP_RECVTTL, &yes, sizeof(yes)) < 0 ||
            setsockopt(fd, IPPROTO_IP, IP_MULTICAST_IF, &group, sizeof(group)) < 0 ||
            setsockopt(fd, IPPROTO_IP, IP_ADD_MEMBERSHIP, &group, sizeof(group)) < 0 ||
            setsockopt(fd, IPPROTO_IP, IP_MULTICAST_TTL, &hops, sizeof(hops)) < 0 ||
            setsockopt(fd, IPPROTO_IP, IP_TTL, &hops, sizeof(hops)) < 0 ||
            setsockopt(fd, IPPROTO_IP, IP_MULTICAST_LOOP, &no, sizeof(no)) < 0) goto fail;
    } else {
        struct ipv6_mreq group; memset(&group, 0, sizeof(group));
        inet_pton(AF_INET6, "ff02::fb", &group.ipv6mr_multiaddr); group.ipv6mr_interface = m->ifindex;
        if (setsockopt(fd, IPPROTO_IPV6, IPV6_RECVPKTINFO, &yes, sizeof(yes)) < 0 ||
            setsockopt(fd, IPPROTO_IPV6, IPV6_RECVHOPLIMIT, &yes, sizeof(yes)) < 0 ||
            setsockopt(fd, IPPROTO_IPV6, IPV6_MULTICAST_IF, &m->ifindex, sizeof(m->ifindex)) < 0 ||
            setsockopt(fd, IPPROTO_IPV6, IPV6_JOIN_GROUP, &group, sizeof(group)) < 0 ||
            setsockopt(fd, IPPROTO_IPV6, IPV6_MULTICAST_HOPS, &hops, sizeof(hops)) < 0 ||
            setsockopt(fd, IPPROTO_IPV6, IPV6_UNICAST_HOPS, &hops, sizeof(hops)) < 0 ||
            setsockopt(fd, IPPROTO_IPV6, IPV6_MULTICAST_LOOP, &no, sizeof(no)) < 0) goto fail;
    }
    return fd;
fail: { int err = errno; close(fd); errno = err; return -1; }
}
static int mdns_tcp_socket(struct carplay_mdns *m, int family, uint16_t *port, bool fallback)
{
    int fd = bound_socket(m, family, SOCK_STREAM);
    if (fd < 0) return -1;
    if (socket_bind_port(fd, family, *port) < 0 &&
        !(fallback && errno == EADDRINUSE && socket_bind_port(fd, family, 0) == 0)) goto fail;
    if (listen(fd, MDNS_CLIENTS) < 0) goto fail;
    struct sockaddr_storage a; socklen_t n = sizeof(a);
    if (getsockname(fd, (struct sockaddr *)&a, &n) < 0) goto fail;
    *port = ntohs(family == AF_INET ? ((struct sockaddr_in *)&a)->sin_port : ((struct sockaddr_in6 *)&a)->sin6_port);
    return fd;
fail: { int err = errno; close(fd); errno = err; return -1; }
}

/* getifaddrs is unavailable on Android API 21. SIOCGIFADDR and the kernel's
 * interface-specific IPv6 list give actual addresses without helper daemons.
 * Publish one IPv4 and one preferred link-local IPv6 to keep the complete
 * response below the ordinary NCM MTU. Refresh after DAD/address changes. */
static size_t mdns_read_addresses(struct carplay_mdns *m, struct local_address *out)
{
    size_t count = 0;
    if (m->tcp4 >= 0) {
        struct ifreq ifr; memset(&ifr, 0, sizeof(ifr)); strcpy(ifr.ifr_name, m->ifname);
        if (ioctl(m->tcp4, SIOCGIFADDR, &ifr) == 0) {
            struct sockaddr_in *a = (struct sockaddr_in *)&ifr.ifr_addr;
            struct dns_record r; memset(&r, 0, sizeof(r)); r.type = 1; memcpy(r.address, &a->sin_addr, 4);
            if (a->sin_family == AF_INET && address_usable(&r)) {
                out[count].family = AF_INET; memcpy(out[count++].b, &a->sin_addr, 4);
            }
        }
    }
    if (m->tcp6 >= 0) {
        FILE *f = fopen("/proc/net/if_inet6", "r");
        if (f) {
            char line[256], hex[33], name[IFNAMSIZ]; unsigned index, prefix, scope, flags;
            struct local_address best = {.family = AF_INET6}; bool have = false, linklocal = false;
            while (fgets(line, sizeof(line), f)) {
                if (sscanf(line, "%32s %x %x %x %x %15s", hex, &index, &prefix, &scope, &flags, name) != 6 ||
                    index != m->ifindex || strcmp(name, m->ifname) || strlen(hex) != 32 || (flags & (0x08 | 0x40))) continue;
                struct dns_record r; memset(&r, 0, sizeof(r)); r.type = 28; bool valid = true;
                for (unsigned i = 0; i < 16; i++) {
                    unsigned byte;
                    if (sscanf(hex + i * 2, "%2x", &byte) != 1) { valid = false; break; }
                    r.address[i] = (uint8_t)byte;
                }
                struct in6_addr a; memcpy(&a, r.address, 16);
                bool islocal = IN6_IS_ADDR_LINKLOCAL(&a);
                if (valid && address_usable(&r) && (!have || (islocal && !linklocal))) {
                    memcpy(best.b, r.address, 16); have = true; linklocal = islocal;
                }
            }
            fclose(f); if (have) out[count++] = best;
        }
    }
    return count;
}
static void mdns_send_multicast(struct carplay_mdns *m, const uint8_t *p, size_t size)
{
    if (!size) return;
    if (m->udp4 >= 0) {
        struct sockaddr_in a = {.sin_family = AF_INET, .sin_port = htons(MDNS_PORT)};
        inet_pton(AF_INET, "224.0.0.251", &a.sin_addr);
        (void)sendto(m->udp4, p, size, MSG_NOSIGNAL, (struct sockaddr *)&a, sizeof(a));
    }
    if (m->udp6 >= 0) {
        struct sockaddr_in6 a = {.sin6_family = AF_INET6, .sin6_port = htons(MDNS_PORT), .sin6_scope_id = m->ifindex};
        inet_pton(AF_INET6, "ff02::fb", &a.sin6_addr);
        (void)sendto(m->udp6, p, size, MSG_NOSIGNAL, (struct sockaddr *)&a, sizeof(a));
    }
}
static void mdns_announce(struct carplay_mdns *m, uint32_t ttl)
{
    uint8_t p[MDNS_PACKET]; size_t n = mdns_build_announcement(m, p, sizeof(p), ttl);
    mdns_send_multicast(m, p, n);
}
static void query_add(struct dns_writer *w, const struct dns_name *name, unsigned type)
{
    if (name->n + 4u > w->cap - w->n) return;
    wire_name(w, name); wire16(w, type); wire16(w, 1); w->answers++;
}
static void mdns_query(struct carplay_mdns *m, uint64_t now)
{
    uint8_t p[MDNS_PACKET] = {0}; struct dns_writer w = {.p = p, .n = 12, .cap = sizeof(p)};
    struct dns_name airplay = dns_literal("_airplay._tcp.local"), carplay = dns_literal("_carplay._tcp.local");
    query_add(&w, &airplay, 12); query_add(&w, &carplay, 12);
    unsigned instances = 0;
    for (size_t i = 0; i < MDNS_RECORDS && instances < MDNS_ENDPOINTS; i++) {
        struct dns_record *ptr = &m->records[i];
        if (ptr->expires <= now || ptr->type != 12) continue;
        query_add(&w, &ptr->target, 33); instances++;
        for (size_t j = 0; j < MDNS_RECORDS; j++) {
            struct dns_record *srv = &m->records[j];
            if (srv->expires <= now || srv->type != 33 || !dns_equal(&srv->name, &ptr->target)) continue;
            query_add(&w, &srv->target, 1); query_add(&w, &srv->target, 28);
        }
    }
    p[4] = (uint8_t)(w.answers >> 8); p[5] = (uint8_t)w.answers;
    if (!w.failed) mdns_send_multicast(m, p, w.n);
}
static bool mdns_query_wants(struct carplay_mdns *m, const uint8_t *p, size_t size, bool *unicast)
{
    if (size < 12 || (p[2] & 0xf8) || (p[3] & 15)) return false;
    unsigned questions = dns_u16(p + 4); size_t pos = 12; bool match = false;
    if (questions > MDNS_RECORDS) return false;
    for (unsigned i = 0; i < questions; i++) {
        struct dns_name name;
        if (dns_name_read(p, size, &pos, &name) || pos + 4 > size) return false;
        unsigned klass = dns_u16(p + pos + 2); pos += 4;
        if ((klass & 0x7fff) != 1 && (klass & 0x7fff) != 255) continue;
        bool relevant = dns_is(&name, "_carplay-ctrl._tcp.local") || dns_is(&name, "_services._dns-sd._udp.local") ||
                        dns_equal(&name, &m->host) || dns_equal(&name, &m->instance);
        for (size_t a = 0; a < m->address_count; a++) {
            struct dns_name reverse = address_reverse(&m->addresses[a]);
            if (dns_equal(&name, &reverse)) relevant = true;
        }
        if (relevant) { match = true; if (klass & 0x8000) *unicast = true; }
    }
    return match;
}
static bool mdns_packet_allowed(const struct carplay_mdns *m, unsigned index, int hop, unsigned port)
{ return index == m->ifindex && index && hop == 255 && port == MDNS_PORT; }
static int mdns_receive(struct carplay_mdns *m, int fd, uint64_t now, uint64_t *last_answer)
{
    uint8_t p[MDNS_PACKET]; struct sockaddr_storage source;
    union { struct cmsghdr align; uint8_t b[256]; } control;
    struct iovec iov = {.iov_base = p, .iov_len = sizeof(p)};
    struct msghdr msg; memset(&msg, 0, sizeof(msg));
    msg.msg_name = &source; msg.msg_namelen = sizeof(source); msg.msg_iov = &iov; msg.msg_iovlen = 1;
    msg.msg_control = control.b; msg.msg_controllen = sizeof(control.b);
    ssize_t size = recvmsg(fd, &msg, MSG_DONTWAIT);
    if (size < 0) return -1;
    if (msg.msg_flags & (MSG_TRUNC | MSG_CTRUNC)) return 0;
    unsigned index = 0, port = 0; int hop = -1;
    for (struct cmsghdr *c = CMSG_FIRSTHDR(&msg); c; c = CMSG_NXTHDR(&msg, c)) {
        if (c->cmsg_level == IPPROTO_IP && c->cmsg_type == IP_PKTINFO && c->cmsg_len >= CMSG_LEN(sizeof(struct in_pktinfo))) {
            struct in_pktinfo info; memcpy(&info, CMSG_DATA(c), sizeof(info)); index = (unsigned)info.ipi_ifindex;
        } else if (c->cmsg_level == IPPROTO_IPV6 && c->cmsg_type == IPV6_PKTINFO && c->cmsg_len >= CMSG_LEN(sizeof(struct in6_pktinfo))) {
            struct in6_pktinfo info; memcpy(&info, CMSG_DATA(c), sizeof(info)); index = info.ipi6_ifindex;
        } else if (((c->cmsg_level == IPPROTO_IP && c->cmsg_type == IP_TTL) ||
                    (c->cmsg_level == IPPROTO_IPV6 && c->cmsg_type == IPV6_HOPLIMIT)) && c->cmsg_len >= CMSG_LEN(sizeof(hop)))
            memcpy(&hop, CMSG_DATA(c), sizeof(hop));
    }
    if (source.ss_family == AF_INET) port = ntohs(((struct sockaddr_in *)&source)->sin_port);
    else if (source.ss_family == AF_INET6) port = ntohs(((struct sockaddr_in6 *)&source)->sin6_port);
    if (!mdns_packet_allowed(m, index, hop, port) || size < 12 || atomic_load(&m->stopping)) return 0;
    if (p[2] & 0x80) {
        if (mdns_ingest(m, p, (size_t)size, now) == 0) mdns_resolve(m, now);
    } else {
        bool unicast = false;
        if (mdns_query_wants(m, p, (size_t)size, &unicast) && now >= *last_answer + 250) {
            size_t n = mdns_build_announcement(m, p, sizeof(p), CTRL_TTL);
            if (unicast && n) (void)sendto(fd, p, n, MSG_NOSIGNAL, (struct sockaddr *)&source, msg.msg_namelen);
            else mdns_send_multicast(m, p, n);
            *last_answer = now;
        }
    }
    return 0;
}

static int control_status(const char *request)
{
    const char *line = strstr(request, "\r\n");
    if (!line) return 400;
    size_t first = (size_t)(line - request);
    const char *valid11 = "GET /ctrl-int/1/connect HTTP/1.1", *valid10 = "GET /ctrl-int/1/connect HTTP/1.0";
    bool path = (first == strlen(valid11) && !memcmp(request, valid11, first)) ||
                (first == strlen(valid10) && !memcmp(request, valid10, first));
    bool identity = false; line += 2;
    while (*line && strncmp(line, "\r\n", 2)) {
        const char *end = strstr(line, "\r\n"); if (!end) return 400;
        const char *colon = memchr(line, ':', (size_t)(end - line));
        if (!colon || colon == line || *line == ' ' || *line == '\t') return 400;
        static const char key[] = "AirPlay-Receiver-Device-ID";
        if ((size_t)(colon - line) == sizeof(key) - 1 && !strncasecmp(line, key, sizeof(key) - 1)) {
            const char *v = colon + 1; while (v < end && (*v == ' ' || *v == '\t')) v++;
            identity = v < end;
        }
        line = end + 2;
    }
    return path && identity ? 200 : 404;
}
static void ctrl_close(struct ctrl_client *c)
{ if (c->fd >= 0) close(c->fd); c->fd = -1; c->used = 0; }
static void ctrl_accept(struct carplay_mdns *m, int fd, uint64_t now)
{
    for (unsigned tries = 0; tries < MDNS_CLIENTS; tries++) {
        int client = accept(fd, NULL, NULL); if (client < 0) return;
        struct ctrl_client *slot = NULL;
        for (size_t i = 0; i < MDNS_CLIENTS; i++) if (m->clients[i].fd < 0) { slot = &m->clients[i]; break; }
        if (!slot || fd_nonblock(client) < 0) { close(client); continue; }
        slot->fd = client; slot->used = 0; slot->deadline = now + 2000;
    }
}
static void ctrl_receive(struct ctrl_client *c)
{
    ssize_t n = recv(c->fd, c->request + c->used, sizeof(c->request) - c->used - 1, MSG_DONTWAIT);
    if (n <= 0) { if (!n || (errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR)) ctrl_close(c); return; }
    if (memchr(c->request + c->used, 0, (size_t)n)) { ctrl_close(c); return; }
    c->used += (size_t)n; c->request[c->used] = 0;
    bool complete = strstr(c->request, "\r\n\r\n") != NULL;
    if (!complete && c->used < sizeof(c->request) - 1) return;
    int status = complete ? control_status(c->request) : 431;
    const char *reason = status == 200 ? "OK" : status == 404 ? "Not Found" : status == 431 ? "Request Header Fields Too Large" : "Bad Request";
    /* Match orangecar's minimal control success reply.
     * Closing the socket delimits this empty response. */
    char response[160]; int length = status == 200 ?
        snprintf(response, sizeof(response), "HTTP/1.1 200 OK\r\n\r\n") :
        snprintf(response, sizeof(response),
            "HTTP/1.1 %d %s\r\nContent-Length: 0\r\nConnection: close\r\n\r\n", status, reason);
    (void)send(c->fd, response, (size_t)length, MSG_NOSIGNAL);
    ctrl_close(c);
}
static void *mdns_worker(void *context)
{
    struct carplay_mdns *m = context;
    uint64_t next_query = 0, next_announce = 0, next_addresses = mdns_now() + 2000, last_answer = 0;
    unsigned interval = 1000, announces = 0;
    while (!atomic_load(&m->stopping)) {
        uint64_t now = mdns_now();
        mdns_resolve(m, now);
        if (now >= next_addresses) {
            struct local_address addresses[MDNS_ADDRESSES] = {{0}};
            size_t count = mdns_read_addresses(m, addresses);
            if (count != m->address_count || memcmp(addresses, m->addresses, count * sizeof(*addresses))) {
                mdns_announce(m, 0); memcpy(m->addresses, addresses, sizeof(addresses)); m->address_count = count;
                next_announce = 0; announces = 0;
            }
            next_addresses = now + 2000;
        }
        if (now >= next_query) { mdns_query(m, now); next_query = now + interval; if (interval < 30000) interval = interval > 15000 ? 30000 : interval * 2; }
        if (now >= next_announce) { mdns_announce(m, CTRL_TTL); next_announce = now + (++announces < 2 ? 1000 : 60000); }
        struct pollfd fds[5 + MDNS_CLIENTS]; int kind[5 + MDNS_CLIENTS]; nfds_t count = 0;
        fds[count] = (struct pollfd){.fd = m->wake[0], .events = POLLIN}; kind[count++] = 0;
        int base[] = {m->udp4, m->udp6, m->tcp4, m->tcp6};
        for (unsigned i = 0; i < 4; i++) if (base[i] >= 0) {
            fds[count] = (struct pollfd){.fd = base[i], .events = POLLIN}; kind[count++] = (int)i + 1;
        }
        for (unsigned i = 0; i < MDNS_CLIENTS; i++) {
            if (m->clients[i].fd >= 0 && now >= m->clients[i].deadline) ctrl_close(&m->clients[i]);
            if (m->clients[i].fd >= 0) {
                fds[count] = (struct pollfd){.fd = m->clients[i].fd, .events = POLLIN}; kind[count++] = (int)i + 5;
            }
        }
        int rc = poll(fds, count, 100);
        if (atomic_load(&m->stopping)) break;
        if (rc < 0) { if (errno == EINTR) continue; break; }
        now = mdns_now();
        for (nfds_t i = 1; i < count && !atomic_load(&m->stopping); i++) {
            if (!fds[i].revents) continue;
            if (kind[i] <= 2) {
                for (unsigned budget = 0; budget < 16 && !atomic_load(&m->stopping); budget++)
                    if (mdns_receive(m, fds[i].fd, now, &last_answer) < 0) break;
            } else if (kind[i] <= 4) ctrl_accept(m, fds[i].fd, now);
            else ctrl_receive(&m->clients[kind[i] - 5]);
        }
    }
    /* Goodbye packets are emitted on the same still-owned interface sockets. */
    mdns_announce(m, 0);
    return NULL;
}
static void mdns_free(struct carplay_mdns *m)
{
    int fds[] = {m->udp4, m->udp6, m->tcp4, m->tcp6, m->wake[0], m->wake[1]};
    for (size_t i = 0; i < sizeof(fds) / sizeof(fds[0]); i++) if (fds[i] >= 0) close(fds[i]);
    for (size_t i = 0; i < MDNS_CLIENTS; i++) ctrl_close(&m->clients[i]);
    free(m);
}
int carplay_mdns_start(struct carplay_mdns **out, const char *ifname,
                      void (*found)(void *, const char *, uint16_t), void *ctx)
{
    if (!out) { errno = EINVAL; return -1; }
    *out = NULL;
    if (!ifname || !*ifname || strlen(ifname) >= IFNAMSIZ || !found) { errno = EINVAL; return -1; }
    unsigned index = if_nametoindex(ifname); if (!index) { errno = ENODEV; return -1; }
    struct carplay_mdns *m = calloc(1, sizeof(*m)); if (!m) return -1;
    strcpy(m->ifname, ifname); m->ifindex = index; m->found = found; m->ctx = ctx;
    m->udp4 = m->udp6 = m->tcp4 = m->tcp6 = m->wake[0] = m->wake[1] = -1;
    for (size_t i = 0; i < MDNS_CLIENTS; i++) m->clients[i].fd = -1;
    atomic_init(&m->stopping, false);
    m->udp4 = mdns_udp_socket(m, AF_INET); m->udp6 = mdns_udp_socket(m, AF_INET6);
    if (m->udp4 < 0 && m->udp6 < 0) goto fail;
    m->ctrl_port = CTRL_PORT;
    m->tcp6 = mdns_tcp_socket(m, AF_INET6, &m->ctrl_port, true);
    m->tcp4 = mdns_tcp_socket(m, AF_INET, &m->ctrl_port, m->tcp6 < 0);
    if (m->tcp4 < 0 && m->tcp6 < 0) m->ctrl_port = 0;
    struct ifreq ifr; memset(&ifr, 0, sizeof(ifr)); strcpy(ifr.ifr_name, ifname);
    unsigned char mac[6] = {0};
    if (ioctl(m->udp4 >= 0 ? m->udp4 : m->udp6, SIOCGIFHWADDR, &ifr) == 0) memcpy(mac, ifr.ifr_hwaddr.sa_data, 6);
    snprintf(m->device_id, sizeof(m->device_id), "%s", CARPLAY_DEVICE_ID);
    char label[96]; snprintf(label, sizeof(label), "boxide-%02x%02x%02x%02x%02x%02x-%u.local", mac[0], mac[1], mac[2], mac[3], mac[4], mac[5], index);
    m->host = dns_literal(label);
    snprintf(label, sizeof(label), "BoxiDe-%02x%02x%02x-%u._carplay-ctrl._tcp.local", mac[3], mac[4], mac[5], index);
    m->instance = dns_literal(label); m->address_count = mdns_read_addresses(m, m->addresses);
    if (pipe(m->wake) < 0 || fd_nonblock(m->wake[0]) < 0 || fd_nonblock(m->wake[1]) < 0) goto fail;
    int rc = pthread_create(&m->thread, NULL, mdns_worker, m);
    if (rc) { errno = rc; goto fail; }
    *out = m;
    fprintf(stderr, "[mDNS] interface=%s index=%u discovery=%s%s control_port=%u addresses=%zu\n", ifname, index,
            m->udp4 >= 0 ? "IPv4 " : "", m->udp6 >= 0 ? "IPv6" : "", m->ctrl_port, m->address_count);
    return 0;
fail: { int err = errno; mdns_free(m); errno = err; return -1; }
}
void carplay_mdns_stop(struct carplay_mdns *m)
{
    if (!m) return;
    atomic_store(&m->stopping, true);
    char byte = 1; (void)write(m->wake[1], &byte, 1);
    (void)pthread_join(m->thread, NULL);
    mdns_free(m);
}
