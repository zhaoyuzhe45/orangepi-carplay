#ifndef AIRPLAY_BPLIST_H
#define AIRPLAY_BPLIST_H

#include <stddef.h>
#include <stdint.h>

enum airplay_bplist_type {
    AIRPLAY_BPLIST_NULL = 0,
    AIRPLAY_BPLIST_BOOLEAN,
    AIRPLAY_BPLIST_INTEGER,
    AIRPLAY_BPLIST_STRING,
    AIRPLAY_BPLIST_DATA,
    AIRPLAY_BPLIST_ARRAY,
    AIRPLAY_BPLIST_DICTIONARY
};

struct airplay_bplist_document {
    const uint8_t *data;
    size_t len;
    size_t offset_table;
    uint64_t object_count;
    uint32_t top_object;
    uint8_t offset_size;
    uint8_t reference_size;
};

struct airplay_bplist_value {
    enum airplay_bplist_type type;
    uint32_t object_index;
    size_t data_offset;
    uint64_t count;
    uint8_t integer_size;
    uint8_t boolean_value;
    uint8_t string_unit_size; /* 1 ASCII byte, 2 UTF-16BE bytes per code unit. */
};

int airplay_bplist_init(struct airplay_bplist_document *document,
                        const uint8_t *data, size_t len);
int airplay_bplist_root(const struct airplay_bplist_document *document,
                        struct airplay_bplist_value *value);
int airplay_bplist_dict_get(const struct airplay_bplist_document *document,
                            const struct airplay_bplist_value *dictionary,
                            const char *key,
                            struct airplay_bplist_value *value);
int airplay_bplist_array_count(
    const struct airplay_bplist_document *document,
    const struct airplay_bplist_value *array, uint64_t *count);
int airplay_bplist_array_get(const struct airplay_bplist_document *document,
                             const struct airplay_bplist_value *array,
                             size_t index,
                             struct airplay_bplist_value *value);
int airplay_bplist_get_uint64(const struct airplay_bplist_document *document,
                              const struct airplay_bplist_value *value,
                              uint64_t *number);
int airplay_bplist_get_bool(const struct airplay_bplist_document *document,
                            const struct airplay_bplist_value *value,
                            int *boolean_value);
int airplay_bplist_string_equals(
    const struct airplay_bplist_document *document,
    const struct airplay_bplist_value *value, const char *expected);
int airplay_bplist_string_copy(
    const struct airplay_bplist_document *document,
    const struct airplay_bplist_value *value,
    char *output, size_t capacity, size_t *output_len);

/*
 * Encodes the uncompressed SETUP dictionary used by the controller side of
 * CarPlay. The returned body is an Apple binary property list (bplist00).
 *
 * `encrypted` is the state of the session, and it decides whether `et` is
 * advertised at all: 0 encodes et = 1 (kAirPlayEncryptionType_None) for an
 * unpaired/plaintext session, 1 omits the key because the streams are
 * ChaCha20-Poly1305 keyed from the pair-verify shared secret. Sending et = 1
 * on a paired session, or omitting it on an unpaired one, are the two ways to
 * lose the session -- see the comment in airplay_bplist_encode_setup().
 */
int airplay_bplist_encode_setup(uint8_t *output, size_t capacity,
                                size_t *output_len, uint16_t timing_port,
                                int keep_alive_low_power, int encrypted);
struct airplay_stream_setup {
    int type;
    int stream_id;
    uint64_t connection_id;
    const char *audio_type;
    uint32_t audio_format;
    uint16_t data_port;
    int input;
    /* Opt-in traditional audio fields; zero retains old HAP serialization. */
    int legacy_audio;
    uint16_t control_port;
};

/*
 * Encodes a multi-stream SETUP. Screen streams leave audio_type NULL. Audio
 * streams include audioType/audioFormat; input-enabled streams also advertise
 * the local dataPort supplied by the caller.
 */
int airplay_bplist_encode_stream_setup(
    uint8_t *output, size_t capacity, size_t *output_len,
    const struct airplay_stream_setup *streams, size_t stream_count);
int airplay_bplist_encode_screen_setup(uint8_t *output, size_t capacity,
                                       size_t *output_len,
                                       uint64_t stream_connection_id);

/* Legacy SETUP / screen stream: type 110, connection ID, latencyMs 25,
 * and the receiver's display UUID when supplied. */
int airplay_bplist_encode_legacy_screen_setup(uint8_t *output, size_t capacity,
                                              size_t *output_len,
                                              uint64_t stream_connection_id,
                                              const char *display_uuid);

/* Encodes {type: requestUI, params: {}} for enhancedRequestCarUI. */
int airplay_bplist_encode_car_ui_request(uint8_t *output, size_t capacity,
                                         size_t *output_len);

struct carplay_modes;

/* status=0: {status:0,params:{resources:[...],appStates:[...]}}.
 * Nonzero status: {status:1}. State may be NULL for a rejection.
 */
int airplay_bplist_encode_modes_response(uint8_t *output, size_t capacity,
                                         size_t *output_len,
                                         const struct carplay_modes *state,
                                         int status);
int airplay_bplist_encode_modes_changed(uint8_t *output, size_t capacity,
                                        size_t *output_len,
                                        const struct carplay_modes *state);

#endif
