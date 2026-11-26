#include "carplay_display.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

void iap2_log(const char *fmt, ...)
{
    (void)fmt;
}

static int failures;
#define EXPECT(c, m) do { if (!(c)) { fprintf(stderr, "FAIL: %s\n", m); failures++; } } while (0)

/* Reproduces the input stack's raw->display mapping for this virtual
 * touchscreen: the uinput device advertises raw 0..65535 on both axes and
 * InputReader maps that range onto the PANEL frame, expressed relative to the
 * logical display origin. Measured on this unit with `dumpsys input`
 * (wm 1920x900 on the 1920x1080 panel): X 0..1919, Y -90..989 - the mapping
 * target is the panel, not the 1920x900 logical display. */
static long input_stack_reads(unsigned injected, unsigned extent, long min,
                              long max)
{
    unsigned long long raw;

    if (extent < 2)
        return min;
    raw = (unsigned long long)injected * 65535ull /
          (unsigned long long)(extent - 1);
    return min + (long)((unsigned long long)(max - min) * raw / 65535ull);
}

static int apply_device_mode(int argc, char **argv)
{
    struct carplay_display_plan plan;
    struct carplay_display_result result;

    if (argc != 6 || strcmp(argv[1], "apply") != 0)
        return -1;
    memset(&plan, 0, sizeof(plan));
    plan.physical_width = (unsigned)strtoul(argv[2], NULL, 10);
    plan.physical_height = (unsigned)strtoul(argv[3], NULL, 10);
    plan.physical_fps = (unsigned)strtoul(argv[4], NULL, 10);
    plan.density = (unsigned)strtoul(argv[5], NULL, 10);
    plan.width = plan.physical_width;
    plan.height = plan.physical_height;
    plan.fps = 30;
    plan.force_physical = 1;
    plan.negotiated = 1;
    if (carplay_display_apply(&plan, &result) < 0) {
        fprintf(stderr, "device apply failed: %s (rollback available)\n",
                result.error);
        return 1;
    }
    printf("device apply passed: physical=%ux%u density=%u native=%d\n",
           result.physical_width, result.physical_height, result.density,
           result.native);
    return 0;
}

int main(int argc, char **argv)
{
    if (argc > 1)
        return apply_device_mode(argc, argv);

    struct carplay_display_state state;
    char arg[32];

    memset(&state, 0, sizeof(state));
    EXPECT(carplay_display_parse_size(
               "Physical size: 1920x1080\nOverride size: 800x480\n",
               &state) == 0 && state.width == 800 &&
               state.height == 480 && state.physical_width == 1920 &&
               state.physical_height == 1080 && state.size_override,
           "size parser prefers override");
    EXPECT(carplay_display_parse_size("Physical size: 1920x1080\n",
                                      &state) == 0 && state.width == 1920 &&
               !state.size_override,
           "size parser uses physical without override");
    EXPECT(carplay_display_parse_density(
               "Physical density: 280\nOverride density: 160\n",
               &state) == 0 && state.density == 160 &&
               state.density_override,
           "density parser prefers override");
    EXPECT(carplay_display_format_size_arg(800, 480, arg, sizeof(arg)) == 0 &&
               strcmp(arg, "800x480") == 0,
           "size argument is bounded and canonical");
    EXPECT(carplay_display_format_density_arg(160, arg, sizeof(arg)) == 0 &&
               strcmp(arg, "160") == 0,
           "density argument is bounded and canonical");
    EXPECT(carplay_display_format_density_arg(119, arg, sizeof(arg)) < 0,
           "invalid density is rejected");
    EXPECT(carplay_display_format_size_arg(801, 480, arg, sizeof(arg)) < 0,
           "odd width is rejected");
    EXPECT(carplay_display_format_physical_mode(800, 480, 60, arg,
                                                sizeof(arg)) == 0 &&
               strcmp(arg, "800x480@60") == 0,
           "physical mode uses the persist.sys.framebuffer.main syntax");
    EXPECT(carplay_display_format_physical_mode(800, 0, 60, arg,
                                                sizeof(arg)) < 0,
           "physical mode rejects incomplete dimensions");
    EXPECT(carplay_display_format_physical_mode(800, 480, 0, arg,
                                                sizeof(arg)) < 0,
           "physical mode rejects missing refresh rate");
    EXPECT(carplay_display_mode_builtin(1920, 900) == 1,
           "1920x900 is in the built-in physical mode catalog");
    EXPECT(carplay_display_mode_builtin(1366, 768) == 0,
           "unknown physical modes are rejected");
    EXPECT(carplay_display_mode_supported(
               "1920x1080\n1280x600\n", 1280, 600, 60) == 1,
           "mode list accepts an exact physical size");
    EXPECT(carplay_display_mode_supported(
               "800x480p60\n1024x480p60\n1280x600p60\n"
               "1280x720p60\n1920x900p60\n1920x1080p60\n",
               1920, 900, 60) == 1,
           "1920x900 is recognized in the built-in mode catalog");
    EXPECT(carplay_display_mode_supported(
               "1920x1080\n1280x720p60\n", 800, 480, 60) == 0,
           "mode list rejects an unavailable physical size");
    EXPECT(carplay_display_mode_supported(
               "1280x720p30\n", 1280, 720, 60) == 0,
           "mode list respects an explicit refresh rate");
    EXPECT(carplay_display_parse_size(
               "Physical size: 800x480\nOverride size: 800x480\n",
               &state) == 0 && state.physical_width == 800 &&
               state.physical_height == 480,
           "physical dimensions remain distinct from the logical override");

    /* Panel-mirror geometry: SurfaceFlinger on this unit renders a logical
     * display smaller than the panel SCALED UP to the largest canvas-aspect
     * rectangle that fits, centered (measured: wm 1280x600 on the 1920x1080
     * panel composes at 0,90-1920,990, 1.5x). The mirror rectangle is
     * therefore that fill rectangle - never the raw canvas - so the
     * WindowManager override makes the composer scale exactly 1.0 and the
     * SPS crop removes the centered letterbox bars. */
    {
        unsigned mw = 0;
        unsigned mh = 0;

        EXPECT(carplay_display_mirror_size(1280, 600, 1920, 1080,
                                           &mw, &mh) == 0 &&
                   mw == 1920 && mh == 900,
               "a smaller canvas fills the panel (SF upscales the logical display)");
        EXPECT(carplay_display_mirror_size(1920, 900, 1920, 1080,
                                           &mw, &mh) == 0 &&
                   mw == 1920 && mh == 900,
               "a fitting wide canvas keeps its pixels");
        EXPECT(carplay_display_mirror_size(3840, 2160, 1920, 1080,
                                           &mw, &mh) == 0 &&
                   mw == 1920 && mh == 1080,
               "an oversized canvas fills the panel");
        EXPECT(carplay_display_mirror_size(2560, 1080, 1920, 1080,
                                           &mw, &mh) == 0 &&
                   mw == 1920 && mh == 1080,
               "an oversized wide canvas fills the panel; the sink stretches it");
        EXPECT(carplay_display_mirror_size(1920, 1200, 1920, 1080,
                                           &mw, &mh) == 0 &&
                   mw == 1920 && mh == 1080,
               "an oversized tall canvas fills the panel; the sink stretches it");
        EXPECT(carplay_display_mirror_size(2560, 1600, 1920, 1080,
                                           &mw, &mh) == 0 &&
                   mw == 1920 && mh == 1080,
               "an oversized 16:10 canvas fills the panel, bars instead of a fit");
        EXPECT(carplay_display_mirror_size(800, 376, 1920, 1080,
                                           &mw, &mh) == 0 &&
                   mw == 1920 && mh == 902,
               "a canvas that fits still floors the fill rectangle to even rows");
        EXPECT(carplay_display_mirror_size(1281, 600, 1920, 1080,
                                           &mw, &mh) == 0 &&
                   mw == 1920 && mh == 900,
               "odd canvas dimensions are evened before filling");
        EXPECT(carplay_display_mirror_size(1920, 800, 1280, 600,
                                           &mw, &mh) == 0 &&
                   mw == 1280 && mh == 600,
               "a canvas wider than a small panel fills that panel");
        EXPECT(carplay_display_mirror_size(1000, 400, 1280, 600,
                                           &mw, &mh) == 0 &&
                   mw == 1280 && mh == 512,
               "a canvas that fits a small panel keeps the panel width");
        EXPECT(carplay_display_mirror_size(0, 600, 1920, 1080,
                                           &mw, &mh) < 0,
               "a zero canvas is rejected");
        EXPECT(carplay_display_mirror_size(1280, 600, 0, 1080,
                                           &mw, &mh) < 0,
               "a zero panel is rejected");

        /* Where the composer actually renders the logical display inside
         * the panel, and the letterbox bars around it. */
        {
            unsigned bx = 99;
            unsigned by = 99;
            unsigned vx = 0;
            unsigned vy = 0;
            unsigned vw = 0;
            unsigned vh = 0;

            EXPECT(carplay_display_letterbox_origin(1920, 1080, 1920, 900,
                                                    &bx, &by) == 0 &&
                       bx == 0 && by == 90,
                   "the letterbox origin centers the mirror vertically");
            EXPECT(carplay_display_letterbox_origin(1920, 1080, 1800, 1080,
                                                    &bx, &by) == 0 &&
                       bx == 60 && by == 0,
                   "the letterbox origin centers the mirror horizontally");
            EXPECT(carplay_display_letterbox_origin(1920, 1080, 1920, 1080,
                                                    &bx, &by) == 0 &&
                       bx == 0 && by == 0,
                   "a full-panel mirror leaves no bars");
            EXPECT(carplay_display_letterbox_origin(1920, 1080, 1921, 900,
                                                    &bx, &by) < 0,
                   "a mirror larger than the panel is rejected");
            /* The viewport is expressed in CAPTURE space. Measured on this
             * unit: a 1920x1080 capture of the 1920x900 logical display
             * carries the frame's first row at y=0 (constant +90 vs the
             * panel, slope 1.0), so the crop origin is 0 -- using the
             * panel's letterbox offset here cuts the top 90 rows of the
             * picture and shifts the sink's image up. */
            EXPECT(carplay_display_viewport_rect(1920, 1080, 1280, 600,
                                                 &vx, &vy, &vw, &vh) == 0 &&
                       vx == 0 && vy == 0 && vw == 1920 && vh == 900,
                   "the 1280x600 canvas viewport sits at the capture origin as 1920x900");
            EXPECT(carplay_display_viewport_rect(1920, 1080, 1920, 1080,
                                                 &vx, &vy, &vw, &vh) == 0 &&
                       vx == 0 && vy == 0 && vw == 1920 && vh == 1080,
                   "a native canvas viewport is the panel itself");
            EXPECT(carplay_display_viewport_rect(1920, 1080, 2560, 1080,
                                                 &vx, &vy, &vw, &vh) == 0 &&
                       vx == 0 && vy == 0 && vw == 1920 && vh == 1080,
                   "an oversized canvas viewport is the whole panel - no crop, no bars");
        }

        /* Touch mapping: the sink reports coordinates in what it SEES (the
         * cropped stream), but Android injects them in the logical
         * (WindowManager) space. The capture holds the frame at its origin,
         * so for the panel-mirror policy stream origin == viewport origin
         * and only the logical/viewport scale remains. */
        {
            unsigned tx = 0;
            unsigned ty = 0;

            EXPECT(carplay_display_input_coordinate(
                       0, 0, 0, 0, 0, 0, 1920, 900, 1280, 600,
                       &tx, &ty) == 0 &&
                       tx == 0 && ty == 0,
                   "the stream's viewport origin maps to the logical origin");
            EXPECT(carplay_display_input_coordinate(
                       640, 300, 0, 0, 0, 0, 1920, 900, 1280, 600,
                       &tx, &ty) == 0 &&
                       tx == 427 && ty == 200,
                   "a mid-canvas touch lands two thirds into the viewport");
            EXPECT(carplay_display_input_coordinate(
                       300, 200, 0, 0, 0, 0, 1920, 900, 1920, 900,
                       &tx, &ty) == 0 &&
                       tx == 300 && ty == 200,
                   "a 1:1 stream maps through unchanged");
            EXPECT(carplay_display_input_coordinate(
                       0, 0, 0, 0, 0, 90, 1920, 900, 1280, 600,
                       &tx, &ty) == 0 &&
                       tx == 0 && ty == 0,
                   "touches above the viewport clamp to the top edge");
            EXPECT(carplay_display_input_coordinate(
                       1280, 600, 0, 0, 0, 0, 1920, 900, 1280, 600,
                       &tx, &ty) == 0 &&
                       tx == 853 && ty == 400,
                   "the bottom-right canvas corner maps inside the viewport");
        }

        /* Injection space: the touchscreen is not mapped to the logical
         * display. Measured on this unit (`dumpsys input`, wm 1920x900 on the
         * 1920x1080 panel): X 0..1919, Y -90..989 -- raw 0..max lands on the
         * PANEL frame, expressed relative to the logical origin. Injecting
         * logical pixels therefore gets scaled by panel/viewport (1.2) and
         * shifted by the letterbox: the centre still lands right while both
         * edges drift away, which reads as "taps near the top go too high,
         * taps near the bottom go too low". The point has to be lifted into
         * panel space and injected against the panel extent. */
        {
            unsigned ix = 0;
            unsigned iy = 0;

            EXPECT(carplay_display_panel_point(0, 0, 1920, 900, 0, 90, 1920,
                                               1080, &ix, &iy) == 0 &&
                       ix == 0 && iy == 90,
                   "the stream origin sits on the panel's letterbox row");
            EXPECT(carplay_display_panel_point(960, 450, 1920, 900, 0, 90,
                                               1920, 1080, &ix, &iy) == 0 &&
                       ix == 960 && iy == 540,
                   "the middle of the stream lands mid-panel");
            EXPECT(carplay_display_panel_point(1919, 899, 1920, 900, 0, 90,
                                               1920, 1080, &ix, &iy) == 0 &&
                       ix == 1919 && iy == 989,
                   "the last streamed pixel lands on the viewport's last row");
            EXPECT(carplay_display_panel_point(960, 540, 1920, 1080, 0, 0,
                                               1920, 1080, &ix, &iy) == 0 &&
                       ix == 960 && iy == 540,
                   "a native canvas needs no lift");
            EXPECT(carplay_display_panel_point(1920, 900, 1920, 900, 0, 90,
                                               1920, 1080, &ix, &iy) == 0 &&
                       ix == 1919 && iy == 989,
                   "a point past the viewport clamps to its last pixel");
        }

        /* Round trip through the input stack's own mapping: whatever
         * carplay_display_panel_point emits, Android must read back as the
         * point the user touched. This is the invariant the old code broke. */
        {
            static const unsigned probes[][2] = {
                { 0, 0 },     { 1, 1 },     { 120, 40 },  { 960, 450 },
                { 1400, 120 }, { 1800, 860 }, { 1919, 899 },
            };
            size_t i;

            for (i = 0; i < sizeof(probes) / sizeof(probes[0]); i++) {
                unsigned ix = 0;
                unsigned iy = 0;
                long rx;
                long ry;

                (void)carplay_display_panel_point(probes[i][0], probes[i][1],
                                                  1920, 900, 0, 90, 1920,
                                                  1080, &ix, &iy);
                rx = input_stack_reads(ix, 1920, 0, 1919);
                ry = input_stack_reads(iy, 1080, -90, 989);
                EXPECT(labs(rx - (long)probes[i][0]) <= 1 &&
                           labs(ry - (long)probes[i][1]) <= 1,
                       "the input stack reads back the point that was touched");
            }
        }

        EXPECT(carplay_display_mirror_density(1280, 600, 200,
                                              1280, 600) == 200,
               "an unclamped canvas keeps the sink density");
        EXPECT(carplay_display_mirror_density(2560, 1080, 200,
                                              1920, 810) == 150,
               "a clamped canvas scales the density by height");
        EXPECT(carplay_display_mirror_density(1920, 1080, 280,
                                              1920, 1080) == 280,
               "a full-panel canvas keeps its density");
        EXPECT(carplay_display_mirror_density(1280, 700, 200,
                                              1280, 600) == 171,
               "the density ratio rounds to nearest");
        EXPECT(carplay_display_mirror_density(1280, 600, 120,
                                              1280, 300) == 120,
               "the density floor is enforced");
        EXPECT(carplay_display_mirror_density(1920, 1080, 480,
                                              1920, 2160) == 480,
               "the density ceiling is enforced");
        EXPECT(carplay_display_mirror_density(0, 600, 200, 1280, 600) == 0,
               "a zero canvas yields no density");
    }
    if (failures) {
        fprintf(stderr, "display policy tests failed: %d\n", failures);
        return 1;
    }
    puts("display policy tests passed");
    return 0;
}
