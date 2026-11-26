/* Address equality is independent of UDP source ports, but link-local scope
 * must match the established control connection. No network changes needed. */
#include "../src/airplay_timing.c"
#include <assert.h>
#include <stdio.h>

int main(void)
{
    struct sockaddr_storage expected = {0}, sender = {0};
    struct sockaddr_in6 *a = (struct sockaddr_in6 *)&expected;
    struct sockaddr_in6 *b = (struct sockaddr_in6 *)&sender;
    a->sin6_family = b->sin6_family = AF_INET6;
    assert(inet_pton(AF_INET6, "fe80::1234", &a->sin6_addr) == 1);
    b->sin6_addr = a->sin6_addr;
    a->sin6_scope_id = b->sin6_scope_id = 7;
    a->sin6_port = htons(1000); b->sin6_port = htons(2000);
    assert(peer_matches(&expected, &sender));
    b->sin6_scope_id = 8;
    assert(!peer_matches(&expected, &sender));
    b->sin6_scope_id = 0;
    assert(!peer_matches(&expected, &sender));
    b->sin6_scope_id = 7;
    b->sin6_addr.s6_addr[15] ^= 1;
    assert(!peer_matches(&expected, &sender));
    assert(inet_pton(AF_INET6, "::1", &a->sin6_addr) == 1);
    b->sin6_addr = a->sin6_addr;
    b->sin6_scope_id = 0;
    assert(peer_matches(&expected, &sender));
    b->sin6_family = AF_INET;
    assert(!peer_matches(&expected, &sender));
    puts("PASS: IPv6 link-local address/scope filter and independent UDP source port");
    return 0;
}
