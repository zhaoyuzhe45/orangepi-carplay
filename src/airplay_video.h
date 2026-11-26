#ifndef AIRPLAY_VIDEO_H
#define AIRPLAY_VIDEO_H

#include <stddef.h>
#include <stdint.h>

#include "airplay_crypto.h"
#include "airplay_legacy.h"

int airplay_screen_encode_legacy_frame(uint8_t *output, size_t capacity,
    size_t *output_len, struct airplay_legacy_cipher *cipher,
    int keyframe, const uint8_t *frame, size_t frame_len, uint64_t sender_ntp);
int airplay_screen_encode_legacy_config(uint8_t *output, size_t capacity,
    size_t *output_len, const uint8_t *config, size_t config_len,
    uint64_t sender_ntp, unsigned content_width, unsigned content_height,
    unsigned display_width, unsigned display_height);
int airplay_screen_encode_keepalive(uint8_t *output, size_t capacity,
    size_t *output_len, int stats_as_body);

#define AIRPLAY_SCREEN_HEADER_BYTES 128u
#define AIRPLAY_SCREEN_OP_VIDEO_FRAME 0u
#define AIRPLAY_SCREEN_OP_VIDEO_CONFIG 1u
#define AIRPLAY_SCREEN_FLAG_KEYFRAME 0x10u

int airplay_screen_encode_config(uint8_t *output, size_t capacity,
                                 size_t *output_len,
                                 const uint8_t *config, size_t config_len,
                                 uint64_t sender_ntp);
int airplay_screen_encode_frame(uint8_t *output, size_t capacity,
                                size_t *output_len,
                                const uint8_t key[AIRPLAY_CHACHA_KEY_BYTES],
                                uint64_t counter,
                                int keyframe,
                                const uint8_t *frame, size_t frame_len,
                                uint64_t sender_ntp);

#endif
