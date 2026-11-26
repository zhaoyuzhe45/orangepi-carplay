#define _CRT_SECURE_NO_WARNINGS
#include "usbmux.h"

#include <assert.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

static unsigned g_tcp_header_logs;
void iap2_log(const char *fmt, ...)
{
    char line[256];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(line, sizeof(line), fmt, ap);
    va_end(ap);
    if (strstr(line, "usbmux TCP"))
        g_tcp_header_logs++;
}

static void put16(uint8_t *p, uint16_t v)
{
    p[0] = (uint8_t)(v >> 8);
    p[1] = (uint8_t)v;
}

static void put32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)(v >> 24);
    p[1] = (uint8_t)(v >> 16);
    p[2] = (uint8_t)(v >> 8);
    p[3] = (uint8_t)v;
}

static size_t make_frame(uint8_t *dst, uint32_t protocol, uint32_t word8,
                         size_t payload_len)
{
    memset(dst, 0, USBMUX_HEADER_BYTES + payload_len);
    put32(dst, protocol);
    put32(dst + 4, (uint32_t)(USBMUX_HEADER_BYTES + payload_len));
    put32(dst + 8, word8);
    return USBMUX_HEADER_BYTES + payload_len;
}

static void handshake(struct usbmux *mux)
{
    uint8_t frame[64];
    size_t n;

    n = make_frame(frame, USBMUX_PROTO_VERSION, USBMUX_VERSION, 4);
    usbmux_feed(mux, frame, n);

    frame[USBMUX_HEADER_BYTES] = 0x07;
    n = make_frame(frame, USBMUX_PROTO_SETUP, 0, 1);
    frame[USBMUX_HEADER_BYTES] = 0x07;
    usbmux_feed(mux, frame, n);

    n = make_frame(frame, USBMUX_PROTO_TCP, USBMUX_MAGIC,
                   TCP_HEADER_BYTES);
    put16(frame + USBMUX_HEADER_BYTES, 49152);
    put16(frame + USBMUX_HEADER_BYTES + 2, USBMUX_LOCAL_PORT);
    put32(frame + USBMUX_HEADER_BYTES + 4, 0);
    frame[USBMUX_HEADER_BYTES + 12] = 5 << 4;
    frame[USBMUX_HEADER_BYTES + 13] = TCP_FLAG_SYN;
    put16(frame + USBMUX_HEADER_BYTES + 14, 512);
    usbmux_feed(mux, frame, n);
    assert(usbmux_connected(mux));
}

static void test_pair_frame_is_reassembled_across_usb_reads(void)
{
    enum { FRAME_BYTES = 4610, FIRST_READ = 4096, PAYLOAD_BYTES = 4574 };
    struct usbmux mux;
    uint8_t frame[FRAME_BYTES];
    uint8_t received[FRAME_BYTES];
    size_t i;
    size_t n;

    usbmux_init(&mux);
    handshake(&mux);

    n = make_frame(frame, USBMUX_PROTO_TCP, USBMUX_MAGIC,
                   TCP_HEADER_BYTES + PAYLOAD_BYTES);
    assert(n == FRAME_BYTES);
    put16(frame + USBMUX_HEADER_BYTES, 49152);
    put16(frame + USBMUX_HEADER_BYTES + 2, USBMUX_LOCAL_PORT);
    put32(frame + USBMUX_HEADER_BYTES + 4, 1);
    frame[USBMUX_HEADER_BYTES + 12] = 5 << 4;
    frame[USBMUX_HEADER_BYTES + 13] = TCP_FLAG_ACK | TCP_FLAG_PSH;
    put16(frame + USBMUX_HEADER_BYTES + 14, 512);
    for (i = 0; i < PAYLOAD_BYTES; i++)
        frame[USBMUX_HEADER_BYTES + TCP_HEADER_BYTES + i] = (uint8_t)i;

    usbmux_feed(&mux, frame, FIRST_READ);
    assert(mux.rx_len == FIRST_READ);
    assert(usbmux_take_in(&mux, received, sizeof(received)) == 0);

    usbmux_feed(&mux, frame + FIRST_READ, FRAME_BYTES - FIRST_READ);
    assert(mux.rx_len == 0);
    assert(usbmux_take_in(&mux, received, sizeof(received)) == PAYLOAD_BYTES);
    for (i = 0; i < PAYLOAD_BYTES; i++)
        assert(received[i] == (uint8_t)i);
}

static void test_endpoint_reconfigure_preserves_tcp_handshake(void)
{
    struct usbmux mux;
    uint8_t frame[USBMUX_HEADER_BYTES + TCP_HEADER_BYTES + 5];
    uint8_t received[16];
    size_t n;
    size_t i;

    usbmux_init(&mux);
    handshake(&mux);
    assert(usbmux_connected(&mux));

    /* The host app may reselect the USB configuration after SYN. Any frame
     * fragment from the old endpoint is invalid, but the TCP relay itself is
     * still in progress and must accept the next ACK/data segment. */
    mux.rx_len = 5;
    memset(mux.rx, 0xa5, 5);
    usbmux_endpoint_reconfigured(&mux);
    assert(mux.rx_len == 0);
    assert(usbmux_connected(&mux));

    n = make_frame(frame, USBMUX_PROTO_TCP, USBMUX_MAGIC,
                   TCP_HEADER_BYTES + 5);
    put16(frame + USBMUX_HEADER_BYTES, 49152);
    put16(frame + USBMUX_HEADER_BYTES + 2, USBMUX_LOCAL_PORT);
    put32(frame + USBMUX_HEADER_BYTES + 4, 1);
    frame[USBMUX_HEADER_BYTES + 12] = 5 << 4;
    frame[USBMUX_HEADER_BYTES + 13] = TCP_FLAG_ACK | TCP_FLAG_PSH;
    put16(frame + USBMUX_HEADER_BYTES + 14, 512);
    for (i = 0; i < 5; i++)
        frame[USBMUX_HEADER_BYTES + TCP_HEADER_BYTES + i] = (uint8_t)i;
    usbmux_feed(&mux, frame, n);
    assert(usbmux_take_in(&mux, received, sizeof(received)) == 5);
    for (i = 0; i < 5; i++)
        assert(received[i] == (uint8_t)i);
}


static void test_tcp_segment_logging_is_sampled(void)
{
    struct usbmux mux;
    uint8_t frame[USBMUX_HEADER_BYTES + TCP_HEADER_BYTES + 1];
    size_t n;
    unsigned i;

    usbmux_init(&mux);
    handshake(&mux);
    g_tcp_header_logs = 0;
    for (i = 0; i < 250; i++) {
        n = make_frame(frame, USBMUX_PROTO_TCP, USBMUX_MAGIC,
                       TCP_HEADER_BYTES + 1);
        put16(frame + USBMUX_HEADER_BYTES, 49152);
        put16(frame + USBMUX_HEADER_BYTES + 2, USBMUX_LOCAL_PORT);
        put32(frame + USBMUX_HEADER_BYTES + 4, i + 1);
        frame[USBMUX_HEADER_BYTES + 12] = 5 << 4;
        frame[USBMUX_HEADER_BYTES + 13] = TCP_FLAG_ACK | TCP_FLAG_PSH;
        put16(frame + USBMUX_HEADER_BYTES + 14, 512);
        frame[USBMUX_HEADER_BYTES + TCP_HEADER_BYTES] = (uint8_t)i;
        usbmux_feed(&mux, frame, n);
    }
    assert(g_tcp_header_logs <= 10);
}

int main(void)
{
    test_tcp_segment_logging_is_sampled();
    test_pair_frame_is_reassembled_across_usb_reads();
    test_endpoint_reconfigure_preserves_tcp_handshake();
    puts("usbmux tests passed");
    return 0;
}
