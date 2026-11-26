#ifndef LOCKDOWN_TLS_H
#define LOCKDOWN_TLS_H

#include <stddef.h>
#include <stdint.h>

struct lockdown_tls;

typedef void (*lockdown_tls_send_fn)(void *ctx, const uint8_t *data,
                                     size_t len);
typedef void (*lockdown_tls_plain_fn)(void *ctx, const uint8_t *data,
                                      size_t len);

struct lockdown_tls *lockdown_tls_create(void);
void lockdown_tls_destroy(struct lockdown_tls *tls);
int lockdown_tls_start(struct lockdown_tls *tls);
int lockdown_tls_feed(struct lockdown_tls *tls, const uint8_t *data,
                      size_t len, lockdown_tls_send_fn send, void *send_ctx,
                      lockdown_tls_plain_fn plain, void *plain_ctx);
int lockdown_tls_write(struct lockdown_tls *tls, const uint8_t *data,
                       size_t len, lockdown_tls_send_fn send, void *send_ctx);
int lockdown_tls_handshake_done(const struct lockdown_tls *tls);
int lockdown_tls_failed(const struct lockdown_tls *tls);

#endif /* LOCKDOWN_TLS_H */
