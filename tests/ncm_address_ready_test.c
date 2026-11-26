#define _GNU_SOURCE
#include <assert.h>
#include <errno.h>
#include <net/if.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

static const char *addresses;
static const char *fail_command;
static int ipv6_routes, ipv4_routes, socket_calls, checks;
static int read_failure;

static FILE *fixture(const char *text)
{
    char path[128];
    FILE *file;
#ifdef __ANDROID__
    snprintf(path, sizeof(path), "/data/local/tmp/ncm-ready-%d", getpid());
#else
    snprintf(path, sizeof(path), "/tmp/ncm-ready-%d", getpid());
#endif
    file = fopen(path, "w+");
    assert(file);
    unlink(path);
    fputs(text, file);
    rewind(file);
    return file;
}
static FILE *test_fopen(const char *path, const char *mode)
{
    if (strcmp(path, "/proc/net/if_inet6") != 0) return fopen(path, mode);
    if (read_failure) { errno = EACCES; return NULL; }
    return fixture(addresses);
}
static int test_system(const char *command)
{
    if (strstr(command, "ip -6 route replace")) ipv6_routes++;
    if (strstr(command, "ip -4 route replace")) ipv4_routes++;
    return fail_command && strstr(command, fail_command) ? 512 : 0;
}
static FILE *test_popen(const char *command, const char *mode)
{
    (void)command; (void)mode;
    return fixture("38: from 169.254.0.1 lookup 100\n50: from fe80::1 lookup 100\n");
}
static unsigned test_if_nametoindex(const char *name)
{ return strcmp(name, "ncmtest0") == 0 ? 7u : 0u; }
static char *test_if_indextoname(unsigned index, char *name)
{ if (index != 7) return NULL; strcpy(name, "ncmtest0"); return name; }
static int test_socket(int family, int type, int protocol)
{ (void)family; (void)type; (void)protocol; socket_calls++; errno = ENETUNREACH; return -1; }
void iap2_log(const char *format, ...) { (void)format; }

#define fopen test_fopen
#define system test_system
#define popen test_popen
#define pclose fclose
#define if_nametoindex test_if_nametoindex
#define if_indextoname test_if_indextoname
#define socket test_socket
#include "../src/ncm_bridge.c"

static void reset(const char *rows)
{
    addresses = rows; fail_command = NULL; read_failure = 0;
    ipv6_routes = ipv4_routes = socket_calls = 0;
    g_tun_index = 7;
}
static void check(int ok, const char *message)
{
    checks++;
    if (!ok) { fprintf(stderr, "FAIL: %s\n", message); exit(1); }
}
int main(void)
{
    static const char ready[] = "fe800000000000000000000000000001 07 40 20 80 ncmtest0\n";
    static const char tentative[] = "fe800000000000000000000000000001 07 40 20 c0 ncmtest0\n";
    char resolved[128];
    reset(tentative);
    check(ensure_airplay_policy_routes("test") == 0 && ipv6_routes == 0 && ipv4_routes == 1,
          "tentative IPv6 must not install a source route; IPv4 remains available");
    check(connect_one("fe80::2", 7000, 0, resolved, sizeof(resolved), 1, 0) < 0 &&
          errno == EAGAIN && socket_calls == 0, "tentative IPv6 waits before socket/bind");
    addresses = ready;
    check(connect_one("fe80::2", 7000, 0, resolved, sizeof(resolved), 1, 0) < 0 &&
          socket_calls == 1 && ipv6_routes == 1, "ready transition installs route before first connection");
    reset(ready);
    check(ensure_airplay_policy_routes("test") == 0 && ipv6_routes == 1, "usable IPv6 route installed");
    reset("fe800000000000000000000000000001 07 40 20 88 ncmtest0\n");
    check(ensure_airplay_policy_routes("test") < 0 && ipv6_routes == 0, "DAD failure is a real error");
    reset("fe800000000000000000000000000001 08 40 20 80 other0\n");
    check(ensure_airplay_policy_routes("test") == 0 && ipv6_routes == 0, "other interface is not ready");
    reset("fe800000000000000000000000000002 07 40 20 80 ncmtest0\ninvalid\n");
    check(ensure_airplay_policy_routes("test") == 0 && ipv6_routes == 0, "wrong/malformed source is not ready");
    reset("fe800000000000000000000000000001 07 40 20 c4 ncmtest0\n");
    check(ensure_airplay_policy_routes("test") == 0 && ipv6_routes == 0, "optimistic tentative address waits");
    reset(ready); read_failure = 1;
    check(ensure_airplay_policy_routes("test") < 0 && ipv6_routes == 0, "proc read failure is not hidden");
    for (unsigned i = 0; i < 4; i++) {
        const char *commands[] = {"ip link set", "ip -6 addr replace", "ip addr replace", "ip -4 route replace"};
        reset(ready); g_tun_index = 0; fail_command = commands[i];
        check(netdev_configure("ncmtest0") < 0 && g_tun_index == 0, "failed configuration cannot publish readiness");
    }
    reset(tentative); g_tun_index = 0;
    check(netdev_configure("ncmtest0") == 0 && g_tun_index == 7 && ipv6_routes == 0,
          "successful interface configuration permits IPv4 while IPv6 DAD settles");
    printf("PASS: %d address-readiness and configuration checks\n", checks);
    return 0;
}
