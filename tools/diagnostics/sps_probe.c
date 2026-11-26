/* Run the production SPS frame-cropping rewrite over a real captured SPS.
 * Usage: sps_probe <file.h264> <display_w> <display_h>
 * Prints the original and rewritten SPS as hex (start code stripped). */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

int airplay_h264_sps_apply_crop_rect(uint8_t *nal, size_t *nal_len,
                                     size_t capacity, unsigned crop_x,
                                     unsigned crop_y, unsigned display_width,
                                     unsigned display_height);

static size_t next_start(const uint8_t *d, size_t len, size_t from, size_t *sc)
{
    size_t i;

    for (i = from; i + 3 <= len; i++) {
        if (d[i] == 0 && d[i + 1] == 0 && d[i + 2] == 1) { *sc = 3; return i; }
        if (i + 4 <= len && d[i] == 0 && d[i + 1] == 0 && d[i + 2] == 0 &&
            d[i + 3] == 1) { *sc = 4; return i; }
    }
    return (size_t)-1;
}

int main(int argc, char **argv)
{
    FILE *fp;
    long size;
    uint8_t *buf, nal[1024];
    size_t pos, sc, beg, end, nal_len;
    unsigned i;

    if (argc < 4) {
        fprintf(stderr, "usage: %s <file.h264> <display_w> <display_h>\n", argv[0]);
        return 2;
    }
    fp = fopen(argv[1], "rb");
    if (!fp) { perror("open"); return 1; }
    fseek(fp, 0, SEEK_END);
    size = ftell(fp);
    fseek(fp, 0, SEEK_SET);
    buf = malloc((size_t)size);
    if (!buf || fread(buf, 1, (size_t)size, fp) != (size_t)size) return 1;
    fclose(fp);

    /* Walk the start codes until an SPS (nal_unit_type 7) shows up. */
    pos = 0;
    nal_len = 0;
    while ((pos = next_start(buf, (size_t)size, pos, &sc)) != (size_t)-1) {
        beg = pos + sc;
        end = next_start(buf, (size_t)size, beg, &sc);
        if (end == (size_t)-1) end = (size_t)size;
        if (beg < (size_t)size && (buf[beg] & 0x1f) == 7) {
            nal_len = end - beg;
            if (nal_len > sizeof(nal)) return 1;
            memcpy(nal, buf + beg, nal_len);
            break;
        }
        pos = beg;
    }
    if (!nal_len) { fprintf(stderr, "no SPS\n"); return 1; }
    printf("original(%u):", (unsigned)nal_len);
    for (i = 0; i < nal_len; i++) printf(" %02x", nal[i]);
    printf("\n");

    if (airplay_h264_sps_apply_crop_rect(nal, &nal_len, sizeof(nal), 0, 0,
                                         (unsigned)atoi(argv[2]),
                                         (unsigned)atoi(argv[3])) != 0) {
        printf("rewrite_failed\n");
        return 3;
    }
    printf("rewritten(%u):", (unsigned)nal_len);
    for (i = 0; i < nal_len; i++) printf(" %02x", nal[i]);
    printf("\n");
    return 0;
}
