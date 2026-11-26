#include "lockdown.h"
#include "lockdown_identity.h"

#include <stdio.h>
#include <string.h>

static uint32_t rd32(const uint8_t *p)
{
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
           ((uint32_t)p[2] << 8) | (uint32_t)p[3];
}

static const uint8_t *find_bytes(const uint8_t *hay, size_t hay_len,
                                 const char *needle)
{
    size_t needle_len = strlen(needle);
    size_t i;

    if (needle_len == 0 || needle_len > hay_len)
        return NULL;
    for (i = 0; i <= hay_len - needle_len; i++)
        if (memcmp(hay + i, needle, needle_len) == 0)
            return hay + i;
    return NULL;
}

static int text_after_key(const uint8_t *xml, size_t len, const char *key,
                          char *dst, size_t cap)
{
    char key_tag[128];
    const uint8_t *at;
    const uint8_t *start;
    const uint8_t *end;
    size_t n;

    snprintf(key_tag, sizeof(key_tag), "<key>%s</key>", key);
    at = find_bytes(xml, len, key_tag);
    if (!at)
        return -1;
    at += strlen(key_tag);
    start = find_bytes(at, len - (size_t)(at - xml), "<string>");
    if (!start)
        return -1;
    start += 8;
    end = find_bytes(start, len - (size_t)(start - xml), "</string>");
    if (!end)
        return -1;
    n = (size_t)(end - start);
    if (n == 0 || n >= cap)
        return -1;
    memcpy(dst, start, n);
    dst[n] = 0;
    return 0;
}

static void send_xml(lockdown_send_fn send, void *ctx, const char *xml)
{
    uint8_t frame[4 + 4096];
    size_t n = strlen(xml);
    size_t total = 4 + n;

    if (!send || total > sizeof(frame))
        return;
    frame[0] = (uint8_t)(n >> 24);
    frame[1] = (uint8_t)(n >> 16);
    frame[2] = (uint8_t)(n >> 8);
    frame[3] = (uint8_t)n;
    memcpy(frame + 4, xml, n);
    send(ctx, frame, total);
}

static void process_message(struct lockdown *l, const uint8_t *xml, size_t len,
                            lockdown_send_fn send, void *ctx)
{
    char request[64];
    char key[128];

    (void)l;
    if (text_after_key(xml, len, "Request", request, sizeof(request)) != 0) {
        send_xml(send, ctx, "<?xml version=\"1.0\"?><plist version=\"1.0\"><dict/></plist>");
        return;
    }

    if (strcmp(request, "GetValue") == 0 &&
        text_after_key(xml, len, "Key", key, sizeof(key)) == 0 &&
        strcmp(key, "DevicePublicKey") == 0) {
        send_xml(send, ctx,
            "<?xml version=\"1.0\"?><plist version=\"1.0\"><dict>"
            "<key>Value</key><data>" LOCKDOWN_DEVICE_PUBLIC_KEY_BASE64 "</data>"
            "</dict></plist>");
        return;
    }
    if (strcmp(request, "GetValue") == 0 &&
        text_after_key(xml, len, "Key", key, sizeof(key)) == 0 &&
        strcmp(key, "WiFiAddress") == 0) {
        send_xml(send, ctx,
            "<?xml version=\"1.0\"?><plist version=\"1.0\"><dict>"
            "<key>Value</key><string>02:00:00:00:00:01</string>"
            "</dict></plist>");
        return;
    }

    if (strcmp(request, "StartSession") == 0) {
        l->tls_requested = 1;
        send_xml(send, ctx,
            "<?xml version=\"1.0\"?><plist version=\"1.0\"><dict>"
            "<key>EnableSessionSSL</key><true/>"
            "</dict></plist>");
        return;
    }

    if (strcmp(request, "StartService") == 0) {
        char service[160];
        char response[512];
        unsigned port = 62079;

        if (text_after_key(xml, len, "Service", service, sizeof(service)) == 0 &&
            strcmp(service, "com.apple.carkit.service") == 0)
            port = 62080;
        snprintf(response, sizeof(response),
            "<?xml version=\"1.0\"?><plist version=\"1.0\"><dict>"
            "<key>Port</key><integer>%u</integer>"
            "<key>EnableServiceSSL</key><false/>"
            "</dict></plist>", port);
        send_xml(send, ctx, response);
        return;
    }

    send_xml(send, ctx, "<?xml version=\"1.0\"?><plist version=\"1.0\"><dict/></plist>");
}

void lockdown_init(struct lockdown *l)
{
    memset(l, 0, sizeof(*l));
}

int lockdown_feed(struct lockdown *l, const uint8_t *data, size_t len,
                  lockdown_send_fn send, void *ctx)
{
    if (len > sizeof(l->in) - l->in_len) {
        iap2_log("lockdown: receive buffer overflow, dropping %zu bytes", len);
        l->in_len = 0;
        return -1;
    }
    memcpy(l->in + l->in_len, data, len);
    l->in_len += len;

    while (l->in_len >= 4) {
        uint32_t message_len = rd32(l->in);
        size_t total;

        if (message_len == 0 || message_len > LOCKDOWN_MAX_MESSAGE) {
            iap2_log("lockdown: invalid plist length %u", message_len);
            l->in_len = 0;
            return -1;
        }
        total = 4 + (size_t)message_len;
        if (l->in_len < total)
            return 0;
        process_message(l, l->in + 4, message_len, send, ctx);
        l->requests++;
        memmove(l->in, l->in + total, l->in_len - total);
        l->in_len -= total;
    }
    return 0;
}
