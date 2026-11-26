#ifndef AIRPLAY_H264_H
#define AIRPLAY_H264_H

#include <stddef.h>
#include <stdint.h>

struct airplay_h264_parser;

typedef int (*airplay_h264_config_fn)(void *context,
                                      const uint8_t *config,
                                      size_t config_len);
typedef int (*airplay_h264_frame_fn)(void *context,
                                     const uint8_t *nal,
                                     size_t nal_len, int keyframe);

struct airplay_h264_parser {
    uint8_t *nal;
    size_t nal_capacity;
    size_t nal_len;
    uint8_t sps[256];
    size_t sps_len;
    uint8_t pps[256];
    size_t pps_len;
    unsigned trailing_zeroes;
    int in_nal;
    int parameters_dirty;
    /* When set, every stored SPS is rewritten so decoders present exactly
     * this display size out of the padded macroblock frame (panel-mirror
     * capture: the black bars the panel letterbox leaves never reach the
     * sink). Zero disables the rewrite. stream_crop_x/y shift the region's
     * origin: the composer renders the logical display scaled and centered
     * inside the panel, so the region the sink must see generally does NOT
     * start at the coded frame's top-left corner. */
    unsigned stream_crop_x;
    unsigned stream_crop_y;
    unsigned stream_crop_width;
    unsigned stream_crop_height;
    airplay_h264_config_fn on_config;
    airplay_h264_frame_fn on_frame;
    void *context;
};

void airplay_h264_parser_init(struct airplay_h264_parser *parser,
                              uint8_t *nal_buffer, size_t nal_capacity,
                              airplay_h264_config_fn on_config,
                              airplay_h264_frame_fn on_frame,
                              void *context);
/* Present the captured stream to the sink as display_width x display_height
 * regardless of the encoder's macroblock padding. Zero size disables it. */
void airplay_h264_parser_set_stream_crop(struct airplay_h264_parser *parser,
                                         unsigned display_width,
                                         unsigned display_height);
/* Same, with an explicit origin: the region starts at crop_x/crop_y inside
 * the coded frame instead of the top-left corner. */
void airplay_h264_parser_set_stream_crop_rect(struct airplay_h264_parser *parser,
                                              unsigned crop_x, unsigned crop_y,
                                              unsigned display_width,
                                              unsigned display_height);
/* Rewrite the frame_cropping fields of an SPS NAL in place so decoders show
 * display_width x display_height of the coded frame. Returns 0 when the NAL
 * was rewritten, 1 when it already codes exactly that display size, and -1 on
 * any parse or capacity failure (the caller then forwards the original NAL). */
int airplay_h264_sps_apply_crop(uint8_t *nal, size_t *nal_len, size_t capacity,
                                unsigned display_width, unsigned display_height);
/* Same, but the region starts at crop_x/crop_y inside the coded frame - the
 * composer renders the logical display scaled and centered in the panel, so
 * the letterbox bars may sit on any side. Offsets must be whole chroma
 * samples. */
int airplay_h264_sps_apply_crop_rect(uint8_t *nal, size_t *nal_len,
                                     size_t capacity,
                                     unsigned crop_x, unsigned crop_y,
                                     unsigned display_width,
                                     unsigned display_height);
int airplay_h264_feed(struct airplay_h264_parser *parser,
                      const uint8_t *data, size_t len);
int airplay_h264_flush(struct airplay_h264_parser *parser);
/* 1 when this VCL NAL begins a new picture, i.e. first_mb_in_slice == 0.
 *
 * The encoder is free to code one picture as several slices, and it does use
 * more slices for expensive pictures (scrolling a full app grid). A picture is
 * the unit the sink decodes, so the packetizer has to know where one ends: this
 * is that boundary. */
int airplay_h264_slice_starts_picture(const uint8_t *nal, size_t nal_len);

#endif
