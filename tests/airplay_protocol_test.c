/* Keep protocol tests isolated from the production audio capture daemon. */
#define CARPLAY_AUDIO_CAPTURE_SOCKET "carplay_audio_capture_test_v1"
#include "airplay_protocol.h"
#include "airplay_bplist.h"
#include "airplay_h264.h"
#include "airplay_screen_fixture.h"
#include "airplay_input.h"
#include "carplay_capabilities.h"
#include "carplay_audio_bridge.h"
#include "carplay_capabilities_test_fixtures.h"
#include <arpa/inet.h>
#include <poll.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>
static uint32_t read_u32_be(const uint8_t *data)
{
    return ((uint32_t)data[0] << 24) | ((uint32_t)data[1] << 16) |
           ((uint32_t)data[2] << 8) | data[3];
}

#ifdef AIRPLAY_TEST_VIDEO
#include "airplay_video.h"
#endif
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
void iap2_log(const char *fmt, ...)
{
#ifdef CARPLAY_AUDIO_TESTING
    va_list arguments;
    va_start(arguments, fmt);
    vfprintf(stderr, fmt, arguments);
    va_end(arguments);
    fputc('\n', stderr);
#else
    (void)fmt;
#endif
}
#include <string.h>

static int failures;

static void expect(int condition, const char *message)
{
    if (!condition) {
        fprintf(stderr, "FAIL: %s\n", message);
        failures++;
    }
}

static void test_event_request_without_content_length_is_bodyless(void)
{
    static const uint8_t request[] =
        "POST /command RTSP/1.0\r\n"
        "CSeq: 7\r\n"
        "\r\n";
    struct airplay_event_request parsed;
    char error[128] = { 0 };
    size_t consumed = 0;

    expect(airplay_event_parse_request(
               request, sizeof(request) - 1, &parsed, &consumed,
               error, sizeof(error)) == AIRPLAY_EVENT_PARSE_COMPLETE,
           "bodyless event request is complete");
    expect(consumed == sizeof(request) - 1,
           "bodyless event consumes exactly its header");
    expect(parsed.content_length == 0,
           "absent Content-Length means a zero-length body");
    expect(parsed.cseq == 7 && parsed.cseq_present,
           "bodyless event preserves its CSeq");
    expect(strcmp(parsed.method, "POST") == 0 &&
               strcmp(parsed.path, "/command") == 0,
           "bodyless event preserves its request line");
}
static void test_state_four_accepts_omitted_proof(void)
{
    static const uint8_t body[] = { 0x06, 0x01, 0x04 };
    uint8_t expected[64] = { 0 };
    int proof_present = -1;

    expect(airplay_pair_setup_check_state_four(
               body, sizeof(body), expected, &proof_present) == 0,
           "state 4 without proof is accepted");
    expect(proof_present == 0, "omitted proof is reported as absent");
}

static void test_state_four_verifies_present_proof(void)
{
    uint8_t body[3 + 2 + 64];
    uint8_t expected[64];
    int proof_present = -1;

    memset(expected, 0xa5, sizeof(expected));
    body[0] = 0x06;
    body[1] = 0x01;
    body[2] = 0x04;
    body[3] = 0x04;
    body[4] = sizeof(expected);
    memcpy(body + 5, expected, sizeof(expected));
    expect(airplay_pair_setup_check_state_four(
               body, sizeof(body), expected, &proof_present) == 0,
           "matching server proof is accepted");
    expect(proof_present == 1, "present proof is reported");

    body[5] ^= 0xff;
    expect(airplay_pair_setup_check_state_four(
               body, sizeof(body), expected, &proof_present) != 0,
           "mismatching server proof is rejected");
}

static void test_state_four_rejects_wrong_state_and_truncation(void)
{
    static const uint8_t wrong_state[] = { 0x06, 0x01, 0x02 };
    static const uint8_t truncated[] = { 0x04, 0x40, 0x00 };
    uint8_t expected[64] = { 0 };
    int proof_present = 0;

    expect(airplay_pair_setup_check_state_four(
               wrong_state, sizeof(wrong_state), expected, &proof_present) != 0,
           "non-state-4 response is rejected");
    expect(airplay_pair_setup_check_state_four(
               truncated, sizeof(truncated), expected, &proof_present) != 0,
           "truncated TLV is rejected");
}

static void test_tlv_append_and_read(void)
{
    uint8_t buffer[256];
    uint8_t value[80];
    uint8_t decoded[80];
    size_t used = 0;
    size_t decoded_len = 0;
    size_t i;

    for (i = 0; i < sizeof(value); i++)
        value[i] = (uint8_t)i;
    expect(airplay_tlv_append(buffer, sizeof(buffer), &used,
                              0x01, (const uint8_t *)"controller", 10) == 0,
           "identifier TLV is appended");
    expect(airplay_tlv_append(buffer, sizeof(buffer), &used,
                              0x03, value, sizeof(value)) == 0,
           "public-key TLV is appended");
    expect(airplay_tlv_get(buffer, used, 0x01, decoded,
                          sizeof(decoded), &decoded_len) == 0,
           "identifier TLV is decoded");
    expect(decoded_len == 10 && memcmp(decoded, "controller", 10) == 0,
           "identifier value round-trips");
    expect(airplay_tlv_get(buffer, used, 0x03, decoded,
                          sizeof(decoded), &decoded_len) == 0,
           "public-key TLV is decoded");
    expect(decoded_len == sizeof(value) &&
               memcmp(decoded, value, sizeof(value)) == 0,
           "public-key value round-trips");
}

static void test_tlv_read_joins_fragments(void)
{
    uint8_t buffer[2 + 255 + 2 + 129];
    uint8_t decoded[384];
    size_t decoded_len = 0;
    size_t i;

    buffer[0] = 0x03;
    buffer[1] = 255;
    for (i = 0; i < 255; i++)
        buffer[2 + i] = (uint8_t)(i * 7);
    buffer[257] = 0x03;
    buffer[258] = 129;
    for (i = 0; i < 129; i++)
        buffer[259 + i] = (uint8_t)(i * 11);
    expect(airplay_tlv_get(buffer, sizeof(buffer), 0x03, decoded,
                          sizeof(decoded), &decoded_len) == 0,
           "fragmented TLV is decoded");
    expect(decoded_len == sizeof(decoded), "fragment lengths are joined");
    for (i = 0; i < 255; i++)
        expect(decoded[i] == (uint8_t)(i * 7), "first fragment byte matches");
    for (i = 0; i < 129; i++)
        expect(decoded[255 + i] == (uint8_t)(i * 11), "second fragment byte matches");
}

static void test_tlv_read_rejects_truncation(void)
{
    static const uint8_t truncated[] = { 0x03, 0x05, 0x41, 0x42 };
    uint8_t decoded[8];
    size_t decoded_len = 0;

    expect(airplay_tlv_get(truncated, sizeof(truncated), 0x03, decoded,
                          sizeof(decoded), &decoded_len) != 0,
           "truncated TLV is rejected");
}
static const uint8_t setup_response_fixture[] = {
    0x62, 0x70, 0x6c, 0x69, 0x73, 0x74, 0x30, 0x30,
    0xd4, 0x01, 0x02, 0x03, 0x04, 0x05, 0x08, 0x09,
    0x0a, 0x5f, 0x10, 0x0f, 0x65, 0x6e, 0x61, 0x62,
    0x6c, 0x65, 0x64, 0x46, 0x65, 0x61, 0x74, 0x75,
    0x72, 0x65, 0x73, 0x59, 0x65, 0x76, 0x65, 0x6e,
    0x74, 0x50, 0x6f, 0x72, 0x74, 0x5f, 0x10, 0x11,
    0x6b, 0x65, 0x65, 0x70, 0x41, 0x6c, 0x69, 0x76,
    0x65, 0x4c, 0x6f, 0x77, 0x50, 0x6f, 0x77, 0x65,
    0x72, 0x5a, 0x74, 0x69, 0x6d, 0x69, 0x6e, 0x67,
    0x50, 0x6f, 0x72, 0x74, 0xa2, 0x06, 0x07, 0x5a,
    0x69, 0x41, 0x50, 0x43, 0x68, 0x61, 0x6e, 0x6e,
    0x65, 0x6c, 0x59, 0x76, 0x69, 0x65, 0x77, 0x41,
    0x72, 0x65, 0x61, 0x73, 0x11, 0x10, 0xe1, 0x09,
    0x11, 0x04, 0xd2, 0x08, 0x11, 0x23, 0x2d, 0x41,
    0x4c, 0x4f, 0x5a, 0x64, 0x67, 0x68, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x01, 0x01, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x0b, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x6b,
};

static void test_bplist_reads_setup_response(void)
{
    struct airplay_bplist_document document;
    struct airplay_bplist_value root;
    struct airplay_bplist_value value;
    struct airplay_bplist_value feature;
    uint64_t number = 0;
    int boolean = 0;
    uint64_t count = 0;

    expect(airplay_bplist_init(&document, setup_response_fixture,
                               sizeof(setup_response_fixture)) == 0,
           "binary plist fixture header and trailer are valid");
    expect(airplay_bplist_root(&document, &root) == 0 &&
               root.type == AIRPLAY_BPLIST_DICTIONARY,
           "fixture root is a dictionary");
    expect(airplay_bplist_dict_get(&document, &root, "eventPort", &value) == 1,
           "eventPort is present");
    expect(airplay_bplist_get_uint64(&document, &value, &number) == 0 &&
               number == 4321,
           "eventPort integer is decoded");
    expect(airplay_bplist_dict_get(&document, &root, "timingPort", &value) == 1 &&
               airplay_bplist_get_uint64(&document, &value, &number) == 0 &&
               number == 1234,
           "timingPort integer is decoded");
    expect(airplay_bplist_dict_get(&document, &root, "keepAliveLowPower", &value) == 1 &&
               airplay_bplist_get_bool(&document, &value, &boolean) == 0 &&
               boolean == 1,
           "keepAliveLowPower boolean is decoded");
    expect(airplay_bplist_dict_get(&document, &root, "enabledFeatures", &value) == 1 &&
               airplay_bplist_array_count(&document, &value, &count) == 0 &&
               count == 2,
           "enabledFeatures array has two entries");
    expect(airplay_bplist_array_get(&document, &value, 0, &feature) == 0 &&
               airplay_bplist_string_equals(&document, &feature, "iAPChannel"),
           "first enabled feature is iAPChannel");
    expect(airplay_bplist_array_get(&document, &value, 1, &feature) == 0 &&
               airplay_bplist_string_equals(&document, &feature, "viewAreas"),
           "second enabled feature is viewAreas");
    expect(airplay_bplist_dict_get(&document, &root, "missing", &value) == 0,
           "unknown dictionary key is reported absent");
}

static void test_bplist_rejects_invalid_buffers(void)
{
    struct airplay_bplist_document document;
    struct airplay_bplist_value root;
    uint8_t truncated[sizeof(setup_response_fixture) - 1];
    uint8_t bad_magic[sizeof(setup_response_fixture)];

    memcpy(truncated, setup_response_fixture, sizeof(truncated));
    expect(airplay_bplist_init(&document, truncated, sizeof(truncated)) != 0,
           "truncated binary plist is rejected");
    memcpy(bad_magic, setup_response_fixture, sizeof(bad_magic));
    bad_magic[7] = 'x';
    expect(airplay_bplist_init(&document, bad_magic, sizeof(bad_magic)) != 0,
           "bad binary plist magic is rejected");
    expect(airplay_bplist_init(&document, setup_response_fixture, 39) != 0,
           "short binary plist is rejected");
    expect(airplay_bplist_init(&document, setup_response_fixture,
                               sizeof(setup_response_fixture)) == 0 &&
               airplay_bplist_root(&document, &root) == 0,
           "valid fixture remains readable after rejection tests");
}

static void test_bplist_encodes_screen_stream_setup(void)
{
    uint8_t buffer[1024];
    size_t len = 0;
    struct airplay_bplist_document document;
    struct airplay_bplist_value root;
    struct airplay_bplist_value streams;
    struct airplay_bplist_value stream;
    struct airplay_bplist_value value;
    uint64_t number = 0;
    uint64_t connection_id = UINT64_C(0x0a0b0c0d0e0f0001);

    expect(airplay_bplist_encode_screen_setup(buffer, sizeof(buffer), &len,
                                             connection_id) == 0,
           "screen stream SETUP plist is encoded");
    expect(airplay_bplist_init(&document, buffer, len) == 0 &&
               airplay_bplist_root(&document, &root) == 0 &&
               airplay_bplist_dict_get(&document, &root, "streams",
                                       &streams) == 1 &&
               airplay_bplist_array_count(&document, &streams, &number) == 0 &&
               number == 1 &&
               airplay_bplist_array_get(&document, &streams, 0,
                                        &stream) == 0,
           "screen stream SETUP contains one streams entry");
    expect(airplay_bplist_dict_get(&document, &stream, "type", &value) == 1 &&
               airplay_bplist_get_uint64(&document, &value, &number) == 0 &&
               number == 110,
           "screen stream type is 110");
    expect(airplay_bplist_dict_get(&document, &stream, "streamID", &value) == 1 &&
               airplay_bplist_get_uint64(&document, &value, &number) == 0 &&
               number == 1,
           "screen stream ID is 1");
    expect(airplay_bplist_dict_get(&document, &stream, "streamConnectionID",
                                   &value) == 1 &&
               airplay_bplist_get_uint64(&document, &value, &number) == 0 &&
               number == connection_id,
           "streamConnectionID round-trips as an unsigned 64-bit integer");
}
static void test_bplist_encodes_audio_stream_setup(void)
{
    struct airplay_stream_setup setups[3] = {
        { .type = 110, .stream_id = 1,
          .connection_id = UINT64_C(0x0a0b0c0d0e0f0001) },
        { .type = 100, .stream_id = 2,
          .connection_id = UINT64_C(0x0a0b0c0d0e0f0002),
          .audio_type = "media", .audio_format = UINT32_C(0x00000800),
          .data_port = 47010, .input = 1 },
        { .type = 101, .stream_id = 3,
          .connection_id = UINT64_C(0x0a0b0c0d0e0f0003),
          .audio_type = "default", .audio_format = UINT32_C(0x00000020) }
    };
    uint8_t buffer[2048];
    size_t len = 0;
    struct airplay_bplist_document document;
    struct airplay_bplist_value root;
    struct airplay_bplist_value streams;
    struct airplay_bplist_value stream;
    struct airplay_bplist_value value;
    uint64_t number = 0;
    char audio_type[32];
    size_t audio_type_len = 0;

    expect(airplay_bplist_encode_stream_setup(buffer, sizeof(buffer), &len,
                                             setups, 3) == 0,
           "multi-stream audio SETUP is encoded");
    expect(airplay_bplist_init(&document, buffer, len) == 0 &&
               airplay_bplist_root(&document, &root) == 0 &&
               airplay_bplist_dict_get(&document, &root, "streams",
                                       &streams) == 1 &&
               airplay_bplist_array_count(&document, &streams, &number) == 0 &&
               number == 3,
           "multi-stream SETUP retains screen and two audio streams");
    expect(airplay_bplist_array_get(&document, &streams, 1, &stream) == 0 &&
               airplay_bplist_dict_get(&document, &stream, "type",
                                       &value) == 1 &&
               airplay_bplist_get_uint64(&document, &value, &number) == 0 &&
               number == 100,
           "main media audio stream type is 100");
    expect(airplay_bplist_dict_get(&document, &stream, "audioType",
                                   &value) == 1 &&
               airplay_bplist_string_copy(&document, &value, audio_type,
                                          sizeof(audio_type),
                                          &audio_type_len) == 0 &&
               strcmp(audio_type, "media") == 0,
           "main audio stream retains the media variant");
    expect(airplay_bplist_dict_get(&document, &stream, "audioFormat",
                                   &value) == 1 &&
               airplay_bplist_get_uint64(&document, &value, &number) == 0 &&
               number == UINT32_C(0x00000800),
           "audio format is encoded as the selected LPCM mask");
    expect(airplay_bplist_dict_get(&document, &stream, "dataPort",
                                   &value) == 1 &&
               airplay_bplist_get_uint64(&document, &value, &number) == 0 &&
               number == 47010,
           "car microphone input advertises its local UDP port");
    expect(airplay_bplist_array_get(&document, &streams, 2, &stream) == 0 &&
               airplay_bplist_dict_get(&document, &stream, "audioType",
                                       &value) == 1 &&
               airplay_bplist_string_copy(&document, &value, audio_type,
                                          sizeof(audio_type),
                                          &audio_type_len) == 0 &&
               strcmp(audio_type, "default") == 0,
           "notification stream keeps its distinct variant");
}

static void test_bplist_encodes_all_negotiated_audio_streams(void)
{
    struct airplay_stream_setup setups[9];
    uint8_t buffer[4096];
    size_t len = 0;
    struct airplay_bplist_document document;
    struct airplay_bplist_value root;
    struct airplay_bplist_value streams;
    struct airplay_bplist_value stream;
    struct airplay_bplist_value value;
    uint64_t number = 0;
    size_t i;

    memset(setups, 0, sizeof(setups));
    setups[0].type = 110;
    setups[0].stream_id = 1;
    setups[0].connection_id = UINT64_C(0x0a0b0c0d0e0f0001);
    for (i = 1; i < sizeof(setups) / sizeof(setups[0]); i++) {
        setups[i].type = 100;
        setups[i].stream_id = (int)i + 1;
        setups[i].connection_id = UINT64_C(0x0a0b0c0d0e0f0001) + i;
        setups[i].audio_type = "media";
        setups[i].audio_format = UINT32_C(0x00008000);
    }

    expect(airplay_bplist_encode_stream_setup(
               buffer, sizeof(buffer), &len, setups,
               sizeof(setups) / sizeof(setups[0])) == 0,
           "screen plus eight negotiated audio streams are encoded");
    expect(airplay_bplist_init(&document, buffer, len) == 0 &&
               airplay_bplist_root(&document, &root) == 0 &&
               airplay_bplist_dict_get(&document, &root, "streams",
                                       &streams) == 1 &&
               airplay_bplist_array_count(&document, &streams, &number) == 0 &&
               number == 9,
           "stream SETUP retains every negotiated output");
    expect(airplay_bplist_array_get(&document, &streams, 8, &stream) == 0 &&
               airplay_bplist_dict_get(&document, &stream, "streamID",
                                       &value) == 1 &&
               airplay_bplist_get_uint64(&document, &value, &number) == 0 &&
               number == 9,
           "the eighth audio stream is the final SETUP entry");
}

static void test_bplist_encodes_setup_request(void)
{
    uint8_t buffer[1024];
    size_t len = 0;
    struct airplay_bplist_document document;
    struct airplay_bplist_value root;
    struct airplay_bplist_value value;
    uint64_t number = 0;
    int boolean = 0;

    expect(airplay_bplist_encode_setup(buffer, sizeof(buffer), &len,
                                      7000, 1, 0) == 0,
           "SETUP request plist is encoded");
    expect(len >= 40 && memcmp(buffer, "bplist00", 8) == 0,
           "encoded SETUP request uses bplist00");
    expect(airplay_bplist_init(&document, buffer, len) == 0 &&
               airplay_bplist_root(&document, &root) == 0 &&
               root.type == AIRPLAY_BPLIST_DICTIONARY,
           "encoded SETUP request round-trips as a dictionary");
    expect(airplay_bplist_dict_get(&document, &root, "name", &value) == 1 &&
               airplay_bplist_string_equals(&document, &value,
                                            "iPhone"),
           "SETUP name is present");
    expect(airplay_bplist_dict_get(&document, &root, "timingPort", &value) == 1 &&
               airplay_bplist_get_uint64(&document, &value, &number) == 0 &&
               number == 7000,
           "SETUP timingPort is present");
    expect(airplay_bplist_dict_get(&document, &root, "keepAliveLowPower", &value) == 1 &&
               airplay_bplist_get_bool(&document, &value, &boolean) == 0 &&
               boolean == 1,
           "SETUP keepAliveLowPower is present");
    expect(airplay_bplist_dict_get(&document, &root, "features", &value) == 1 &&
               airplay_bplist_array_count(&document, &value, &number) == 0 &&
               number == 0,
           "legacy SETUP proposes no HAP-only features");
}

/*
 * `et` is a statement about the streams that follow, and it is only valid on
 * an unpaired session:
 *
 *   unpaired  -> et = 1 (kAirPlayEncryptionType_None), plaintext streams
 *   paired    -> key absent, streams keyed from the pair-verify secret
 *
 * Both mistakes are silent on the control channel and fatal on the data
 * channel: an absent key on an unpaired session draws 466 Key Management
 * Error, and et = 1 on a paired session makes the receiver treat ChaCha20
 * ciphertext as plaintext NAL units.
 */
static void test_bplist_setup_encryption_type(void)
{
    uint8_t buffer[1024];
    size_t len = 0;
    struct airplay_bplist_document document;
    struct airplay_bplist_value root;
    struct airplay_bplist_value value;
    struct airplay_bplist_value feature;
    uint64_t number = 0;
    int found;

    expect(airplay_bplist_encode_setup(buffer, sizeof(buffer), &len,
                                      7000, 1, 0) == 0 &&
               airplay_bplist_init(&document, buffer, len) == 0 &&
               airplay_bplist_root(&document, &root) == 0,
           "unpaired SETUP plist round-trips");
    found = airplay_bplist_dict_get(&document, &root, "et", &value);
    expect(found == 1 &&
               airplay_bplist_get_uint64(&document, &value, &number) == 0 &&
               number == 1,
           "unpaired SETUP carries et = 1 (no encryption)");

    expect(airplay_bplist_encode_setup(buffer, sizeof(buffer), &len,
                                      7000, 1, 1) == 0 &&
               airplay_bplist_init(&document, buffer, len) == 0 &&
               airplay_bplist_root(&document, &root) == 0,
           "paired SETUP plist round-trips");
    found = airplay_bplist_dict_get(&document, &root, "et", &value);
    expect(found == 0,
           "paired SETUP omits et; the ChaCha20 streams come from pair-verify");
    expect(airplay_bplist_dict_get(&document, &root, "timingPort", &value) == 1,
           "paired SETUP still carries the rest of the dictionary");
    expect(airplay_bplist_dict_get(&document, &root, "features", &value) == 1 &&
               airplay_bplist_array_count(&document, &value, &number) == 0 &&
               number == 2 &&
               airplay_bplist_array_get(&document, &value, 0, &feature) == 0 &&
               airplay_bplist_string_equals(&document, &feature, "iAPChannel") &&
               airplay_bplist_array_get(&document, &value, 1, &feature) == 0 &&
               airplay_bplist_string_equals(&document, &feature, "viewAreas"),
           "HAP SETUP preserves exactly iAPChannel and viewAreas");
}

#ifdef AIRPLAY_TEST_VIDEO
static uint64_t read_u64_le(const uint8_t *data)
{
    uint64_t value = 0;
    size_t i;

    for (i = 0; i < 8; i++)
        value |= (uint64_t)data[i] << (i * 8);
    return value;
}

static uint32_t read_u32_le(const uint8_t *data)
{
    return (uint32_t)data[0] | ((uint32_t)data[1] << 8) |
           ((uint32_t)data[2] << 16) | ((uint32_t)data[3] << 24);
}

static void test_screen_video_header_and_frame_round_trip(void)
{
    static const uint8_t key[AIRPLAY_CHACHA_KEY_BYTES] = {
        0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07,
        0x08, 0x09, 0x0a, 0x0b, 0x0c, 0x0d, 0x0e, 0x0f,
        0x10, 0x11, 0x12, 0x13, 0x14, 0x15, 0x16, 0x17,
        0x18, 0x19, 0x1a, 0x1b, 0x1c, 0x1d, 0x1e, 0x1f
    };
    static const uint8_t frame[] = { 0x00, 0x00, 0x00, 0x01, 0x65, 0x88 };
    static const uint8_t config[] = { 0x01, 0x64, 0x00, 0x1f, 0xff };
    const uint64_t sender_ntp = UINT64_C(0x1122334455667788);
    uint8_t message[256];
    uint8_t plaintext[32];
    uint8_t nonce[AIRPLAY_CHACHA_NONCE_BYTES] = { 0 };
    size_t message_len = 0;
    size_t plaintext_len = 0;

    expect(airplay_crypto_init() == 0, "crypto initializes for video tests");
    expect(airplay_screen_encode_config(message, sizeof(message), &message_len,
                                         config, sizeof(config), sender_ntp) == 0,
           "screen video config message is encoded");
    expect(message_len == AIRPLAY_SCREEN_HEADER_BYTES + sizeof(config),
           "video config message contains a 128-byte header and clear body");
    expect(read_u32_le(message) == sizeof(config),
           "video config body size is little-endian");
    expect(message[4] == AIRPLAY_SCREEN_OP_VIDEO_CONFIG,
           "video config opcode is encoded at byte four");
    expect(read_u64_le(message + 8) == sender_ntp,
           "sender timestamps are little-endian at byte eight");

    expect(airplay_screen_encode_frame(message, sizeof(message), &message_len,
                                        key, 7, 1, frame, sizeof(frame),
                                        sender_ntp) == 0,
           "encrypted screen video frame message is encoded");
    expect(message_len == AIRPLAY_SCREEN_HEADER_BYTES + sizeof(frame) +
                                   AIRPLAY_CHACHA_TAG_BYTES,
           "video frame message contains a header, ciphertext, and tag");
    expect(read_u32_le(message) == sizeof(frame) + AIRPLAY_CHACHA_TAG_BYTES,
           "sealed video body size includes the authentication tag");
    expect(message[4] == AIRPLAY_SCREEN_OP_VIDEO_FRAME,
           "video frame opcode is encoded at byte four");
    expect(message[5] == AIRPLAY_SCREEN_FLAG_KEYFRAME,
           "video keyframes set the keyframe flag at byte five");
    for (size_t i = 0; i < 8; i++)
        nonce[4 + i] = (uint8_t)(UINT64_C(7) >> (i * 8));
    expect(airplay_chacha20_poly1305_open(
               key, nonce, message + AIRPLAY_SCREEN_HEADER_BYTES,
               message_len - AIRPLAY_SCREEN_HEADER_BYTES,
               message, AIRPLAY_SCREEN_HEADER_BYTES,
               plaintext, sizeof(plaintext), &plaintext_len) == 0 &&
               plaintext_len == sizeof(frame) &&
               memcmp(plaintext, frame, sizeof(frame)) == 0,
           "video frame decrypts with the full header as AAD");
}


static void test_screen_h264_fixture_shape(void)
{
    uint16_t sps_len;
    uint16_t pps_len;

    expect(airplay_screen_test_config_len >= 11 &&
               airplay_screen_test_config[0] == 1 &&
               airplay_screen_test_config[1] == 0x42 &&
               (airplay_screen_test_config[5] & 0x1f) == 1,
           "screen fixture starts with a constrained-baseline avcC record");
    sps_len = (uint16_t)((airplay_screen_test_config[6] << 8) |
                         airplay_screen_test_config[7]);
    expect(8u + sps_len + 3u <= airplay_screen_test_config_len &&
               (airplay_screen_test_config[8] & 0x1f) == 7,
           "screen fixture avcC contains exactly one SPS");
    pps_len = (uint16_t)((airplay_screen_test_config[9u + sps_len] << 8) |
                         airplay_screen_test_config[10u + sps_len]);
    expect(11u + sps_len + pps_len == airplay_screen_test_config_len &&
               (airplay_screen_test_config[11u + sps_len] & 0x1f) == 8,
           "screen fixture avcC contains exactly one PPS");
    {
        uint32_t first_len = read_u32_be(airplay_screen_test_frame);
        size_t offset = 4u + first_len;
        uint32_t second_len;
        size_t idr_offset;
        uint32_t idr_len;

        expect(airplay_screen_test_frame_len > 12 &&
                   first_len == sps_len &&
                   (airplay_screen_test_frame[4] & 0x1f) == 7,
               "screen fixture frame starts with a length-prefixed SPS");
        expect(offset + 4 <= airplay_screen_test_frame_len,
               "screen fixture frame contains a PPS length");
        second_len = read_u32_be(airplay_screen_test_frame + offset);
        expect(second_len == pps_len &&
                   offset + 4 + second_len <= airplay_screen_test_frame_len &&
                   (airplay_screen_test_frame[offset + 4] & 0x1f) == 8,
               "screen fixture frame contains a length-prefixed PPS");
        idr_offset = offset + 4u + second_len;
        expect(idr_offset + 4 <= airplay_screen_test_frame_len,
               "screen fixture frame contains an IDR length");
        idr_len = read_u32_be(airplay_screen_test_frame + idr_offset);
        expect(idr_offset + 4u + idr_len == airplay_screen_test_frame_len &&
                   (airplay_screen_test_frame[idr_offset + 4] & 0x1f) == 5,
               "screen fixture frame ends with one length-prefixed IDR");
    }
}
#endif


struct h264_capture {
    uint8_t bytes[16384];
    size_t len;
    unsigned configs;
    unsigned frames;
    unsigned keyframes;
};

static int capture_h264_config(void *context, const uint8_t *config,
                               size_t config_len)
{
    struct h264_capture *capture = context;

    if (config_len > sizeof(capture->bytes))
        return -1;
    memcpy(capture->bytes, config, config_len);
    capture->len = config_len;
    capture->configs++;
    return 0;
}

static int capture_h264_frame(void *context, const uint8_t *nal, size_t nal_len,
                              int keyframe)
{
    struct h264_capture *capture = context;

    if (!nal_len || nal_len > sizeof(capture->bytes))
        return -1;
    memcpy(capture->bytes, nal, nal_len);
    capture->len = nal_len;
    capture->frames++;
    if (keyframe)
        capture->keyframes++;
    return 0;
}

static size_t append_start_code_nals(uint8_t *output, size_t capacity,
                                     const uint8_t *length_prefixed,
                                     size_t length)
{
    size_t input = 0;
    size_t used = 0;

    while (input + 4 <= length) {
        uint32_t nal_len = read_u32_be(length_prefixed + input);
        input += 4;
        if (!nal_len || nal_len > length - input ||
            used + 4 + nal_len > capacity)
            return 0;
        output[used++] = 0;
        output[used++] = 0;
        output[used++] = 0;
        output[used++] = 1;
        memcpy(output + used, length_prefixed + input, nal_len);
        used += nal_len;
        input += nal_len;
    }
    return input == length ? used : 0;
}

static void test_h264_parser_emits_parameter_sets_and_frames(void)
{
    struct airplay_h264_parser parser;
    struct h264_capture capture = { 0 };
    /* The placeholder frame grew to a 9501-byte 800x480 picture (SPS+PPS+IDR);
     * these buffers must hold the whole Annex-B stream and its largest NAL. */
    uint8_t nal_buffer[16384];
    uint8_t annex_b[16384];
    uint8_t p_slice[] = { 0x00, 0x00, 0x00, 0x01, 0x41, 0x80 };
    size_t annex_len = append_start_code_nals(
        annex_b, sizeof(annex_b), airplay_screen_test_frame,
        airplay_screen_test_frame_len);

    expect(annex_len > 0, "H.264 fixture converts to Annex B");
    airplay_h264_parser_init(&parser, nal_buffer, sizeof(nal_buffer),
                             capture_h264_config, capture_h264_frame,
                             &capture);
    for (size_t i = 0; i < annex_len; i++)
        expect(airplay_h264_feed(&parser, annex_b + i, 1) == 0,
               "fragmented H.264 input is accepted");
    expect(airplay_h264_flush(&parser) == 0,
           "H.264 parser flushes the final NAL");
    expect(capture.configs == 1, "H.264 parser emits one avcC config");
    expect(capture.frames == 1 && capture.keyframes == 1,
           "H.264 parser emits the IDR frame");
    expect(airplay_h264_feed(&parser, p_slice, sizeof(p_slice)) == 0 &&
               airplay_h264_flush(&parser) == 0,
           "H.264 parser accepts a following P slice");
    expect(capture.frames == 2 && capture.keyframes == 1,
           "H.264 parser reports a non-key frame");
}

static void test_input_parses_hid_touch_report(void)
{
    static const uint8_t command[] = {
        0x62, 0x70, 0x6c, 0x69, 0x73, 0x74, 0x30, 0x30,
        0xd3, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06,
        0x59, 0x68, 0x69, 0x64, 0x52, 0x65, 0x70, 0x6f,
        0x72, 0x74, 0x54, 0x74, 0x79, 0x70, 0x65, 0x54,
        0x75, 0x75, 0x69, 0x64, 0x4c, 0x00, 0x01, 0x20,
        0x03, 0x40, 0x02, 0x01, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x5d, 0x68, 0x69, 0x64, 0x53, 0x65, 0x6e,
        0x64, 0x52, 0x65, 0x70, 0x6f, 0x72, 0x74, 0x58,
        0x32, 0x61, 0x32, 0x61, 0x32, 0x61, 0x32, 0x61,
        0x08, 0x0f, 0x19, 0x1e, 0x23, 0x30, 0x3e, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x01, 0x01, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x07, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x47
    };
    struct airplay_input_command parsed;

    expect(airplay_input_parse(command, sizeof(command), &parsed) == 1,
           "CarPlay HID command parses");
    expect(parsed.kind == AIRPLAY_INPUT_HID_REPORT,
           "CarPlay HID command has the HID kind");
    expect(strcmp(parsed.uuid, "2a2a2a2a") == 0,
           "CarPlay HID command preserves the device UUID");
    expect(parsed.report_len == 12 && parsed.report &&
               parsed.report[0] == 0x00 && parsed.report[1] == 0x01 &&
               parsed.report[2] == 0x20 && parsed.report[3] == 0x03,
           "CarPlay HID command exposes the touch report bytes");
    expect(airplay_input_parse(command, 32, &parsed) == 0,
           "truncated CarPlay control commands are rejected");
}

static void test_input_decodes_touch_point_and_media_keys(void)
{
    static const uint8_t report[] = {
        0x00, 0x01, 0x20, 0x03, 0x40, 0x02,
        0x01, 0x00, 0x00, 0x00, 0x00, 0x00
    };
    struct airplay_input_touch touch;

    expect(airplay_input_touch_point(report, sizeof(report), &touch) == 1,
           "touch HID report decodes");
    expect(touch.down == 1 && touch.x == 0x0320 && touch.y == 0x0240,
           "touch HID report exposes down state and little-endian coordinates");
    expect(airplay_input_media_keycode(1) == 126,
           "media play maps to Android KEYCODE_MEDIA_PLAY");
    expect(airplay_input_media_keycode(3) == 85,
           "media toggle maps to Android KEYCODE_MEDIA_PLAY_PAUSE");
    expect(airplay_input_media_keycode(4) == 87,
           "media next maps to Android KEYCODE_MEDIA_NEXT");
    expect(airplay_input_media_keycode(0) == 0,
           "release and unknown media reports have no key action");
    expect(airplay_input_knob_keycode(0x01, 0, 0, 0) == 66,
           "knob select maps to Android DPAD_CENTER");
    expect(airplay_input_knob_keycode(0x04, 0, 0, 0) == 4,
           "knob back maps to Android BACK");
    expect(airplay_input_knob_keycode(0, 0, 0, 1) == 22,
           "clockwise knob movement maps to DPAD_RIGHT");
    expect(airplay_input_knob_keycode(0, 0, 0, -1) == 21,
           "counterclockwise knob movement maps to DPAD_LEFT");
    expect(airplay_input_knob_keycode(0, 0, -1, 0) == 19,
           "tilting the knob still maps to DPAD_UP");
    expect(airplay_input_knob_keycode(0, 0, 0, 0) == 0,
           "idle knob report has no key action");
}


static void test_capabilities_negotiates_audi_display_and_hid(void)
{
    struct carplay_session_capabilities capabilities;

    expect(carplay_capabilities_parse(
               airplay_info_audi_no_touch,
               airplay_info_audi_no_touch_len,
               &capabilities) == 0,
           "Audi capability description parses");
    expect(capabilities.display.present == 1 &&
               capabilities.display.width_pixels == 800 &&
               capabilities.display.height_pixels == 480,
           "Audi display is negotiated as 800x480");
    expect(capabilities.display.primary_input_device == 3,
           "Audi primary input device is retained from the car");
    expect(capabilities.hid_count == 2,
           "Audi advertises knob and keyboard HID devices");
    expect(capabilities.has_touch == 0,
           "Audi no-touch profile does not invent a touchscreen");
    expect(capabilities.has_keyboard == 1,
           "standard keyboard HID is classified by descriptor");
    expect(capabilities.has_knob == 1,
           "rotary HID is classified by descriptor");
    expect(capabilities.audio.description_present == 1 &&
               capabilities.audio.media_output == 1 &&
               capabilities.audio.microphone_input == 1,
           "audio output and car microphone capabilities are retained");
    expect(strcmp(capabilities.display.uuid, "audi-main") == 0,
           "display UUID is retained");
}

static void test_bplist_encodes_car_ui_request(void)
{
    struct airplay_bplist_document document;
    struct airplay_bplist_value root;
    struct airplay_bplist_value value;
    uint8_t buffer[256];
    size_t len = 0;

    expect(airplay_bplist_encode_car_ui_request(buffer, sizeof(buffer), &len) == 0 &&
               len > 0,
           "car UI request plist encodes");
    expect(airplay_bplist_init(&document, buffer, len) == 0 &&
               airplay_bplist_root(&document, &root) == 0 &&
               root.type == AIRPLAY_BPLIST_DICTIONARY,
           "car UI request is a bplist dictionary");
    expect(airplay_bplist_dict_get(&document, &root, "type", &value) == 1 &&
               airplay_bplist_string_equals(&document, &value, "requestUI"),
           "car UI request type is requestUI");
    expect(airplay_bplist_dict_get(&document, &root, "params", &value) == 1 &&
               value.type == AIRPLAY_BPLIST_DICTIONARY && value.count == 0,
           "car UI request has an empty params dictionary");
}

static void test_capabilities_remote_ui_fields(void)
{
    struct carplay_session_capabilities capabilities;

    expect(carplay_capabilities_parse(airplay_info_oem_icon,
                                      airplay_info_oem_icon_len,
                                      &capabilities) == 0 &&
               capabilities.oem_icon_present == 1 &&
               capabilities.request_car_ui_supported == 1,
           "OEM icon and enhancedRequestCarUI are detected");
    expect(carplay_capabilities_parse(airplay_info_no_oem_icon,
                                      airplay_info_no_oem_icon_len,
                                      &capabilities) == 0 &&
               capabilities.oem_icon_present == 0 &&
               capabilities.request_car_ui_supported == 1,
           "car without an OEM icon is eligible for the fallback");
    expect(carplay_capabilities_parse(airplay_info_no_car_ui,
                                      airplay_info_no_car_ui_len,
                                      &capabilities) == 0 &&
               capabilities.request_car_ui_supported == 0,
           "missing enhancedRequestCarUI rejects car UI navigation");
}

static void test_capabilities_negotiates_touch_ranges(void)
{
    struct carplay_session_capabilities capabilities;
    const struct carplay_hid_device_capabilities *touch;

    expect(carplay_capabilities_parse(
               airplay_info_audi_touch,
               airplay_info_audi_touch_len,
               &capabilities) == 0,
           "touch capability description parses");
    expect(capabilities.has_touch == 1,
           "touch HID is advertised when present");
    touch=carplay_capabilities_find_hid(&capabilities,"touch-main");
    expect(touch != NULL && touch->kind == CARPLAY_HID_TOUCH,
           "touch HID is located by the UUID returned by the car");
    expect(touch && touch->touch_x_max == 0x031f,
           "touch X logical maximum comes from the HID descriptor");
    expect(touch && touch->touch_y_max == 0x01df,
           "touch Y logical maximum comes from the HID descriptor");
}

static void test_audio_bridge_prepares_lpcm_and_round_trips_rtp(void)
{
    struct carplay_session_capabilities capabilities;
    struct carplay_audio_format_capabilities *media;
    struct carplay_audio_bridge *bridge = NULL;
    struct airplay_stream_setup setups[CARPLAY_AUDIO_MAX_STREAMS];
    uint8_t secret[32];
    uint8_t key[32];
    uint8_t pcm_little[] = { 0x34, 0x12, 0x78, 0x56 };
    uint8_t packet[128];
    uint8_t opened[32];
    size_t packet_len = 0;
    size_t opened_len = 0;
    struct carplay_audio_rtp_counters counters = {
        .sequence = 7, .timestamp = 441, .nonce = 9,
        .ssrc = UINT32_C(0x12345678)
    };
    memset(secret, 0x42, sizeof(secret));
    memset(key, 0x24, sizeof(key));
    memset(&capabilities, 0, sizeof(capabilities));
    capabilities.audio.description_present = 1;
    capabilities.audio.media_output = 1;
    capabilities.audio.microphone_input = 1;
    capabilities.audio.format_count = 1;
    media = &capabilities.audio.formats[0];
    media->present = 1;
    media->type = 100;
    strcpy(media->audio_type, "media");
    media->output_formats = UINT32_C(0x00000800);
    media->input_formats = UINT32_C(0x00000010) | UINT32_C(0x00000020) | UINT32_C(0x00000004);

    expect(airplay_crypto_init() == 0, "audio test crypto initializes");
    expect(carplay_audio_bridge_init(&bridge, secret, 1, 1) == 0,
           "audio bridge initializes");
    expect(carplay_audio_bridge_prepare(bridge, &capabilities) == 0 &&
               carplay_audio_bridge_setup_count(bridge) == 1,
           "audio bridge prepares a supported LPCM stream");
    expect(carplay_audio_bridge_fill_setups(bridge, setups,
                                            CARPLAY_AUDIO_MAX_STREAMS) == 0 &&
               setups[0].type == 100 &&
               strcmp(setups[0].audio_type, "media") == 0 &&
               setups[0].audio_format == UINT32_C(0x00000800) &&
               setups[0].input == 1,
           "audio SETUP keeps media format and car microphone input");
    {
        char resolved[64];
        size_t resolved_len = 0;
        expect(carplay_audio_bridge_resolve_response(
                   bridge, 100, setups[0].connection_id, resolved,
                   sizeof(resolved), &resolved_len) == 0 &&
                   resolved_len == 5 && strcmp(resolved, "media") == 0,
               "audio response resolves its audioType by connection ID");
        expect(carplay_audio_bridge_resolve_response(
                   bridge, 101, setups[0].connection_id, resolved,
                   sizeof(resolved), &resolved_len) < 0,
               "audio response rejects a mismatched stream type");
    }
    expect(carplay_audio_seal_rtp_pcm(
               key, 100, &counters, pcm_little, sizeof(pcm_little),
               packet, sizeof(packet), &packet_len) == 0,
           "microphone RTP packet is sealed");
    expect(packet_len == 12 + sizeof(pcm_little) + 16 + 8 &&
               packet[1] == 100 && counters.sequence == 8 &&
               counters.nonce == 10,
           "sealed RTP has the wired CarPlay header and tail");
    expect(carplay_audio_decrypt_rtp(key, packet, packet_len, opened,
                                     sizeof(opened), &opened_len) == 0 &&
               opened_len == sizeof(pcm_little) &&
               opened[0] == 0x12 && opened[1] == 0x34 &&
               opened[2] == 0x56 && opened[3] == 0x78,
           "audio RTP round-trips and converts PCM to network byte order");
    packet[13] ^= 1;
    expect(carplay_audio_decrypt_rtp(key, packet, packet_len, opened,
                                     sizeof(opened), &opened_len) < 0,
           "audio RTP rejects a modified authentication tag");
    carplay_audio_bridge_destroy(&bridge);

    capabilities.audio.formats[0].output_formats = UINT32_C(0x00400000);
    expect(carplay_audio_bridge_init(&bridge, secret, 1, 0) == 0 &&
               carplay_audio_bridge_prepare(bridge, &capabilities) == 0 &&
               carplay_audio_bridge_setup_count(bridge) == 0,
           "compressed-only audio is reported unavailable");
    carplay_audio_bridge_destroy(&bridge);
}

static void test_capabilities_select_audio_formats(void)
{
    struct carplay_session_capabilities capabilities;
    struct carplay_audio_format_capabilities *media;
    struct carplay_audio_format_capabilities *prompt;

    memset(&capabilities, 0, sizeof(capabilities));
    capabilities.audio.description_present = 1;
    capabilities.audio.media_output = 1;
    capabilities.audio.microphone_input = 1;
    capabilities.audio.format_count = 2;
    media = &capabilities.audio.formats[0];
    media->present = 1;
    media->type = 100;
    strcpy(media->audio_type, "media");
    media->output_formats = UINT32_C(0x00000400) |
                            UINT32_C(0x00000800) |
                            UINT32_C(0x00004000);
    media->input_formats = UINT32_C(0x00000010) |
                           UINT32_C(0x00000020) |
                           UINT32_C(0x00000004);
    prompt = &capabilities.audio.formats[1];
    prompt->present = 1;
    prompt->type = 101;
    strcpy(prompt->audio_type, "default");
    prompt->output_formats = UINT32_C(0x00000008);

    expect(carplay_capabilities_select_audio_output(
               &capabilities, 100, "media") == UINT32_C(0x00000800),
           "audio output prefers 44.1 kHz stereo LPCM");
    expect(carplay_capabilities_select_audio_input(
               &capabilities, 100, "media") == UINT32_C(0x00000010),
           "car microphone input prefers 16 kHz mono LPCM");
    expect(carplay_capabilities_select_audio_output(
               &capabilities, 101, "default") == UINT32_C(0x00000008),
           "notification audio selects a prompt-sized LPCM format");
    expect(carplay_capabilities_select_audio_output(
               &capabilities, 100, "missing") == 0,
           "missing audioType is reported unavailable");
    expect(carplay_capabilities_select_audio_output(
               &capabilities, 999, "media") == 0,
           "missing stream type is reported unavailable");
    expect(carplay_capabilities_select_audio_output(
               NULL, 100, "media") == 0,
           "audio selection handles an absent capability snapshot");
}

static void test_audio_bridge_sends_captured_media_to_receiver(void)
{
    struct carplay_session_capabilities capabilities;
    struct carplay_audio_format_capabilities *media;
    struct carplay_audio_bridge *bridge = NULL;
    struct airplay_stream_setup setups[CARPLAY_AUDIO_MAX_STREAMS];
    struct sockaddr_in address;
    socklen_t address_len = sizeof(address);
    struct pollfd pfd;
    uint8_t secret[32];
    uint8_t output_key[AIRPLAY_CHACHA_KEY_BYTES];
    uint8_t pcm[] = { 0x34, 0x12, 0x78, 0x56 };
    uint8_t capture_frame[3840] = { 0 };
    uint8_t packet[4096];
    uint8_t opened[4000];
    struct sockaddr_un capture_address;
    char salt[96];
    size_t opened_len = 0;
    int salt_len;
    int receiver_fd;
    int capture_fd;
    int capture_ready;
    ssize_t received;

    memset(secret, 0x42, sizeof(secret));
    memset(&capabilities, 0, sizeof(capabilities));
    capabilities.audio.description_present = 1;
    capabilities.audio.media_output = 1;
    capabilities.audio.format_count = 1;
    media = &capabilities.audio.formats[0];
    media->present = 1;
    media->type = 100;
    strcpy(media->audio_type, "media");
    media->output_formats = UINT32_C(0x00008000);

    receiver_fd = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    expect(receiver_fd >= 0, "audio receiver test socket is created");
    memset(&address, 0, sizeof(address));
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    address.sin_port = 0;
    expect(bind(receiver_fd, (const struct sockaddr *)&address,
                sizeof(address)) == 0,
           "audio receiver test socket binds loopback");
    expect(getsockname(receiver_fd, (struct sockaddr *)&address,
                       &address_len) == 0,
           "audio receiver test socket has a port");

    expect(airplay_crypto_init() == 0, "capture uplink crypto initializes");
    expect(carplay_audio_bridge_init(&bridge, secret, 1, 0) == 0,
           "capture uplink bridge initializes");
    expect(carplay_audio_bridge_prepare(bridge, &capabilities) == 0 &&
               carplay_audio_bridge_fill_setups(
                   bridge, setups, CARPLAY_AUDIO_MAX_STREAMS) == 0,
           "capture uplink bridge prepares media");
    expect(carplay_audio_bridge_start_stream(
               bridge, 100, "media", ntohs(address.sin_port), 0,
               "127.0.0.1") == 0,
           "capture uplink connects to the receiver data port");
    expect(carplay_audio_bridge_submit_capture_pcm(
               bridge, pcm, sizeof(pcm)) == 0,
           "captured media PCM is submitted");

    pfd.fd = receiver_fd;
    pfd.events = POLLIN;
    pfd.revents = 0;
    expect(poll(&pfd, 1, 1000) == 1 && (pfd.revents & POLLIN),
           "receiver receives a captured audio RTP packet");
    received = recv(receiver_fd, packet, sizeof(packet), 0);
    expect(received > 0, "captured audio RTP packet is readable");
    salt_len = snprintf(salt, sizeof(salt), "DataStream-Salt%llu",
                        (unsigned long long)setups[0].connection_id);
    expect(salt_len > 0 && (size_t)salt_len < sizeof(salt) &&
               airplay_hkdf_sha512(
                   secret, sizeof(secret), (const uint8_t *)salt,
                   (size_t)salt_len,
                   (const uint8_t *)"DataStream-Output-Encryption-Key",
                   strlen("DataStream-Output-Encryption-Key"),
                   output_key, sizeof(output_key)) == 0,
           "capture uplink output key derives from the stream connection ID");
    expect(received > 0 &&
               carplay_audio_decrypt_rtp(
                   output_key, packet, (size_t)received, opened,
                   sizeof(opened), &opened_len) == 0 &&
               opened_len == sizeof(pcm) &&
               opened[0] == 0x12 && opened[1] == 0x34 &&
               opened[2] == 0x56 && opened[3] == 0x78,
           "receiver decrypts captured PCM with the output key");

    capture_frame[0] = 0x78;
    capture_frame[1] = 0x56;
    capture_frame[3838] = 0x12;
    capture_frame[3839] = 0x34;
    capture_fd = socket(AF_UNIX, SOCK_STREAM, 0);
    memset(&capture_address, 0, sizeof(capture_address));
    capture_address.sun_family = AF_UNIX;
    capture_address.sun_path[0] = '\0';
    snprintf(capture_address.sun_path + 1,
             sizeof(capture_address.sun_path) - 1, "%s",
             CARPLAY_AUDIO_CAPTURE_SOCKET);
    expect(capture_fd >= 0 && connect(
               capture_fd, (const struct sockaddr *)&capture_address,
               offsetof(struct sockaddr_un, sun_path) + 1 +
                   strlen(CARPLAY_AUDIO_CAPTURE_SOCKET)) == 0,
           "audio bridge exposes the Android capture socket");
    expect(send(capture_fd, capture_frame, sizeof(capture_frame),
                MSG_NOSIGNAL) ==
               (ssize_t)sizeof(capture_frame),
           "Android capture frame is written to the bridge");
    pfd.fd = receiver_fd;
    pfd.events = POLLIN;
    pfd.revents = 0;
    capture_ready = poll(&pfd, 1, 1000);
    expect(capture_ready == 1 && (pfd.revents & POLLIN),
           "receiver receives audio from the Android capture socket");
    received = capture_ready == 1 ?
        recv(receiver_fd, packet, sizeof(packet), 0) : -1;
    expect(received > 0 &&
               carplay_audio_decrypt_rtp(
                   output_key, packet, (size_t)received, opened,
                   sizeof(opened), &opened_len) == 0 &&
               opened_len == sizeof(capture_frame) &&
               opened[0] == 0x56 && opened[1] == 0x78 &&
               opened[3838] == 0x34 && opened[3839] == 0x12,
           "capture socket frame reaches the receiver with PCM byte order");
    if (capture_fd >= 0)
        close(capture_fd);
    carplay_audio_bridge_destroy(&bridge);
    close(receiver_fd);
}

static void test_audio_bridge_uses_all_receiver_streams(void)
{
    struct carplay_session_capabilities capabilities;
    struct carplay_audio_bridge *bridge = NULL;
    uint8_t secret[32];
    static const struct {
        int type;
        const char *audio_type;
        uint32_t format;
    } entries[] = {
        { 100, "compatibility", UINT32_C(0x0000c3fc) },
        { 101, "default", UINT32_C(0x7000c3fc) },
        { 102, "speechRecognition", UINT32_C(0x00800020) },
        { 100, "telephony", UINT32_C(0x70004154) },
        { 100, "media", UINT32_C(0x7000c3fc) }
    };
    size_t i;

    memset(secret, 0x42, sizeof(secret));
    memset(&capabilities, 0, sizeof(capabilities));
    capabilities.audio.description_present = 1;
    capabilities.audio.media_output = 1;
    capabilities.audio.format_count = sizeof(entries) / sizeof(entries[0]);
    for (i = 0; i < capabilities.audio.format_count; i++) {
        capabilities.audio.formats[i].present = 1;
        capabilities.audio.formats[i].type = entries[i].type;
        strcpy(capabilities.audio.formats[i].audio_type,
               entries[i].audio_type);
        capabilities.audio.formats[i].output_formats = entries[i].format;
    }
    {
        struct airplay_stream_setup setups[CARPLAY_AUDIO_MAX_STREAMS];
        expect(carplay_audio_bridge_init(&bridge, secret, 1, 0) == 0 &&
                   carplay_audio_bridge_prepare(bridge, &capabilities) == 0 &&
                   carplay_audio_bridge_setup_count(bridge) == 5 &&
                   carplay_audio_bridge_fill_setups(
                       bridge, setups, CARPLAY_AUDIO_MAX_STREAMS) == 0 &&
                   setups[0].audio_format == UINT32_C(0x00008000) &&
                   setups[1].audio_format == UINT32_C(0x00000020) &&
                   setups[2].audio_format == UINT32_C(0x00000020) &&
                   setups[3].audio_format == UINT32_C(0x00004000) &&
                   setups[4].audio_format == UINT32_C(0x00008000),
               "combined receiver masks select one usable LPCM format per stream");
    }
    carplay_audio_bridge_destroy(&bridge);

    {
        struct carplay_audio_pcm_format parsed;
        expect(carplay_audio_lpcm_format(
                   UINT32_C(0x0000c3fc), &parsed) == 0 &&
                   parsed.sample_rate == 48000 && parsed.channels == 2,
               "combined mask selects 48 kHz stereo LPCM");
    }
}

static void test_audio_capture_falls_back_to_default_stream(void)
{
    struct carplay_session_capabilities capabilities;
    struct carplay_audio_bridge *bridge = NULL;
    struct airplay_stream_setup setups[CARPLAY_AUDIO_MAX_STREAMS];
    struct sockaddr_in address;
    socklen_t address_len = sizeof(address);
    struct pollfd pfd;
    uint8_t secret[32];
    uint8_t output_key[AIRPLAY_CHACHA_KEY_BYTES];
    uint8_t pcm[3840] = { 0x34, 0x12, 0x78, 0x56 };
    uint8_t packet[4096];
    uint8_t opened[4096];
    size_t opened_len = 0;
    int receiver_fd;
    char salt[96];
    int salt_len;
    ssize_t received;

    memset(secret, 0x42, sizeof(secret));
    memset(&capabilities, 0, sizeof(capabilities));
    capabilities.audio.description_present = 1;
    capabilities.audio.media_output = 1;
    capabilities.audio.format_count = 3;
    capabilities.audio.formats[0].present = 1;
    capabilities.audio.formats[0].type = 100;
    strcpy(capabilities.audio.formats[0].audio_type, "compatibility");
    capabilities.audio.formats[0].output_formats = UINT32_C(0x00000020);
    capabilities.audio.formats[1].present = 1;
    capabilities.audio.formats[1].type = 101;
    strcpy(capabilities.audio.formats[1].audio_type, "default");
    capabilities.audio.formats[1].output_formats = UINT32_C(0x00008000);
    capabilities.audio.formats[2].present = 1;
    capabilities.audio.formats[2].type = 102;
    strcpy(capabilities.audio.formats[2].audio_type, "speechRecognition");
    capabilities.audio.formats[2].output_formats = UINT32_C(0x00000020);

    receiver_fd = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    memset(&address, 0, sizeof(address));
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    expect(receiver_fd >= 0 &&
               bind(receiver_fd, (const struct sockaddr *)&address,
                    sizeof(address)) == 0 &&
               getsockname(receiver_fd, (struct sockaddr *)&address,
                           &address_len) == 0,
           "default fallback receiver binds");
    expect(carplay_audio_bridge_init(&bridge, secret, 1, 0) == 0 &&
               carplay_audio_bridge_prepare(bridge, &capabilities) == 0 &&
               carplay_audio_bridge_fill_setups(
                   bridge, setups, CARPLAY_AUDIO_MAX_STREAMS) == 0 &&
               carplay_audio_bridge_start_stream(
                   bridge, 101, "default", ntohs(address.sin_port), 0,
                   "127.0.0.1") == 0,
           "default audio stream is active when media is absent");
    expect(carplay_audio_bridge_submit_capture_pcm(
               bridge, pcm, sizeof(pcm)) == 0,
           "capture accepts the default audio fallback");
    pfd.fd = receiver_fd;
    pfd.events = POLLIN;
    pfd.revents = 0;
    received = poll(&pfd, 1, 1000) == 1 ?
        recv(receiver_fd, packet, sizeof(packet), 0) : -1;
    salt_len = snprintf(salt, sizeof(salt), "DataStream-Salt%llu",
                        (unsigned long long)setups[1].connection_id);
    expect(received > 0 &&
               packet[1] == 101 &&
               salt_len > 0 && (size_t)salt_len < sizeof(salt) &&
               airplay_hkdf_sha512(
                   secret, sizeof(secret), (const uint8_t *)salt,
                   (size_t)salt_len,
                   (const uint8_t *)"DataStream-Output-Encryption-Key",
                   strlen("DataStream-Output-Encryption-Key"),
                   output_key, sizeof(output_key)) == 0 &&
               carplay_audio_decrypt_rtp(
                   output_key,
                   packet, (size_t)received, opened, sizeof(opened),
                   &opened_len) == 0,
           "default fallback carries sealed audio to the car");
    carplay_audio_bridge_destroy(&bridge);
    close(receiver_fd);
}

#ifdef CARPLAY_AUDIO_TESTING
static void test_audio_capture_retries_another_output_after_udp_failure(void)
{
    struct carplay_session_capabilities capabilities;
    struct carplay_audio_bridge *bridge = NULL;
    struct airplay_stream_setup setups[CARPLAY_AUDIO_MAX_STREAMS];
    struct sockaddr_in media_address;
    struct sockaddr_in fallback_address;
    socklen_t media_address_len = sizeof(media_address);
    socklen_t fallback_address_len = sizeof(fallback_address);
    struct pollfd pfd;
    uint8_t secret[32];
    uint8_t output_key[AIRPLAY_CHACHA_KEY_BYTES];
    uint8_t pcm[3840] = { 0x34, 0x12, 0x78, 0x56 };
    uint8_t packet[4096];
    uint8_t opened[4096];
    size_t opened_len = 0;
    int media_probe_fd;
    int fallback_fd;
    ssize_t received = -1;
    char salt[96];
    int salt_len;

    memset(secret, 0x42, sizeof(secret));
    memset(&capabilities, 0, sizeof(capabilities));
    capabilities.audio.description_present = 1;
    capabilities.audio.media_output = 1;
    capabilities.audio.format_count = 2;
    capabilities.audio.formats[0].present = 1;
    capabilities.audio.formats[0].type = 100;
    strcpy(capabilities.audio.formats[0].audio_type, "media");
    capabilities.audio.formats[0].output_formats = UINT32_C(0x00008000);
    capabilities.audio.formats[1].present = 1;
    capabilities.audio.formats[1].type = 101;
    strcpy(capabilities.audio.formats[1].audio_type, "default");
    capabilities.audio.formats[1].output_formats = UINT32_C(0x00008000);

    /* Reserve then release a loopback UDP port so media sends fail at runtime. */
    media_probe_fd = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    memset(&media_address, 0, sizeof(media_address));
    media_address.sin_family = AF_INET;
    media_address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    expect(media_probe_fd >= 0 &&
               bind(media_probe_fd,
                    (const struct sockaddr *)&media_address,
                    sizeof(media_address)) == 0 &&
               getsockname(media_probe_fd,
                           (struct sockaddr *)&media_address,
                           &media_address_len) == 0,
           "failed media receiver test port is reserved");
    close(media_probe_fd);

    fallback_fd = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    memset(&fallback_address, 0, sizeof(fallback_address));
    fallback_address.sin_family = AF_INET;
    fallback_address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    expect(fallback_fd >= 0 &&
               bind(fallback_fd,
                    (const struct sockaddr *)&fallback_address,
                    sizeof(fallback_address)) == 0 &&
               getsockname(fallback_fd,
                           (struct sockaddr *)&fallback_address,
                           &fallback_address_len) == 0,
           "default fallback receiver test port is reserved");

    expect(airplay_crypto_init() == 0 &&
               carplay_audio_bridge_init(&bridge, secret, 1, 0) == 0 &&
               carplay_audio_bridge_prepare(bridge, &capabilities) == 0 &&
               carplay_audio_bridge_fill_setups(
                   bridge, setups, CARPLAY_AUDIO_MAX_STREAMS) == 0 &&
               carplay_audio_bridge_start_stream(
                   bridge, 100, "media", ntohs(media_address.sin_port), 0,
                   "127.0.0.1") == 0 &&
               carplay_audio_bridge_start_stream(
                   bridge, 101, "default",
                   ntohs(fallback_address.sin_port), 0,
                   "127.0.0.1") == 0,
           "media and default UDP outputs are both negotiated");

    expect(carplay_audio_bridge_test_force_send_failure(
               bridge, 100, "media") == 0,
           "test can force the next preferred media send to fail");
    pfd.fd = fallback_fd;
    pfd.events = POLLIN;
    pfd.revents = 0;
    expect(carplay_audio_bridge_submit_capture_pcm(
               bridge, pcm, sizeof(pcm)) == 0,
           "capture is rerouted in the same submit after preferred send failure");
    if (poll(&pfd, 1, 1000) == 1 && (pfd.revents & POLLIN))
        received = recv(fallback_fd, packet, sizeof(packet), 0);
    salt_len = snprintf(salt, sizeof(salt), "DataStream-Salt%llu",
                        (unsigned long long)setups[1].connection_id);
    expect(received > 0 && packet[1] == 101 &&
               salt_len > 0 && (size_t)salt_len < sizeof(salt) &&
               airplay_hkdf_sha512(
                   secret, sizeof(secret), (const uint8_t *)salt,
                   (size_t)salt_len,
                   (const uint8_t *)"DataStream-Output-Encryption-Key",
                   strlen("DataStream-Output-Encryption-Key"),
                   output_key, sizeof(output_key)) == 0 &&
               carplay_audio_decrypt_rtp(
                   output_key, packet, (size_t)received,
                   opened, sizeof(opened), &opened_len) == 0 &&
               opened_len == sizeof(pcm) &&
               opened[0] == 0x12 && opened[1] == 0x34,
           "capture falls back from a failed media socket to default output");

    carplay_audio_bridge_destroy(&bridge);
    close(fallback_fd);
}
#endif

static void test_audio_capture_matches_receiver_rate_and_channels(void)
{
    struct carplay_session_capabilities capabilities;
    struct carplay_audio_bridge *bridge = NULL;
    struct airplay_stream_setup setups[CARPLAY_AUDIO_MAX_STREAMS];
    struct sockaddr_in address;
    socklen_t address_len = sizeof(address);
    struct pollfd pfd;
    uint8_t secret[32];
    uint8_t output_key[AIRPLAY_CHACHA_KEY_BYTES];
    uint8_t capture_frame[3840] = { 0 };
    uint8_t packet[4096];
    uint8_t opened[4096];
    char salt[96];
    size_t opened_len = 0;
    int salt_len;
    int receiver_fd;
    ssize_t received;

    memset(secret, 0x42, sizeof(secret));
    memset(&capabilities, 0, sizeof(capabilities));
    capabilities.audio.description_present = 1;
    capabilities.audio.media_output = 1;
    capabilities.audio.format_count = 1;
    capabilities.audio.formats[0].present = 1;
    capabilities.audio.formats[0].type = 100;
    strcpy(capabilities.audio.formats[0].audio_type, "media");
    capabilities.audio.formats[0].output_formats = UINT32_C(0x00000010);

    receiver_fd = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    memset(&address, 0, sizeof(address));
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    expect(receiver_fd >= 0 &&
               bind(receiver_fd, (const struct sockaddr *)&address,
                    sizeof(address)) == 0 &&
               getsockname(receiver_fd, (struct sockaddr *)&address,
                           &address_len) == 0,
           "rate adaptation receiver binds");
    expect(carplay_audio_bridge_init(&bridge, secret, 1, 0) == 0 &&
               carplay_audio_bridge_prepare(bridge, &capabilities) == 0 &&
               carplay_audio_bridge_fill_setups(
                   bridge, setups, CARPLAY_AUDIO_MAX_STREAMS) == 0 &&
               carplay_audio_bridge_start_stream(
                   bridge, 100, "media", ntohs(address.sin_port), 0,
                   "127.0.0.1") == 0 &&
               carplay_audio_bridge_submit_capture_pcm(
                   bridge, capture_frame, sizeof(capture_frame)) == 0,
           "bridge accepts a 48 kHz stereo capture frame");
    pfd.fd = receiver_fd;
    pfd.events = POLLIN;
    pfd.revents = 0;
    received = poll(&pfd, 1, 1000) == 1 ?
        recv(receiver_fd, packet, sizeof(packet), 0) : -1;
    salt_len = snprintf(salt, sizeof(salt), "DataStream-Salt%llu",
                        (unsigned long long)setups[0].connection_id);
    expect(received > 0 && salt_len > 0 &&
               (size_t)salt_len < sizeof(salt) &&
               airplay_hkdf_sha512(
                   secret, sizeof(secret), (const uint8_t *)salt,
                   (size_t)salt_len,
                   (const uint8_t *)"DataStream-Output-Encryption-Key",
                   strlen("DataStream-Output-Encryption-Key"),
                   output_key, sizeof(output_key)) == 0 &&
               carplay_audio_decrypt_rtp(
                   output_key, packet, (size_t)received, opened,
                   sizeof(opened), &opened_len) == 0,
           "rate adapted packet decrypts");
    expect(opened_len == 640,
           "20 ms stereo 48 kHz capture becomes mono 16 kHz LPCM");
    carplay_audio_bridge_destroy(&bridge);
    close(receiver_fd);
}

#ifdef CARPLAY_AUDIO_TESTING
static void test_audio_bridge_accepts_bracketed_ipv6_endpoint(void)
{
    struct carplay_session_capabilities capabilities;
    struct carplay_audio_bridge *bridge = NULL;
    struct airplay_stream_setup setups[CARPLAY_AUDIO_MAX_STREAMS];
    struct sockaddr_in6 address;
    socklen_t address_len = sizeof(address);
    struct pollfd pfd;
    uint8_t secret[32];
    uint8_t pcm[640] = { 0 };
    uint8_t packet[4096];
    int receiver_fd;
    int received;

    memset(secret, 0x42, sizeof(secret));
    memset(&capabilities, 0, sizeof(capabilities));
    capabilities.audio.description_present = 1;
    capabilities.audio.media_output = 1;
    capabilities.audio.format_count = 1;
    capabilities.audio.formats[0].present = 1;
    capabilities.audio.formats[0].type = 100;
    strcpy(capabilities.audio.formats[0].audio_type, "media");
    capabilities.audio.formats[0].output_formats = UINT32_C(0x00000010);

    receiver_fd = socket(AF_INET6, SOCK_DGRAM, IPPROTO_UDP);
    memset(&address, 0, sizeof(address));
    address.sin6_family = AF_INET6;
    address.sin6_addr = in6addr_loopback;
    expect(receiver_fd >= 0 &&
               bind(receiver_fd, (const struct sockaddr *)&address,
                    sizeof(address)) == 0 &&
               getsockname(receiver_fd, (struct sockaddr *)&address,
                           &address_len) == 0,
           "bracketed IPv6 audio receiver binds");
    expect(carplay_audio_bridge_init(&bridge, secret, 1, 0) == 0 &&
               carplay_audio_bridge_prepare(bridge, &capabilities) == 0 &&
               carplay_audio_bridge_fill_setups(
                   bridge, setups, CARPLAY_AUDIO_MAX_STREAMS) == 0 &&
               carplay_audio_bridge_start_stream(
                   bridge, 100, "media", ntohs(address.sin6_port), 0,
                   "[::1]") == 0,
           "bracketed IPv6 audio endpoint is accepted");
    expect(carplay_audio_bridge_submit_capture_pcm(
               bridge, pcm, sizeof(pcm)) == 0,
           "bracketed IPv6 audio stream accepts capture");
    pfd.fd = receiver_fd;
    pfd.events = POLLIN;
    pfd.revents = 0;
    received = poll(&pfd, 1, 1000) == 1 ?
        recv(receiver_fd, packet, sizeof(packet), 0) : -1;
    expect(received > 0, "bracketed IPv6 audio stream sends an RTP packet");
    carplay_audio_bridge_destroy(&bridge);
    close(receiver_fd);
}
#endif
static void test_capabilities_reject_invalid_display(void)
{
    struct carplay_session_capabilities capabilities;

    expect(carplay_capabilities_parse(
               airplay_info_invalid_display,
               airplay_info_invalid_display_len,
               &capabilities) == 0,
           "description with an invalid display remains readable");
    expect(capabilities.display.present == 0,
           "zero display dimensions are not accepted as negotiated capability");
    expect(capabilities.display.width_pixels == 0 &&
               capabilities.display.height_pixels == 0,
           "invalid dimensions do not inherit a hardcoded fallback");
}

static void test_capabilities_reject_non_plist(void)
{
    static const uint8_t malformed[] = { 0x62, 0x70, 0x6c, 0x69, 0x73, 0x74, 0x30, 0x30 };
    struct carplay_session_capabilities capabilities;

    expect(carplay_capabilities_parse(malformed, sizeof(malformed),
                                      &capabilities) != 0,
           "truncated capability description is rejected");
}


static void test_input_decodes_keyboard_press_and_release(void)
{
    static const uint8_t released[8] = { 0 };
    static const uint8_t shift_a[8] = { 0x02, 0, 0x04, 0, 0, 0, 0, 0 };
    static const uint8_t enter[8] = { 0, 0, 0x28, 0, 0, 0, 0, 0 };
    static const uint8_t malformed[7] = { 0 };
    struct airplay_input_keyboard_change changes[8];
    int count;

    count=airplay_input_keyboard_changed(released,sizeof(released),shift_a,
                                           sizeof(shift_a),changes,8);
    expect(count == 1 && changes[0].character == 'A' &&
               changes[0].keycode == 0,
           "standard keyboard Shift+A report produces uppercase text");
    count=airplay_input_keyboard_changed(shift_a,sizeof(shift_a),shift_a,
                                           sizeof(shift_a),changes,8);
    expect(count == 0,
           "holding a keyboard key does not repeat it in the injector");
    count=airplay_input_keyboard_changed(released,sizeof(released),enter,
                                           sizeof(enter),changes,8);
    expect(count == 1 && changes[0].keycode == 66,
           "keyboard enter maps to Android KEYCODE_ENTER");
    count=airplay_input_keyboard_changed(released,sizeof(released),malformed,
                                           sizeof(malformed),changes,8);
    expect(count == -1,
           "short keyboard HID report is rejected");
}


static void test_capabilities_select_even_output_size(void)
{
    struct carplay_session_capabilities capabilities;
    unsigned width = 0;
    unsigned height = 0;

    expect(carplay_capabilities_parse(
               airplay_info_audi_no_touch,
               airplay_info_audi_no_touch_len,
               &capabilities) == 0,
           "Audi capabilities remain available for size selection");
    expect(carplay_capabilities_output_size(&capabilities, 1920, 1080,
                                            &width, &height) == 1 &&
               width == 800 && height == 480,
           "negotiated car dimensions win over the local fallback");
    capabilities.display.present = 0;
    expect(carplay_capabilities_output_size(&capabilities, 123, 79,
                                            &width, &height) == 0 &&
               width == 122 && height == 78,
           "explicit local fallback is aligned to even pixels");
}

static void test_event_parser_waits_for_complete_fragmented_header(void)
{
    static const uint8_t request[] =
        "POST /command RTSP/1.0\r\n"
        "Content-Length: 0\r\n"
        "CSeq: 19\r\n"
        "\r\n";
    struct airplay_event_request parsed;
    char error[128] = { 0 };
    size_t consumed = 0;

    expect(airplay_event_parse_request(
               request, 11, &parsed, &consumed, error, sizeof(error)) ==
               AIRPLAY_EVENT_PARSE_INCOMPLETE,
           "fragmented event header waits for completion");
    expect(airplay_event_parse_request(
               request, sizeof(request) - 2, &parsed, &consumed,
               error, sizeof(error)) == AIRPLAY_EVENT_PARSE_INCOMPLETE,
           "event header split inside CRLF remains incomplete");
    expect(airplay_event_parse_request(
               request, sizeof(request) - 1, &parsed, &consumed,
               error, sizeof(error)) == AIRPLAY_EVENT_PARSE_COMPLETE,
           "completed event header is parsed after fragmentation");
    expect(parsed.cseq_present && parsed.cseq == 19,
           "fragmented header preserves CSeq");
}

static void test_event_parser_consumes_plist_body_without_losing_suffix(void)
{
    static const uint8_t request[] =
        "POST /command RTSP/1.0\r\n"
        "Content-Length: 4\r\n"
        "CSeq: 23\r\n"
        "\r\n"
        "ABCD"
        "next";
    struct airplay_event_request parsed;
    char error[128] = { 0 };
    size_t consumed = 0;

    expect(airplay_event_parse_request(
               request, sizeof(request) - 1, &parsed, &consumed,
               error, sizeof(error)) == AIRPLAY_EVENT_PARSE_COMPLETE,
           "event with a plist body is complete");
    expect(parsed.content_length_present && parsed.content_length == 4,
           "event body length is parsed");
    expect(consumed == sizeof("POST /command RTSP/1.0\r\n"
                              "Content-Length: 4\r\n"
                              "CSeq: 23\r\n"
                              "\r\n"
                              "ABCD") - 1,
           "event consumes only its own request");
    expect(memcmp(request + parsed.body_offset, "ABCD", 4) == 0,
           "event body starts immediately after its headers");
}

static void test_event_parser_reports_invalid_content_length(void)
{
    static const uint8_t request[] =
        "POST /command RTSP/1.0\r\n"
        "Content-Length: nope\r\n"
        "CSeq: 31\r\n"
        "\r\n";
    struct airplay_event_request parsed;
    char error[128] = { 0 };
    size_t consumed = 0;

    expect(airplay_event_parse_request(
               request, sizeof(request) - 1, &parsed, &consumed,
               error, sizeof(error)) == AIRPLAY_EVENT_PARSE_INVALID,
           "non-numeric Content-Length is rejected");
    expect(strstr(error, "Content-Length") != NULL,
           "invalid Content-Length carries a specific diagnostic");
}
int main(void)
{
    test_event_parser_waits_for_complete_fragmented_header();
    test_event_parser_consumes_plist_body_without_losing_suffix();
    test_event_parser_reports_invalid_content_length();
    test_event_request_without_content_length_is_bodyless();
    test_state_four_accepts_omitted_proof();
    test_state_four_verifies_present_proof();
    test_state_four_rejects_wrong_state_and_truncation();
    test_tlv_append_and_read();
    test_tlv_read_joins_fragments();
    test_tlv_read_rejects_truncation();
    test_bplist_reads_setup_response();
    test_bplist_rejects_invalid_buffers();
    test_bplist_encodes_setup_request();
    test_bplist_setup_encryption_type();
    test_bplist_encodes_screen_stream_setup();
    test_bplist_encodes_audio_stream_setup();
    test_bplist_encodes_all_negotiated_audio_streams();
    test_bplist_encodes_car_ui_request();
    test_capabilities_negotiates_audi_display_and_hid();
    test_capabilities_remote_ui_fields();
    test_capabilities_negotiates_touch_ranges();
    test_audio_bridge_prepares_lpcm_and_round_trips_rtp();
    test_audio_bridge_sends_captured_media_to_receiver();
    test_audio_bridge_uses_all_receiver_streams();
    test_audio_capture_falls_back_to_default_stream();
#ifdef CARPLAY_AUDIO_TESTING
    test_audio_bridge_accepts_bracketed_ipv6_endpoint();
#endif
#ifdef CARPLAY_AUDIO_TESTING
    test_audio_capture_retries_another_output_after_udp_failure();
#endif
    test_audio_capture_matches_receiver_rate_and_channels();
    test_capabilities_select_audio_formats();
    test_capabilities_reject_invalid_display();
    test_capabilities_reject_non_plist();
    test_capabilities_select_even_output_size();
    test_h264_parser_emits_parameter_sets_and_frames();
    test_input_parses_hid_touch_report();
    test_input_decodes_touch_point_and_media_keys();
    test_input_decodes_keyboard_press_and_release();
#ifdef AIRPLAY_TEST_VIDEO
    test_screen_video_header_and_frame_round_trip();
    test_screen_h264_fixture_shape();
#endif
    if (failures) {
        fprintf(stderr, "%d test(s) failed\n", failures);
        return 1;
    }
    puts("airplay protocol tests passed");
    return 0;
}
