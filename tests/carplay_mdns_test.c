#define _GNU_SOURCE
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <net/if.h>

/* The baseline is deliberately an inert discovery implementation. Build with
 * -DMDNS_TEST_BASELINE to demonstrate the missing feature before implementation. */
#ifdef MDNS_TEST_BASELINE
struct carplay_mdns {
    char ifname[IFNAMSIZ];
    unsigned ifindex;
    void (*found)(void *, const char *, uint16_t);
    void *ctx;
};
static int mdns_ingest(struct carplay_mdns *m, const uint8_t *p, size_t n, uint64_t now)
{ (void)m; (void)p; (void)n; (void)now; return 0; }
static void mdns_resolve(struct carplay_mdns *m, uint64_t now)
{ (void)m; (void)now; }
#else
#include "../src/carplay_mdns.c"
#endif

struct packet { uint8_t b[4096]; size_t n; unsigned records; };
struct observed { unsigned calls; char address[96]; uint16_t port; };
static void found(void *ctx, const char *address, uint16_t port)
{
    struct observed *o = ctx;
    o->calls++;
    snprintf(o->address, sizeof(o->address), "%s", address);
    o->port = port;
}
static struct carplay_mdns *fresh(struct observed *o)
{
    struct carplay_mdns *m = calloc(1, sizeof(*m));
    assert(m);
    strcpy(m->ifname, "usb0"); m->ifindex = 9; m->found = found; m->ctx = o;
    return m;
}
static void p16(struct packet *p, unsigned v)
{ p->b[p->n++] = v >> 8; p->b[p->n++] = v; }
static void p32(struct packet *p, uint32_t v)
{ p16(p, v >> 16); p16(p, v); }
static size_t pname(struct packet *p, const char *s)
{
    size_t start = p->n;
    while (*s) {
        const char *dot = strchr(s, '.');
        size_t n = dot ? (size_t)(dot - s) : strlen(s);
        assert(n <= 63); p->b[p->n++] = n;
        memcpy(p->b + p->n, s, n); p->n += n;
        if (!dot) break;
        s = dot + 1;
    }
    p->b[p->n++] = 0;
    return start;
}
static void init_packet(struct packet *p)
{ memset(p, 0, sizeof(*p)); p->n = 12; p->b[2] = 0x84; }
static size_t rr(struct packet *p, const char *name, unsigned type, uint32_t ttl)
{
    pname(p, name); p16(p, type); p16(p, 1); p32(p, ttl);
    size_t pos = p->n; p16(p, 0); p->records++;
    p->b[6] = p->records >> 8; p->b[7] = p->records;
    return pos;
}
static void end_rr(struct packet *p, size_t pos)
{ size_t n = p->n - pos - 2; p->b[pos] = n >> 8; p->b[pos + 1] = n; }
static void ptr(struct packet *p, const char *service, const char *instance, uint32_t ttl)
{
    size_t pos = rr(p, service, 12, ttl);
    /* Compress the service suffix against the owner at byte 12. */
    if (p->records == 1 && strlen(instance) > strlen(service) &&
        strcmp(instance + strlen(instance) - strlen(service), service) == 0) {
        size_t label = strlen(instance) - strlen(service) - 1;
        p->b[p->n++] = label; memcpy(p->b + p->n, instance, label); p->n += label;
        p16(p, 0xc00c);
    } else pname(p, instance);
    end_rr(p, pos);
}
static void srv(struct packet *p, const char *instance, const char *host, uint16_t port, uint32_t ttl)
{
    size_t pos = rr(p, instance, 33, ttl);
    p16(p, 0); p16(p, 0); p16(p, port); pname(p, host); end_rr(p, pos);
}
static void addr(struct packet *p, const char *host, int v6, uint32_t ttl)
{
    static const uint8_t ip6[16] = { 0xfe,0x80,0,0,0,0,0,0,0,0,0,0,0,0,0,0x42 };
    static const uint8_t ip4[4] = { 192,168,20,1 };
    size_t pos = rr(p, host, v6 ? 28 : 1, ttl);
    size_t n = v6 ? 16 : 4;
    memcpy(p->b + p->n, v6 ? ip6 : ip4, n); p->n += n; end_rr(p, pos);
}
static void feed(struct carplay_mdns *m, struct packet *p, uint64_t now)
{ assert(mdns_ingest(m, p->b, p->n, now) == 0); mdns_resolve(m, now); }

static void split_compressed_records_resolve_and_stay_stable(void)
{
    struct observed o = {0}; struct carplay_mdns *m = fresh(&o); struct packet p;
    init_packet(&p); srv(&p, "Dash._airplay._tcp.local", "head.local", 7000, 120); feed(m, &p, 1000);
    init_packet(&p); addr(&p, "head.local", 1, 120); feed(m, &p, 1001);
    assert(o.calls == 0);
    init_packet(&p); ptr(&p, "_airplay._tcp.local", "Dash._airplay._tcp.local", 120); feed(m, &p, 1002);
    assert(o.calls == 1);
    assert(strcmp(o.address, "fe80::42%usb0") == 0 && o.port == 7000);
    feed(m, &p, 1100); assert(o.calls == 1);
    /* A newly arriving alternative address must not cause an endpoint flood. */
    init_packet(&p); addr(&p, "head.local", 0, 120); feed(m, &p, 1200); assert(o.calls == 1);
    free(m);
}
static void unrelated_services_and_invalid_ptr_do_not_connect(void)
{
    struct observed o = {0}; struct carplay_mdns *m = fresh(&o); struct packet p;
    init_packet(&p); ptr(&p, "_http._tcp.local", "Dash._http._tcp.local", 120);
    srv(&p, "Dash._http._tcp.local", "head.local", 7000, 120); addr(&p, "head.local", 0, 120); feed(m, &p, 1);
    assert(o.calls == 0);
    init_packet(&p); ptr(&p, "_airplay._tcp.local", "Dash._http._tcp.local", 120); feed(m, &p, 2);
    assert(o.calls == 0); free(m);
}
static void goodbye_and_expiry_prevent_stale_resolution(void)
{
    struct observed o = {0}; struct carplay_mdns *m = fresh(&o); struct packet p;
    init_packet(&p); ptr(&p, "_carplay._tcp.local", "Dash._carplay._tcp.local", 120);
    srv(&p, "Dash._carplay._tcp.local", "head.local", 7001, 120); addr(&p, "head.local", 0, 1); feed(m, &p, 1);
    assert(o.calls == 1 && o.port == 7001);
    mdns_resolve(m, 1002);
    init_packet(&p); addr(&p, "head.local", 0, 0); feed(m, &p, 1003); assert(o.calls == 1);
    init_packet(&p); addr(&p, "head.local", 0, 120); feed(m, &p, 1004); assert(o.calls == 2);
    init_packet(&p); ptr(&p, "_carplay._tcp.local", "Dash._carplay._tcp.local", 0); feed(m, &p, 1005);
    init_packet(&p); addr(&p, "head.local", 0, 120); feed(m, &p, 1006); assert(o.calls == 2);
    free(m);
}
static void malformed_packet_is_rejected_atomically(void)
{
    struct observed o = {0}; struct carplay_mdns *m = fresh(&o); struct packet p;
    init_packet(&p); ptr(&p, "_airplay._tcp.local", "Dash._airplay._tcp.local", 120);
    srv(&p, "Dash._airplay._tcp.local", "head.local", 7000, 120); addr(&p, "head.local", 0, 120);
    p.b[7]++; p16(&p, 0xc000 | p.n); /* Self-referential pointer. */
    assert(mdns_ingest(m, p.b, p.n, 1) < 0); mdns_resolve(m, 1); assert(o.calls == 0);
    init_packet(&p); ptr(&p, "_airplay._tcp.local", "Dash._airplay._tcp.local", 120);
    assert(mdns_ingest(m, p.b, p.n - 1, 2) < 0);
    /* A complete name is still invalid if it escapes this RR's RDLENGTH. */
    p.b[p.n - 2] = 0; p.b[p.n - 1] = 0;
    assert(mdns_ingest(m, p.b, p.n, 2) < 0);
    free(m);
}
static void stale_partial_records_and_zero_ports_do_not_connect(void)
{
    struct observed o = {0}; struct carplay_mdns *m = fresh(&o); struct packet p;
    init_packet(&p); ptr(&p, "_airplay._tcp.local", "Dash._airplay._tcp.local", 1); addr(&p, "head.local", 0, 120); feed(m, &p, 1);
    init_packet(&p); srv(&p, "Dash._airplay._tcp.local", "head.local", 7000, 120); feed(m, &p, 1002); assert(o.calls == 0);
    init_packet(&p); ptr(&p, "_airplay._tcp.local", "Dash._airplay._tcp.local", 120);
    srv(&p, "Dash._airplay._tcp.local", "head.local", 0, 120); feed(m, &p, 1003);
    assert(o.calls == 0); free(m);
}
static void compressed_srv_and_aaaa_names_resolve(void)
{
    struct observed o = {0}; struct carplay_mdns *m = fresh(&o); struct packet p;
    init_packet(&p); ptr(&p, "_airplay._tcp.local", "Dash._airplay._tcp.local", 120); feed(m, &p, 1);
    init_packet(&p);
    size_t pos = rr(&p, "Dash._airplay._tcp.local", 33, 120);
    /* The owner's final 'local' label starts at byte 31. */
    p16(&p, 0); p16(&p, 0); p16(&p, 7000); p.b[p.n++] = 4;
    memcpy(p.b + p.n, "head", 4); p.n += 4; p16(&p, 0xc01f); end_rr(&p, pos); feed(m, &p, 2);
    init_packet(&p); p.b[5] = 1;
    size_t host = pname(&p, "head.local"); p16(&p, 28); p16(&p, 1);
    p16(&p, 0xc000 | host); p16(&p, 28); p16(&p, 1); p32(&p, 120); p16(&p, 16);
    uint8_t ip[16] = {0xfe, 0x80}; ip[15] = 0x42; memcpy(p.b + p.n, ip, 16); p.n += 16; p.b[7] = 1;
    feed(m, &p, 3); assert(o.calls == 1 && !strcmp(o.address, "fe80::42%usb0")); free(m);
}
static void cache_flush_replaces_old_address_after_grace(void)
{
    struct observed o = {0}; struct carplay_mdns *m = fresh(&o); struct packet p;
    init_packet(&p); ptr(&p, "_airplay._tcp.local", "Dash._airplay._tcp.local", 120);
    srv(&p, "Dash._airplay._tcp.local", "head.local", 7000, 120); addr(&p, "head.local", 0, 120); feed(m, &p, 1);
    assert(o.calls == 1);
    init_packet(&p); size_t len = rr(&p, "head.local", 1, 120); p.b[len - 6] = 0x80;
    uint8_t newer[4] = {192, 168, 20, 2}; memcpy(p.b + p.n, newer, 4); p.n += 4; end_rr(&p, len);
    feed(m, &p, 2002); assert(o.calls == 1);
    mdns_resolve(m, 3003); assert(o.calls == 2 && !strcmp(o.address, "192.168.20.2")); free(m);
}
static void oversized_cache_input_and_truncations_are_bounded(void)
{
    struct observed o = {0}; struct carplay_mdns *m = fresh(&o); struct packet p;
    init_packet(&p); ptr(&p, "_airplay._tcp.local", "Dash._airplay._tcp.local", 120);
    srv(&p, "Dash._airplay._tcp.local", "head.local", 7000, 120); addr(&p, "head.local", 0, 120);
    for (size_t n = 0; n < p.n; n++) assert(mdns_ingest(m, p.b, n, 1) < 0);
    init_packet(&p);
    for (unsigned i = 0; i < 97; i++) addr(&p, "head.local", 0, 120);
    assert(mdns_ingest(m, p.b, p.n, 1) < 0); mdns_resolve(m, 1); assert(o.calls == 0);
    uint32_t seed = 0x12345678;
    for (unsigned trial = 0; trial < 2000; trial++) {
        size_t n = trial % 512;
        for (size_t i = 0; i < n; i++) { seed = seed * 1664525u + 1013904223u; p.b[i] = seed >> 24; }
        (void)mdns_ingest(m, p.b, n, trial + 2);
    }
    free(m);
}
static void control_success_has_minimal_wire_response(void)
{
    int pair[2]; assert(socketpair(AF_UNIX, SOCK_STREAM, 0, pair) == 0);
    struct ctrl_client client = {.fd = pair[0]};
    const char *request = "GET /ctrl-int/1/connect HTTP/1.1\r\nAirPlay-Receiver-Device-ID: test\r\n\r\n";
    assert(send(pair[1], request, strlen(request), 0) == (ssize_t)strlen(request));
    ctrl_receive(&client);
    char response[256] = {0};
    ssize_t n = recv(pair[1], response, sizeof(response), 0);
    assert(n == (ssize_t)strlen("HTTP/1.1 200 OK\r\n\r\n"));
    assert(!memcmp(response, "HTTP/1.1 200 OK\r\n\r\n", (size_t)n));
    assert(recv(pair[1], response, sizeof(response), 0) == 0);
    assert(client.fd == -1);
    close(pair[1]);
}
int main(void)
{
    control_success_has_minimal_wire_response();
    split_compressed_records_resolve_and_stay_stable();
    unrelated_services_and_invalid_ptr_do_not_connect();
    goodbye_and_expiry_prevent_stale_resolution();
    malformed_packet_is_rejected_atomically();
    stale_partial_records_and_zero_ports_do_not_connect();
    compressed_srv_and_aaaa_names_resolve();
    cache_flush_replaces_old_address_after_grace();
    oversized_cache_input_and_truncations_are_bounded();
    puts("PASS: mDNS compressed cross-packet discovery, filtering, stability, withdrawal, expiry, malformed packets");
    return 0;
}
