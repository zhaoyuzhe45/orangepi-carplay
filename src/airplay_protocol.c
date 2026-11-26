#include "airplay_protocol.h"

#include <limits.h>
#include <stdio.h>
#include <string.h>

static int tlv_last_item(const uint8_t *buffer, size_t len,
                         uint8_t *type, uint8_t *item_len)
{
    size_t off = 0;

    if (!buffer || !type || !item_len)
        return -1;
    while (off + 2 <= len) {
        unsigned current_type = buffer[off++];
        unsigned current_len = buffer[off++];
        if (off + current_len > len)
            return -1;
        *type = (uint8_t)current_type;
        *item_len = (uint8_t)current_len;
        off += current_len;
    }
    return off == len ? 0 : -1;
}

int airplay_tlv_append(uint8_t *buffer, size_t capacity, size_t *used,
                       uint8_t type, const uint8_t *value, size_t value_len)
{
    size_t off;
    uint8_t previous_type = 0;
    uint8_t previous_len = 0;
    int have_previous;

    if (!buffer || !used || (!value && value_len) || *used > capacity)
        return -1;
    have_previous = tlv_last_item(buffer, *used, &previous_type,
                                  &previous_len) == 0 && *used != 0;
    off = 0;
    do {
        size_t chunk = value_len - off;
        size_t separator = 0;

        if (chunk > 255)
            chunk = 255;
        if (off == 0 && have_previous && previous_type == type &&
            previous_len != 255)
            separator = 2;
        if (*used + separator + 2 + chunk > capacity)
            return -1;
        if (separator) {
            buffer[(*used)++] = 0xff;
            buffer[(*used)++] = 0;
        }
        buffer[(*used)++] = type;
        buffer[(*used)++] = (uint8_t)chunk;
        if (chunk)
            memcpy(buffer + *used, value + off, chunk);
        *used += chunk;
        off += chunk;
    } while (off < value_len);
    return 0;
}

int airplay_tlv_get(const uint8_t *buffer, size_t len, uint8_t type,
                    uint8_t *out, size_t out_capacity, size_t *out_len)
{
    size_t off = 0;
    size_t result_len = 0;
    uint8_t previous_type = 0xff;
    uint8_t previous_len = 0;
    int found = 0;

    if (!buffer || !out || !out_len)
        return -1;
    while (off + 2 <= len) {
        unsigned current_type = buffer[off++];
        unsigned current_len = buffer[off++];

        if (off + current_len > len)
            return -1;
        if (current_type == type) {
            int continuation = found && previous_type == type &&
                               previous_len == 255;
            if (!continuation) {
                result_len = 0;
                found = 1;
            }
            if (result_len + current_len > out_capacity)
                return -1;
            if (current_len)
                memcpy(out + result_len, buffer + off, current_len);
            result_len += current_len;
        }
        previous_type = (uint8_t)current_type;
        previous_len = (uint8_t)current_len;
        off += current_len;
    }
    if (off != len || !found)
        return -1;
    *out_len = result_len;
    return 0;
}

int airplay_pair_setup_check_state_four(
    const uint8_t *body, size_t len,
    const uint8_t expected_server_proof[AIRPLAY_SHA512_BYTES],
    int *proof_present)
{
    const uint8_t *proof = NULL;
    size_t proof_len = 0;
    unsigned state = 0;
    size_t off = 0;

    if (!body || !expected_server_proof || !proof_present)
        return -1;
    *proof_present = 0;

    while (off + 2 <= len) {
        unsigned type = body[off++];
        unsigned item_len = body[off++];

        if (off + item_len > len)
            return -1;
        if (type == 0x06 && item_len)
            state = body[off];
        if (type == 0x04) {
            if (proof)
                return -1;
            proof = body + off;
            proof_len = item_len;
        }
        off += item_len;
    }
    if (off != len || state != 4)
        return -1;
    if (!proof)
        return 0;
    if (proof_len != AIRPLAY_SHA512_BYTES)
        return -1;
    *proof_present = 1;
    return memcmp(proof, expected_server_proof, AIRPLAY_SHA512_BYTES) == 0 ? 0 : -1;
}
static int event_ascii_equal(char left, char right)
{
    if (left >= 'A' && left <= 'Z')
        left = (char)(left - 'A' + 'a');
    if (right >= 'A' && right <= 'Z')
        right = (char)(right - 'A' + 'a');
    return left == right;
}

static int event_header_uint(
    const uint8_t *data, size_t begin, size_t end, const char *name,
    size_t *value)
{
    size_t name_len = strlen(name);
    size_t pos = begin;

    *value = 0;
    while (pos < end) {
        size_t line_end = pos;
        size_t colon;

        while (line_end + 1 < end &&
               !(data[line_end] == '\r' && data[line_end + 1] == '\n'))
            line_end++;
        if (line_end + 1 >= end)
            return -1;
        if (line_end == pos) {
            pos = line_end + 2;
            continue;
        }
        colon = pos;
        while (colon < line_end && data[colon] != ':')
            colon++;
        if (colon == line_end)
            return -1;
        if (colon - pos == name_len) {
            int match = 1;
            for (size_t i = 0; i < name_len; i++) {
                if (!event_ascii_equal((char)data[pos + i], name[i])) {
                    match = 0;
                    break;
                }
            }
            if (match) {
                size_t current = colon + 1;
                size_t parsed = 0;
                while (current < line_end &&
                       (data[current] == ' ' || data[current] == '\t'))
                    current++;
                if (current == line_end)
                    return -1;
                while (current < line_end &&
                       data[current] >= '0' && data[current] <= '9') {
                    size_t digit = (size_t)(data[current] - '0');
                    if (parsed > (SIZE_MAX - digit) / 10)
                        return -1;
                    parsed = parsed * 10 + digit;
                    current++;
                }
                while (current < line_end &&
                       (data[current] == ' ' || data[current] == '\t'))
                    current++;
                if (current != line_end)
                    return -1;
                *value = parsed;
                return 1;
            }
        }
        pos = line_end + 2;
    }
    return 0;
}

int airplay_event_parse_request(
    const uint8_t *data, size_t len,
    struct airplay_event_request *request, size_t *consumed,
    char *error, size_t error_capacity)
{
    size_t header_end = 0;
    size_t line_end = 0;
    size_t first_space;
    size_t second_space;
    size_t method_len;
    size_t path_len;
    size_t content_length = 0;
    size_t cseq = 0;
    int content_length_present;
    int cseq_present;
    int rc;

    if (error && error_capacity)
        error[0] = '\0';
    if (!data || !request || !consumed || (!error && error_capacity))
        return AIRPLAY_EVENT_PARSE_INVALID;
    memset(request, 0, sizeof(*request));
    *consumed = 0;

    for (size_t i = 0; i + 3 < len; i++) {
        if (data[i] == '\r' && data[i + 1] == '\n' &&
            data[i + 2] == '\r' && data[i + 3] == '\n') {
            header_end = i + 4;
            break;
        }
    }
    if (!header_end)
        return AIRPLAY_EVENT_PARSE_INCOMPLETE;

    while (line_end + 1 < header_end &&
           !(data[line_end] == '\r' && data[line_end + 1] == '\n'))
        line_end++;
    first_space = 0;
    while (first_space < line_end && data[first_space] != ' ')
        first_space++;
    second_space = first_space + 1;
    while (second_space < line_end && data[second_space] != ' ')
        second_space++;
    if (first_space == 0 || second_space >= line_end) {
        if (error && error_capacity)
            snprintf(error, error_capacity, "invalid event request line");
        return AIRPLAY_EVENT_PARSE_INVALID;
    }
    method_len = first_space;
    path_len = second_space - first_space - 1;
    if (method_len >= sizeof(request->method) ||
        path_len >= sizeof(request->path)) {
        if (error && error_capacity)
            snprintf(error, error_capacity, "event request target too long");
        return AIRPLAY_EVENT_PARSE_INVALID;
    }
    memcpy(request->method, data, method_len);
    request->method[method_len] = '\0';
    memcpy(request->path, data + first_space + 1, path_len);
    request->path[path_len] = '\0';

    rc = event_header_uint(data, line_end + 2, header_end,
                           "Content-Length", &content_length);
    content_length_present = rc == 1;
    if (rc < 0) {
        if (error && error_capacity)
            snprintf(error, error_capacity, "invalid Content-Length header");
        return AIRPLAY_EVENT_PARSE_INVALID;
    }
    rc = event_header_uint(data, line_end + 2, header_end, "CSeq", &cseq);
    cseq_present = rc == 1;
    if (rc < 0) {
        if (error && error_capacity)
            snprintf(error, error_capacity, "invalid CSeq header");
        return AIRPLAY_EVENT_PARSE_INVALID;
    }
    if (cseq_present && cseq > UINT_MAX) {
        if (error && error_capacity)
            snprintf(error, error_capacity, "CSeq value overflow");
        return AIRPLAY_EVENT_PARSE_INVALID;
    }
    if (content_length_present && content_length > UINT_MAX) {
        if (error && error_capacity)
            snprintf(error, error_capacity, "Content-Length value overflow");
        return AIRPLAY_EVENT_PARSE_INVALID;
    }    if (content_length > SIZE_MAX - header_end) {
        if (error && error_capacity)
            snprintf(error, error_capacity, "event body length overflow");
        return AIRPLAY_EVENT_PARSE_INVALID;
    }
    if (len < header_end + content_length)
        return AIRPLAY_EVENT_PARSE_INCOMPLETE;

    request->cseq = (unsigned)cseq;
    request->cseq_present = cseq_present;
    request->body_offset = header_end;
    request->content_length = content_length;
    request->content_length_present = content_length_present;
    *consumed = header_end + content_length;
    return AIRPLAY_EVENT_PARSE_COMPLETE;
}