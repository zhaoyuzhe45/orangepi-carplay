/*
 * iap2_session_test.c -- identification stage machine and its retransmissions.
 *
 * The link layer marks control frames acknowledged immediately when the peer
 * advertises maxRetransmissions=0, so a single lost frame used to park the
 * session in identification until a replug. These checks pin the recovery
 * behaviour: unanswered non-reliable stages re-emit on a timer, reliable
 * one-packet windows drain 0x1D02 before queueing 0xAA00, the phone/accessory
 * direction is respected, and identification restarts after leaving NORMAL.
 *
 * The session object is exercised against a stub link so no USB endpoint is
 * needed; only the CSM bytes handed to iap2_link_send_control are inspected.
 */
#include "iap2session.h"
#include "carplay_now_playing.h"
#include "carplay_calls.h"
#ifdef IAP2_REAL_VEHICLE_STATE
#include "carplay_vehicle_state.h"
#endif

#include <assert.h>
#include <errno.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Fail without a platform crash dialog in offline Windows runs. */
#undef assert
#define assert(expr) do { if (!(expr)) { \
	fprintf(stderr, "FAIL %s:%d: %s\n", __func__, __LINE__, #expr); \
	exit(EXIT_FAILURE); } } while (0)

/* ------------------------------------------------------------------ stubs */

static char g_last_identity_log[256];
static char g_last_unimplemented_log[256];
static unsigned g_csm_rx_logs;
static unsigned g_location_logs;

void iap2_log(const char *fmt, ...)
{
	va_list args;
	char line[256];
	va_start(args, fmt);
	vsnprintf(line, sizeof(line), fmt, args);
	va_end(args);
	if (strstr(line, "identity Messages"))
		snprintf(g_last_identity_log, sizeof(g_last_identity_log), "%s", line);
	if (strstr(line, "unimplemented"))
		snprintf(g_last_unimplemented_log, sizeof(g_last_unimplemented_log), "%s", line);
	if (strstr(line, "csm RX message="))
		g_csm_rx_logs++;
	if (strstr(line, "car location updated"))
		g_location_logs++;
}

#define CAPTURE_RING 8
#define CAPTURE_BODY 512

struct capture {
	uint16_t id;
	size_t len;
	uint8_t body[CAPTURE_BODY];
};

static struct capture g_caps[CAPTURE_RING];
static size_t g_send_total;
static uint16_t g_fail_message;
static unsigned g_window_outstanding;
static unsigned g_window_limit;
static int g_connect_result;
static int g_now_available;
static unsigned g_app_commands;
static int g_comm_available;
static int g_calls_available;
static int g_route_available;
static struct carplay_route_snapshot g_route;
static int g_file_blocked,g_art_fail,g_file_reply_pending;
static unsigned g_file_sent,g_art_reads;
static uint8_t g_file_packet[1030],g_file_reply[2];static size_t g_file_size;
int iap2_link_send_file(struct iap2_link *l,const uint8_t *p,size_t n,uint64_t now){
    (void)now;if(!l->peer_file_session || g_file_blocked)return -1;
    assert(n<=sizeof(g_file_packet));memcpy(g_file_packet,p,n);g_file_size=n;g_file_sent++;return 0;
}
int iap2_link_take_file_control(struct iap2_link *l,uint8_t p[2]){
    (void)l;if(!g_file_reply_pending)return 0;memcpy(p,g_file_reply,2);g_file_reply_pending=0;return 1;
}
int carplay_art_read(uint32_t generation,uint32_t total,uint32_t offset,uint8_t *out,size_t cap,size_t *len){
    (void)generation;g_art_reads++;if(g_art_fail){g_art_fail--;return -1;}
    assert(offset<total);*len=total-offset;if(*len>2)*len=2;assert(*len<=cap);
    for(size_t i=0;i<*len;i++)out[i]=(uint8_t)(offset+i+1);return 0;
}
int carplay_route_read(uint64_t now,struct carplay_route_snapshot *out) {
    (void)now;if(!g_route_available)return -1;*out=g_route;return 0;
}
static struct carplay_call_list g_calls;
int carplay_calls_read(uint64_t now,struct carplay_call_list *out) {
    (void)now;if(!g_calls_available)return -1;*out=g_calls;return 0;
}
int carplay_comm_read(uint64_t now,struct carplay_communications *out) {
    if(!g_comm_available)return -1;
    memset(out,0,sizeof(*out));out->sampled_ms=now;return 0;
}
static struct carplay_now_playing g_now;
int carplay_now_read(uint64_t now, struct carplay_now_playing *out) {
    (void)now; if(!g_now_available)return -1; *out=g_now;return 0;
}
int carplay_app_command(uint32_t op,uint32_t arg) {
    assert(op==3 && arg==1000);g_app_commands++;return 0;
}
static unsigned g_connect_calls;

static const struct capture *sent_back(size_t back)
{
	assert(back < CAPTURE_RING);
	assert(back < g_send_total);
	return &g_caps[(g_send_total - 1 - back) % CAPTURE_RING];
}

/* The real link layer is not part of this test: the stage machine only needs a
 * link object whose state the test can drive. */
void iap2_link_init(struct iap2_link *l, uint64_t now_ms)
{
	(void)now_ms;
	memset(l, 0, sizeof(*l));
	l->state = IAP2_IDLE;
	l->sent_seq = IAP2_INITIAL_SEQUENCE;
}

int iap2_link_send_control(struct iap2_link *l, const uint8_t *csm, size_t len,
			   uint64_t now_ms)
{
	struct capture *slot;

	(void)now_ms;
	if (l->state != IAP2_NORMAL)
		return -1;
	assert(len >= IAP2_CSM_HEADER_BYTES);
	assert(csm[0] == 0x40 && csm[1] == 0x40);
	assert((size_t)(((size_t)csm[2] << 8) | csm[3]) == len);
	if ((((uint16_t)csm[4] << 8) | csm[5]) == g_fail_message)
		return -1;
	if (g_window_limit && g_window_outstanding >= g_window_limit) {
		errno = EAGAIN;
		return -1;
	}
	slot = &g_caps[g_send_total % CAPTURE_RING];
	slot->id = (uint16_t)(((uint16_t)csm[4] << 8) | csm[5]);
	slot->len = len - IAP2_CSM_HEADER_BYTES;
	assert(slot->len <= CAPTURE_BODY);
	memcpy(slot->body, csm + IAP2_CSM_HEADER_BYTES, slot->len);
	g_send_total++;
	if (g_window_limit)
		g_window_outstanding++;
	return 0;
}

int ncm_bridge_set_peer_mac(const char *identifier)
{
	(void)identifier;
	return 0;
}

int ncm_bridge_connect_endpoint(const char *const addresses[], size_t count,
				uint16_t port)
{
	(void)addresses;
	(void)count;
	(void)port;
	g_connect_calls++;
	return g_connect_result;
}

int carplay_time_sync_from_apple_epoch(uint64_t seconds,
				       int64_t *utc_epoch_seconds)
{
	*utc_epoch_seconds = (int64_t)seconds;
	return 0;
}

int carplay_time_sync_apply_epoch(int64_t utc_epoch_seconds, char *detail,
				  size_t detail_capacity)
{
	(void)utc_epoch_seconds;
	if (detail_capacity)
		detail[0] = 0;
	return 0;
}

int carplay_time_sync_apply_nmea(const char *nmea, size_t len, char *detail,
				 size_t detail_capacity)
{
	(void)nmea;
	(void)len;
	if (detail_capacity)
		detail[0] = 0;
	return 0;
}

#ifndef IAP2_REAL_VEHICLE_STATE
void carplay_vehicle_state_clear_location(struct carplay_vehicle_state *state, uint64_t generation)
{ (void)state; (void)generation; }

int carplay_vehicle_state_get_nmea(const struct carplay_vehicle_state *state,
				   char *out, size_t capacity, size_t *written)
{
	(void)state;
	(void)out;
	(void)capacity;
	if (written)
		*written = 0;
	return -1;
}

size_t carplay_vehicle_state_encode_location_start(uint8_t *out,
						   size_t capacity,
						   uint16_t interval_ms)
{
	(void)interval_ms;
	if (capacity < 10)
		return 0;
	out[0] = 0x40;
	out[1] = 0x40;
	out[2] = 0;
	out[3] = 10;
	out[4] = 0xFF;
	out[5] = 0xFA;
	return 10;
}

size_t carplay_vehicle_state_encode_location_stop(uint8_t *out, size_t capacity)
{
	if (capacity < 6)
		return 0;
	out[0] = 0x40;
	out[1] = 0x40;
	out[2] = 0;
	out[3] = 6;
	out[4] = 0xFF;
	out[5] = 0xFC;
	return 6;
}

int carplay_vehicle_state_decode_location_information(
	struct carplay_vehicle_state *state, uint64_t generation,
	const uint8_t *body, size_t body_len, char *error, size_t error_capacity)
{
	(void)state;
	(void)generation;
	(void)body;
	(void)body_len;
	if (error_capacity)
		error[0] = 0;
	return 1;
}

/* ---------------------------------------------------------------- helpers */
#endif

static size_t put_param(uint8_t *dst, uint16_t id, const void *value,
			size_t len)
{
	dst[0] = (uint8_t)((4u + len) >> 8);
	dst[1] = (uint8_t)(4u + len);
	dst[2] = (uint8_t)(id >> 8);
	dst[3] = (uint8_t)id;
	if (len)
		memcpy(dst + 4, value, len);
	return 4u + len;
}

static size_t build_csm(uint16_t id, const uint8_t *body, size_t body_len,
			uint8_t *out)
{
	size_t total = IAP2_CSM_HEADER_BYTES + body_len;

	out[0] = 0x40;
	out[1] = 0x40;
	out[2] = (uint8_t)(total >> 8);
	out[3] = (uint8_t)total;
	out[4] = (uint8_t)(id >> 8);
	out[5] = (uint8_t)id;
	if (body_len)
		memcpy(out + IAP2_CSM_HEADER_BYTES, body, body_len);
	return total;
}

static void feed_message(struct iap2_session *s, struct iap2_link *l,
			 uint16_t id, const uint8_t *body, size_t body_len,
			 uint64_t now_ms)
{
	uint8_t frame[2048];
	assert(body_len <= sizeof(frame) - IAP2_CSM_HEADER_BYTES);
	size_t len = build_csm(id, body, body_len, frame);

	assert(len <= sizeof(frame));
	iap2_session_feed(s, l, frame, len, now_ms);
}

static void reset_capture(void)
{
	g_send_total = 0;
	g_fail_message = 0;
	g_window_outstanding = 0;
	g_window_limit = 0;
	g_connect_result = 0;
	g_connect_calls = 0;
}

static void start_link(struct iap2_link *l, struct iap2_session *s,
		       uint64_t now_ms)
{
	iap2_link_init(l, now_ms);
	l->state = IAP2_NORMAL;
	iap2_session_init(s);
	reset_capture();
	iap2_session_tick(s, l, now_ms);
}

/* ------------------------------------------------------------- test cases */

static void test_waits_for_link(void)
{
	struct iap2_link link;
	struct iap2_session s;

	iap2_link_init(&link, 0);
	iap2_session_init(&s);
	reset_capture();

	iap2_session_tick(&s, &link, 1000);
	assert(g_send_total == 0);
	assert(iap2_session_stage(&s) == IAP2_SESSION_STAGE_LINK);
	assert(iap2_session_stage_deadline(&s) == 0);
}

static void test_sends_identification_start(void)
{
	struct iap2_link link;
	struct iap2_session s;

	iap2_link_init(&link, 0);
	link.state = IAP2_NORMAL;
	iap2_session_init(&s);
	reset_capture();

	iap2_session_tick(&s, &link, 2000);
	assert(g_send_total == 1);
	assert(sent_back(0)->id == 0x1D00);
	assert(sent_back(0)->len == 0);
	assert(iap2_session_stage(&s) == IAP2_SESSION_STAGE_IDENT_START);
	assert(iap2_session_stage_deadline(&s) == 2000 + IAP2_SESSION_RETRY_MS);

	/* Nothing is resent before the deadline. */
	iap2_session_tick(&s, &link, 3999);
	assert(g_send_total == 1);
}

static void test_retransmits_then_backs_off(void)
{
	struct iap2_link link;
	struct iap2_session s;
	unsigned i;
	size_t before;
	uint64_t at;

	start_link(&link, &s, 1000);
	assert(g_send_total == 1);
	assert(iap2_session_stage_deadline(&s) ==
	       1000 + IAP2_SESSION_RETRY_MS);

	for (i = 1; i <= IAP2_SESSION_RETRY_LIMIT; i++) {
		at = 1000 + (uint64_t)i * IAP2_SESSION_RETRY_MS;
		iap2_session_tick(&s, &link, at);
		assert(g_send_total == 1 + i);
		assert(sent_back(0)->id == 0x1D00);
		assert(iap2_session_stage_deadline(&s) ==
		       at + IAP2_SESSION_RETRY_MS);
	}

	/* The fast retries are exhausted; the next tick backs off but still
	 * re-sends once so a late reply can still complete the handshake. */
	at = 1000 + (uint64_t)(IAP2_SESSION_RETRY_LIMIT + 1) *
	     IAP2_SESSION_RETRY_MS;
	before = g_send_total;
	iap2_session_tick(&s, &link, at - 1);
	assert(g_send_total == before);
	iap2_session_tick(&s, &link, at);
	assert(g_send_total == before + 1);
	assert(sent_back(0)->id == 0x1D00);
	assert(iap2_session_stage_deadline(&s) ==
	       at + IAP2_SESSION_SLOW_RETRY_MS);

	/* Slow retries repeat without consuming fast attempts. */
	at = iap2_session_stage_deadline(&s);
	before = g_send_total;
	iap2_session_tick(&s, &link, at - 1);
	assert(g_send_total == before);
	iap2_session_tick(&s, &link, at);
	assert(g_send_total == before + 1);
	assert(s.stage_attempts == IAP2_SESSION_RETRY_LIMIT);
}

static void test_identification_accepted_is_empty(void)
{
	struct iap2_link link;
	struct iap2_session s;
	const struct capture *info_frame;
	size_t before;

	start_link(&link, &s, 1000);
	feed_message(&s, &link, 0x1D01, NULL, 0, 2000);

	assert(iap2_session_stage(&s) == IAP2_SESSION_STAGE_IDENT_INFO);
	assert(g_send_total == 3);
	assert(sent_back(0)->id == 0xAA00);
	info_frame = sent_back(1);
	assert(info_frame->id == 0x1D02);
	/* Phone-side acknowledgement, corroborated by DiPlay's receiver log:
	 * 40 40 00 06 1d 02. Identity belongs to the accessory's 0x1D01. */
	assert(info_frame->len == 0);
	before = g_send_total;
	iap2_session_tick(&s, &link, 2000 + IAP2_SESSION_RETRY_MS);
	assert(g_send_total == before + 1);
	/* maxRetransmissions=0 needs an application AA00 retry, but 1D02 must
	 * never be hand-sent a second time. */
	assert(sent_back(0)->id == 0xAA00);
	assert(sent_back(1)->id == 0xAA00);
}

static void test_identification_accept_send_failure_preserves_order(void)
{
	struct iap2_link link;
	struct iap2_session s;
	start_link(&link, &s, 1000);
	g_fail_message = 0x1D02;
	feed_message(&s, &link, 0x1D01, NULL, 0, 2000);
	assert(g_send_total == 1); /* Do not request certificate before acceptance. */
	assert(!s.mfi_requested);
	g_fail_message = 0;
	iap2_session_tick(&s, &link, 2000 + IAP2_SESSION_RETRY_MS);
	assert(sent_back(1)->id == 0x1D02 && sent_back(1)->len == 0);
	assert(sent_back(0)->id == 0xAA00);
}

static void test_identification_respects_single_packet_window(void)
{
	struct iap2_link link;
	struct iap2_session s;
	size_t before;

	start_link(&link, &s, 1000);
	/* Audi advertises a reliable link with one control packet in flight. */
	link.peer_max_retransmissions = 30;
	link.peer_max_outgoing = 1;
	link.peer_retransmission_timeout_ms = 2000;
	g_window_limit = 1;

	feed_message(&s, &link, 0x1D01, NULL, 0, 2000);
	assert(g_send_total == 2);
	assert(sent_back(0)->id == 0x1D02);
	assert(g_window_outstanding == 1);
	assert(s.stage == IAP2_SESSION_STAGE_IDENT_INFO);
	assert(iap2_session_stage_deadline(&s) <= 2000 + 100);

	/* The window is still occupied: polling must not duplicate 1D02 or
	 * advance the application retry counter. */
	before = g_send_total;
	iap2_session_tick(&s, &link, 2050);
	assert(g_send_total == before);
	assert(s.stage_attempts == 0);

	/* The peer ACKs 1D02. The next bounded poll queues AA00 once. */
	g_window_outstanding = 0;
	iap2_session_tick(&s, &link, 2070);
	assert(g_send_total == before + 1);
	assert(sent_back(0)->id == 0xAA00);
	assert(s.mfi_requested);
	assert(g_window_outstanding == 1);

	/* A later stage retry may renew AA00 for peers without reliable-link
	 * retransmission, but must never hand-send 1D02 again. */
	g_window_outstanding = 0;
	before = g_send_total;
	iap2_session_tick(&s, &link,
		2070 + IAP2_SESSION_RETRY_MS);
	assert(g_send_total == before);
}

static void test_accessory_identity_logs_capabilities(void)
{
	struct iap2_link link;
	struct iap2_session s;
	uint8_t body[64];
	/* From DiPlay's advertised MessagesReceivedFromDevice list. */
	const uint8_t messages[] = { 0xaa, 0x00, 0xaa, 0x02, 0x43, 0x00 };
	size_t len = put_param(body, 7, messages, sizeof(messages));
	start_link(&link, &s, 1000);
	g_last_identity_log[0] = 0;
	feed_message(&s, &link, 0x1D01, body, len, 2000);
	assert(strstr(g_last_identity_log, "0xaa00 0xaa02 0x4300") != NULL);
	assert(sent_back(1)->id == 0x1D02 && sent_back(1)->len == 0);
}

static void test_malformed_identity_is_not_accepted(void)
{
	const uint8_t malformed[][6] = {
		{0, 8, 0, 7, 0x43, 0}, /* truncated parameter */
		{0, 3, 0, 7, 0, 0},   /* length shorter than header */
		{0, 5, 0, 7, 0x43, 0}, /* odd message-id list */
		{0, 4, 0, 7, 0, 0}    /* trailing partial header */
	};
	for (size_t i = 0; i < sizeof(malformed) / sizeof(malformed[0]); i++) {
		struct iap2_link link;
		struct iap2_session s;
		start_link(&link, &s, 1000);
		feed_message(&s, &link, 0x1D01, malformed[i], sizeof(malformed[i]), 2000);
		assert(s.stage == IAP2_SESSION_STAGE_IDENT_START);
		assert(!s.identification_accepted);
		assert(s.malformed_frames == 1);
		assert(g_send_total == 1);
		/* A later complete identity is still accepted. */
		feed_message(&s, &link, 0x1D01, NULL, 0, 2100);
		assert(s.stage == IAP2_SESSION_STAGE_IDENT_INFO);
	}
}

static void test_duplicate_identification_reply_is_ignored(void)
{
	struct iap2_link link;
	struct iap2_session s;
	size_t before;

	start_link(&link, &s, 1000);
	feed_message(&s, &link, 0x1D01, NULL, 0, 2000);
	before = g_send_total;
	feed_message(&s, &link, 0x1D01, NULL, 0, 2100);
	assert(g_send_total == before);
	assert(iap2_session_stage(&s) == IAP2_SESSION_STAGE_IDENT_INFO);
}

static void test_audi_identity_uses_20_byte_challenge(void)
{
	struct iap2_link link;
	struct iap2_session s;
	uint8_t body[128], certificate[8] = {0x30,0x82,0x00,0x04,1,2,3,4};
	size_t len;

	start_link(&link, &s, 1000);
	len = put_param(body, 0, "AUDI MMI\0", 9);
	len += put_param(body + len, 1,
		"CLU8P_MCM_AU_CN_G21_radio_only_012PROD\0", 40);
	len += put_param(body + len, 2, "DELPHI\0", 7);
	feed_message(&s, &link, 0x1D01, body, len, 1200);
	len = put_param(body, 0, certificate, sizeof(certificate));
	feed_message(&s, &link, 0xAA01, body, len, 1300);

	assert(s.mfi_challenge_length == 20);
	assert(sent_back(0)->id == 0xAA02);
	assert(sent_back(0)->len == 4 + 20);
	assert(sent_back(0)->body[1] == 24);
}
static void test_full_identification_flow(void)
{
	struct iap2_link link;
	struct iap2_session s;
	uint8_t body[256];
	uint8_t challenge[32];
	uint8_t signature[64];
	uint8_t certificate[16] = { 0x30, 0x82, 0x01 };
	size_t len;
	size_t i;

	start_link(&link, &s, 1000);

	feed_message(&s, &link, 0x1D01, NULL, 0, 2000);
	assert(iap2_session_stage(&s) == IAP2_SESSION_STAGE_IDENT_INFO);

	len = put_param(body, 0, certificate, sizeof(certificate));
	feed_message(&s, &link, 0xAA01, body, len, 2100);
	assert(iap2_session_stage(&s) == IAP2_SESSION_STAGE_MFI_CHALLENGE);
	assert(sent_back(0)->id == 0xAA02);
	/* The challenge parameter carries 32 bytes. */
	assert(sent_back(0)->len == 4 + 32);
	assert(sent_back(0)->body[0] == 0);
	assert(sent_back(0)->body[1] == 36);
	memcpy(challenge, sent_back(0)->body + 4, sizeof(challenge));

	/* A duplicate certificate does not mint a new challenge. */
	{
		size_t before = g_send_total;

		feed_message(&s, &link, 0xAA01, body, len, 2150);
		assert(g_send_total == before);
	}

	/* The retransmission replays the same challenge bytes. */
	iap2_session_tick(&s, &link, 2100 + IAP2_SESSION_RETRY_MS);
	assert(sent_back(0)->id == 0xAA02);
	assert(memcmp(sent_back(0)->body + 4, challenge, sizeof(challenge)) == 0);

	for (i = 0; i < sizeof(signature); i++)
		signature[i] = (uint8_t)(i + 1);
	len = put_param(body, 0, signature, sizeof(signature));
	feed_message(&s, &link, 0xAA03, body, len, 5000);
	assert(iap2_session_stage(&s) == IAP2_SESSION_STAGE_SESSION_WAIT);
	assert(g_send_total >= 2);
	assert(sent_back(0)->id == 0x4300);
	assert(sent_back(1)->id == 0xAA05);

	{
		uint8_t wired[64];
		size_t wired_len;
		/* airPlayPort travels big endian in the 0x4301 body. */
		uint8_t port[4] = { 0x00, 0x00, 0x1B, 0x58 };	/* 7000 */
		size_t body_len;
		uint8_t address[] = "192.168.2.75\0";

		wired_len = put_param(wired, 0, address, sizeof(address));
		body_len = put_param(body, 0, wired, wired_len);
		body_len += put_param(body + body_len, 2, port, sizeof(port));
		body_len += put_param(body + body_len, 3, "02:00:00:00:00:01\0", 18);
		feed_message(&s, &link, 0x4301, body, body_len, 5100);
	}
	assert(iap2_session_stage(&s) == IAP2_SESSION_STAGE_READY);
	assert(iap2_session_stage_deadline(&s) == 0);

	{
		size_t before = g_send_total;

		iap2_session_tick(&s, &link, 600000);
		assert(g_send_total == before);
	}
}

static void test_reliable_mfi_retry_is_link_owned(void)
{
	struct iap2_link link;
	struct iap2_session s;
	uint8_t body[64];
	uint8_t certificate[8] = {0x30,0x82,0x00,0x04,0x01,0x02,0x03,0x04};
	size_t len, before;

	start_link(&link, &s, 1000);
	feed_message(&s, &link, 0x1D01, NULL, 0, 2000);
	len = put_param(body, 0, certificate, sizeof(certificate));
	feed_message(&s, &link, 0xAA01, body, len, 2100);
	assert(s.stage == IAP2_SESSION_STAGE_MFI_CHALLENGE);
	assert(sent_back(0)->id == 0xAA02);

	/* A reliable link retains and retransmits the original challenge with
	 * its original sequence. Minting a second 0xAA02 at the app deadline can
	 * violate the peer's one-packet transaction/window state. */
	link.peer_max_retransmissions = 30;
	before = g_send_total;
	iap2_session_tick(&s, &link, 2100 + IAP2_SESSION_RETRY_MS);
	assert(g_send_total == before);
	assert(s.stage == IAP2_SESSION_STAGE_MFI_CHALLENGE);
}

static void test_out_of_role_identification_cannot_advance(void)
{
	struct iap2_link link;
	struct iap2_session s;
	size_t before;

	start_link(&link, &s, 1000);
	before = g_send_total;
	/* 0x1D02/03 are sent BY the phone, not acceptance BY the accessory.
	 * Unknown 0x1D04 must not disable this session's retry timer either. */
	for (uint16_t id = 0x1D02; id <= 0x1D04; id++)
		feed_message(&s, &link, id, NULL, 0, 1500);
	assert(iap2_session_stage(&s) == IAP2_SESSION_STAGE_IDENT_START);
	assert(iap2_session_stage_deadline(&s) == 1000 + IAP2_SESSION_RETRY_MS);
	assert(g_send_total == before);
}

static void test_link_loss_restarts_identification(void)
{
	struct iap2_link link;
	struct iap2_session s;
	size_t before;

	start_link(&link, &s, 1000);
	feed_message(&s, &link, 0x1D01, NULL, 0, 2000);
	assert(iap2_session_stage(&s) == IAP2_SESSION_STAGE_IDENT_INFO);

	link.state = IAP2_NEGOTIATING;
	before = g_send_total;
	iap2_session_tick(&s, &link, 3000);
	assert(g_send_total == before);
	assert(iap2_session_stage(&s) == IAP2_SESSION_STAGE_LINK);
	assert(s.identification_started == 0);
	assert(s.availability_sent == 0);

	/* Once NORMAL returns the whole identification restarts. */
	link.state = IAP2_NORMAL;
	iap2_session_tick(&s, &link, 4000);
	assert(iap2_session_stage(&s) == IAP2_SESSION_STAGE_IDENT_START);
	assert(sent_back(0)->id == 0x1D00);
}

static void test_session_wait_retries_frames(void)
{
	struct iap2_link link;
	struct iap2_session s;
	size_t before;
	start_link(&link, &s, 1000);
	s.stage = IAP2_SESSION_STAGE_SESSION_WAIT;
	s.stage_next_ms = 2000;
	iap2_session_tick(&s, &link, 2000);
	before = g_send_total;
	iap2_session_tick(&s, &link, 4000);
	assert(g_send_total == before + 2);
	assert(sent_back(1)->id == 0xAA05);
	assert(sent_back(0)->id == 0x4300);
}

static void test_auth_report_waits_for_send_success(void)
{
	struct iap2_link link;
	struct iap2_session s;
	start_link(&link, &s, 1000);
	s.stage = IAP2_SESSION_STAGE_SESSION_WAIT;
	s.stage_next_ms = 2000;
	g_fail_message = 0xAA05;
	iap2_session_tick(&s, &link, 2000);
	assert(!s.authentication_reported);
	assert(!s.availability_sent);
	g_fail_message = 0;
	iap2_session_tick(&s, &link, 4000);
	assert(s.authentication_reported && s.availability_sent);
	assert(sent_back(1)->id == 0xAA05);
	assert(sent_back(0)->id == 0x4300);
}

static void test_ready_link_loss_restarts_identification(void)
{
	struct iap2_link link;
	struct iap2_session s;
	start_link(&link, &s, 1000);
	s.stage = IAP2_SESSION_STAGE_READY;
	s.authenticated = 1;
	s.authentication_reported = 1;
	s.availability_sent = 1;
	s.received_carplay_start_session = 1;
	s.location_subscription_started = 1;
	s.rx[0] = 0x40;
	s.rx_len = 1;
	link.state = IAP2_NEGOTIATING;
	iap2_session_tick(&s, &link, 2000);
	assert(s.stage == IAP2_SESSION_STAGE_LINK);
	assert(!s.authenticated && !s.authentication_reported);
	assert(!s.received_carplay_start_session && !s.location_subscription_started);
	assert(s.rx_len == 0);
	link.state = IAP2_NORMAL;
	iap2_session_tick(&s, &link, 3000);
	assert(sent_back(0)->id == 0x1D00);
}

static void test_start_session_rejects_unusable_endpoint(int scenario)
{
	struct iap2_link link;
	struct iap2_session s;
	uint8_t group[200], body[256];
	uint8_t port[4] = {0,0,0x1b,0x58};
	const char *address = "192.168.2.75";
	size_t group_len, len, before;
	start_link(&link, &s, 1000);
	s.stage = IAP2_SESSION_STAGE_SESSION_WAIT;
	s.stage_next_ms = 2000;
	s.authentication_reported = 1;
	s.authenticated = 1;
	s.availability_sent = 1;
	if (scenario == 1) port[0] = 1; /* would truncate to valid 7000 */
	if (scenario == 2) memset(port, 0, sizeof(port));
	if (scenario == 3) address = "not-an-ip";
	if (scenario == 4) g_connect_result = -1;
	group_len = put_param(group, 0, address, strlen(address) + 1);
	len = put_param(body, 0, group, group_len);
	len += put_param(body + len, 2, port, sizeof(port));
	feed_message(&s, &link, 0x4301, body, scenario == 0 ? 0 : len, 1500);
	assert(s.stage == IAP2_SESSION_STAGE_SESSION_WAIT);
	assert(!s.received_carplay_start_session);
	assert(s.stage_next_ms == 2000);
	assert(g_connect_calls == (scenario == 4 ? 1u : 0u));
	before = g_send_total;
	iap2_session_tick(&s, &link, 2000);
	assert(g_send_total == before + 2);
	/* A later valid reply can still complete this same session. */
	g_connect_result = 0;
	port[0] = 0; port[1] = 0; port[2] = 0x1b; port[3] = 0x58;
	address = "fe80::1234";
	group_len = put_param(group, 0, address, strlen(address) + 1);
	len = put_param(body, 0, group, group_len);
	len += put_param(body + len, 2, port, sizeof(port));
	feed_message(&s, &link, 0x4301, body, len, 2100);
	assert(s.stage == IAP2_SESSION_STAGE_READY);
	assert(s.received_carplay_start_session);
}

static void complete_auth(struct iap2_session *s, struct iap2_link *l)
{
	const uint8_t certificate[] = {0, 5, 0, 0, 0x30};
	const uint8_t signature[] = {0, 5, 0, 0, 1};
	feed_message(s, l, 0xAA01, certificate, sizeof(certificate), 2100);
	feed_message(s, l, 0xAA03, signature, sizeof(signature), 2200);
}

static void test_declared_exclusion_stops_availability_retries(int empty)
{
	struct iap2_link link;
	struct iap2_session s;
	uint8_t body[128];
	/* Jaguar list from orangecar/docs/protocol.md: no 4300/4301. */
	const uint8_t messages[] = {0x4c,1, 0x4c,4, 0x50,1, 0xff,0xfa,
		0xff,0xfc, 0x41,0x58, 0x41,0x55, 0x4e,9, 0x4e,0xa,
		0x4e,0xb, 0x4e,4, 0xae,1};
	size_t before, len = put_param(body, 7, messages, empty ? 0 : sizeof(messages));
	start_link(&link, &s, 1000);
	feed_message(&s, &link, 0x1D01, body, len, 2000);
	complete_auth(&s, &link);
	assert(s.authenticated && s.authentication_reported);
	assert(!s.availability_sent);
	assert(s.stage == IAP2_SESSION_STAGE_READY);
	assert(s.stage_next_ms == 0);
	before = g_send_total;
	for (uint64_t at = 3000; at < 60000; at += 1000) {
		iap2_session_tick(&s, &link, at);
		feed_message(&s, &link, 0x4E01, NULL, 0, at);
	}
	assert(g_send_total == before);
}

static void test_unsolicited_auth_cannot_advance(void)
{
	struct iap2_link link;
	struct iap2_session s;
	const uint8_t value[] = {0,5,0,0,1};
	start_link(&link, &s, 1000);
	feed_message(&s, &link, 0xAA01, value, sizeof(value), 1100);
	feed_message(&s, &link, 0xAA03, value, sizeof(value), 1200);
	assert(s.stage == IAP2_SESSION_STAGE_IDENT_START);
	assert(g_send_total == 1 && !s.authenticated);
	feed_message(&s, &link, 0x1D01, NULL, 0, 1300);
	feed_message(&s, &link, 0xAA03, value, sizeof(value), 1400);
	assert(s.stage == IAP2_SESSION_STAGE_IDENT_INFO);
}

static void test_failed_auth_queue_cannot_advance(void)
{
	struct iap2_link link;
	struct iap2_session s;
	const uint8_t value[] = {0,5,0,0,1};
	start_link(&link, &s, 1000);
	g_fail_message = 0xAA00;
	feed_message(&s, &link, 0x1D01, NULL, 0, 2000);
	feed_message(&s, &link, 0xAA01, value, sizeof(value), 2100);
	assert(!s.mfi_requested && s.stage == IAP2_SESSION_STAGE_IDENT_INFO);
	g_fail_message = 0;
	iap2_session_tick(&s, &link, 4000);
	g_fail_message = 0xAA02;
	feed_message(&s, &link, 0xAA01, value, sizeof(value), 4100);
	feed_message(&s, &link, 0xAA03, value, sizeof(value), 4200);
	assert(!s.mfi_challenge_sent && s.stage == IAP2_SESSION_STAGE_MFI_CHALLENGE);
	g_fail_message = 0;
	iap2_session_tick(&s, &link, 6100);
	feed_message(&s, &link, 0xAA03, value, sizeof(value), 6200);
	assert(s.authenticated && s.availability_sent);
}

static void test_unsolicited_endpoint_cannot_connect(void)
{
	struct iap2_link link;
	struct iap2_session s;
	uint8_t body[80], group[32];
	const uint8_t port[] = {0,0,0x1b,0x58};
	size_t n = put_param(group, 0, "192.168.2.75", 13);
	size_t len = put_param(body, 0, group, n);
	len += put_param(body + len, 2, port, sizeof(port));
	start_link(&link, &s, 1000);
	feed_message(&s, &link, 0x4301, body, len, 1500);
	assert(g_connect_calls == 0);
	assert(s.stage == IAP2_SESSION_STAGE_IDENT_START);
	feed_message(&s, &link, 0x1D01, NULL, 0, 2000);
	g_fail_message = 0x4300;
	complete_auth(&s, &link);
	feed_message(&s, &link, 0x4301, body, len, 2500);
	assert(g_connect_calls == 0); /* Availability was not queued. */
}

static void test_empty_library_and_bluetooth_replies(void)
{
	struct iap2_link link;
	struct iap2_session s;
	uint8_t body[64];
	const uint8_t component[] = {0x01,0x23};
	const uint8_t expected_bt[] = {0,6,0,0,1,0x23, 0,4,0,1};
	size_t len, before;
	start_link(&link, &s, 1000);
	feed_message(&s, &link, 0x1D01, NULL, 0, 2000);
	complete_auth(&s, &link);
	feed_message(&s, &link, 0x4C00, NULL, 0, 2300);
	assert(sent_back(0)->id == 0x4C01);
	/* One explicitly empty daemon-owned local library, not Android's media. */
	const uint8_t expected_library[] = {
		0,39,0,0, 0,13,0,0,'B','o','x',' ','i','A','P','2',0,
		0,17,0,1,'b','o','x','-','e','m','p','t','y','-','v','1',0,
		0,5,0,2,0};
	/* Assert nested IDs and exact lengths independently of implementation. */
	assert(sent_back(0)->len == sizeof(expected_library));
	assert(memcmp(sent_back(0)->body, expected_library, sizeof(expected_library)) == 0);
	len = put_param(body, 0, "box-empty-v1", 13);
	feed_message(&s, &link, 0x4C03, body, len, 2400);
	assert(sent_back(0)->id == 0x4C04);
	const uint8_t expected_update[] = {0,17,0,0,'b','o','x','-','e','m','p','t','y','-','v','1',0,
		0,6,0,1,'1',0};
	assert(sent_back(0)->len == sizeof(expected_update));
	assert(memcmp(sent_back(0)->body, expected_update, sizeof(expected_update)) == 0);
	len = put_param(body, 0, component, sizeof(component));
	feed_message(&s, &link, 0x4E03, body, len, 2500);
	assert(sent_back(0)->id == 0x4E04);
	assert(sent_back(0)->len == sizeof(expected_bt));
	assert(memcmp(sent_back(0)->body, expected_bt, sizeof(expected_bt)) == 0);
	before = g_send_total;
	feed_message(&s, &link, 0x4E03, NULL, 0, 2600);
	feed_message(&s, &link, 0x4C03, NULL, 0, 2700);
	assert(g_send_total == before); /* Missing request identifiers are not guessed. */
}

static void test_power_update_subscription_replies_once(void)
{
	struct iap2_link link;
	struct iap2_session s;
	uint8_t source[16];
	const uint8_t current[] = {0x08,0x34};
	const uint8_t should_charge[] = {0x01};
	const uint8_t field0[] = {0,6,0,0,0x03,0xe8};
	const uint8_t field1[] = {0,5,0,1,0x01};
	const uint8_t field5[] = {0,5,0,5,1};
	const uint8_t field6[] = {0,6,0,6,0,0x62};
	const uint8_t request[] = {0,4,0,0, 0,4,0,1, 0,4,0,5, 0,4,0,6};
	size_t len, before;

	start_link(&link, &s, 1000);
	feed_message(&s, &link, 0x1D01, NULL, 0, 2000);
	complete_auth(&s, &link);
	before = g_send_total;

	/* 0xAE03 carries the car's USB budget and has no reply of its own. */
	len = put_param(source, 0, current, sizeof(current));
	len += put_param(source + len, 1, should_charge, sizeof(should_charge));
	feed_message(&s, &link, 0xAE03, source, len, 2300);
	assert(g_send_total == before);
	assert(!s.received_power); /* A source update is not a subscription. */

	/* The reference transmitter sends one parameter per AE01 and waits for
	 * each reliable-window ACK before selecting the next requested field. */
	feed_message(&s, &link, 0xAE00, request, sizeof(request), 2310);
	assert(g_send_total == before + 1);
	assert(sent_back(0)->id == 0xAE01);
	assert(s.power_update_fields == 0x63);
	assert(s.power_update_step == 1 && s.power_update_pending);
	assert(sent_back(0)->len == sizeof(field0));
	assert(!memcmp(sent_back(0)->body, field0, sizeof(field0)));

	/* Duplicate starts and later source updates must not restart or duplicate
	 * the in-progress field sequence. */
	feed_message(&s, &link, 0xAE00, request, sizeof(request), 2320);
	feed_message(&s, &link, 0xAE03, source, len, 2330);
	assert(g_send_total == before + 1);

	g_window_outstanding = 0;
	iap2_session_tick(&s, &link, 2340);
	assert(g_send_total == before + 2);
	assert(s.power_update_step == 2 && s.power_update_pending);
	assert(sent_back(0)->len == sizeof(field1));
	assert(!memcmp(sent_back(0)->body, field1, sizeof(field1)));
	g_window_outstanding = 0;
	iap2_session_tick(&s, &link, 2360);
	assert(g_send_total == before + 3);
	assert(s.power_update_step == 4 && s.power_update_pending);
	assert(sent_back(0)->len == sizeof(field5));
	assert(!memcmp(sent_back(0)->body, field5, sizeof(field5)));
	g_window_outstanding = 0;
	iap2_session_tick(&s, &link, 2380);
	assert(g_send_total == before + 4);
	assert(s.power_update_step == 5 && !s.power_update_pending);
	assert(sent_back(0)->len == sizeof(field6));
	assert(!memcmp(sent_back(0)->body, field6, sizeof(field6)));

	/* Stop clears the subscription without issuing a reply. */
	feed_message(&s, &link, 0xAE02, NULL, 0, 2400);
	assert(!s.received_power);
	assert(s.power_update_step == 0);
	assert(g_send_total == before + 4);

	/* A later Start in the same session must restart with field 0. */
	feed_message(&s, &link, 0xAE00, NULL, 0, 2410);
	assert(s.received_power);
	assert(s.power_update_step == 1 && s.power_update_pending);
	assert(g_send_total == before + 5);
	assert(sent_back(0)->id == 0xAE01);
	assert(!memcmp(sent_back(0)->body, field0, sizeof(field0)));
}

static void test_power_update_respects_single_packet_window(void)
{
	struct iap2_link link;
	struct iap2_session s;
	const uint8_t field0[] = {0,6,0,0,0x03,0xe8};
	const uint8_t field1[] = {0,5,0,1,0x01};
	const uint8_t field5[] = {0,5,0,5,1};
	const uint8_t field6[] = {0,6,0,6,0,0x62};
	const uint8_t request[] = {0,4,0,0, 0,4,0,1, 0,4,0,5, 0,4,0,6};
	size_t before;

	start_link(&link, &s, 1000);
	feed_message(&s, &link, 0x1D01, NULL, 0, 2000);
	complete_auth(&s, &link);
	g_window_limit = 1;
	before = g_send_total;

	/* Field 0 fills the only reliable slot. Field 1 stays queued until its ACK. */
	feed_message(&s, &link, 0xAE00, request, sizeof(request), 2300);
	assert(g_send_total == before + 1);
	assert(sent_back(0)->id == 0xAE01);
	assert(!memcmp(sent_back(0)->body, field0, sizeof(field0)));
	iap2_session_tick(&s, &link, 2301);
	assert(g_send_total == before + 1);
	iap2_session_tick(&s, &link, 2325);
	assert(g_send_total == before + 1 && g_window_outstanding == 1);

	g_window_outstanding = 0;
	iap2_session_tick(&s, &link, 2350);
	assert(g_send_total == before + 2);
	assert(!memcmp(sent_back(0)->body, field1, sizeof(field1)));
	g_window_outstanding = 0;
	iap2_session_tick(&s, &link, 2375);
	assert(g_send_total == before + 3);
	assert(!memcmp(sent_back(0)->body, field5, sizeof(field5)));
	g_window_outstanding = 0;
	iap2_session_tick(&s, &link, 2400);
	assert(g_send_total == before + 4 && !s.power_update_pending);
	assert(!memcmp(sent_back(0)->body, field6, sizeof(field6)));
	g_window_limit = 0;
}
static void test_power_update_gates_and_fields(void)
{
	struct iap2_link link;
	struct iap2_session s;
	uint8_t identity[8], request[8];
	size_t before, len;
	start_link(&link, &s, 1000);
	feed_message(&s, &link, 0xAE00, NULL, 0, 1100);
	assert(!s.power_update_pending); /* No deferred pre-auth subscription. */
	len = put_param(identity, 7, NULL, 0); /* Explicitly excludes AE01. */
	feed_message(&s, &link, 0x1D01, identity, len, 2000);
	complete_auth(&s, &link);
	before = g_send_total;
	feed_message(&s, &link, 0xAE00, request, sizeof(request), 2300);
	assert(g_send_total == before && !s.power_update_pending);

	start_link(&link, &s, 1000);
	feed_message(&s, &link, 0x1D01, NULL, 0, 2000);
	complete_auth(&s, &link);
	before = g_send_total;
	len = put_param(request, 1, NULL, 0); /* Only charge flag requested. */
	feed_message(&s, &link, 0xAE00, request, len, 2300);
	assert(g_send_total == before + 1);
	assert(sent_back(0)->id == 0xAE01 && sent_back(0)->body[3] == 1);
	{ const uint8_t bad_flag[] = {0,5,0,0,1};
	  feed_message(&s, &link, 0xAE00, bad_flag, sizeof(bad_flag), 2305);
	  assert(s.malformed_frames == 1 && s.power_update_fields == 2);
	  assert(g_send_total == before + 1); }
	feed_message(&s, &link, 0xAE02, NULL, 0, 2310);
	before = g_send_total;
	len = put_param(request, 5, NULL, 0); /* Charging state is a compatibility value. */
	feed_message(&s, &link, 0xAE00, request, len, 2320);
	assert(g_send_total == before + 1 && !s.power_update_pending);
	{ const uint8_t charging[] = {0,5,0,5,1};
	  assert(sent_back(0)->id == 0xAE01 && sent_back(0)->len == sizeof(charging));
	  assert(!memcmp(sent_back(0)->body, charging, sizeof(charging))); }
}

static void test_power_update_failure_stop_and_reset(void)
{
	struct iap2_link link;
	struct iap2_session s;
	size_t before;
	start_link(&link, &s, 1000);
	feed_message(&s, &link, 0x1D01, NULL, 0, 2000);
	complete_auth(&s, &link);
	g_fail_message = 0xAE01;
	before = g_send_total;
	feed_message(&s, &link, 0xAE00, NULL, 0, 2300);
	assert(s.power_update_step == 0 && s.power_update_pending);
	assert(g_send_total == before);
	g_fail_message = 0;
	g_window_limit = 1;
	iap2_session_tick(&s, &link, 2320);
	assert(g_send_total == before + 1 && s.power_update_step == 1 &&
	       s.power_update_pending);
	feed_message(&s, &link, 0xAE02, NULL, 0, 2330);
	g_window_outstanding = 0;
	iap2_session_tick(&s, &link, 2340);
	assert(g_send_total == before + 1 && !s.power_update_pending);
	feed_message(&s, &link, 0xAE00, NULL, 0, 2350);
	assert(g_send_total == before + 2);
	assert(sent_back(0)->id == 0xAE01);
	assert(s.power_update_step == 1 && s.power_update_pending);
	link.state = IAP2_DEAD;
	iap2_session_tick(&s, &link, 2360);
	assert(!s.power_update_pending && s.power_update_step == 0);
	assert(!s.authentication_reported && s.authentication_completed_once);
	iap2_session_init(&s);
	assert(!s.authentication_completed_once);
	g_window_limit = 0;
}

static void test_post_auth_notifications_precede_power_update(void)
{
#ifdef IAP2_REAL_VEHICLE_STATE
    struct carplay_vehicle_state vehicle;
    carplay_vehicle_state_init(&vehicle);
    carplay_vehicle_state_begin_session(&vehicle, 1);
#endif
    struct iap2_link link;
    struct iap2_session s;
    uint8_t body[128];
    const uint8_t sends[] = {0xff,0xfb};
    const uint8_t receives[] = {0x4e,0x09,0x4e,0x0a,0xae,0x01,0xff,0xfa};
    size_t len = put_param(body, 6, sends, sizeof(sends));
    len += put_param(body + len, 7, receives, sizeof(receives));
    size_t before;

    start_link(&link, &s, 1000);
    iap2_session_set_device_identity(&s, "rk3399-mid", "zh-Hans-CN");
#ifdef IAP2_REAL_VEHICLE_STATE
    iap2_session_set_vehicle_state(&s, &vehicle, 1);
#else
    iap2_session_set_vehicle_state(
        &s, (struct carplay_vehicle_state *)(void *)&link, 1);
#endif
    feed_message(&s, &link, 0x1D01, body, len, 2000);
    feed_message(&s, &link, 0xAA01,
        (const uint8_t[]){0,5,0,0,0x30}, 5, 2100);

    /* AA05 occupies the one-packet reliable window. The car acknowledges it
     * in the same exchange that carries AE00. The reference transmitter sends
     * the declared device name, language and location notifications before
     * starting the power update sequence. */
    g_window_limit = 1;
    feed_message(&s, &link, 0xAA03,
        (const uint8_t[]){0,5,0,0,1}, 5, 2200);
    assert(s.authentication_reported);
    /* Session-private service bits: device name (0x08) and language (0x10). */
    assert(s.service_pending == 0x38u);
    assert(g_window_outstanding == 1);
    g_window_outstanding = 0;

    before = g_send_total;
    feed_message(&s, &link, 0xAE00, NULL, 0, 4300);
    /* Production pump feeds CSM, then ticks the session before draining TX. */
    iap2_session_tick(&s, &link, 4300);
    assert(g_send_total == before + 1);
    assert(sent_back(0)->id == 0x4E09);
    assert(s.power_update_step == 0 && s.power_update_pending);
    assert(!(s.service_pending & 0x08u));

    g_window_outstanding = 0;
    before = g_send_total;
    iap2_session_tick(&s, &link, 4325);
    assert(g_send_total == before + 1);
    assert(sent_back(0)->id == 0x4E0A);
    assert(s.power_update_step == 0 && s.power_update_pending);
    assert(s.service_pending == 0x20u);

    g_window_outstanding = 0;
    before = g_send_total;
    iap2_session_tick(&s, &link, 4350);
    assert(g_send_total == before + 1);
    assert(sent_back(0)->id == 0xAE01);
    assert(s.power_update_step == 1 && s.power_update_pending);
    assert(s.service_pending == 0x20u);

    g_window_outstanding = 0;
    before = g_send_total;
    iap2_session_tick(&s, &link, 4375);
    assert(g_send_total == before + 1);
    assert(sent_back(0)->id == 0xAE01);
    assert(s.power_update_step == 2 && !s.power_update_pending);

    g_window_outstanding = 0;
    before = g_send_total;
    iap2_session_tick(&s, &link, 4400);
    assert(g_send_total == before + 1);
    assert(sent_back(0)->id == 0xFFFA);
    assert(s.power_update_step == 2 && !s.power_update_pending);
    assert(!s.service_pending);
    g_window_limit = 0;
#ifdef IAP2_REAL_VEHICLE_STATE
    carplay_vehicle_state_destroy(&vehicle);
#endif
}

static void test_auth_completion_single_packet_window(void)
{
	struct iap2_link link;
	struct iap2_session s;
	const uint8_t value[] = {0,5,0,0,1};
	size_t before;
	start_link(&link, &s, 1000);
	feed_message(&s, &link, 0x1D01, NULL, 0, 2000);
	feed_message(&s, &link, 0xAA01, value, sizeof(value), 2100);
	link.peer_max_retransmissions = 30;
	g_window_limit = 1;
	feed_message(&s, &link, 0xAA03, value, sizeof(value), 2200);
	assert(sent_back(0)->id == 0xAA05 && !s.availability_sent);
	before = g_send_total;
	g_window_outstanding = 0; /* AA05 ACKed, 4300 must get the next slot. */
	iap2_session_tick(&s, &link, 2220);
	assert(g_send_total == before + 1 && sent_back(0)->id == 0x4300);
	g_window_outstanding = 0;
	iap2_session_tick(&s, &link, 4220);
	assert(sent_back(0)->id == 0x4300); /* Never mint another AA05 sequence. */
	g_window_limit = 0;
}

static void test_real_now_playing(void)
{
    struct iap2_link link;struct iap2_session s;
    const uint8_t request[]={0,8,0,0,0,4,0,1,0,8,0,1,0,4,0,0};
    const uint8_t seek[]={0,8,0,0,0,0,3,232};
    start_link(&link,&s,1000);
    feed_message(&s,&link,0x5000,request,sizeof(request),1100);
    assert(!s.received_now_playing_subscription);
    feed_message(&s,&link,0x1D01,NULL,0,2000);complete_auth(&s,&link);
    s.stage=IAP2_SESSION_STAGE_READY;s.stage_next_ms=0;
    feed_message(&s,&link,0x5000,request,sizeof(request),2300);
    assert(s.received_now_playing_subscription);
    size_t before=g_send_total;
    iap2_session_tick(&s,&link,2301);assert(g_send_total==before);
    memset(&g_now,0,sizeof(g_now));g_now.status=1;g_now.can_seek=1;
    strcpy(g_now.title,"Real title");g_now_available=1;
    g_fail_message=0x5001;iap2_session_tick(&s,&link,3301);
    assert(g_send_total==before);
    g_fail_message=0;s.now_next_ms=0;link.peer_max_length=64;
    iap2_session_tick(&s,&link,4301);
    assert(g_send_total==before+1 && sent_back(0)->id==0x5001);
    assert(sent_back(0)->len+IAP2_CSM_HEADER_BYTES+IAP2_HEADER_BYTES+IAP2_CHECKSUM_BYTES<=64);
    iap2_session_tick(&s,&link,5301);assert(g_send_total==before+1);
    feed_message(&s,&link,0x5000,request,sizeof(request)-1,5400);
    assert(s.received_now_playing_subscription); /* preserve previous request */
    feed_message(&s,&link,0x5003,seek,sizeof(seek),5500);assert(g_app_commands==1);
    g_now.status=2;iap2_session_tick(&s,&link,6301);assert(g_send_total==before+2);
    feed_message(&s,&link,0x5002,NULL,0,6400);
    feed_message(&s,&link,0x5003,seek,sizeof(seek),6500);assert(g_app_commands==1);
    iap2_session_tick(&s,&link,7301);assert(g_send_total==before+2);
    feed_message(&s,&link,0x5000,request,sizeof(request),7400);
    link.state=IAP2_IDLE;iap2_session_tick(&s,&link,7500);
    assert(!s.received_now_playing_subscription);
    g_now_available=0;
}

static void test_artwork_session(void) {
    struct iap2_link link;struct iap2_session s;
    const uint8_t request[]={0,8,0,0,0,4,0,26}; /* artwork only */
    start_link(&link,&s,1000);link.peer_max_length=64;link.peer_file_session=27;
    feed_message(&s,&link,0x1D01,NULL,0,2000);complete_auth(&s,&link);
    s.stage=IAP2_SESSION_STAGE_READY;s.stage_next_ms=0;
    memset(&g_now,0,sizeof(g_now));g_now.media_generation=10;g_now.art_size=5;g_now_available=1;
    g_file_sent=g_art_reads=0;g_file_blocked=g_art_fail=g_file_reply_pending=0;
    feed_message(&s,&link,0x5000,request,sizeof(request),2300);
    size_t before=g_send_total;
    iap2_session_tick(&s,&link,2301);assert(g_art_reads==1 && s.art_loaded==2 && !g_file_sent);
    iap2_session_tick(&s,&link,2302);iap2_session_tick(&s,&link,2303);
    g_fail_message=0x5001;iap2_session_tick(&s,&link,2304);
    assert(g_send_total==before && s.art_announce && !g_file_sent);
    g_fail_message=0;g_file_blocked=1;iap2_session_tick(&s,&link,2305);
    assert(g_send_total==before+1 && sent_back(0)->body[8]==128 && s.art_transfer.phase==1);
    g_file_blocked=0;iap2_session_tick(&s,&link,2306);
    assert(g_file_sent==1 && g_file_size==10 && g_file_packet[1]==4 && s.art_transfer.phase==2);
    g_file_reply[0]=128;g_file_reply[1]=1;g_file_reply_pending=1;g_file_blocked=1;
    iap2_session_tick(&s,&link,2307);assert(!s.art_transfer.offset && s.art_transfer.phase==3);
    g_file_blocked=0;iap2_session_tick(&s,&link,2308);
    assert(g_file_sent==2 && g_file_size==7 && g_file_packet[1]==0xc0 && g_file_packet[6]==5);
    g_file_reply[1]=5;g_file_reply_pending=1;iap2_session_tick(&s,&link,2309);assert(!s.art_transfer.phase);
    /* Same-generation temporary IPC failure gets a bounded delayed retry. */
    g_now.media_generation=11;g_art_fail=1;s.now_next_ms=0;
    iap2_session_tick(&s,&link,2400);assert(s.art_loading);
    unsigned reads=g_art_reads;iap2_session_tick(&s,&link,2401);assert(g_art_reads==reads);
    iap2_session_tick(&s,&link,3401);assert(g_art_reads==reads+1 && s.art_loaded==2);
    iap2_session_tick(&s,&link,3402);iap2_session_tick(&s,&link,3403);iap2_session_tick(&s,&link,3404);
    assert(s.art_transfer.phase==2);
    /* Source loss cancels, including a blocked Cancel. No old data can escape. */
    g_now_available=0;s.now_next_ms=0;g_file_blocked=1;unsigned sent=g_file_sent;
    iap2_session_tick(&s,&link,3500);assert(g_file_sent==sent && s.art_cancel);
    g_file_blocked=0;iap2_session_tick(&s,&link,3501);
    assert(g_file_sent==sent+1 && g_file_packet[1]==2 && !s.art_transfer.phase);
    /* Empty artwork is an explicit zero-length file clearing the prior image. */
    g_now_available=1;g_now.media_generation=12;g_now.art_size=0;s.now_next_ms=0;
    iap2_session_tick(&s,&link,3502);assert(s.art_transfer.phase==2 && g_file_packet[9]==0);
    g_file_reply[0]=s.art_transfer.id;g_file_reply[1]=1;g_file_reply_pending=1;
    iap2_session_tick(&s,&link,3503);assert(g_file_size==2 && g_file_packet[1]==0xc0);
    feed_message(&s,&link,0x5002,NULL,0,3504);iap2_session_tick(&s,&link,3505);
    assert(!s.art_transfer.phase && g_file_packet[1]==2);
    link.state=IAP2_IDLE;iap2_session_tick(&s,&link,3506);
    assert(!s.art_generation && !s.art_loading && !s.art_cancel);
    start_link(&link,&s,4000);link.peer_max_length=64;link.peer_file_session=27;
    feed_message(&s,&link,0x1D01,NULL,0,4100);complete_auth(&s,&link);
    s.stage=IAP2_SESSION_STAGE_READY;s.stage_next_ms=0;
    g_now.media_generation=20;g_now.art_size=5;g_art_reads=0;
    feed_message(&s,&link,0x5000,request,sizeof(request),5000);
    iap2_session_tick(&s,&link,5001);assert(s.art_loaded==2);
    g_now.media_generation=21;s.now_next_ms=0;
    iap2_session_tick(&s,&link,5002);
    assert(s.art_target_generation==21 && s.art_loaded==2 && !s.art_transfer.phase);
    /* Three failed IPC reads stop retrying until a genuinely new generation. */
    g_art_fail=9;g_art_reads=0;
    iap2_session_tick(&s,&link,5003);iap2_session_tick(&s,&link,6003);iap2_session_tick(&s,&link,7003);
    assert(g_art_reads==3 && !s.art_loading);
    iap2_session_tick(&s,&link,8003);assert(g_art_reads==3 && !s.art_transfer.phase);
    g_art_fail=0;
    g_now_available=0;
}

static void test_usb_artwork_uses_short_transfers(void)
{
    struct iap2_link link;
    struct iap2_session s;
    const uint8_t request[] = {0,8,0,0,0,4,0,26};
    const size_t overhead = IAP2_HEADER_BYTES + IAP2_CHECKSUM_BYTES;
    start_link(&link, &s, 1000);
    link.peer_max_length = 4096;
    link.peer_file_session = 27;
    link.usb_out_max_packet = 512;
    feed_message(&s, &link, 0x1D01, NULL, 0, 2000);
    complete_auth(&s, &link);
    feed_message(&s, &link, 0x5000, request, sizeof(request), 2300);
    g_now_available = 1;
    memset(&g_now, 0, sizeof(g_now));
    memset(s.art_transfer.data, 0xa5, 1524);
    s.now_provider_status = 1;
    s.now_next_ms = 10000;
    assert(carplay_file_begin(&s.art_transfer, 128, s.art_transfer.data, 1524, 2, 2301) == 0);
    iap2_session_tick(&s, &link, 2302);
    assert(s.art_transfer.phase == 2);
    g_file_reply[0] = 128;
    g_file_reply[1] = 1;
    g_file_reply_pending = 1;
    for (unsigned i = 0; i < 4; i++) {
        iap2_session_tick(&s, &link, 2303 + i);
        assert(g_file_size + overhead < link.usb_out_max_packet);
        assert(g_file_packet[g_file_size - 1] == 0xa5);
    }
    assert(s.art_transfer.phase == 5 && s.art_transfer.offset == 1524);
    assert(g_file_packet[1] == 0x40);
    g_now_available = 0;
}

static void test_route_provider(void) {
    struct iap2_link link;struct iap2_session s;
    uint8_t identity[]={0,23,0,30,0,6,0,0,0,7,0,7,0,1,'H','U',0,0,6,0,2,0,16};
    uint8_t request[]={0,6,0,0,0,7,0,4,0,1,0,4,0,2};
    start_link(&link,&s,1000);
    feed_message(&s,&link,0x5200,request,sizeof(request),1100);
    assert(!s.received_route_subscription);
    feed_message(&s,&link,0x1D01,identity,sizeof(identity),2000);complete_auth(&s,&link);
    assert(s.route_components.count==1);
    s.stage=IAP2_SESSION_STAGE_READY;s.stage_next_ms=0;
    feed_message(&s,&link,0x5200,request,sizeof(request),2300);
    assert(s.received_route_subscription);
    memset(&g_route,0,sizeof(g_route));g_route.state=1;g_route.index=3;g_route.maneuver=1;
    strcpy(g_route.current,"road");g_route_available=1;
    size_t before=g_send_total;g_fail_message=0x5202;
    iap2_session_tick(&s,&link,2301);assert(g_send_total==before);
    g_fail_message=0;iap2_session_tick(&s,&link,2801);
    assert(g_send_total==before+2 && sent_back(1)->id==0x5202 && sent_back(0)->id==0x5201);
    iap2_session_tick(&s,&link,3301);assert(g_send_total==before+2);
    g_route_available=0;iap2_session_tick(&s,&link,3801);
    assert(g_send_total==before+3 && sent_back(0)->id==0x5201);
    feed_message(&s,&link,0x5203,request,6,3900);assert(!s.received_route_subscription);
    g_route_available=1;iap2_session_tick(&s,&link,4301);assert(g_send_total==before+3);
    feed_message(&s,&link,0x5200,request,sizeof(request),4400);
    link.state=IAP2_IDLE;iap2_session_tick(&s,&link,4500);
    assert(!s.received_route_subscription && !s.route_components.count);
    g_route_available=0;
}

static void test_communications_provider(void) {
    struct iap2_link link;struct iap2_session s;
    const uint8_t request[]={0,4,0,5,0,4,0,6,0,4,0,10};
    start_link(&link,&s,1000);
    feed_message(&s,&link,0x4157,request,sizeof(request),1100);
    assert(!s.received_communications_subscription);
    feed_message(&s,&link,0x1D01,NULL,0,2000);complete_auth(&s,&link);
    s.stage=IAP2_SESSION_STAGE_READY;s.stage_next_ms=0;
    feed_message(&s,&link,0x4157,request,sizeof(request),2300);
    size_t before=g_send_total;iap2_session_tick(&s,&link,2301);assert(g_send_total==before);
    g_comm_available=1;g_fail_message=0x4158;
    iap2_session_tick(&s,&link,3301);assert(g_send_total==before);
    g_fail_message=0;iap2_session_tick(&s,&link,4301);
    assert(g_send_total==before+1 && sent_back(0)->id==0x4158 && sent_back(0)->len==10);
    assert(sent_back(0)->body[3]==5 && sent_back(0)->body[8]==6);
    iap2_session_tick(&s,&link,5301);assert(g_send_total==before+1);
    feed_message(&s,&link,0x4159,NULL,0,5400);assert(!s.received_communications_subscription);
    iap2_session_tick(&s,&link,6301);assert(g_send_total==before+1);g_comm_available=0;
}

static void test_bluetooth_call_updates(void) {
    struct iap2_link link;struct iap2_session s;
    const uint8_t request[]={0,4,0,2,0,4,0,3,0,4,0,4,0,4,0,8};
    start_link(&link,&s,1000);
    feed_message(&s,&link,0x4154,request,sizeof(request),1100);
    assert(!s.received_call_subscription);
    feed_message(&s,&link,0x1D01,NULL,0,2000);complete_auth(&s,&link);
    s.stage=IAP2_SESSION_STAGE_READY;s.stage_next_ms=0;
    feed_message(&s,&link,0x4154,request,sizeof(request),2300);
    size_t before=g_send_total;iap2_session_tick(&s,&link,2301);assert(g_send_total==before);
    memset(&g_calls,0,sizeof(g_calls));g_calls.count=1;g_calls_available=1;
    strcpy(g_calls.calls[0].uuid,"01234567-89ab-cdef-0123-456789abcdef");
    g_calls.calls[0].status=2;g_calls.calls[0].direction=1;
    g_fail_message=0x4155;iap2_session_tick(&s,&link,3301);assert(g_send_total==before);
    g_fail_message=0;iap2_session_tick(&s,&link,4301);
    assert(g_send_total==before+1 && sent_back(0)->id==0x4155 && sent_back(0)->body[4]==2);
    iap2_session_tick(&s,&link,5301);assert(g_send_total==before+1);
    feed_message(&s,&link,0x4154,request,sizeof(request)-1,5400);
    g_calls.calls[0].status=4;iap2_session_tick(&s,&link,6301);
    assert(g_send_total==before+2 && sent_back(0)->body[4]==4);
    g_calls_available=0;g_calls.count=0;iap2_session_tick(&s,&link,7301);
    assert(g_send_total==before+2); /* Losing Bluetooth does not claim a call ended. */
    g_calls_available=1;g_fail_message=0x4155;iap2_session_tick(&s,&link,8301);
    assert(g_send_total==before+2);
    g_fail_message=0;iap2_session_tick(&s,&link,9301);
    assert(g_send_total==before+3 && sent_back(0)->body[4]==0);
    iap2_session_tick(&s,&link,10301);assert(g_send_total==before+3);
    feed_message(&s,&link,0x4156,NULL,0,10400);
    g_calls.count=1;iap2_session_tick(&s,&link,11301);assert(g_send_total==before+3);
    feed_message(&s,&link,0x4154,request,sizeof(request),11400);
    link.state=IAP2_IDLE;iap2_session_tick(&s,&link,11500);
    assert(!s.received_call_subscription);g_calls_available=0;
}

static void test_declared_notifications_only_and_retry(void)
{
	struct iap2_link link;
	struct iap2_session s;
	uint8_t body[32];
	const uint8_t messages[] = {0x4e,9,0x4e,0xa};
	size_t len = put_param(body, 7, messages, sizeof(messages));
	start_link(&link, &s, 1000);
	iap2_session_set_device_identity(&s, "Test Box", "zh-CN");
	g_fail_message = 0x4E09;
	feed_message(&s, &link, 0x1D01, body, len, 2000);
	complete_auth(&s, &link);
	assert(sent_back(0)->id == 0x4E0A);
	const uint8_t language[] = {0,10,0,0,'z','h','-','C','N',0};
	assert(sent_back(0)->len == sizeof(language));
	assert(memcmp(sent_back(0)->body, language, sizeof(language)) == 0);
	size_t before = g_send_total;
	g_fail_message = 0;
	iap2_session_tick(&s, &link, 4200);
	assert(g_send_total == before + 1);
	assert(sent_back(0)->id == 0x4E09);
	const uint8_t name[] = {0,13,0,0,'T','e','s','t',' ','B','o','x',0};
	assert(sent_back(0)->len == sizeof(name));
	assert(memcmp(sent_back(0)->body, name, sizeof(name)) == 0);
	iap2_session_tick(&s, &link, 60000);
	assert(g_send_total == before + 1);
	/* Missing declaration preserves old DiPlay frames without new notifications. */
	start_link(&link, &s, 1000);
	iap2_session_set_device_identity(&s, "Test Box", "zh-CN");
	feed_message(&s, &link, 0x1D01, NULL, 0, 2000);
	complete_auth(&s, &link);
	assert(sent_back(0)->id == 0x4300 && sent_back(1)->id == 0xAA05);
}

static void test_long_capability_lists_and_reset(void)
{
	struct iap2_link link;
	struct iap2_session s;
	uint8_t body[1024], messages[600];
	size_t len = 0;
	/* Beyond both the 32-parameter and 128-ID diagnostic bounds. */
	for (int i = 0; i < 33; i++)
		len += put_param(body + len, (uint16_t)(100 + i), NULL, 0);
	for (int i = 0; i < 300; i++) {
		messages[2*i] = (uint8_t)(i >> 8);
		messages[2*i+1] = (uint8_t)i;
	}
	messages[598] = 0x43; messages[599] = 0;
	len += put_param(body + len, 7, messages, sizeof(messages));
	start_link(&link, &s, 1000);
	feed_message(&s, &link, 0x1D01, body, len, 2000);
	assert(iap2_session_accessory_receives(&s, 0x4300) == IAP2_CAP_DECLARED);
	assert(iap2_session_accessory_receives(&s, 0xffff) == IAP2_CAP_EXCLUDED);
	assert(iap2_session_accessory_sends(&s, 0x4301) == IAP2_CAP_UNKNOWN);
	complete_auth(&s, &link);
	assert(s.availability_sent);
	link.state = IAP2_NEGOTIATING;
	iap2_session_tick(&s, &link, 3000);
	assert(iap2_session_accessory_receives(&s, 0x4300) == IAP2_CAP_UNKNOWN);
	assert(!s.service_pending);
}

static void test_duplicate_parameters_are_rejected(void)
{
	struct iap2_link link;
	struct iap2_session s;
	uint8_t body[32];
	const uint8_t value[] = {1};
	const uint8_t component[] = {0,1};
	start_link(&link, &s, 1000);
	size_t len = put_param(body, 7, NULL, 0);
	len += put_param(body + len, 7, NULL, 0);
	feed_message(&s, &link, 0x1D01, body, len, 1500);
	assert(s.stage == IAP2_SESSION_STAGE_IDENT_START);
	assert(iap2_session_accessory_receives(&s, 0x4300) == IAP2_CAP_UNKNOWN);
	feed_message(&s, &link, 0x1D01, NULL, 0, 2000);
	len = put_param(body, 0, value, sizeof(value));
	len += put_param(body + len, 0, value, sizeof(value));
	feed_message(&s, &link, 0xAA01, body, len, 2100);
	assert(s.stage == IAP2_SESSION_STAGE_IDENT_INFO);
	complete_auth(&s, &link);
	len = put_param(body, 0, component, sizeof(component));
	len += put_param(body + len, 0, component, sizeof(component));
	size_t before = g_send_total;
	feed_message(&s, &link, 0x4E03, body, len, 2300);
	assert(g_send_total == before);
}


static void test_location_message_logging_is_sampled(void)
{
#ifdef IAP2_REAL_VEHICLE_STATE
	struct carplay_vehicle_state vehicle;
	carplay_vehicle_state_init(&vehicle);
	carplay_vehicle_state_begin_session(&vehicle, 1);
#endif
	struct iap2_link link;
	struct iap2_session s;
	uint8_t body[64];
	const uint8_t nmea[] = "$GPRMC,test";
	size_t len = put_param(body, 0, nmea, sizeof(nmea));
	unsigned i;

	start_link(&link, &s, 1000);
#ifdef IAP2_REAL_VEHICLE_STATE
	iap2_session_set_vehicle_state(&s, &vehicle, 1);
#else
	iap2_session_set_vehicle_state(&s, (struct carplay_vehicle_state *)(void *)&link, 1);
#endif
	g_csm_rx_logs = 0;
	g_location_logs = 0;
	for (i = 1; i <= 250; i++)
		feed_message(&s, &link, 0xFFFB, body, len, 1000 + i);
	assert(g_csm_rx_logs <= 10);
	assert(g_location_logs <= 10);
#ifdef IAP2_REAL_VEHICLE_STATE
	carplay_vehicle_state_destroy(&vehicle);
#endif
}

static void test_declared_location_without_4301_and_retry(void)
{
#ifdef IAP2_REAL_VEHICLE_STATE
	struct carplay_vehicle_state vehicle;
	carplay_vehicle_state_init(&vehicle);
	carplay_vehicle_state_begin_session(&vehicle, 1);
#endif
	struct iap2_link link;
	struct iap2_session s;
	uint8_t body[32];
	const uint8_t sends[] = {0xff,0xfb}, receives[] = {0xff,0xfa,0xff,0xfc};
	size_t len = put_param(body, 6, sends, sizeof(sends));
	len += put_param(body + len, 7, receives, sizeof(receives));
	start_link(&link, &s, 1000);
#ifdef IAP2_REAL_VEHICLE_STATE
	iap2_session_set_vehicle_state(&s, &vehicle, 1);
#else
	iap2_session_set_vehicle_state(&s, (struct carplay_vehicle_state *)(void *)&link, 1);
#endif
	feed_message(&s, &link, 0x1D01, body, len, 2000);
	g_fail_message = 0xFFFA;
	complete_auth(&s, &link);
	assert(!s.location_subscription_started);
	assert(!s.received_carplay_start_session && s.stage == IAP2_SESSION_STAGE_READY);
	g_fail_message = 0;
	iap2_session_tick(&s, &link, 4200);
	assert(s.location_subscription_started);
	assert(sent_back(0)->id == 0xFFFA);
#ifdef IAP2_REAL_VEHICLE_STATE
	assert(sent_back(0)->len == 12);
	assert(sent_back(0)->body[3] == 1 && sent_back(0)->body[7] == 2 && sent_back(0)->body[11] == 4);
#endif
	size_t before = g_send_total;
	iap2_session_tick(&s, &link, 19199);
	assert(g_send_total == before);
	iap2_session_tick(&s, &link, 19200);
	assert(g_send_total == before + 1 && sent_back(0)->id == 0xFFFC);
	assert(!s.location_subscription_started);
	/* Reopen only after the stop has entered the reliable window. */
	iap2_session_tick(&s, &link, 19201);
	assert(g_send_total == before + 2 && sent_back(0)->id == 0xFFFA);
	assert(s.location_subscription_started);
	/* Backpressure must preserve the healthy subscription and cannot spin. */
	g_fail_message = 0xFFFC;
	iap2_session_tick(&s, &link, 34200);
	assert(s.location_subscription_started && g_send_total == before + 2);
	g_fail_message = 0;
	iap2_session_tick(&s, &link, 64200);
	assert(g_send_total == before + 3 && sent_back(0)->id == 0xFFFC);
	assert(!s.location_subscription_started);
	iap2_session_tick(&s, &link, 64201);
	assert(g_send_total == before + 4 && sent_back(0)->id == 0xFFFA);
#ifdef IAP2_REAL_VEHICLE_STATE
	/* Healthy updates defer recovery; pending reliable traffic has priority. */
	const uint8_t nmea[]="$GPRMC,test";
	len=put_param(body,0,nmea,sizeof(nmea));
	feed_message(&s,&link,0xFFFB,body,len,100000);
	iap2_session_tick(&s,&link,109200);
	assert(g_send_total == before + 4);
	link.peer_max_retransmissions=3;link.pending_count=1;
	iap2_session_tick(&s,&link,115000);
	assert(g_send_total == before + 4);
	link.pending_count=0;s.authenticated=0;
	iap2_session_tick(&s,&link,115001);
	assert(g_send_total == before + 4);
	s.authenticated=1;
	iap2_session_tick(&s,&link,115002);
	assert(g_send_total == before + 5 && sent_back(0)->id == 0xFFFC);
	struct carplay_vehicle_snapshot snapshot;
	carplay_vehicle_state_snapshot(&vehicle, 115002, &snapshot);
	assert(snapshot.received && !strcmp(snapshot.rmc, (const char *)nmea));
	iap2_session_tick(&s,&link,115003);
	assert(g_send_total == before + 6 && sent_back(0)->id == 0xFFFA);
	before += 2;
#endif
	/* Explicit stop takes precedence over recovery, even on duplicate start. */
	size_t stopped_total = g_send_total;
	feed_message(&s,&link,0xFFFC,NULL,0,120000);
	iap2_session_tick(&s,&link,160000);
	assert(!s.location_subscription_started && g_send_total == stopped_total);
#ifdef IAP2_REAL_VEHICLE_STATE
	carplay_vehicle_state_snapshot(&vehicle, 160000, &snapshot);
	assert(!snapshot.received && !snapshot.rmc[0]);
#endif
	iap2_session_set_vehicle_state(&s,s.vehicle_state,2);
	assert(!s.location_subscription_stopped && !s.location_received_ms && !s.location_retry_ms);
#ifdef IAP2_REAL_VEHICLE_STATE
	carplay_vehicle_state_destroy(&vehicle);
#endif
}

static void test_declared_power_defers_optional_gps(void)
{
#ifdef IAP2_REAL_VEHICLE_STATE
    struct carplay_vehicle_state vehicle;
    carplay_vehicle_state_init(&vehicle);
    carplay_vehicle_state_begin_session(&vehicle, 1);
#endif
    struct iap2_link link;
    struct iap2_session s;
    uint8_t body[32];
    const uint8_t sends[] = {0xae,0,0xff,0xfb};
    const uint8_t receives[] = {0xae,1,0xff,0xfa,0xff,0xfc};
    size_t len = put_param(body, 6, sends, sizeof(sends));
    len += put_param(body + len, 7, receives, sizeof(receives));
    start_link(&link, &s, 1000);
#ifdef IAP2_REAL_VEHICLE_STATE
    iap2_session_set_vehicle_state(&s, &vehicle, 1);
#else
    iap2_session_set_vehicle_state(&s, (struct carplay_vehicle_state *)(void *)&link, 1);
#endif
    feed_message(&s, &link, 0x1D01, body, len, 2000);
    complete_auth(&s, &link);
    assert(!s.location_subscription_started);
    size_t before = g_send_total;
    iap2_session_tick(&s, &link, 4300);
    assert(g_send_total == before); /* Explicit power-capable peer has not subscribed yet. */
    g_fail_message = 0xFFFA; /* Optional service failure must not starve AE01. */
    feed_message(&s, &link, 0xAE00, NULL, 0, 4325);
    assert(sent_back(0)->id == 0xAE01);
    iap2_session_tick(&s, &link, 4350);
    assert(g_send_total == before + 2 && !s.power_update_pending);
    assert(!s.location_subscription_started);
    g_fail_message = 0;
    iap2_session_tick(&s, &link, 6500);
    assert(s.location_subscription_started);
    link.state = IAP2_IDLE;
    iap2_session_tick(&s, &link, 6600);
    assert(!s.power_exchange_seen);
#ifdef IAP2_REAL_VEHICLE_STATE
    carplay_vehicle_state_destroy(&vehicle);
#endif
}

static void test_services_gated_and_cancel_pending(void)
{
	struct iap2_link link;
	struct iap2_session s;
	uint8_t body[32];
	size_t len = put_param(body, 7, NULL, 0);
	start_link(&link, &s, 1000);
	feed_message(&s, &link, 0x4C00, NULL, 0, 1500);
	assert(g_send_total == 1);
	feed_message(&s, &link, 0x1D01, body, len, 2000);
	complete_auth(&s, &link);
	size_t before = g_send_total;
	feed_message(&s, &link, 0x4C00, NULL, 0, 2300);
	assert(g_send_total == before);
	start_link(&link, &s, 1000);
	feed_message(&s, &link, 0x1D01, NULL, 0, 2000);
	complete_auth(&s, &link);
	g_fail_message = 0x4C01;
	feed_message(&s, &link, 0x4C00, NULL, 0, 2300);
	assert(s.service_pending);
	g_fail_message = 0;
	iap2_session_tick(&s, &link, 4300);
	assert(sent_back(2)->id == 0x4C01); /* followed by AA05, 4300 stage retry */
	g_fail_message = 0x4C01;
	feed_message(&s, &link, 0x4C00, NULL, 0, 4400);
	feed_message(&s, &link, 0x4C02, NULL, 0, 4500);
	assert(!s.service_pending);
}

static void test_airplay_waits_for_auth_ack(void)
{
    struct iap2_session s;
    struct iap2_link l;
    reset_capture(); iap2_session_init(&s); iap2_link_init(&l, 0);
    l.state = IAP2_NORMAL; l.peer_max_retransmissions = 2;
    s.stage = IAP2_SESSION_STAGE_SESSION_WAIT;
    s.stage_next_ms = 1;
    iap2_session_tick(&s, &l, 1);
    assert(s.authentication_reported);
    l.pending_count = 1; l.pending[0].sequence = (uint8_t)l.sent_seq;
    assert(!iap2_session_authentication_ready(&s, &l));
    l.pending[0].sequence = (uint8_t)(l.sent_seq + 1);
    assert(iap2_session_authentication_ready(&s, &l)); /* later service remains pending */
    l.pending[0].sequence = (uint8_t)l.sent_seq;
    assert(iap2_session_authentication_ready(&s, &l)); /* sequence wrap cannot revoke proven ACK */
    l.state = IAP2_DEAD;
    assert(!iap2_session_authentication_ready(&s, &l));
    iap2_session_tick(&s, &l, 2);
    l.state = IAP2_NORMAL;
    assert(!iap2_session_authentication_ready(&s, &l));
    iap2_session_init(&s); l.pending_count = 0; l.peer_max_retransmissions = 0;
    s.authenticated = s.authentication_reported = 1;
    assert(iap2_session_authentication_ready(&s, &l)); /* no negotiated ACK tracking */
}
int main(int argc, char **argv)
{
	int scenario;
	test_airplay_waits_for_auth_ack();
	if (argc == 2) {
		if (strcmp(argv[1], "location-declared") == 0)
			test_declared_location_without_4301_and_retry();
		else if (strcmp(argv[1], "duplicate-params") == 0)
			test_duplicate_parameters_are_rejected();
		else if (strcmp(argv[1], "notifications") == 0)
			test_declared_notifications_only_and_retry();
		else if (strcmp(argv[1], "cap-long") == 0)
			test_long_capability_lists_and_reset();
		else if (strcmp(argv[1], "service-gate") == 0)
			test_services_gated_and_cancel_pending();
		else if (strcmp(argv[1], "cap-exclude") == 0)
			test_declared_exclusion_stops_availability_retries(0);
		else if (strcmp(argv[1], "cap-empty") == 0)
			test_declared_exclusion_stops_availability_retries(1);
		else if (strcmp(argv[1], "auth-unsolicited") == 0)
			test_unsolicited_auth_cannot_advance();
		else if (strcmp(argv[1], "auth-queue") == 0)
			test_failed_auth_queue_cannot_advance();
		else if (strcmp(argv[1], "endpoint-unsolicited") == 0)
			test_unsolicited_endpoint_cannot_connect();
		else if (strcmp(argv[1], "services") == 0)
			test_empty_library_and_bluetooth_replies();
		else if (strcmp(argv[1], "power") == 0) {
			test_power_update_subscription_replies_once();
			test_power_update_respects_single_packet_window();
		} else if (strcmp(argv[1], "power-gate") == 0)
			test_power_update_gates_and_fields();
		else if (strcmp(argv[1], "power-reset") == 0)
			test_power_update_failure_stop_and_reset();
		else if (strcmp(argv[1], "post-auth-priority") == 0)
			test_post_auth_notifications_precede_power_update();
		else if (strcmp(argv[1], "auth-window") == 0)
			test_auth_completion_single_packet_window();
		else if (strcmp(argv[1], "ident-ack") == 0)
			test_identification_accepted_is_empty();
		else if (strcmp(argv[1], "ident-order") == 0)
			test_identification_accept_send_failure_preserves_order();
		else if (strcmp(argv[1], "ident-window") == 0)
			test_identification_respects_single_packet_window();
		else if (strcmp(argv[1], "mfi-reliable-window") == 0)
			test_reliable_mfi_retry_is_link_owned();
		else if (strcmp(argv[1], "ident-direction") == 0)
			test_out_of_role_identification_cannot_advance();
		else if (strcmp(argv[1], "ident-log") == 0)
			test_accessory_identity_logs_capabilities();
		else if (strcmp(argv[1], "ident-malformed") == 0)
			test_malformed_identity_is_not_accepted();
		else if (argv[1][0] >= '0' && argv[1][0] <= '4')
			test_start_session_rejects_unusable_endpoint(argv[1][0] - '0');
		else if (strcmp(argv[1], "retry") == 0)
			test_session_wait_retries_frames();
		else if (strcmp(argv[1], "ready-loss") == 0)
			test_ready_link_loss_restarts_identification();
		else
			test_auth_report_waits_for_send_success();
		return 0;
	}
	test_waits_for_link();
	test_sends_identification_start();
	test_retransmits_then_backs_off();
	test_identification_accepted_is_empty();
	test_identification_accept_send_failure_preserves_order();
	test_identification_respects_single_packet_window();
	test_accessory_identity_logs_capabilities();
	test_malformed_identity_is_not_accepted();
	test_duplicate_identification_reply_is_ignored();
	test_audi_identity_uses_20_byte_challenge();
	test_full_identification_flow();
	test_reliable_mfi_retry_is_link_owned();
	test_out_of_role_identification_cannot_advance();
	test_link_loss_restarts_identification();
	test_session_wait_retries_frames();
	test_auth_report_waits_for_send_success();
	test_ready_link_loss_restarts_identification();
	test_declared_exclusion_stops_availability_retries(0);
	test_declared_exclusion_stops_availability_retries(1);
	test_unsolicited_auth_cannot_advance();
	test_failed_auth_queue_cannot_advance();
	test_unsolicited_endpoint_cannot_connect();
	test_empty_library_and_bluetooth_replies();
	test_power_update_subscription_replies_once();
	test_power_update_respects_single_packet_window();
	test_power_update_gates_and_fields();
	test_power_update_failure_stop_and_reset();
	test_post_auth_notifications_precede_power_update();
	test_auth_completion_single_packet_window();
	test_real_now_playing();
	test_route_provider();
	test_artwork_session();
	test_usb_artwork_uses_short_transfers();
	test_communications_provider();
	test_bluetooth_call_updates();
	test_declared_notifications_only_and_retry();
	test_long_capability_lists_and_reset();
	test_services_gated_and_cancel_pending();
	test_duplicate_parameters_are_rejected();
	test_location_message_logging_is_sampled();
	test_declared_location_without_4301_and_retry();
	test_declared_power_defers_optional_gps();
	for (scenario = 0; scenario <= 4; scenario++)
		test_start_session_rejects_unusable_endpoint(scenario);
	printf("iap2 session stage tests passed\n");
	return 0;
}
