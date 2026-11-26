#ifndef LOCKDOWN_H
#define LOCKDOWN_H

#include <stddef.h>
#include <stdint.h>

#define LOCKDOWN_MAX_MESSAGE (1024u * 1024u)
#define LOCKDOWN_BUFFER_BYTES (LOCKDOWN_MAX_MESSAGE + 4u)

typedef void (*lockdown_send_fn)(void *ctx, const uint8_t *data, size_t len);

struct lockdown {
    uint8_t in[LOCKDOWN_BUFFER_BYTES];
    size_t in_len;
    uint32_t requests;
    int tls_requested;
};

extern void iap2_log(const char *fmt, ...);

void lockdown_init(struct lockdown *l);
int lockdown_feed(struct lockdown *l, const uint8_t *data, size_t len,
                  lockdown_send_fn send, void *ctx);

#endif
