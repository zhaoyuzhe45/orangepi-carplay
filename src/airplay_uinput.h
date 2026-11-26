#ifndef AIRPLAY_UINPUT_H
#define AIRPLAY_UINPUT_H

struct airplay_uinput;

/* Raw values reported for one position frame. */
struct airplay_uinput_axis_values {
    int x;
    int y;
};

/* Opens the Android virtual input device used for low-latency touch delivery.
 * The kernel's legacy uinput API is used because Android 8.1's RK3399 kernel
 * predates UI_DEV_SETUP. */
struct airplay_uinput *airplay_uinput_open(void);
void airplay_uinput_close(struct airplay_uinput *device);

/* Resolves one frame's raw values, alternating `parity` between consecutive
 * frames.
 *
 * The Linux input core discards an EV_ABS event whose value equals the one it
 * already holds (input_handle_abs_event -> INPUT_IGNORE_EVENT). A real
 * touchscreen never notices because its axes always carry noise, while a
 * synthetic stream is exact: an axis that does not move produces no event at
 * all. Measured here (`getevent -lt /dev/input/event4`, 622 frames of a 12 s
 * vertical drag): X appeared in only 338 frames while holding 28174. Anything
 * that rebuilds the pointer state from a frame - SYN_DROPPED, a device
 * reconfigure, a fresh tracking id - then reads the absent axis as 0, which
 * pins the pointer to the left edge on vertical drags and to the top edge on
 * horizontal ones.
 *
 * Alternating the parity keeps both axes changing even when the position is
 * identical. One raw unit is 1/65535 of the display (0.03 px at 1920 wide).
 * Returns 0, or -1 for a zero extent or a missing output. */
int airplay_uinput_axis_values(unsigned x, unsigned y, unsigned width,
                               unsigned height, unsigned parity,
                               struct airplay_uinput_axis_values *out);

int airplay_uinput_touch_down(struct airplay_uinput *device,
                              unsigned x, unsigned y,
                              unsigned width, unsigned height);
int airplay_uinput_touch_move(struct airplay_uinput *device,
                              unsigned x, unsigned y,
                              unsigned width, unsigned height);
int airplay_uinput_touch_up(struct airplay_uinput *device,
                            unsigned x, unsigned y,
                            unsigned width, unsigned height);

#endif
