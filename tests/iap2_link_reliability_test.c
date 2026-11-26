/* Offline regression tests; build with src/iap2link.c only. */
#include "iap2link.h"
#include <errno.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

static int failures;
#define CHECK(expr) do { if (!(expr)) { \
    printf("FAIL %s:%d: %s\n", __func__, __LINE__, #expr); failures++; } } while (0)
static unsigned g_link_rx_logs;
void iap2_log(const char *fmt, ...)
{
    char line[256];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(line, sizeof(line), fmt, ap);
    va_end(ap);
    if (strstr(line, "link RX"))
        g_link_rx_logs++;
}

static void ready(struct iap2_link *l)
{
    iap2_link_init(l, 0);
    l->state = IAP2_NORMAL;
    l->last_recv = 99;
    l->peer_max_length = 65535;
    l->peer_max_outgoing = 30;
    l->peer_max_acknowledgements = 3;
    l->peer_acknowledgement_timeout_ms = 500;
}

static size_t frame(uint8_t *p, uint8_t ctl, uint8_t seq, uint8_t session,
                    const uint8_t *body, size_t n)
{
    size_t i, total = 9 + (n ? n + 1 : 0);
    uint8_t sum = 0;
    p[0] = 0xff; p[1] = 0x5a; p[2] = (uint8_t)(total >> 8);
    p[3] = (uint8_t)total; p[4] = ctl; p[5] = seq; p[6] = 99;
    p[7] = session;
    for (i = 0; i < 8; i++) sum = (uint8_t)(sum + p[i]);
    p[8] = (uint8_t)(0 - sum);
    if (n) {
        memcpy(p + 9, body, n); sum = 0;
        for (i = 0; i < n; i++) sum = (uint8_t)(sum + body[i]);
        p[9+n] = (uint8_t)(0 - sum);
    }
    return total;
}

static void set_frame_ack(uint8_t *p, uint8_t ack)
{
    uint8_t sum = 0;
    size_t i;
    p[6] = ack;
    for (i = 0; i < 8; i++) sum = (uint8_t)(sum + p[i]);
    p[8] = (uint8_t)(0 - sum);
}

static void data(struct iap2_link *l, int seq, uint64_t at)
{
    uint8_t p[32], body[] = {0x40, 0x40, 0, 6, 0x1d, 1};
    size_t n = frame(p, IAP2_CTRL_ACK, (uint8_t)seq, 10, body, sizeof(body));
    iap2_link_feed(l, p, n, at);
}

static void test_link_frame_logging_is_sampled(void)
{
    struct iap2_link l;
    unsigned i;

    ready(&l);
    g_link_rx_logs = 0;
    for (i = 1; i <= 250; i++)
        data(&l, (int)(100 + i), i);
    CHECK(g_link_rx_logs <= 10);
}

static void test_failed_send_preserves_sequence(void)
{
    struct iap2_link l;
    uint8_t csm[6] = {0x40,0x40,0,6,0x1d,0};
    ready(&l); l.out_len = sizeof(l.out);
    CHECK(iap2_link_send_control(&l, csm, sizeof(csm), 1) < 0);
    CHECK(l.sent_seq == 99);
    l.out_len = 0;
    CHECK(iap2_link_send_control(&l, csm, sizeof(csm), 2) == 0);
    CHECK(l.out[5] == 100);
    l.out_len=0;
    CHECK(iap2_link_send_file(&l,csm,sizeof(csm),3)<0); /* no negotiated file session */
    l.peer_file_session=27;
    CHECK(iap2_link_send_file(&l,csm,sizeof(csm),4)==0 && l.out[7]==27);
    uint8_t p[16], reply[]={128,1},taken[2];
    size_t n=frame(p,IAP2_CTRL_ACK,100,27,reply,2);
    iap2_link_feed(&l,p,n,5);
    CHECK(iap2_link_take_file_control(&l,taken)==1 && taken[0]==128 && taken[1]==1);
    CHECK(iap2_link_take_file_control(&l,taken)==0);
}

static void test_unusable_peer_length(void)
{
    struct iap2_link l;
    uint8_t csm[6] = {0x40,0x40,0,6,0x1d,0};
    int limit;
    for (limit = 0; limit <= 10; limit++) {
        ready(&l); l.peer_max_length = limit;
        CHECK(iap2_link_send_control(&l, csm, sizeof(csm), 1) < 0);
        CHECK(l.sent_seq == 99 && l.out_len == 0);
    }
}

static void test_failed_ack_retried(void)
{
    struct iap2_link l;
    ready(&l); l.peer_max_acknowledgements = 0;
    l.out_len = sizeof(l.out);
    data(&l, 100, 1);
    CHECK(l.ack_pending);
    l.out_len = 0;
    iap2_link_tick(&l, 2);
    CHECK(l.out_len == 9 && l.out[6] == 100);
    CHECK(!l.ack_pending);
}

static void test_ack_deadline_not_postponed(void)
{
    struct iap2_link l;
    ready(&l); l.peer_max_acknowledgements = 30;
    data(&l, 100, 100);
    data(&l, 101, 500);
    iap2_link_tick(&l, 600);
    CHECK(l.out_len == 9 && l.out[6] == 101);
}

static void test_ack_window_and_zero_timeout(void)
{
    struct iap2_link l;
    ready(&l);
    data(&l, 100, 1); data(&l, 101, 2); data(&l, 102, 3);
    CHECK(l.out_len == 9 && l.out[6] == 102);
    ready(&l); l.peer_acknowledgement_timeout_ms = 0;
    data(&l, 100, 0);
    CHECK(l.out_len == 9 && l.out[6] == 100);
}

static void test_corrupt_header_resynchronizes(void)
{
    struct iap2_link l;
    uint8_t p[32];
    ready(&l);
    frame(p, IAP2_CTRL_ACK, 99, 0, NULL, 0);
    p[2] = 0xff; p[3] = 0xff; /* bad checksum, bogus long frame */
    frame(p + 9, IAP2_CTRL_ACK, 99, 0, NULL, 0);
    iap2_link_feed(&l, p, 18, 1);
    CHECK(l.rx_frames == 1 && l.rx_len == 0);
}

static void test_control_overflow_does_not_ack_lost_data(void)
{
    struct iap2_link l;
    ready(&l); l.control_rx_len = sizeof(l.control_rx);
    data(&l, 100, 1);
    CHECK(l.last_recv == 99);
    l.control_rx_len = 0;
    data(&l, 100, 2);
    CHECK(l.last_recv == 100 && l.control_rx_len == 6);
}

static void test_duplicate_syn_ack_is_not_session_data(void)
{
    struct iap2_link l;
    uint8_t p[32], sync[10] = {1,30,0xff,0xff,0x0f,0xa0,1,0xf4,4,3};
    size_t n;
    ready(&l);
    /* A delayed SYN+ACK must not consume a data sequence after NORMAL. */
    n = frame(p, IAP2_CTRL_SYNCHRONIZE | IAP2_CTRL_ACK, 100, 0, sync, 10);
    iap2_link_feed(&l, p, n, 1);
    CHECK(l.last_recv == 99 && l.control_rx_len == 0);
}

static void test_handshake_and_fragmented_data(void)
{
    struct iap2_link l;
    uint8_t p[32], sync[10] = {1,30,0xff,0xff,0x0f,0xa0,1,0xf4,4,3};
    size_t n, i;
    iap2_link_init(&l, 0);
    iap2_link_start(&l, 0);
    CHECK(l.state == IAP2_DETECTING && l.out_len == 0);
    n = frame(p, IAP2_CTRL_SYNCHRONIZE, 99, 0, sync, 10);
    for (i = 0; i < n; i++) iap2_link_feed(&l, p+i, 1, 1);
    CHECK(l.state == IAP2_NEGOTIATING);
    CHECK(l.out_len == 20 && l.out[4] ==
          (IAP2_CTRL_SYNCHRONIZE | IAP2_CTRL_ACK));
    CHECK(l.out[5] == 99 && l.out[6] == 99);
    l.out_len = 0;
    n = frame(p, IAP2_CTRL_ACK, 99, 0, NULL, 0);
    iap2_link_feed(&l, p, n, 2);
    CHECK(l.state == IAP2_NORMAL && l.last_recv == 99);
    l.out_len = 0;
    data(&l, 100, 3);
    data(&l, 100, 4);
    CHECK(l.control_rx_len == 6 && l.last_recv == 100);
}

static void test_device_answers_audi_syn_and_uses_its_session_ids(void)
{
    struct iap2_link l;
    const uint8_t marker[6] = {0xff,0x55,0x02,0x00,0xee,0x10};
    const uint8_t audi_sync[] = {
        0x01,0x01,0x10,0x00,0x07,0xd0,0x00,0x16,0x1e,0x01,0x01,
        0x00,0x02, 0x02,0x01,0x01, 0x03,0x02,0x01
    };
    const uint8_t csm[6] = {0x40,0x40,0,6,0x1d,0};
    uint8_t syn[64], ack[16];
    size_t n;

    iap2_link_init(&l, 0);
    iap2_link_start(&l, 0);
    CHECK(l.state == IAP2_DETECTING && l.out_len == 0);

    n = frame(syn, IAP2_CTRL_SYNCHRONIZE, 25, 0,
              audi_sync, sizeof(audi_sync));
    set_frame_ack(syn, 0);
    iap2_link_feed(&l, marker, sizeof(marker), 1);
    iap2_link_feed(&l, syn, n, 2);

    CHECK(l.state == IAP2_NEGOTIATING);
    CHECK(l.last_recv == 25 && l.sent_seq == 99);
    CHECK(l.out_len == 6 + 9 + sizeof(audi_sync) + 1);
    CHECK(memcmp(l.out, marker, sizeof(marker)) == 0);
    CHECK(l.out[6] == 0xff && l.out[7] == 0x5a);
    CHECK(l.out[10] == (IAP2_CTRL_SYNCHRONIZE | IAP2_CTRL_ACK));
    CHECK(l.out[11] == 99 && l.out[12] == 25 && l.out[13] == 0);
    CHECK(l.out[15] == 1 && l.out[16] == 127);
    CHECK(l.out[17] == 0xff && l.out[18] == 0xff);
    CHECK(memcmp(l.out + 19, audi_sync + 4,
                 sizeof(audi_sync) - 4) == 0);

    l.out_len = 0;
    n = frame(ack, IAP2_CTRL_ACK, 26, 0, NULL, 0);
    iap2_link_feed(&l, ack, n, 3);
    CHECK(l.state == IAP2_NORMAL);

    CHECK(iap2_link_send_control(&l, csm, sizeof(csm), 4) == 0);
    CHECK(l.out_len == 16 && l.out[5] == 100 && l.out[6] == 26);
    CHECK(l.out[7] == 1);
}

static void test_file_session_negotiation(void)
{
    struct iap2_link l;uint8_t p[64];
    uint8_t sync[]={1,30,0xff,0xff,0x0f,0xa0,1,0xf4,4,3,10,0,1,27,1,1};
    for(unsigned kind=0;kind<5;kind++){
        iap2_link_init(&l,0);iap2_link_start(&l,0);l.out_len=0;
        sync[10]=kind==2?27:10;sync[13]=kind==3?0:27;sync[15]=kind==1?2:1;
        size_t n=frame(p,IAP2_CTRL_SYNCHRONIZE,99,0,sync,kind==4?15:16);
        iap2_link_feed(&l,p,n,1);
        CHECK(l.peer_file_session==(kind==0?27:0));
    }
}

static void test_negotiation_ack_queue_failure_recovers(void)
{
    struct iap2_link l;
    uint8_t p[32], sync[10] = {1,30,0xff,0xff,0x0f,0xa0,1,0xf4,4,3};
    size_t n;
    iap2_link_init(&l, 0); iap2_link_start(&l, 0);
    l.out_len = sizeof(l.out);
    n = frame(p, IAP2_CTRL_SYNCHRONIZE, 99, 0, sync, 10);
    iap2_link_feed(&l, p, n, 0);
    CHECK(l.state == IAP2_NEGOTIATING && l.next_sync_ms == 0);
    l.out_len = 0;
    iap2_link_tick(&l, 0);
    CHECK(l.out_len == 20 && l.out[4] ==
          (IAP2_CTRL_SYNCHRONIZE | IAP2_CTRL_ACK));
}

static void test_piggyback_clears_ack_and_sequence_wraps(void)
{
    struct iap2_link l;
    uint8_t body[6] = {0x40,0x40,0,6,0x1d,0};
    ready(&l); l.sent_seq = 255;
    data(&l, 100, 1);
    CHECK(l.ack_pending);
    CHECK(iap2_link_send_control(&l, body, sizeof(body), 2) == 0);
    CHECK(l.sent_seq == 0 && l.out[5] == 0 && l.out[6] == 100);
    iap2_link_tick(&l, 1000);
    CHECK(l.out_len == 16 && !l.ack_pending);
}

static const uint8_t retry_csm[] = {0x40,0x40,0,6,0x1d,0};

static void reliable(struct iap2_link *l, int window)
{
    ready(l);
    l->peer_max_outgoing = window;
    l->peer_max_retransmissions = 2;
    l->peer_retransmission_timeout_ms = 100;
}

static void acknowledgement(struct iap2_link *l, unsigned ack, uint64_t now)
{
    uint8_t p[9];
    unsigned i, sum = 0;
    frame(p, IAP2_CTRL_ACK, 99, 0, NULL, 0);
    p[6] = (uint8_t)ack;
    for (i = 0; i < 8; i++) sum += p[i];
    p[8] = (uint8_t)(0u - sum);
    iap2_link_feed(l, p, sizeof(p), now);
}

static void test_timeout_replays_original_sequence_and_payload(void)
{
    struct iap2_link l;
    uint8_t output[32], mutable_csm[sizeof(retry_csm)];
    reliable(&l, 3);
    memcpy(mutable_csm, retry_csm, sizeof(retry_csm));
    CHECK(iap2_link_send_control(&l, mutable_csm, sizeof(mutable_csm), 10) == 0);
    CHECK(iap2_link_take(&l, output, sizeof(output)) == 16);
    memset(mutable_csm, 0, sizeof(mutable_csm));
    iap2_link_tick(&l, 109);
    CHECK(l.out_len == 0);
    iap2_link_tick(&l, 110);
    CHECK(l.out_len == 16 && l.out[5] == 100 && l.sent_seq == 100);
    CHECK(memcmp(l.out + 9, retry_csm, sizeof(retry_csm)) == 0);
    l.out_len = 0;
    acknowledgement(&l, 100, 111);
    iap2_link_tick(&l, 10000);
    CHECK(l.out_len == 0 && l.state == IAP2_NORMAL);
}

static void test_ack_releases_window_and_preserves_order(void)
{
    struct iap2_link l;
    reliable(&l, 2);
    CHECK(iap2_link_send_control(&l, retry_csm, sizeof(retry_csm), 0) == 0);
    CHECK(iap2_link_send_control(&l, retry_csm, sizeof(retry_csm), 0) == 0);
    l.out_len = 0; errno = 0;
    CHECK(iap2_link_send_control(&l, retry_csm, sizeof(retry_csm), 1) < 0);
    CHECK(errno == EAGAIN && l.sent_seq == 101 && l.out_len == 0);
    acknowledgement(&l, 100, 2);
    CHECK(iap2_link_send_control(&l, retry_csm, sizeof(retry_csm), 3) == 0);
    CHECK(l.out_len == 16 && l.out[5] == 102);
    l.out_len = 0;
    iap2_link_tick(&l, 103);
    CHECK(l.out_len == 32 && l.out[5] == 101 && l.out[21] == 102);
}

static void test_wrapped_cumulative_ack_rejects_stale_and_future(void)
{
    struct iap2_link l;
    reliable(&l, 3); l.sent_seq = 253;
    CHECK(iap2_link_send_control(&l, retry_csm, sizeof(retry_csm), 0) == 0);
    CHECK(iap2_link_send_control(&l, retry_csm, sizeof(retry_csm), 0) == 0);
    CHECK(iap2_link_send_control(&l, retry_csm, sizeof(retry_csm), 0) == 0);
    CHECK(l.sent_seq == 0);
    l.out_len = 0;
    acknowledgement(&l, 253, 1); /* duplicate ACK preceding pending */
    acknowledgement(&l, 252, 2); /* stale */
    acknowledgement(&l, 1, 3);   /* future */
    acknowledgement(&l, 128, 4); /* unrelated half-range */
    CHECK(iap2_link_send_control(&l, retry_csm, sizeof(retry_csm), 5) < 0);
    CHECK(l.sent_seq == 0);
    acknowledgement(&l, 255, 6); /* ACKs 254 and 255, retains 0 */
    iap2_link_tick(&l, 100);
    CHECK(l.out_len == 16 && l.out[5] == 0);
    l.out_len = 0;
    acknowledgement(&l, 254, 101); /* no longer in pending range */
    iap2_link_tick(&l, 200);
    CHECK(l.out_len == 16 && l.out[5] == 0);
    acknowledgement(&l, 0, 201);
    l.out_len = 0;
    iap2_link_tick(&l, 300);
    CHECK(l.state == IAP2_NORMAL && l.out_len == 0);
}

static void test_retry_output_pressure_preserves_packet_and_budget(void)
{
    struct iap2_link l;
    reliable(&l, 2);
    CHECK(iap2_link_send_control(&l, retry_csm, sizeof(retry_csm), 0) == 0);
    l.out_len = sizeof(l.out);
    iap2_link_tick(&l, 100);
    iap2_link_tick(&l, 200);
    iap2_link_tick(&l, 300);
    CHECK(l.state == IAP2_NORMAL && l.sent_seq == 100);
    l.out_len = 0;
    iap2_link_tick(&l, 301);
    CHECK(l.out_len == 16 && l.out[5] == 100);
    l.out_len = 0;
    iap2_link_tick(&l, 401);
    CHECK(l.out_len == 16 && l.out[5] == 100);
    l.out_len = 0;
    iap2_link_tick(&l, 501);
    CHECK(l.state == IAP2_DEAD && l.out_len == 0);
    CHECK(iap2_link_send_control(&l, retry_csm, sizeof(retry_csm), 502) < 0);
}

static void test_pending_payload_bytes_are_bounded(void)
{
    static uint8_t payload[40000];
    struct iap2_link l;
    reliable(&l, 30);
    CHECK(iap2_link_send_control(&l, payload, sizeof(payload), 0) == 0);
    l.out_len = 0; errno = 0;
    CHECK(iap2_link_send_control(&l, payload, sizeof(payload), 1) < 0);
    CHECK(errno == EAGAIN && l.sent_seq == 100 && l.out_len == 0);
    acknowledgement(&l, 100, 2);
    CHECK(iap2_link_send_control(&l, payload, sizeof(payload), 3) == 0);
    CHECK(l.sent_seq == 101);
}

static void test_pending_packet_count_is_bounded(void)
{
    struct iap2_link l;
    unsigned i;
    reliable(&l, 255);
    for (i = 0; i < 64; i++) {
        CHECK(iap2_link_send_control(&l, retry_csm, sizeof(retry_csm), 0) == 0);
        l.out_len = 0;
    }
    errno = 0;
    CHECK(iap2_link_send_control(&l, retry_csm, sizeof(retry_csm), 0) < 0);
    CHECK(errno == EAGAIN && l.sent_seq == 163);
}

static void test_extended_ack_replays_only_pending_missing_packets(void)
{
    struct iap2_link l;
    uint8_t p[32], missing[] = {55,100,100,102};
    size_t n;
    reliable(&l, 3);
    CHECK(iap2_link_send_control(&l, retry_csm, sizeof(retry_csm), 0) == 0);
    CHECK(iap2_link_send_control(&l, retry_csm, sizeof(retry_csm), 0) == 0);
    l.out_len = 0;
    n = frame(p, IAP2_CTRL_EXT_ACK, 99, 0, missing, sizeof(missing));
    iap2_link_feed(&l, p, n, 1);
    CHECK(l.out_len == 16 && l.out[5] == 100 && l.sent_seq == 101);
}

static void test_zero_retransmission_mode_never_blocks_or_replays(void)
{
    struct iap2_link l;
    unsigned i;
    ready(&l); l.peer_max_outgoing = 0;
    for (i = 0; i < 70; i++) {
        CHECK(iap2_link_send_control(&l, retry_csm, sizeof(retry_csm), 0) == 0);
        l.out_len = 0;
    }
    iap2_link_tick(&l, 100000);
    CHECK(l.out_len == 0 && l.state == IAP2_NORMAL);
}

static void test_ack_compacts_mixed_payloads_and_piggyback_ack(void)
{
    struct iap2_link l;
    uint8_t a[3] = {1,2,3}, b[5] = {4,5,6,7,8};
    uint8_t p[32];
    unsigned i, sum = 0;
    size_t n;
    reliable(&l, 2);
    CHECK(iap2_link_send_control(&l, a, sizeof(a), 0) == 0);
    CHECK(iap2_link_send_control(&l, b, sizeof(b), 0) == 0);
    l.out_len = 0;
    n = frame(p, IAP2_CTRL_ACK, 100, 10, retry_csm, sizeof(retry_csm));
    p[6] = 100;
    for (i = 0; i < 8; i++) sum += p[i];
    p[8] = (uint8_t)(0u - sum);
    iap2_link_feed(&l, p, n, 10);
    CHECK(l.control_rx_len == sizeof(retry_csm));
    CHECK(iap2_link_send_control(&l, a, sizeof(a), 20) == 0);
    l.out_len = 0;
    iap2_link_tick(&l, 120);
    CHECK(l.out_len == 28 && l.out[5] == 101 && l.out[20] == 102);
    CHECK(memcmp(l.out + 9, b, sizeof(b)) == 0);
    CHECK(memcmp(l.out + 24, a, sizeof(a)) == 0);
}

static void test_failed_initial_queue_has_no_phantom_pending(void)
{
    struct iap2_link l;
    reliable(&l, 1); l.out_len = sizeof(l.out);
    CHECK(iap2_link_send_control(&l, retry_csm, sizeof(retry_csm), 0) < 0);
    l.out_len = 0;
    iap2_link_tick(&l, 1000);
    CHECK(l.out_len == 0 && l.state == IAP2_NORMAL);
    CHECK(iap2_link_send_control(&l, retry_csm, sizeof(retry_csm), 1001) == 0);
    CHECK(l.sent_seq == 100 && l.out[5] == 100);
}

static void test_dead_link_ignores_frames_after_reset(void)
{
    struct iap2_link l;
    uint8_t p[64];
    size_t n;
    reliable(&l, 1);
    CHECK(iap2_link_send_control(&l, retry_csm, sizeof(retry_csm), 0) == 0);
    n = frame(p, IAP2_CTRL_RESET, 99, 0, NULL, 0);
    n += frame(p + n, IAP2_CTRL_ACK, 100, 10, retry_csm, sizeof(retry_csm));
    iap2_link_feed(&l, p, n, 10);
    CHECK(l.state == IAP2_DEAD && l.control_rx_len == 0 && l.out_len == 0);
}

static void test_dropped_middle_packet_recovers_between_real_engines(void)
{
    struct iap2_link sender, receiver;
    uint8_t p[64], output[64], expected[18];
    size_t n;
    reliable(&sender, 3); ready(&receiver);
    receiver.peer_max_acknowledgements = 0;
    CHECK(iap2_link_send_control(&sender, retry_csm, sizeof(retry_csm), 0) == 0);
    CHECK(iap2_link_send_control(&sender, retry_csm, sizeof(retry_csm), 0) == 0);
    CHECK(iap2_link_send_control(&sender, retry_csm, sizeof(retry_csm), 0) == 0);
    n = iap2_link_take(&sender, p, sizeof(p));
    CHECK(n == 48);
    iap2_link_feed(&receiver, p, 16, 1);
    iap2_link_feed(&receiver, p + 32, 16, 2); /* Drop seq101. */
    n = iap2_link_take(&receiver, output, sizeof(output));
    iap2_link_feed(&sender, output, n, 3);
    iap2_link_tick(&sender, 100);
    n = iap2_link_take(&sender, p, sizeof(p));
    CHECK(n == 32 && p[5] == 101 && p[21] == 102);
    iap2_link_feed(&receiver, p, n, 101);
    n = iap2_link_take(&receiver, output, sizeof(output));
    iap2_link_feed(&sender, output, n, 102);
    memcpy(expected, retry_csm, 6);
    memcpy(expected + 6, retry_csm, 6);
    memcpy(expected + 12, retry_csm, 6);
    CHECK(receiver.control_rx_len == sizeof(expected));
    CHECK(memcmp(receiver.control_rx, expected, sizeof(expected)) == 0);
    iap2_link_tick(&sender, 10000);
    CHECK(sender.out_len == 0 && sender.state == IAP2_NORMAL);
}

static void test_zero_retry_timeout_is_bounded_and_zero_window_blocks(void)
{
    struct iap2_link l;
    reliable(&l, 0); errno = 0;
    CHECK(iap2_link_send_control(&l, retry_csm, sizeof(retry_csm), 0) < 0);
    CHECK(errno == EAGAIN && l.sent_seq == 99 && l.out_len == 0);
    l.peer_max_outgoing = 1; l.peer_retransmission_timeout_ms = 0;
    CHECK(iap2_link_send_control(&l, retry_csm, sizeof(retry_csm), 0) == 0);
    l.out_len = 0;
    iap2_link_tick(&l, 0);
    CHECK(l.out_len == 0);
    iap2_link_tick(&l, 1);
    CHECK(l.out_len == 16 && l.state == IAP2_NORMAL);
    l.out_len = 0;
    iap2_link_tick(&l, 1);
    CHECK(l.out_len == 0 && l.state == IAP2_NORMAL);
}

static void test_duplicate_peer_data_during_unacked_send(void)
{
    struct iap2_link l;
    uint8_t wire[64], delivered[32];
    reliable(&l, 1); l.peer_max_acknowledgements = 0;
    CHECK(iap2_link_send_control(&l, retry_csm, sizeof(retry_csm), 0) == 0);
    size_t original = iap2_link_take(&l, wire, sizeof(wire));
    CHECK(original == 16 && wire[5] == 100);
    data(&l, 100, 1); /* peer still acknowledges only 99 */
    CHECK(iap2_link_take_control(&l, delivered, sizeof(delivered)) == 6);
    CHECK(iap2_link_take(&l, wire, sizeof(wire)) == 9);
    CHECK(wire[4] == IAP2_CTRL_ACK && wire[5] == 100 && wire[6] == 100 && wire[7] == 0);
    data(&l, 100, 2); /* duplicate must be ACKed without delivering twice */
    CHECK(iap2_link_take_control(&l, delivered, sizeof(delivered)) == 0);
    CHECK(iap2_link_take(&l, wire, sizeof(wire)) == 9);
    CHECK(wire[5] == 100 && wire[6] == 100 && wire[7] == 0);
    CHECK(l.sent_seq == 100 && l.pending_count == 1);
    iap2_link_tick(&l, 100);
    CHECK(iap2_link_take(&l, wire, sizeof(wire)) == original);
    CHECK(wire[5] == 100 && !memcmp(wire + 9, retry_csm, sizeof(retry_csm)));
    acknowledgement(&l, 100, 101);
    CHECK(l.pending_count == 0);
    CHECK(iap2_link_send_control(&l, retry_csm, sizeof(retry_csm), 102) == 0);
    CHECK(l.sent_seq == 101);
}
int main(void)
{
    test_link_frame_logging_is_sampled();
    test_duplicate_peer_data_during_unacked_send();
    test_failed_send_preserves_sequence();
    test_unusable_peer_length();
    test_failed_ack_retried();
    test_ack_deadline_not_postponed();
    test_ack_window_and_zero_timeout();
    test_corrupt_header_resynchronizes();
    test_control_overflow_does_not_ack_lost_data();
    test_duplicate_syn_ack_is_not_session_data();
    test_handshake_and_fragmented_data();
    test_device_answers_audi_syn_and_uses_its_session_ids();
    test_file_session_negotiation();
    test_negotiation_ack_queue_failure_recovers();
    test_piggyback_clears_ack_and_sequence_wraps();
    test_timeout_replays_original_sequence_and_payload();
    test_ack_releases_window_and_preserves_order();
    test_wrapped_cumulative_ack_rejects_stale_and_future();
    test_retry_output_pressure_preserves_packet_and_budget();
    test_pending_payload_bytes_are_bounded();
    test_pending_packet_count_is_bounded();
    test_extended_ack_replays_only_pending_missing_packets();
    test_zero_retransmission_mode_never_blocks_or_replays();
    test_ack_compacts_mixed_payloads_and_piggyback_ack();
    test_failed_initial_queue_has_no_phantom_pending();
    test_dead_link_ignores_frames_after_reset();
    test_dropped_middle_packet_recovers_between_real_engines();
    test_zero_retry_timeout_is_bounded_and_zero_window_blocks();
    CHECK(sizeof(struct iap2_link) < 3u * IAP2_LINK_BUFFER_BYTES + 128u * 1024u);
    printf("iap2 link object: %lu bytes\n", (unsigned long)sizeof(struct iap2_link));
    printf("iap2 link reliability: %d failures\n", failures);
    return failures != 0;
}
