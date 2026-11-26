#include "airplay_h264.h"

#include <string.h>

static int emit_config(struct airplay_h264_parser *parser)
{
    uint8_t config[4 + 2 + 256 + 1 + 2 + 256];
    size_t offset = 0;

    if (!parser->parameters_dirty || !parser->sps_len || !parser->pps_len)
        return 0;
    config[offset++] = 1;
    config[offset++] = parser->sps[1];
    config[offset++] = parser->sps[2];
    config[offset++] = parser->sps[3];
    config[offset++] = 0xff;
    config[offset++] = 0xe1;
    config[offset++] = (uint8_t)(parser->sps_len >> 8);
    config[offset++] = (uint8_t)parser->sps_len;
    memcpy(config + offset, parser->sps, parser->sps_len);
    offset += parser->sps_len;
    config[offset++] = 1;
    config[offset++] = (uint8_t)(parser->pps_len >> 8);
    config[offset++] = (uint8_t)parser->pps_len;
    memcpy(config + offset, parser->pps, parser->pps_len);
    offset += parser->pps_len;
    if (!parser->on_config)
        return 0;
    if (parser->on_config(parser->context, config, offset) < 0)
        return -1;
    parser->parameters_dirty = 0;
    return 0;
}

static int process_nal(struct airplay_h264_parser *parser)
{
    unsigned type;

    if (!parser->nal_len)
        return 0;
    type = parser->nal[0] & 0x1f;
    if (type == 7) {
        const uint8_t *stored = parser->nal;
        size_t stored_len = parser->nal_len;
        uint8_t rewritten[512];
        size_t rewritten_len = parser->nal_len;

        /* Panel-mirror capture codes the padded macroblock frame; the sink
         * must see the canvas-aspect region, so the SPS the parser stores -
         * and everything downstream of it (avcC config record, keyframe
         * prologue) - carries the frame_cropping rewrite. A parse failure
         * forwards the original NAL: a wrong-aspect stream degrades the
         * picture but keeps the session alive. */
        if (parser->stream_crop_width && parser->stream_crop_height &&
            parser->nal_len <= sizeof(rewritten)) {
            memcpy(rewritten, parser->nal, parser->nal_len);
            if (airplay_h264_sps_apply_crop_rect(rewritten, &rewritten_len,
                                                 sizeof(rewritten),
                                                 parser->stream_crop_x,
                                                 parser->stream_crop_y,
                                                 parser->stream_crop_width,
                                                 parser->stream_crop_height) == 0) {
                stored = rewritten;
                stored_len = rewritten_len;
            }
        }
        if (stored_len > sizeof(parser->sps))
            return -1;
        memcpy(parser->sps, stored, stored_len);
        parser->sps_len = stored_len;
        parser->parameters_dirty = 1;
        return 0;
    }
    if (type == 8) {
        if (parser->nal_len > sizeof(parser->pps))
            return -1;
        memcpy(parser->pps, parser->nal, parser->nal_len);
        parser->pps_len = parser->nal_len;
        parser->parameters_dirty = 1;
        return emit_config(parser);
    }
    if ((type == 1 || type == 5) && parser->on_frame &&
        parser->on_frame(parser->context, parser->nal, parser->nal_len,
                         type == 5) < 0)
        return -1;
    return 0;
}

void airplay_h264_parser_init(struct airplay_h264_parser *parser,
                              uint8_t *nal_buffer, size_t nal_capacity,
                              airplay_h264_config_fn on_config,
                              airplay_h264_frame_fn on_frame,
                              void *context)
{
    if (!parser)
        return;
    memset(parser, 0, sizeof(*parser));
    parser->nal = nal_buffer;
    parser->nal_capacity = nal_capacity;
    parser->on_config = on_config;
    parser->on_frame = on_frame;
    parser->context = context;
}

int airplay_h264_feed(struct airplay_h264_parser *parser,
                      const uint8_t *data, size_t len)
{
    size_t i;

    if (!parser || !parser->nal || !parser->nal_capacity ||
        (!data && len))
        return -1;
    for (i = 0; i < len; i++) {
        uint8_t byte = data[i];
        if (!parser->in_nal) {
            if (byte == 0) {
                if (parser->trailing_zeroes < 3)
                    parser->trailing_zeroes++;
            } else if (byte == 1 && parser->trailing_zeroes >= 2) {
                parser->in_nal = 1;
                parser->nal_len = 0;
                parser->trailing_zeroes = 0;
            } else {
                parser->trailing_zeroes = 0;
            }
            continue;
        }
        if (byte == 1 && parser->trailing_zeroes >= 2) {
            parser->nal_len -= parser->trailing_zeroes;
            if (process_nal(parser) < 0)
                return -1;
            parser->nal_len = 0;
            parser->trailing_zeroes = 0;
            continue;
        }
        if (parser->nal_len >= parser->nal_capacity)
            return -1;
        parser->nal[parser->nal_len++] = byte;
        if (byte == 0)
            parser->trailing_zeroes++;
        else
            parser->trailing_zeroes = 0;
    }
    return 0;
}

int airplay_h264_flush(struct airplay_h264_parser *parser)
{
    int result;

    if (!parser)
        return -1;
    if (!parser->in_nal || !parser->nal_len)
        return 0;
    result = process_nal(parser);
    parser->nal_len = 0;
    parser->in_nal = 0;
    parser->trailing_zeroes = 0;
    return result;
}

/* Read one Exp-Golomb ue(v) at *bit. Returns -1 when the bits run out, which for
 * first_mb_in_slice means the slice header is truncated and the NAL cannot be
 * trusted as a picture boundary.
 *
 * Encoding: k leading zeroes, a terminating 1, then k suffix bits, and the value
 * is (2^k - 1) + suffix. Dropping that bias makes ue(v)=1 ("010") decode as 0 and
 * every second slice of a picture look like a boundary. */
static int read_ue(const uint8_t *data, size_t len, size_t *bit)
{
    unsigned leading_zeroes = 0;
    unsigned long long value = 0;
    unsigned index;

    while (*bit < len * 8u) {
        unsigned bit_value = (data[*bit >> 3] >> (7u - (*bit & 7u))) & 1u;

        if (bit_value)
            break;
        if (++leading_zeroes > 31u)
            return -1;
        (*bit)++;
    }
    if (*bit >= len * 8u)
        return -1;
    (*bit)++; /* the terminating 1 bit */
    for (index = 0; index < leading_zeroes; index++) {
        value <<= 1;
        if (*bit < len * 8u)
            value |= (data[*bit >> 3] >> (7u - (*bit & 7u))) & 1u;
        (*bit)++;
    }
    value += (1ULL << leading_zeroes) - 1ULL;
    if (value > 0x7fffffffu)
        return -1;
    return (int)value;
}

int airplay_h264_slice_starts_picture(const uint8_t *nal, size_t nal_len)
{
    size_t bit = 8u; /* skip the 1-byte NAL header */

    if (!nal || nal_len < 2u)
        return 0;
    /* first_mb_in_slice is the first ue(v) of the slice header. 0 means this
     * slice starts at macroblock 0, i.e. it begins a new picture. Slice headers
     * are read before any emulation-prevention byte can appear, so the raw
     * buffer can be parsed directly. */
    return read_ue(nal, nal_len, &bit) == 0;
}

/* ---- SPS frame-cropping rewrite (panel-mirror capture) ------------------- */

#define SPS_RBSP_CAPACITY 400

void airplay_h264_parser_set_stream_crop(struct airplay_h264_parser *parser,
                                         unsigned display_width,
                                         unsigned display_height)
{
    airplay_h264_parser_set_stream_crop_rect(parser, 0u, 0u,
                                             display_width, display_height);
}

void airplay_h264_parser_set_stream_crop_rect(struct airplay_h264_parser *parser,
                                              unsigned crop_x, unsigned crop_y,
                                              unsigned display_width,
                                              unsigned display_height)
{
    if (!parser)
        return;
    parser->stream_crop_x = crop_x;
    parser->stream_crop_y = crop_y;
    parser->stream_crop_width = display_width;
    parser->stream_crop_height = display_height;
}

struct sps_bits {
    const uint8_t *data;
    size_t len_bits;
    size_t pos;
    int overrun;
};

static int sb_bit(struct sps_bits *b)
{
    int value;

    if (b->pos >= b->len_bits) {
        b->overrun = 1;
        return 0;
    }
    value = (b->data[b->pos >> 3] >> (7u - (b->pos & 7u))) & 1u;
    b->pos++;
    return value;
}

static unsigned sb_u(struct sps_bits *b, int count)
{
    unsigned value = 0;
    int i;

    for (i = 0; i < count; i++)
        value = (value << 1) | (unsigned)sb_bit(b);
    return value;
}

static int sb_ue(struct sps_bits *b)
{
    unsigned leading_zeroes = 0;
    unsigned long long value = 0;
    unsigned i;

    while (!sb_bit(b)) {
        if (b->overrun || ++leading_zeroes > 31u) {
            b->overrun = 1;
            return -1;
        }
    }
    if (b->overrun)
        return -1;
    for (i = 0; i < leading_zeroes; i++) {
        value = (value << 1) | (unsigned)sb_bit(b);
        if (b->overrun)
            return -1;
    }
    value += (1ULL << leading_zeroes) - 1ULL;
    if (value > 0x7fffffffULL) {
        b->overrun = 1;
        return -1;
    }
    return (int)value;
}

static int sb_se(struct sps_bits *b)
{
    int ue = sb_ue(b);

    if (ue < 0)
        return -1;
    if (ue & 1u)
        return (ue + 1) / 2;
    return -(ue / 2);
}

static void sb_copy_bits(struct sps_bits *b, size_t end_bit, uint8_t *out,
                         size_t *out_pos)
{
    while (b->pos < end_bit && !b->overrun) {
        int bit = sb_bit(b);
        size_t byte = *out_pos >> 3;

        out[byte] &= (uint8_t)~(0x80u >> (*out_pos & 7u));
        if (bit)
            out[byte] |= (uint8_t)(0x80u >> (*out_pos & 7u));
        (*out_pos)++;
    }
}

struct sps_writer {
    uint8_t *data;
    size_t cap_bits;
    size_t pos;
    int overrun;
};

static void sw_bit(struct sps_writer *w, int bit)
{
    size_t byte;

    if (w->pos >= w->cap_bits) {
        w->overrun = 1;
        return;
    }
    byte = w->pos >> 3;
    w->data[byte] &= (uint8_t)~(0x80u >> (w->pos & 7u));
    if (bit)
        w->data[byte] |= (uint8_t)(0x80u >> (w->pos & 7u));
    w->pos++;
}

static void sw_ue(struct sps_writer *w, unsigned value)
{
    unsigned encoded = value + 1u;
    int bits = 0;
    unsigned long long t = encoded;
    unsigned i;

    while (t > 1u) {
        t >>= 1;
        bits++;
    }
    for (i = 0; i < (unsigned)bits; i++)
        sw_bit(w, 0);
    for (i = 0; i <= (unsigned)bits; i++)
        sw_bit(w, (int)((encoded >> (unsigned)(bits - i)) & 1u));
}

/* Remove emulation-prevention bytes, keeping the NAL header at rbsp[0]. */
static size_t sps_unescape(const uint8_t *nal, size_t nal_len, uint8_t *rbsp,
                           size_t capacity)
{
    size_t out = 1; /* NAL header byte */
    size_t i = 1;
    int zeroes = 0;

    if (!nal_len || nal_len > capacity)
        return 0;
    rbsp[0] = nal[0];
    while (i < nal_len && out < capacity) {
        if (zeroes == 2 && nal[i] == 0x03) {
            zeroes = 0;
            i++;
            continue;
        }
        rbsp[out++] = nal[i];
        zeroes = nal[i] == 0 ? zeroes + 1 : 0;
        i++;
    }
    return i == nal_len ? out : 0;
}

/* Re-insert emulation-prevention bytes after the NAL header. Returns 0 when
 * the buffer is too small to hold the escaped form. */
static size_t sps_escape(const uint8_t *rbsp, size_t rbsp_len, uint8_t *nal,
                         size_t capacity)
{
    size_t out = 0;
    size_t i;
    int zeroes = 0;

    if (rbsp_len < 1)
        return 0;
    for (i = 0; i < rbsp_len; i++) {
        if (zeroes == 2 && rbsp[i] <= 0x03) {
            if (out + 1u >= capacity)
                return 0;
            nal[out++] = 0x03;
            zeroes = 0;
        }
        if (out >= capacity)
            return 0;
        nal[out++] = rbsp[i];
        zeroes = rbsp[i] == 0 ? zeroes + 1 : 0;
    }
    return out;
}

int airplay_h264_sps_apply_crop(uint8_t *nal, size_t *nal_len, size_t capacity,
                                unsigned display_width, unsigned display_height)
{
    return airplay_h264_sps_apply_crop_rect(nal, nal_len, capacity, 0u, 0u,
                                            display_width, display_height);
}

int airplay_h264_sps_apply_crop_rect(uint8_t *nal, size_t *nal_len,
                                     size_t capacity,
                                     unsigned crop_x, unsigned crop_y,
                                     unsigned display_width,
                                     unsigned display_height)
{
    uint8_t rbsp[SPS_RBSP_CAPACITY];
    uint8_t output[SPS_RBSP_CAPACITY];
    struct sps_bits in;
    struct sps_writer out;
    size_t rbsp_len;
    size_t crop_flag_bit;
    size_t tail_bit;
    unsigned profile_idc;
    unsigned chroma_idc = 1;
    int separate_colour = 0;
    unsigned coded_width;
    unsigned coded_height;
    unsigned crop_unit_x;
    unsigned crop_unit_y;
    unsigned right_units;
    unsigned bottom_units;
    unsigned left_units;
    unsigned top_units;
    int high_profile;

    if (!nal || !nal_len || *nal_len < 2u || *nal_len > capacity ||
        !display_width || !display_height)
        return -1;
    rbsp_len = sps_unescape(nal, *nal_len, rbsp, sizeof(rbsp));
    if (!rbsp_len)
        return -1;

    memset(&in, 0, sizeof(in));
    in.data = rbsp;
    in.len_bits = rbsp_len * 8u;
    in.pos = 8u; /* NAL header */
    profile_idc = sb_u(&in, 8);
    (void)sb_u(&in, 8); /* constraint flags */
    (void)sb_u(&in, 8); /* level_idc */
    if (sb_ue(&in) < 0 || in.overrun)
        return -1;
    high_profile = profile_idc == 100u || profile_idc == 110u ||
                   profile_idc == 122u || profile_idc == 244u ||
                   profile_idc == 44u || profile_idc == 83u ||
                   profile_idc == 86u || profile_idc == 118u ||
                   profile_idc == 128u || profile_idc == 138u ||
                   profile_idc == 139u || profile_idc == 134u ||
                   profile_idc == 135u;
    if (high_profile) {
        int rc = sb_ue(&in);

        if (rc < 0 || in.overrun)
            return -1;
        chroma_idc = (unsigned)rc;
        if (chroma_idc == 3u) {
            separate_colour = sb_bit(&in);
            if (in.overrun)
                return -1;
        }
        if (sb_ue(&in) < 0 || sb_ue(&in) < 0 || in.overrun)
            return -1;
        (void)sb_bit(&in); /* qpprime_y_zero_transform_bypass_flag */
        if (sb_bit(&in)) { /* seq_scaling_matrix_present_flag */
            unsigned count = chroma_idc == 3u ? 12u : 8u;
            unsigned j;

            if (in.overrun)
                return -1;
            for (j = 0; j < count; j++) {
                if (sb_bit(&in)) {
                    unsigned size = j < 6u ? 16u : 64u;
                    int last = 8;
                    int next = 8;
                    unsigned k;

                    if (in.overrun)
                        return -1;
                    for (k = 0; k < size; k++) {
                        if (next != 0) {
                            int delta = sb_se(&in);

                            if (delta < 0 || in.overrun)
                                return -1;
                            next = (last + delta + 256) % 256;
                        }
                        if (next != 0)
                            last = next;
                    }
                }
            }
        }
    }
    if (sb_ue(&in) < 0 || in.overrun)
        return -1; /* log2_max_frame_num_minus4 */
    {
        int poc_type = sb_ue(&in);

        if (poc_type < 0 || in.overrun)
            return -1;
        if (poc_type == 0) {
            if (sb_ue(&in) < 0 || in.overrun)
                return -1;
        } else if (poc_type == 1) {
            int always_zero = sb_bit(&in);
            int rc;

            if (in.overrun)
                return -1;
            rc = sb_se(&in);
            if (rc < 0 || sb_se(&in) < 0 || in.overrun)
                return -1;
            (void)always_zero;
            rc = sb_ue(&in);
            if (rc < 0 || in.overrun)
                return -1;
            if (rc > 255)
                return -1;
            {
                unsigned j;

                for (j = 0; j < (unsigned)rc; j++) {
                    if (sb_se(&in) < 0 || in.overrun)
                        return -1;
                }
            }
        }
    }
    if (sb_ue(&in) < 0 || in.overrun)
        return -1; /* max_num_ref_frames */
    (void)sb_bit(&in);     /* gaps_in_frame_num_value_allowed_flag */
    if (in.overrun)
        return -1;
    {
        int w_mbs = sb_ue(&in);
        int h_units = sb_ue(&in);
        int frame_mbs_only;

        if (w_mbs < 0 || h_units < 0 || in.overrun)
            return -1;
        frame_mbs_only = sb_bit(&in);
        if (in.overrun)
            return -1;
        if (!frame_mbs_only) {
            (void)sb_bit(&in); /* mb_adaptive_frame_field_flag */
            if (in.overrun)
                return -1;
        }
        coded_width = ((unsigned)w_mbs + 1u) * 16u;
        coded_height = ((unsigned)h_units + 1u) * 16u *
                       (frame_mbs_only ? 1u : 2u);
    }
    (void)sb_bit(&in); /* direct_8x8_inference_flag */
    if (in.overrun)
        return -1;
    crop_flag_bit = in.pos;
    if (sb_bit(&in)) { /* existing frame_cropping_flag */
        if (sb_ue(&in) < 0 || sb_ue(&in) < 0 || sb_ue(&in) < 0 ||
            sb_ue(&in) < 0 || in.overrun)
            return -1;
    }
    tail_bit = in.pos;
    if (in.overrun || tail_bit >= in.len_bits)
        return -1;

    if (!display_width || !display_height ||
        crop_x > coded_width || crop_y > coded_height ||
        display_width > coded_width - crop_x ||
        display_height > coded_height - crop_y)
        return -1;
    /* ChromaArrayType 0 (monochrome or separate planes) crops in whole
     * pixels; otherwise the units follow SubWidthC/SubHeightC. */
    if (separate_colour)
        chroma_idc = 0;
    crop_unit_x = chroma_idc == 3u ? 1u : 2u;
    crop_unit_y = chroma_idc == 2u ? 1u : 2u;
    if (chroma_idc == 0u) {
        crop_unit_x = 1u;
        crop_unit_y = 1u;
    }
    {
        unsigned left_px = crop_x;
        unsigned right_px = coded_width - crop_x - display_width;
        unsigned top_px = crop_y;
        unsigned bottom_px = coded_height - crop_y - display_height;

        if (left_px % crop_unit_x || right_px % crop_unit_x ||
            top_px % crop_unit_y || bottom_px % crop_unit_y)
            return -1;
        right_units = right_px / crop_unit_x;
        bottom_units = bottom_px / crop_unit_y;
        left_units = left_px / crop_unit_x;
        top_units = top_px / crop_unit_y;
    }

    memset(output, 0, sizeof(output));
    memset(&out, 0, sizeof(out));
    out.data = output;
    out.cap_bits = sizeof(output) * 8u;
    in.pos = 0;
    sb_copy_bits(&in, crop_flag_bit, output, &out.pos);
    if (in.overrun || out.overrun)
        return -1;
    if (left_units || right_units || top_units || bottom_units) {
        sw_bit(&out, 1);
        sw_ue(&out, left_units);
        sw_ue(&out, right_units);
        sw_ue(&out, top_units);
        sw_ue(&out, bottom_units);
    } else {
        sw_bit(&out, 0);
    }
    if (out.overrun)
        return -1;
    in.pos = tail_bit;
    sb_copy_bits(&in, in.len_bits, output, &out.pos);
    /* The new crop field usually has a different bit width than the one the
     * encoder wrote (e.g. screenrecord pads 1080 to 1088 with a 9-bit field;
     * a 1280x600 canvas needs 36 bits), so the copied tail lands off the byte
     * boundary. Pad with alignment zeroes: the tail already carries the
     * rbsp stop bit, and parsers stop reading there, so trailing zeroes are
     * ignored. Failing here instead would silently keep the un-cropped SPS
     * and the sink would letterbox the stream again. */
    while (out.pos & 7u)
        sw_bit(&out, 0);
    if (in.overrun || out.overrun)
        return -1;
    {
        uint8_t escaped[SPS_RBSP_CAPACITY];
        size_t escaped_len = sps_escape(output, out.pos / 8u, escaped,
                                        sizeof(escaped));

        if (!escaped_len || escaped_len > capacity)
            return -1;
        if (escaped_len == *nal_len && memcmp(escaped, nal, escaped_len) == 0)
            return 1;
        memcpy(nal, escaped, escaped_len);
        *nal_len = escaped_len;
        return 0;
    }
}
