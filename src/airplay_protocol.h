#ifndef AIRPLAY_PROTOCOL_H
#define AIRPLAY_PROTOCOL_H

#include <stddef.h>
#include <stdint.h>

#include "airplay_crypto.h"

/*
 * Accepts a pair-setup M4 response. Some accessory implementations omit the
 * optional 64-byte server proof after verifying M1; when it is present, it
 * must match the proof calculated by the SRP client.
 */
int airplay_tlv_append(uint8_t *buffer, size_t capacity, size_t *used,
                       uint8_t type, const uint8_t *value, size_t value_len);

int airplay_tlv_get(const uint8_t *buffer, size_t len, uint8_t type,
                    uint8_t *out, size_t out_capacity, size_t *out_len);

int airplay_pair_setup_check_state_four(
    const uint8_t *body, size_t len,
    const uint8_t expected_server_proof[AIRPLAY_SHA512_BYTES],
    int *proof_present);

#define AIRPLAY_EVENT_METHOD_CAPACITY 32
#define AIRPLAY_EVENT_PATH_CAPACITY 128
#define AIRPLAY_EVENT_ERROR_CAPACITY 160

enum airplay_event_parse_result {
    AIRPLAY_EVENT_PARSE_INVALID = -1,
    AIRPLAY_EVENT_PARSE_INCOMPLETE = 0,
    AIRPLAY_EVENT_PARSE_COMPLETE = 1
};

struct airplay_event_request {
    char method[AIRPLAY_EVENT_METHOD_CAPACITY];
    char path[AIRPLAY_EVENT_PATH_CAPACITY];
    unsigned cseq;
    int cseq_present;
    size_t body_offset;
    size_t content_length;
    int content_length_present;
};

/* Parse one RTSP event request from a possibly fragmented byte stream. */
int airplay_event_parse_request(
    const uint8_t *data, size_t len,
    struct airplay_event_request *request, size_t *consumed,
    char *error, size_t error_capacity);
#endif