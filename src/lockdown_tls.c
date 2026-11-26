#define _GNU_SOURCE
#include "lockdown_tls.h"
#include "lockdown_tls_identity.h"

#include <dlfcn.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

extern void iap2_log(const char *fmt, ...);

struct tls_api {
    void *ssl_lib;
    void *crypto_lib;

    const void *(*TLS_method)(void);
    void *(*SSL_CTX_new)(const void *);
    void (*SSL_CTX_free)(void *);
    int (*SSL_CTX_use_certificate)(void *, void *);
    int (*SSL_CTX_use_PrivateKey)(void *, void *);
    int (*SSL_CTX_check_private_key)(const void *);
    void *(*SSL_new)(void *);
    void (*SSL_free)(void *);
    void (*SSL_set_accept_state)(void *);
    void (*SSL_set_bio)(void *, void *, void *);
    int (*SSL_do_handshake)(void *);
    int (*SSL_read)(void *, void *, int);
    int (*SSL_write)(void *, const void *, int);
    int (*SSL_get_error)(const void *, int);
    int (*SSL_shutdown)(void *);

    const void *(*BIO_s_mem)(void);
    void *(*BIO_new)(const void *);
    int (*BIO_write)(void *, const void *, int);
    int (*BIO_read)(void *, void *, int);
    int (*BIO_free)(void *);
    void *(*BIO_new_mem_buf)(const void *, int);
    void *(*PEM_read_bio_X509)(void *, void **, void *, void *);
    void *(*PEM_read_bio_PrivateKey)(void *, void **, void *, void *);
    void (*X509_free)(void *);
    void (*EVP_PKEY_free)(void *);
    unsigned long (*ERR_get_error)(void);
    void (*ERR_error_string_n)(unsigned long, char *, size_t);
};

struct lockdown_tls {
    struct tls_api api;
    void *ctx;
    void *ssl;
    void *rbio;
    void *wbio;
    int active;
    int handshake_done;
    int failed;
};

#define LOAD(api, lib, field, symbol) do { \
    void *sym_ = dlsym((lib), (symbol)); \
    if (!sym_) { \
        iap2_log("tls: missing symbol %s: %s", (symbol), dlerror()); \
        goto load_fail; \
    } \
    memcpy(&(api)->field, &sym_, sizeof(sym_)); \
} while (0)

static int load_api(struct tls_api *api)
{
    memset(api, 0, sizeof(*api));
    api->ssl_lib = dlopen("libssl.so", RTLD_NOW | RTLD_LOCAL);
    if (!api->ssl_lib) {
        iap2_log("tls: dlopen libssl.so failed: %s", dlerror());
        return -1;
    }
    api->crypto_lib = dlopen("libcrypto.so", RTLD_NOW | RTLD_LOCAL);
    if (!api->crypto_lib) {
        iap2_log("tls: dlopen libcrypto.so failed: %s", dlerror());
        goto load_fail;
    }

    LOAD(api, api->ssl_lib, TLS_method, "TLS_method");
    LOAD(api, api->ssl_lib, SSL_CTX_new, "SSL_CTX_new");
    LOAD(api, api->ssl_lib, SSL_CTX_free, "SSL_CTX_free");
    LOAD(api, api->ssl_lib, SSL_CTX_use_certificate,
         "SSL_CTX_use_certificate");
    LOAD(api, api->ssl_lib, SSL_CTX_use_PrivateKey,
         "SSL_CTX_use_PrivateKey");
    LOAD(api, api->ssl_lib, SSL_CTX_check_private_key,
         "SSL_CTX_check_private_key");
    LOAD(api, api->ssl_lib, SSL_new, "SSL_new");
    LOAD(api, api->ssl_lib, SSL_free, "SSL_free");
    LOAD(api, api->ssl_lib, SSL_set_accept_state, "SSL_set_accept_state");
    LOAD(api, api->ssl_lib, SSL_set_bio, "SSL_set_bio");
    LOAD(api, api->ssl_lib, SSL_do_handshake, "SSL_do_handshake");
    LOAD(api, api->ssl_lib, SSL_read, "SSL_read");
    LOAD(api, api->ssl_lib, SSL_write, "SSL_write");
    LOAD(api, api->ssl_lib, SSL_get_error, "SSL_get_error");
    LOAD(api, api->ssl_lib, SSL_shutdown, "SSL_shutdown");

    LOAD(api, api->crypto_lib, BIO_s_mem, "BIO_s_mem");
    LOAD(api, api->crypto_lib, BIO_new, "BIO_new");
    LOAD(api, api->crypto_lib, BIO_write, "BIO_write");
    LOAD(api, api->crypto_lib, BIO_read, "BIO_read");
    LOAD(api, api->crypto_lib, BIO_free, "BIO_free");
    LOAD(api, api->crypto_lib, BIO_new_mem_buf, "BIO_new_mem_buf");
    LOAD(api, api->crypto_lib, PEM_read_bio_X509, "PEM_read_bio_X509");
    LOAD(api, api->crypto_lib, PEM_read_bio_PrivateKey,
         "PEM_read_bio_PrivateKey");
    LOAD(api, api->crypto_lib, X509_free, "X509_free");
    LOAD(api, api->crypto_lib, EVP_PKEY_free, "EVP_PKEY_free");
    LOAD(api, api->crypto_lib, ERR_get_error, "ERR_get_error");
    LOAD(api, api->crypto_lib, ERR_error_string_n, "ERR_error_string_n");
    return 0;

load_fail:
    if (api->crypto_lib)
        dlclose(api->crypto_lib);
    if (api->ssl_lib)
        dlclose(api->ssl_lib);
    memset(api, 0, sizeof(*api));
    return -1;
}

static void log_ssl_errors(struct lockdown_tls *tls, const char *where)
{
    unsigned long code;
    int count = 0;

    while ((code = tls->api.ERR_get_error()) != 0 && count++ < 4) {
        char text[256];

        tls->api.ERR_error_string_n(code, text, sizeof(text));
        iap2_log("tls: %s: %s", where, text);
    }
    if (count == 0)
        iap2_log("tls: %s (no OpenSSL error queued)", where);
}

static int flush_output(struct lockdown_tls *tls, lockdown_tls_send_fn send,
                        void *send_ctx)
{
    uint8_t buf[4096];

    if (!tls->wbio || !send)
        return 0;
    for (;;) {
        int n = tls->api.BIO_read(tls->wbio, buf, sizeof(buf));

        if (n <= 0)
            break;
        send(send_ctx, buf, (size_t)n);
    }
    return 0;
}

static int fail_tls(struct lockdown_tls *tls, const char *where, int code)
{
    tls->failed = 1;
    iap2_log("tls: %s failed (ssl_error=%d)", where, code);
    log_ssl_errors(tls, where);
    return -1;
}

struct lockdown_tls *lockdown_tls_create(void)
{
    return calloc(1, sizeof(struct lockdown_tls));
}

void lockdown_tls_destroy(struct lockdown_tls *tls)
{
    if (!tls)
        return;
    if (tls->ssl && tls->api.SSL_free)
        tls->api.SSL_free(tls->ssl);
    tls->ssl = NULL;
    tls->rbio = NULL;
    tls->wbio = NULL;
    if (tls->ctx && tls->api.SSL_CTX_free)
        tls->api.SSL_CTX_free(tls->ctx);
    tls->ctx = NULL;
    if (tls->api.ssl_lib)
        dlclose(tls->api.ssl_lib);
    if (tls->api.crypto_lib)
        dlclose(tls->api.crypto_lib);
    free(tls);
}

int lockdown_tls_start(struct lockdown_tls *tls)
{
    void *cert_bio = NULL;
    void *key_bio = NULL;
    void *cert = NULL;
    void *key = NULL;
    const void *method;
    int ret;

    if (!tls || tls->active)
        return tls && tls->active ? 0 : -1;
    if (load_api(&tls->api) < 0)
        return -1;

    method = tls->api.TLS_method();
    tls->ctx = tls->api.SSL_CTX_new(method);
    if (!tls->ctx) {
        log_ssl_errors(tls, "SSL_CTX_new");
        goto fail;
    }

    cert_bio = tls->api.BIO_new_mem_buf(lockdown_tls_certificate_pem, -1);
    key_bio = tls->api.BIO_new_mem_buf(lockdown_tls_private_key_pem, -1);
    if (!cert_bio || !key_bio) {
        iap2_log("tls: could not allocate PEM BIOs");
        goto fail;
    }
    cert = tls->api.PEM_read_bio_X509(cert_bio, NULL, NULL, NULL);
    key = tls->api.PEM_read_bio_PrivateKey(key_bio, NULL, NULL, NULL);
    if (!cert || !key) {
        log_ssl_errors(tls, "PEM identity parse");
        goto fail;
    }
    tls->api.BIO_free(cert_bio);
    cert_bio = NULL;
    tls->api.BIO_free(key_bio);
    key_bio = NULL;

    ret = tls->api.SSL_CTX_use_certificate(tls->ctx, cert);
    if (ret != 1) {
        log_ssl_errors(tls, "SSL_CTX_use_certificate");
        goto fail;
    }
    ret = tls->api.SSL_CTX_use_PrivateKey(tls->ctx, key);
    if (ret != 1) {
        log_ssl_errors(tls, "SSL_CTX_use_PrivateKey");
        goto fail;
    }
    if (tls->api.SSL_CTX_check_private_key(tls->ctx) != 1) {
        log_ssl_errors(tls, "SSL_CTX_check_private_key");
        goto fail;
    }
    tls->api.X509_free(cert);
    cert = NULL;
    tls->api.EVP_PKEY_free(key);
    key = NULL;

    tls->ssl = tls->api.SSL_new(tls->ctx);
    if (!tls->ssl) {
        log_ssl_errors(tls, "SSL_new");
        goto fail;
    }
    tls->rbio = tls->api.BIO_new(tls->api.BIO_s_mem());
    tls->wbio = tls->api.BIO_new(tls->api.BIO_s_mem());
    if (!tls->rbio || !tls->wbio) {
        iap2_log("tls: could not allocate memory BIOs");
        goto fail;
    }
    tls->api.SSL_set_bio(tls->ssl, tls->rbio, tls->wbio);
    tls->api.SSL_set_accept_state(tls->ssl);
    tls->active = 1;
    tls->failed = 0;
    tls->handshake_done = 0;
    iap2_log("tls: server context ready (system libssl, project RSA identity)");
    return 0;

fail:
    if (cert_bio)
        tls->api.BIO_free(cert_bio);
    if (key_bio)
        tls->api.BIO_free(key_bio);
    if (cert)
        tls->api.X509_free(cert);
    if (key)
        tls->api.EVP_PKEY_free(key);
    if (tls->ssl)
        tls->api.SSL_free(tls->ssl);
    tls->ssl = NULL;
    tls->rbio = NULL;
    tls->wbio = NULL;
    if (tls->ctx)
        tls->api.SSL_CTX_free(tls->ctx);
    tls->ctx = NULL;
    if (tls->api.ssl_lib)
        dlclose(tls->api.ssl_lib);
    if (tls->api.crypto_lib)
        dlclose(tls->api.crypto_lib);
    memset(&tls->api, 0, sizeof(tls->api));
    tls->failed = 1;
    return -1;
}

static int pump_handshake(struct lockdown_tls *tls, lockdown_tls_send_fn send,
                          void *send_ctx)
{
    int ret;
    int err;

    if (tls->handshake_done)
        return 0;
    ret = tls->api.SSL_do_handshake(tls->ssl);
    flush_output(tls, send, send_ctx);
    if (ret == 1) {
        tls->handshake_done = 1;
        iap2_log("tls: handshake complete");
        return 0;
    }
    err = tls->api.SSL_get_error(tls->ssl, ret);
    if (err == 2 || err == 3) /* WANT_READ / WANT_WRITE */
        return 0;
    return fail_tls(tls, "handshake", err);
}

int lockdown_tls_feed(struct lockdown_tls *tls, const uint8_t *data,
                      size_t len, lockdown_tls_send_fn send, void *send_ctx,
                      lockdown_tls_plain_fn plain, void *plain_ctx)
{
    size_t written = 0;

    if (!tls || !tls->active || tls->failed)
        return -1;
    if (!tls->rbio || !tls->wbio) {
        iap2_log("tls: memory BIO handles are unavailable");
        tls->failed = 1;
        return -1;
    }
    while (written < len) {
        size_t remain = len - written;
        int chunk = remain > INT_MAX ? INT_MAX : (int)remain;
        int n = tls->api.BIO_write(tls->rbio, data + written, chunk);

        if (n <= 0) {
            log_ssl_errors(tls, "BIO_write");
            tls->failed = 1;
            return -1;
        }
        written += (size_t)n;
    }

    if (pump_handshake(tls, send, send_ctx) < 0)
        return -1;
    if (!tls->handshake_done)
        return 0;

    for (;;) {
        uint8_t clear[4096];
        int n = tls->api.SSL_read(tls->ssl, clear, sizeof(clear));

        if (n > 0) {
            if (plain)
                plain(plain_ctx, clear, (size_t)n);
            continue;
        }
        {
            int err = tls->api.SSL_get_error(tls->ssl, n);

            flush_output(tls, send, send_ctx);
            if (err == 2 || err == 3) /* WANT_READ / WANT_WRITE */
                return 0;
            if (err == 6) /* ZERO_RETURN */
                return 0;
            return fail_tls(tls, "SSL_read", err);
        }
    }
}

int lockdown_tls_write(struct lockdown_tls *tls, const uint8_t *data,
                       size_t len, lockdown_tls_send_fn send, void *send_ctx)
{
    size_t written = 0;

    if (!tls || !tls->active || !tls->handshake_done || tls->failed)
        return -1;
    while (written < len) {
        size_t remain = len - written;
        int chunk = remain > 16384 ? 16384 : (int)remain;
        int n = tls->api.SSL_write(tls->ssl, data + written, chunk);

        flush_output(tls, send, send_ctx);
        if (n <= 0) {
            int err = tls->api.SSL_get_error(tls->ssl, n);

            if (err == 3) /* WANT_WRITE; output was drained, retry */
                continue;
            return fail_tls(tls, "SSL_write", err);
        }
        written += (size_t)n;
    }
    return 0;
}

int lockdown_tls_handshake_done(const struct lockdown_tls *tls)
{
    return tls && tls->handshake_done;
}

int lockdown_tls_failed(const struct lockdown_tls *tls)
{
    return !tls || tls->failed;
}


