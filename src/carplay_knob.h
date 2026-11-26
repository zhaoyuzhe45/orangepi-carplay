#ifndef CARPLAY_KNOB_H
#define CARPLAY_KNOB_H
#include <stddef.h>
#include <stdint.h>

#define CARPLAY_KNOB_REPORTS 8u
#define CARPLAY_KNOB_FIELDS 32u
#define CARPLAY_KEY_BATCH_ENTRIES CARPLAY_KNOB_FIELDS
struct carplay_key_batch {
    unsigned count;
    struct { int keycode; uint32_t count; } entries[CARPLAY_KEY_BATCH_ENTRIES];
};
/* Bounded descriptor layout. Offsets exclude the optional report-ID byte. */
struct carplay_knob_layout {
    unsigned valid, report_count, field_count;
    struct { uint16_t bits; uint8_t id; } reports[CARPLAY_KNOB_REPORTS];
    struct {
        uint16_t offset;
        uint8_t size, report, action, relative;
        int32_t minimum, maximum;
    } fields[CARPLAY_KNOB_FIELDS];
};
/* One zero-initialized state per receiver HID device. */
struct carplay_knob_state { int32_t values[CARPLAY_KNOB_FIELDS]; };
int carplay_knob_parse(const uint8_t *, size_t, struct carplay_knob_layout *);
int carplay_knob_owns_report(const struct carplay_knob_layout *, const uint8_t *, size_t);
/* Valid report => 0, including idle. Invalid => -1, state unchanged and batch empty.
 * Entries preserve descriptor field order. Relative magnitudes retain every step;
 * absolute direction changes and button rising edges each emit one action. */
int carplay_knob_decode(const struct carplay_knob_layout *, struct carplay_knob_state *,
    const uint8_t *, size_t, struct carplay_key_batch *);
#endif
