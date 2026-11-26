#define _GNU_SOURCE
#include "lockdown.h"

#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

struct capture {
    uint8_t bytes[8192];
    size_t len;
};


void iap2_log(const char *fmt, ...) { (void)fmt; }
static void capture_send(void *ctx, const uint8_t *data, size_t len)
{
    struct capture *c = ctx;
    assert(c->len + len <= sizeof(c->bytes));
    memcpy(c->bytes + c->len, data, len);
    c->len += len;
}

static size_t frame_xml(uint8_t *dst, const char *xml)
{
    size_t n = strlen(xml);
    dst[0] = (uint8_t)(n >> 24);
    dst[1] = (uint8_t)(n >> 16);
    dst[2] = (uint8_t)(n >> 8);
    dst[3] = (uint8_t)n;
    memcpy(dst + 4, xml, n);
    return n + 4;
}

static void test_fragmented_set_value_is_routed_once(void)
{
    struct lockdown lockdown;
    struct capture out = {0};
    const char *xml =
        "<?xml version=\"1.0\"?><plist version=\"1.0\"><dict>"
        "<key>Request</key><string>SetValue</string>"
        "<key>Key</key><string>UntrustedHostBUID</string>"
        "<key>Value</key><string>test-buid</string>"
        "</dict></plist>";
    uint8_t frame[1024];
    size_t n = frame_xml(frame, xml);

    lockdown_init(&lockdown);
    assert(lockdown_feed(&lockdown, frame, 3, capture_send, &out) == 0);
    assert(out.len == 0);
    assert(lockdown_feed(&lockdown, frame + 3, n - 3, capture_send, &out) == 0);
    assert(lockdown.requests == 1);
    assert(out.len >= 4);
    assert(out.bytes[0] == 0 && out.bytes[1] == 0);
    assert(out.bytes[2] == (uint8_t)(out.len >> 8));
    assert(out.bytes[3] == (uint8_t)(out.len - 4));
    out.bytes[out.len] = 0;
    assert(strstr((const char *)out.bytes + 4, "<dict/>") != NULL);
}

static void send_one(struct lockdown *l, struct capture *out, const char *xml)
{
    uint8_t frame[4096];
    size_t n = frame_xml(frame, xml);
    assert(lockdown_feed(l, frame, n, capture_send, out) == 0);
    out->bytes[out->len] = 0;
}

static void test_get_value_returns_device_public_key(void)
{
    struct lockdown lockdown;
    struct capture out = {0};
    lockdown_init(&lockdown);
    send_one(&lockdown, &out,
        "<?xml version=\"1.0\"?><plist version=\"1.0\"><dict>"
        "<key>Request</key><string>GetValue</string>"
        "<key>Key</key><string>DevicePublicKey</string></dict></plist>");
    assert(lockdown.requests == 1);
    assert(strstr((const char *)out.bytes + 4, "<key>Value</key><data>") != NULL);
    assert(strstr((const char *)out.bytes + 4, "LS0tLS1CRUdJTiBSU0EgUFVCTElDIEtFWS0tLS0t") != NULL);
}
static void test_get_value_returns_wifi_address(void)
{
    struct lockdown lockdown;
    struct capture out = {0};
    lockdown_init(&lockdown);
    send_one(&lockdown, &out,
        "<?xml version=\"1.0\"?><plist version=\"1.0\"><dict>"
        "<key>Request</key><string>GetValue</string>"
        "<key>Key</key><string>WiFiAddress</string></dict></plist>");
    assert(strstr((const char *)out.bytes + 4, "<key>Value</key><string>02:00:00:00:00:01</string>") != NULL);
}
static void test_start_session_enables_tls(void)
{
    struct lockdown lockdown;
    struct capture out = {0};
    lockdown_init(&lockdown);
    send_one(&lockdown, &out,
        "<?xml version=\"1.0\"?><plist version=\"1.0\"><dict>"
        "<key>Request</key><string>StartSession</string>"
        "<key>HostID</key><string>host</string></dict></plist>");
    assert(strstr((const char *)out.bytes + 4, "<key>EnableSessionSSL</key><true/>") != NULL);
}
static void test_start_carkit_service_returns_a_port(void)
{
    struct lockdown lockdown;
    struct capture out = {0};
    lockdown_init(&lockdown);
    send_one(&lockdown, &out,
        "<?xml version=\"1.0\"?><plist version=\"1.0\"><dict>"
        "<key>Request</key><string>StartService</string>"
        "<key>Service</key><string>com.apple.carkit.service</string></dict></plist>");
    assert(strstr((const char *)out.bytes + 4, "<key>Port</key><integer>62080</integer>") != NULL);
    assert(strstr((const char *)out.bytes + 4, "<key>EnableServiceSSL</key><false/>") != NULL);
}
int main(void)
{
    test_fragmented_set_value_is_routed_once();
    test_get_value_returns_device_public_key();
    test_get_value_returns_wifi_address();
    test_start_session_enables_tls();
    test_start_carkit_service_returns_a_port();
    puts("lockdown tests passed");
    return 0;
}