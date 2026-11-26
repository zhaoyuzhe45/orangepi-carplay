#ifndef AIRPLAY_INPUT_H
#define AIRPLAY_INPUT_H

#include <stddef.h>
#include <stdint.h>

enum airplay_input_kind {
    AIRPLAY_INPUT_OTHER = 0,
    AIRPLAY_INPUT_NIGHT_MODE,
    AIRPLAY_INPUT_FORCE_KEY_FRAME,
    AIRPLAY_INPUT_HID_REPORT,
    AIRPLAY_INPUT_SIRI
};

struct airplay_input_touch {
    int down;
    uint16_t x;
    uint16_t y;
};

struct airplay_input_command {
    enum airplay_input_kind kind;
    int boolean_value;
    unsigned siri_action;
    char uuid[65];
    const uint8_t *report;
    size_t report_len;
};

/* One new keypress derived from a standard boot-keyboard HID report. */
struct airplay_input_keyboard_change {
    int character; /* Printable ASCII, or 0 when keycode is used. */
    int keycode;
};

/* Returns 1 for a complete control plist and 0 for malformed/truncated data. */
int airplay_input_parse(const uint8_t *data, size_t len,
                        struct airplay_input_command *command);
int airplay_input_touch_point(const uint8_t *report, size_t report_len,
                              struct airplay_input_touch *touch);
/* Legacy Consumer report fallback only when /info has no usable descriptor. */
int airplay_input_media_keycode(unsigned report_id);
int airplay_input_knob_keycode(unsigned buttons, signed char x,
                               signed char y, signed char wheel);
/*
 * Compares two eight-byte boot-keyboard reports and returns new non-modifier
 * key presses. Releases intentionally produce no input because Android input
 * keyevent injects a complete down/up pair.
 */
int airplay_input_keyboard_changed(
    const uint8_t *previous, size_t previous_len,
    const uint8_t *current, size_t current_len,
    struct airplay_input_keyboard_change *changes, size_t capacity);

#endif
