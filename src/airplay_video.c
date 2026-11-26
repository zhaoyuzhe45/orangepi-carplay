#include "airplay_video.h"

#include <limits.h>
#include <string.h>

static void write_u32_le(uint8_t *output, uint32_t value)
{
    output[0] = (uint8_t)value;
    output[1] = (uint8_t)(value >> 8);
    output[2] = (uint8_t)(value >> 16);
    output[3] = (uint8_t)(value >> 24);
}

static void write_u64_le(uint8_t *output, uint64_t value)
{
    size_t i;

    for (i = 0; i < 8; i++)
        output[i] = (uint8_t)(value >> (i * 8));
}

static void write_float_le(uint8_t *output, float value)
{
    uint32_t bits;
    memcpy(&bits, &value, sizeof(bits));
    write_u32_le(output, bits);
}

static void write_header(uint8_t header[AIRPLAY_SCREEN_HEADER_BYTES],
                         uint32_t body_len, uint8_t opcode,
                         uint64_t sender_ntp)
{
    memset(header, 0, AIRPLAY_SCREEN_HEADER_BYTES);
    write_u32_le(header, body_len);
    header[4] = opcode;
    write_u64_le(header + 8, sender_ntp);
}

int airplay_screen_encode_config(uint8_t *output, size_t capacity,
                                 size_t *output_len,
                                 const uint8_t *config, size_t config_len,
                                 uint64_t sender_ntp)
{
    if (!output || !output_len || (!config && config_len) ||
        config_len > UINT32_MAX ||
        config_len > capacity ||
        AIRPLAY_SCREEN_HEADER_BYTES > capacity - config_len)
        return -1;
    write_header(output, (uint32_t)config_len,
                 AIRPLAY_SCREEN_OP_VIDEO_CONFIG, sender_ntp);
    if (config_len)
        memcpy(output + AIRPLAY_SCREEN_HEADER_BYTES, config, config_len);
    *output_len = AIRPLAY_SCREEN_HEADER_BYTES + config_len;
    return 0;
}

int airplay_screen_encode_frame(uint8_t *output, size_t capacity,
                                size_t *output_len,
                                const uint8_t key[AIRPLAY_CHACHA_KEY_BYTES],
                                uint64_t counter,
                                int keyframe,
                                const uint8_t *frame, size_t frame_len,
                                uint64_t sender_ntp)
{
    uint8_t nonce[AIRPLAY_CHACHA_NONCE_BYTES] = { 0 };
    size_t sealed_len = 0;
    size_t i;

    if (!output || !output_len || !key || (!frame && frame_len) ||
        frame_len > UINT32_MAX - AIRPLAY_CHACHA_TAG_BYTES ||
        frame_len > capacity ||
        AIRPLAY_SCREEN_HEADER_BYTES > capacity - frame_len ||
        AIRPLAY_CHACHA_TAG_BYTES >
            capacity - AIRPLAY_SCREEN_HEADER_BYTES - frame_len)
        return -1;
    write_header(output,
                 (uint32_t)(frame_len + AIRPLAY_CHACHA_TAG_BYTES),
                 AIRPLAY_SCREEN_OP_VIDEO_FRAME, sender_ntp);
    if (keyframe)
        output[5] = AIRPLAY_SCREEN_FLAG_KEYFRAME;
    for (i = 0; i < 8; i++)
        nonce[4 + i] = (uint8_t)(counter >> (i * 8));
    if (airplay_chacha20_poly1305_seal(
            key, nonce, frame, frame_len,
            output, AIRPLAY_SCREEN_HEADER_BYTES,
            output + AIRPLAY_SCREEN_HEADER_BYTES,
            capacity - AIRPLAY_SCREEN_HEADER_BYTES,
            &sealed_len) < 0 ||
        sealed_len != frame_len + AIRPLAY_CHACHA_TAG_BYTES)
        return -1;
    *output_len = AIRPLAY_SCREEN_HEADER_BYTES + sealed_len;
    return 0;
}

int airplay_screen_encode_legacy_config(uint8_t *output, size_t capacity,
    size_t *output_len, const uint8_t *config, size_t config_len,
    uint64_t sender_ntp, unsigned content_width, unsigned content_height,
    unsigned display_width, unsigned display_height)
{
    if (!content_width || !content_height || !display_width || !display_height ||
        airplay_screen_encode_config(output, capacity, output_len, config,
                                     config_len, sender_ntp) < 0)
        return -1;
    /* The receiver reads these three pairs independently. content/source are
     * the encoded picture geometry; display is the canvas negotiated by the
     * head unit. Collapsing them to the negotiated size makes the receiver
     * allocate one decoder surface and then receive a different SPS size. */
    output[6] = 0x14; /* encrypted | unused4 */
    output[7] = 1;
    write_float_le(output + 16, (float)content_width);
    write_float_le(output + 20, (float)content_height);
    write_float_le(output + 40, (float)content_width);
    write_float_le(output + 44, (float)content_height);
    write_float_le(output + 56, (float)display_width);
    write_float_le(output + 60, (float)display_height);
    return 0;
}

int airplay_screen_encode_legacy_frame(uint8_t *output, size_t capacity,
    size_t *output_len, struct airplay_legacy_cipher *cipher,
    int keyframe, const uint8_t *frame, size_t frame_len, uint64_t sender_ntp)
{
    if (!output || !output_len || !cipher || (!frame && frame_len) ||
        frame_len > UINT32_MAX || frame_len > capacity ||
        AIRPLAY_SCREEN_HEADER_BYTES > capacity - frame_len)
        return -1;
    write_header(output, (uint32_t)frame_len, AIRPLAY_SCREEN_OP_VIDEO_FRAME, sender_ntp);
    if (keyframe)
        output[5] = AIRPLAY_SCREEN_FLAG_KEYFRAME;
    if (airplay_legacy_crypt(cipher, frame, output + AIRPLAY_SCREEN_HEADER_BYTES, frame_len) < 0)
        return -1;
    *output_len = AIRPLAY_SCREEN_HEADER_BYTES + frame_len;
    return 0;
}

int airplay_screen_encode_keepalive(uint8_t *output, size_t capacity,
    size_t *output_len, int stats_as_body)
{
    /* Canonical bplist containing an empty dictionary, no stream cipher bytes. */
    static const uint8_t empty[42] = {
        'b','p','l','i','s','t','0','0',0xd0,8,
        0,0,0,0,0,0,1,1,0,0,0,0,0,0,0,1,
        0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,9
    };
    size_t len = AIRPLAY_SCREEN_HEADER_BYTES + (stats_as_body ? sizeof(empty) : 0);
    if (!output || !output_len || capacity < len)
        return -1;
    write_header(output, stats_as_body ? (uint32_t)sizeof(empty) : 0,
                 stats_as_body ? 5 : 2, 0);
    if (stats_as_body) {
        write_float_le(output + 124, (float)sizeof(empty));
        memcpy(output + AIRPLAY_SCREEN_HEADER_BYTES, empty, sizeof(empty));
    }
    *output_len = len;
    return 0;
}
