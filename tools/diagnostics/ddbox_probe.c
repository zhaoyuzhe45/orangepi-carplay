/*
 * ddbox_probe.c - minimal usbfs host probe for a CarPlay accessory.
 *
 * Why this exists
 * ---------------
 * Every earlier attempt to talk iAP2 to DDBox010 went through DiPlay, which before doing
 * anything else sends several Apple vendor requests (0x53, 0x52, 0x45) and writes to the
 * PTP interface. A 120 s zero-interference observation proved DDBox does not reset on its
 * own (0 disconnects) while six consecutive DiPlay cycles each killed it within a
 * millisecond of an 0x51 request. So the accessory has to be tested with the smallest
 * possible sequence, and nothing else.
 *
 * This program performs exactly one sequence and reports each step:
 *
 *   SET_CONFIGURATION(cfg)  ->  CLAIMINTERFACE(iface)  ->  BULK OUT 20 bytes  ->  BULK IN
 *
 * No vendor request, no PTP traffic, no Apple Ethernet claim. Options let each step be
 * dropped so a failure can be attributed to one specific step.
 *
 * Build (Windows host, Android NDK):
 *   $NDK/toolchains/llvm/prebuilt/windows-x86_64/bin/aarch64-linux-android27-clang \
 *       -O2 -Wall -pie -fPIE ddbox_probe.c -o ddbox_probe
 *
 * Run on the box as root:
 *   adb push ddbox_probe /data/local/tmp/ && adb shell su 0 /data/local/tmp/ddbox_probe
 */

#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdarg.h>
#include <fcntl.h>
#include <unistd.h>
#include <dirent.h>
#include <errno.h>
#include <time.h>
#include <sys/ioctl.h>
#include <linux/usbdevice_fs.h>

#define SYSFS_USB "/sys/bus/usb/devices"

/* USBMUX version message, byte-identical to what DiPlay sends: three little-endian u32
 * (protocol 0, length 20, version 2) followed by zero padding to 20 bytes. */
#define VERSION_MESSAGE_BYTES 20

static long long t0;

static long long now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (long long)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

static void logf_(const char *fmt, ...)
{
    va_list ap;
    printf("[%6lld ms] ", now_ms() - t0);
    va_start(ap, fmt);
    vprintf(fmt, ap);
    va_end(ap);
    printf("\n");
    fflush(stdout);
}

static int read_attr(const char *dir, const char *name, char *out, size_t outlen)
{
    char path[512];
    snprintf(path, sizeof(path), "%s/%s", dir, name);
    FILE *f = fopen(path, "r");
    if (!f) return -1;
    if (!fgets(out, (int)outlen, f)) {
        fclose(f);
        return -1;
    }
    fclose(f);
    out[strcspn(out, "\r\n")] = '\0';
    return 0;
}

static int read_attr_int(const char *dir, const char *name, int base)
{
    char buf[64];
    if (read_attr(dir, name, buf, sizeof(buf)) != 0) return -1;
    return (int)strtol(buf, NULL, base);
}

static void hex_dump(const char *label, const unsigned char *data, int len)
{
    if (len <= 0) return;
    char line[256];
    int pos = snprintf(line, sizeof(line), "%s", label);
    for (int i = 0; i < len && pos < (int)sizeof(line) - 5; i++)
        pos += snprintf(line + pos, sizeof(line) - pos, "%02x ", data[i]);
    logf_("%s", line);
}

/* Locate the Apple device on the bus and return its sysfs directory plus bus/device numbers. */
static int locate_apple(char *devdir, size_t devlen, int *busnum, int *devnum,
                        const char *want_vendor, const char *want_product)
{
    DIR *d = opendir(SYSFS_USB);
    if (!d) {
        logf_("cannot open %s: %s", SYSFS_USB, strerror(errno));
        return -1;
    }
    struct dirent *e;
    int found = -1;
    while ((e = readdir(d)) != NULL) {
        if (e->d_name[0] == '.') continue;
        char dir[512];
        snprintf(dir, sizeof(dir), "%s/%s", SYSFS_USB, e->d_name);
        char vendor[32], product[32];
        if (read_attr(dir, "idVendor", vendor, sizeof(vendor)) != 0) continue;
        if (strcasecmp(vendor, want_vendor) != 0) continue;
        if (read_attr(dir, "idProduct", product, sizeof(product)) != 0) continue;
        if (want_product[0] && strcasecmp(product, want_product) != 0) continue;
        *busnum = read_attr_int(dir, "busnum", 10);
        *devnum = read_attr_int(dir, "devnum", 10);
        snprintf(devdir, devlen, "%s", dir);
        logf_("device %s %s:%s bus=%d dev=%d", e->d_name, vendor, product, *busnum, *devnum);
        found = 0;
        break;
    }
    closedir(d);
    if (found != 0) logf_("no device matching %s:%s on the bus", want_vendor, want_product);
    return found;
}

/* Interface directory in sysfs is named "<bus>-<port>:<configuration>.<interface>". */
static int find_bulk_endpoints(const char *devdir, int config, int iface,
                               uint8_t *ep_in, uint8_t *ep_out)
{
    char idir[512];
    snprintf(idir, sizeof(idir), "%s:%d.%d", devdir, config, iface);

    int count = read_attr_int(idir, "bNumEndpoints", 10);
    if (count < 0) {
        logf_("interface directory %s missing (config %d not selected?)", idir, config);
        return -1;
    }
    logf_("interface %s: bNumEndpoints=%d", idir, count);

    *ep_in = 0;
    *ep_out = 0;
    DIR *d = opendir(idir);
    if (!d) return -1;
    struct dirent *e;
    while ((e = readdir(d)) != NULL) {
        if (strncmp(e->d_name, "ep_", 3) != 0) continue;
        char epdir[600];
        snprintf(epdir, sizeof(epdir), "%s/%s", idir, e->d_name);
        int addr = read_attr_int(epdir, "bEndpointAddress", 16);
        /* sysfs reports endpoint attributes in hex; bmAttributes low two bits are the
         * transfer type (2 = bulk). "type" is not the transfer type here, so use bmAttributes
         * when it exists and fall back to taking whatever direction is present. */
        int attrs = read_attr_int(epdir, "bmAttributes", 16);
        int maxp = read_attr_int(epdir, "wMaxPacketSize", 16);
        if (addr < 0) continue;
        logf_("  endpoint 0x%02x bmAttributes=0x%02x maxpacket=%d (%s)", addr, attrs, maxp,
              (addr & 0x80) ? "IN" : "OUT");
        if (attrs >= 0 && (attrs & 0x03) != 2) continue; /* keep bulk only */
        if (addr & 0x80) {
            if (!*ep_in) *ep_in = (uint8_t)addr;
        } else {
            if (!*ep_out) *ep_out = (uint8_t)addr;
        }
    }
    closedir(d);
    return (*ep_in && *ep_out) ? 0 : -1;
}

static void build_version_message(unsigned char *buf)
{
    memset(buf, 0, VERSION_MESSAGE_BYTES);
    /* little-endian u32 triples: protocol, length, usbmux version */
    buf[0] = 0; buf[1] = 0; buf[2] = 0; buf[3] = 0;
    buf[4] = (unsigned char)(VERSION_MESSAGE_BYTES & 0xff);
    buf[5] = 0; buf[6] = 0; buf[7] = 0;
    buf[8] = 2; buf[9] = 0; buf[10] = 0; buf[11] = 0;
}

static int bulk_xfer(int fd, uint8_t ep, void *data, int len, int timeout_ms, const char *what)
{
    struct usbdevfs_bulktransfer bt;
    memset(&bt, 0, sizeof(bt));
    bt.ep = ep;
    bt.len = (unsigned int)len;
    bt.timeout = (unsigned int)timeout_ms;
    bt.data = data;

    long long start = now_ms();
    int rc = ioctl(fd, USBDEVFS_BULK, &bt);
    long long elapsed = now_ms() - start;
    if (rc < 0)
        logf_("%s ep=0x%02x len=%d FAILED after %lld ms: %s", what, ep, len, elapsed, strerror(errno));
    else
        logf_("%s ep=0x%02x len=%d -> %d bytes in %lld ms", what, ep, len, rc, elapsed);
    return rc;
}

static void usage(const char *argv0)
{
    fprintf(stderr,
            "usage: %s [options]\n"
            "  --config N     configuration to select, 0 to skip (default 4)\n"
            "  --iface N      interface to claim and write to (default 1)\n"
            "  --timeout MS   bulk timeout in milliseconds (default 4000)\n"
            "  --read-first   read the IN endpoint before writing\n"
            "  --no-claim     skip the interface claim\n"
            "  --no-write     stop after claiming\n"
            "  --restore      set the configuration back to 1 at the end\n"
            "  --set-interface ALT   issue SET_INTERFACE(iface, ALT) before writing\n"
            "  --retries N    repeat the write/read pair up to N times (default 1)\n"
            "  --control RQT:RQ:VALUE:INDEX:LEN\n"
            "                 extra vendor control transfer, hex; LEN>0 reads IN\n"
            "  --vendor ID / --product ID   select a different device (hex, default 05ac/12a8)\n",
            argv0);
}

int main(int argc, char **argv)
{
    int config = 4, iface = 1, timeout_ms = 4000;
    int read_first = 0, do_claim = 1, do_write = 1, restore = 0;
    int set_alt = -1, retries = 1;
    char want_vendor[16] = "05ac";
    char want_product[16] = "12a8";
    int have_control = 0;
    unsigned ctl[5] = {0};

    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--config") && i + 1 < argc) config = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--iface") && i + 1 < argc) iface = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--timeout") && i + 1 < argc) timeout_ms = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--read-first")) read_first = 1;
        else if (!strcmp(argv[i], "--no-claim")) do_claim = 0;
        else if (!strcmp(argv[i], "--no-write")) do_write = 0;
        else if (!strcmp(argv[i], "--restore")) restore = 1;
        else if (!strcmp(argv[i], "--set-interface") && i + 1 < argc) set_alt = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--retries") && i + 1 < argc) retries = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--vendor") && i + 1 < argc) snprintf(want_vendor, sizeof(want_vendor), "%s", argv[++i]);
        else if (!strcmp(argv[i], "--product") && i + 1 < argc) snprintf(want_product, sizeof(want_product), "%s", argv[++i]);
        else if (!strcmp(argv[i], "--control") && i + 1 < argc) {
            sscanf(argv[++i], "%x:%x:%x:%x:%x", &ctl[0], &ctl[1], &ctl[2], &ctl[3], &ctl[4]);
            have_control = 1;
        } else {
            usage(argv[0]);
            return 2;
        }
    }

    t0 = now_ms();

    char devdir[512];
    int busnum = -1, devnum = -1;
    if (locate_apple(devdir, sizeof(devdir), &busnum, &devnum, want_vendor, want_product) != 0)
        return 1;

    char product[64] = "?";
    read_attr(devdir, "product", product, sizeof(product));
    char manufacturer[64] = "?";
    read_attr(devdir, "manufacturer", manufacturer, sizeof(manufacturer));
    logf_("identity: manufacturer=\"%s\" product=\"%s\"", manufacturer, product);

    int before = read_attr_int(devdir, "bConfigurationValue", 10);
    int numcfg = read_attr_int(devdir, "bNumConfigurations", 10);
    logf_("before: configuration=%d bNumConfigurations=%d", before, numcfg);

    char node[64];
    snprintf(node, sizeof(node), "/dev/bus/usb/%03d/%03d", busnum, devnum);
    int fd = open(node, O_RDWR);
    if (fd < 0) {
        logf_("open %s failed: %s (root required)", node, strerror(errno));
        return 1;
    }
    logf_("opened %s", node);

    if (config > 0) {
        if (before == config) {
            logf_("configuration already %d, skipping SET_CONFIGURATION", config);
        } else {
            int rc = ioctl(fd, USBDEVFS_SETCONFIGURATION, &config);
            if (rc < 0)
                logf_("SET_CONFIGURATION(%d) failed: %s", config, strerror(errno));
            else
                logf_("SET_CONFIGURATION(%d) ok", config);
        }
    }

    int active = read_attr_int(devdir, "bConfigurationValue", 10);
    logf_("after SET_CONFIGURATION: configuration=%d", active);

    if (have_control) {
        unsigned char buf[256];
        int len = (int)ctl[4];
        if (len < 0) len = 0;
        if (len > (int)sizeof(buf)) len = (int)sizeof(buf);
        memset(buf, 0, sizeof(buf));
        struct usbdevfs_ctrltransfer ct;
        memset(&ct, 0, sizeof(ct));
        ct.bRequestType = (uint8_t)ctl[0];
        ct.bRequest = (uint8_t)ctl[1];
        ct.wValue = (uint16_t)ctl[2];
        ct.wIndex = (uint16_t)ctl[3];
        ct.wLength = (uint16_t)len;
        ct.timeout = (unsigned int)timeout_ms;
        ct.data = buf;
        int rc = ioctl(fd, USBDEVFS_CONTROL, &ct);
        if (rc < 0)
            logf_("control %02x/%02x value=%d index=%d len=%d FAILED: %s",
                  ctl[0], ctl[1], ctl[2], ctl[3], len, strerror(errno));
        else {
            logf_("control %02x/%02x value=%d index=%d len=%d -> %d bytes",
                  ctl[0], ctl[1], ctl[2], ctl[3], len, rc);
            hex_dump("  data:", buf, rc);
        }
    }

    uint8_t ep_in = 0, ep_out = 0;
    if (find_bulk_endpoints(devdir, active > 0 ? active : config, iface, &ep_in, &ep_out) != 0) {
        logf_("no bulk endpoint pair on interface %d; giving up", iface);
        close(fd);
        return 1;
    }
    logf_("bulk pair: OUT=0x%02x IN=0x%02x", ep_out, ep_in);

    unsigned int claimed = (unsigned int)iface;
    if (do_claim) {
        int rc = ioctl(fd, USBDEVFS_CLAIMINTERFACE, &claimed);
        if (rc < 0)
            logf_("CLAIMINTERFACE(%d) failed: %s", iface, strerror(errno));
        else
            logf_("CLAIMINTERFACE(%d) ok", iface);
    } else {
        logf_("claim skipped on request");
    }

    /* A halted endpoint answers every transfer with EPROTO until it is cleared, which looks
     * exactly like "nobody reads this pipe". Clear both directions before judging anything. */
    uint8_t halt_eps[2];
    halt_eps[0] = ep_out;
    halt_eps[1] = ep_in;
    for (int i = 0; i < 2; i++) {
        int rc = ioctl(fd, USBDEVFS_CLEAR_HALT, &halt_eps[i]);
        if (rc < 0)
            logf_("CLEAR_HALT(0x%02x) failed: %s", halt_eps[i], strerror(errno));
        else
            logf_("CLEAR_HALT(0x%02x) ok", halt_eps[i]);
    }

    if (set_alt >= 0) {
        struct usbdevfs_setinterface si;
        memset(&si, 0, sizeof(si));
        si.interface = (unsigned int)iface;
        si.altsetting = (unsigned int)set_alt;
        int rc = ioctl(fd, USBDEVFS_SETINTERFACE, &si);
        if (rc < 0)
            logf_("SETINTERFACE(%d, alt %d) failed: %s", iface, set_alt, strerror(errno));
        else
            logf_("SETINTERFACE(%d, alt %d) ok", iface, set_alt);
    }

    unsigned char payload[VERSION_MESSAGE_BYTES];
    build_version_message(payload);
    hex_dump("handshake:", payload, sizeof(payload));

    unsigned char reply[512];

    if (read_first) {
        memset(reply, 0, sizeof(reply));
        bulk_xfer(fd, ep_in, reply, sizeof(reply), timeout_ms, "READ before write");
        hex_dump("  data:", reply, 32);
    }

    if (do_write) {
        for (int attempt = 1; attempt <= retries; attempt++) {
            logf_("write attempt %d/%d", attempt, retries);
            int written = bulk_xfer(fd, ep_out, payload, sizeof(payload), timeout_ms, "WRITE handshake");
            memset(reply, 0, sizeof(reply));
            int rc = bulk_xfer(fd, ep_in, reply, sizeof(reply), timeout_ms, "READ version reply");
            if (rc > 0) {
                hex_dump("  data:", reply, rc < 32 ? rc : 32);
                break;
            }
            if (written > 0 && rc < 0) {
                /* handshake accepted but nothing came back: one more look before giving up */
                continue;
            }
        }
    }

    if (do_claim) ioctl(fd, USBDEVFS_RELEASEINTERFACE, &claimed);

    if (restore && config > 0) {
        int back = 1;
        int rc = ioctl(fd, USBDEVFS_SETCONFIGURATION, &back);
        logf_("restore SET_CONFIGURATION(1) %s", rc < 0 ? strerror(errno) : "ok");
    }

    close(fd);
    logf_("done in %lld ms total", now_ms() - t0);
    return 0;
}
