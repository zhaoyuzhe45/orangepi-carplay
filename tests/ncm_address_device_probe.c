/* Root-only isolated TAP probe. Never changes the live NCM interface/routes. */
#define _GNU_SOURCE
#include <assert.h>
void iap2_log(const char *format, ...) { (void)format; }
#include "../src/ncm_bridge.c"

int main(void)
{
    struct ifreq request;
    struct sockaddr_in6 source;
    uint64_t start;
    int tap, index, state, sock;
    if (if_nametoindex("cpdadtest")) return 2;
    tap = open("/dev/tun", O_RDWR | O_CLOEXEC);
    if (tap < 0) { perror("open tun"); return 2; }
    memset(&request, 0, sizeof(request));
    strcpy(request.ifr_name, "cpdadtest");
    request.ifr_flags = IFF_TAP | IFF_NO_PI;
    if (ioctl(tap, TUNSETIFF, &request) < 0) { perror("TUNSETIFF"); close(tap); return 2; }
    if (system("ip link set dev cpdadtest up && ip -6 addr add fe80::1/64 dev cpdadtest")) {
        close(tap); return 3;
    }
    index = (int)if_nametoindex("cpdadtest");
    assert(index > 0);
    assert(ipv6_source_state(index) == 0 && errno == EAGAIN);
    memset(&source, 0, sizeof(source));
    source.sin6_family = AF_INET6;
    source.sin6_scope_id = (unsigned)index;
    inet_pton(AF_INET6, "fe80::1", &source.sin6_addr);
    sock = socket(AF_INET6, SOCK_STREAM, 0);
    assert(sock >= 0);
    assert(bind(sock, (struct sockaddr *)&source, sizeof(source)) < 0 && errno == EADDRNOTAVAIL);
    start = ncm_now_ms();
    do {
        usleep(50000);
        state = ipv6_source_state(index);
    } while (state == 0 && ncm_now_ms() - start < 5000);
    assert(state == 1);
    assert(bind(sock, (struct sockaddr *)&source, sizeof(source)) == 0);
    printf("PASS: production readiness gate waits for real DAD; bind succeeds after %llums\n",
           (unsigned long long)(ncm_now_ms() - start));
    close(sock);
    close(tap);
    assert(if_nametoindex("cpdadtest") == 0);
    puts("PASS: temporary interface removed; live NCM interface untouched");
    return 0;
}
