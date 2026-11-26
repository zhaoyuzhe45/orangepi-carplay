/* Regression tests for the uinput position frame.
 *
 * The Linux input core drops an EV_ABS event whose value equals the one it
 * already holds (input_handle_abs_event -> INPUT_IGNORE_EVENT). A real
 * touchscreen never notices because its axes always carry noise; a synthetic
 * stream is exact, so an axis that does not move produces NO event at all.
 * Dragging along one axis therefore used to emit frames with a single axis,
 * and anything that rebuilds the pointer state from a frame - SYN_DROPPED, a
 * device reconfigure, a fresh tracking id - reads the missing axis as 0. That
 * pinned the pointer to the left edge on vertical drags and to the top edge on
 * horizontal ones, which is what the field report described.
 *
 * Measured on this unit (`getevent -lt /dev/input/event4`, 622 frames of a 12 s
 * vertical drag): X appeared in 338 frames, Y in 412, X held 28174 throughout.
 * Both axes must reach the driver once per frame whatever the movement.
 */
#include "airplay_uinput.h"

#include <stdarg.h>
#include <stdio.h>

void iap2_log(const char *fmt, ...)
{
    (void)fmt;
}

static int failures;
#define EXPECT(c, m) do { if (!(c)) { fprintf(stderr, "FAIL: %s\n", m); failures++; } } while (0)

#define RAW_MAX 65535

/* Mirrors the kernel's cache: an event only reaches a consumer when its value
 * differs from the cached one. */
struct kernel_cache {
    int x;
    int y;
};

static int frame(unsigned x, unsigned y, unsigned w, unsigned h,
                 unsigned parity, struct kernel_cache *cache, const char *what)
{
    struct airplay_uinput_axis_values values;
    int x_passed;
    int y_passed;

    if (airplay_uinput_axis_values(x, y, w, h, parity, &values) < 0) {
        fprintf(stderr, "FAIL: %s: frame rejected\n", what);
        failures++;
        return 0;
    }
    EXPECT(values.x >= 0 && values.x <= RAW_MAX, "x stays inside the declared raw range");
    EXPECT(values.y >= 0 && values.y <= RAW_MAX, "y stays inside the declared raw range");
    x_passed = values.x != cache->x;
    y_passed = values.y != cache->y;
    cache->x = values.x;
    cache->y = values.y;
    EXPECT(x_passed, what);
    EXPECT(y_passed, what);
    return 1;
}

int main(void)
{
    struct airplay_uinput_axis_values values;

    /* Scaling: the endpoints of the frame map to the raw endpoints. */
    EXPECT(airplay_uinput_axis_values(0, 0, 1920, 1080, 0, &values) == 0 &&
               values.x == 0 && values.y == 0,
           "the top-left frame corner scales to raw 0");
    EXPECT(airplay_uinput_axis_values(1919, 1079, 1920, 1080, 0, &values) == 0 &&
               values.x == RAW_MAX && values.y == RAW_MAX,
           "the bottom-right frame corner scales to the raw maximum");

    /* Parity is what keeps an unchanged axis changing. */
    EXPECT(airplay_uinput_axis_values(960, 540, 1920, 1080, 0, &values) == 0,
           "a mid frame is accepted");
    {
        int even_x = values.x;
        int even_y = values.y;

        EXPECT(airplay_uinput_axis_values(960, 540, 1920, 1080, 1, &values) == 0 &&
                   values.x != even_x && values.y != even_y,
               "the opposite parity moves both axes by one raw unit");
        EXPECT(values.x - even_x <= 1 && even_x - values.x <= 1,
               "the parity offset is a single raw unit");
    }

    /* The lower-right corner must not overflow the declared range and must
     * still differ between parities, or the last pixel would go silent. */
    {
        struct airplay_uinput_axis_values even;
        struct airplay_uinput_axis_values odd;

        EXPECT(airplay_uinput_axis_values(1919, 1079, 1920, 1080, 0, &even) == 0 &&
                   airplay_uinput_axis_values(1919, 1079, 1920, 1080, 1, &odd) == 0,
               "the corner frame is accepted at both parities");
        EXPECT(even.x == RAW_MAX, "the corner keeps the exact raw maximum");
        EXPECT(odd.x < RAW_MAX && odd.x >= 0, "the odd corner stays inside the range");
        EXPECT(odd.x != even.x && odd.y != even.y,
               "the corner still changes both axes at the odd parity");
    }

    /* An axis that does not move must still reach the driver every frame.
     * The cache starts at the value the previous gesture left behind, which is
     * exactly the case that used to collapse to zero. */
    {
        struct kernel_cache cache;
        unsigned parity = 0;
        unsigned step;

        EXPECT(airplay_uinput_axis_values(960, 540, 1920, 1080, 0, &values) == 0,
               "the vertical seed frame is accepted");
        cache.x = values.x;
        cache.y = values.y;
        for (step = 1; step <= 24; step++) {
            parity ^= 1u;
            frame(960, 540 + step * 20, 1920, 1080, parity, &cache,
                  "a vertical drag reports both axes on every frame");
        }
    }

    {
        struct kernel_cache cache;
        unsigned parity = 0;
        unsigned step;

        EXPECT(airplay_uinput_axis_values(960, 540, 1920, 1080, 0, &values) == 0,
               "the horizontal seed frame is accepted");
        cache.x = values.x;
        cache.y = values.y;
        for (step = 1; step <= 24; step++) {
            parity ^= 1u;
            frame(960 + step * 30, 540, 1920, 1080, parity, &cache,
                  "a horizontal drag reports both axes on every frame");
        }
    }

    /* A stationary finger still has to publish a frame that survives the
     * kernel's filter, otherwise the next derived state starts from zero. */
    {
        struct kernel_cache cache;
        unsigned parity = 0;
        unsigned step;

        EXPECT(airplay_uinput_axis_values(700, 400, 1920, 1080, 0, &values) == 0,
               "the stationary seed frame is accepted");
        cache.x = values.x;
        cache.y = values.y;
        for (step = 1; step <= 24; step++) {
            parity ^= 1u;
            frame(700, 400, 1920, 1080, parity, &cache,
                  "a stationary finger keeps both axes live");
        }
    }

    /* Degenerate sizes are rejected instead of dividing by zero. */
    EXPECT(airplay_uinput_axis_values(1, 1, 0, 1080, 0, &values) < 0,
           "a zero width is rejected");
    EXPECT(airplay_uinput_axis_values(1, 1, 1920, 0, 0, &values) < 0,
           "a zero height is rejected");
    EXPECT(airplay_uinput_axis_values(1, 1, 1920, 1080, 0, NULL) < 0,
           "a missing output is rejected");

    /* Out-of-range input clamps instead of wrapping. */
    EXPECT(airplay_uinput_axis_values(5000, 5000, 1920, 1080, 0, &values) == 0 &&
               values.x == RAW_MAX && values.y == RAW_MAX,
           "a coordinate past the frame clamps to the raw maximum");

    if (failures) {
        fprintf(stderr, "uinput axis frame tests FAILED (%d)\n", failures);
        return 1;
    }
    printf("uinput axis frame tests PASS (per-frame parity, both axes live,"
           " raw range respected)\n");
    return 0;
}
