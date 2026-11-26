/*
 * Unit test for the H.264 picture-boundary detector.
 *
 * The packetizer sends one AirPlay packet per *picture*, not per slice, because
 * a sink that receives half a picture paints a mosaic until the next IDR. That
 * decision hinges on first_mb_in_slice == 0, which is the first ue(v) of the
 * slice header. Getting that decode wrong - in particular forgetting the
 * (2^leadingZeroes - 1) bias, which makes ue(v)=1 decode as 0 - turns every
 * second slice into a false boundary and silently restores the per-slice
 * packetization this code exists to avoid.
 *
 * Built and run on the host: it is a pure function over a byte buffer.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../src/airplay_h264.c"

static int failures;

static void expect(const char *name, const unsigned char *nal, size_t len,
                   int want)
{
    int got = airplay_h264_slice_starts_picture(nal, len);

    if (got != want) {
        printf("FAIL: %s: got %d want %d\n", name, got, want);
        failures++;
        return;
    }
    printf("ok: %s\n", name);
}

int main(void)
{
    /* NAL header 0x65 = IDR slice, 0x41 = non-IDR slice. The byte after it is
     * the start of the slice header: first_mb_in_slice as ue(v), most
     * significant bit first. */
    static const unsigned char ue0[] = { 0x65, 0x80 };  /* "1"     -> 0 */
    static const unsigned char ue1[] = { 0x65, 0x40 };  /* "010"   -> 1 */
    static const unsigned char ue2[] = { 0x65, 0x60 };  /* "011"   -> 2 */
    static const unsigned char ue3[] = { 0x65, 0x20 };  /* "00100" -> 3 */
    static const unsigned char ue4[] = { 0x41, 0x28 };  /* "00101" -> 4 */
    static const unsigned char ue255[] = { 0x41, 0x00, 0x1c, 0x00 };
    static const unsigned char idr_first[] = { 0x65, 0xb0 };  /* 1,011 -> 0 */
    static const unsigned char p_first[] = { 0x41, 0xc0 };    /* 1,1   -> 0 */
    static const unsigned char truncated[] = { 0x65 };
    static const unsigned char all_zero[] = { 0x65, 0x00 };

    expect("first_mb=0 (IDR) starts a picture", ue0, sizeof(ue0), 1);
    expect("first_mb=0 with I slice_type starts a picture", idr_first,
           sizeof(idr_first), 1);
    expect("first_mb=0 with P slice_type starts a picture", p_first,
           sizeof(p_first), 1);
    expect("first_mb=1 is a continuation slice", ue1, sizeof(ue1), 0);
    expect("first_mb=2 is a continuation slice", ue2, sizeof(ue2), 0);
    expect("first_mb=3 is a continuation slice", ue3, sizeof(ue3), 0);
    expect("first_mb=4 is a continuation slice", ue4, sizeof(ue4), 0);
    expect("large first_mb is a continuation slice", ue255, sizeof(ue255), 0);
    expect("1-byte NAL cannot be a slice", truncated, sizeof(truncated), 0);
    expect("unterminated ue(v) is not a boundary", all_zero,
           sizeof(all_zero), 0);

    if (failures) {
        printf("h264 slice boundary: %d failure(s)\n", failures);
        return 1;
    }
    printf("h264 slice boundary: ok\n");
    return 0;
}
