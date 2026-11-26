#ifndef CARPLAY_CAPABILITIES_H
#define CARPLAY_CAPABILITIES_H

#include <stddef.h>
#include <stdint.h>

#define CARPLAY_MAX_HID_DEVICES 12u
#define CARPLAY_UUID_CAPACITY 65u
#define CARPLAY_MAX_AUDIO_FORMATS 16u

/* Telephone controls are decoded from the receiver's descriptor, never from
 * assumed byte positions. Unsupported layouts remain disabled. */
#define CARPLAY_PHONE_HID_REPORTS 8u
#define CARPLAY_PHONE_HID_FIELDS 16u
struct carplay_phone_hid {
    unsigned valid, report_count, field_count;
    struct { uint16_t bits; uint8_t id; } reports[CARPLAY_PHONE_HID_REPORTS];
    struct { uint16_t offset; uint8_t size, report, action; } fields[CARPLAY_PHONE_HID_FIELDS];
};
struct carplay_phone_hid_state { uint8_t pressed[CARPLAY_PHONE_HID_REPORTS]; };
int carplay_phone_hid_parse(const uint8_t *data, size_t len, struct carplay_phone_hid *out);
int carplay_phone_hid_owns_report(const struct carplay_phone_hid *,const uint8_t *,size_t);
/* Returns rising-edge mask (1 Hook, 2 Drop), or -1 without changing state. */
int carplay_phone_hid_press(const struct carplay_phone_hid *layout,
    struct carplay_phone_hid_state *state, const uint8_t *report, size_t len);

enum carplay_hid_kind {
    CARPLAY_HID_UNKNOWN = 0,
    CARPLAY_HID_TOUCH,
    CARPLAY_HID_KEYBOARD,
    CARPLAY_HID_KNOB,
    CARPLAY_HID_MEDIA,
    CARPLAY_HID_TELEPHONY
};

struct carplay_display_capabilities {
    int present;
    int width_pixels;
    int height_pixels;
    int width_physical_mm;
    int height_physical_mm;
    int max_fps;
    int primary_input_device;
    uint64_t features;
    char uuid[CARPLAY_UUID_CAPACITY];
};

struct carplay_hid_device_capabilities {
    int present;
    enum carplay_hid_kind kind;
    uint16_t vendor_id;
    uint16_t product_id;
    uint16_t touch_x_max;
    uint16_t touch_y_max;
    size_t descriptor_len;
    /* Lookup table for a descriptor-backed, single-byte Consumer array. */
    struct {
        int valid;
        uint8_t keycodes[256];
    } media;
    struct carplay_phone_hid phone;
    char uuid[CARPLAY_UUID_CAPACITY];
};

struct carplay_audio_format_capabilities {
    int present;
    int type;
    char audio_type[CARPLAY_UUID_CAPACITY];
    uint32_t output_formats;
    uint32_t input_formats;
};

struct carplay_audio_capabilities {
    int description_present;
    int media_output;
    int microphone_input;
    size_t format_count;
    struct carplay_audio_format_capabilities formats[CARPLAY_MAX_AUDIO_FORMATS];
};

struct carplay_display_plan {
    /* Receiver canvas/input pixels; physical_* is the closest preset source. */
    unsigned width;
    unsigned height;
    /* Exact encoded canvas; only codec allocation padding is removed. */
    unsigned output_width;
    unsigned output_height;
    unsigned fps;
    unsigned density;
    unsigned physical_width;
    unsigned physical_height;
    unsigned physical_fps;
    int force_physical;
    int negotiated;
    int density_automatic;
    int stretched;
};

struct carplay_session_capabilities {
    struct carplay_display_capabilities display;
    struct carplay_hid_device_capabilities hid[CARPLAY_MAX_HID_DEVICES];
    size_t hid_count;
    struct carplay_audio_capabilities audio;
    int has_touch;
    int has_keyboard;
    int has_knob;
    int has_media;
    int has_telephony;
    int appearance_known;
    int night_mode;
    /* The receiver supplied an OEM car icon in /info. */
    int oem_icon_present;
    /* The receiver accepts requestUI for returning from CarPlay to its own UI. */
    int request_car_ui_supported;
};

/*
 * Parses the AirPlay /info binary property list returned by the connected
 * receiver. Missing optional capabilities are cleared and reported through
 * has_* flags. The function returns 0 for a valid plist, even when an optional
 * display entry is invalid or absent.
 */
int carplay_capabilities_parse(const uint8_t *data, size_t len,
                               struct carplay_session_capabilities *capabilities);
const struct carplay_hid_device_capabilities *carplay_capabilities_find_hid(
    const struct carplay_session_capabilities *capabilities,
    const char *uuid);
/*
 * Selects an even-pixel output size. Returns 1 for a negotiated display and 0
 * when the explicit local fallback was used.
 */
/*
 * Selects a format supported by both the receiver and this box. LPCM is chosen
 * for wired CarPlay; 0 means the requested type/audioType is unavailable.
 */
uint32_t carplay_capabilities_select_audio_output(
    const struct carplay_session_capabilities *capabilities,
    int type, const char *audio_type);
uint32_t carplay_capabilities_select_audio_input(
    const struct carplay_session_capabilities *capabilities,
    int type, const char *audio_type);
int carplay_capabilities_output_size(
    const struct carplay_session_capabilities *capabilities,
    unsigned fallback_width, unsigned fallback_height,
    unsigned *width, unsigned *height);
/* 0 exact, 1 closest preset, -1 invalid. Squared relative axis error, with
 * smaller pixel area breaking ties. Output canvas itself is never replaced. */
int carplay_capabilities_display_preset(unsigned width, unsigned height,
    unsigned *physical_width, unsigned *physical_height);
/*
 * Builds the exact dimensions, density and FPS used by the native video path.
 * Density 0 means automatic; automatic maps 1080p to 280 and 480p to 160.
 * Width/height overrides are zero together, or a validated administrator size.
 */
/*
 * Fits the capture size inside the hardware AVC rectangle without changing
 * aspect ratio. Dimensions are rounded to the encoder alignment when a
 * negotiated display cannot be encoded natively.
 */
int carplay_capabilities_capture_size(
    unsigned width, unsigned height,
    unsigned maximum_width, unsigned maximum_height,
    unsigned alignment_width, unsigned alignment_height,
    unsigned *capture_width, unsigned *capture_height);
int carplay_capabilities_display_plan(
    const struct carplay_session_capabilities *capabilities,
    unsigned fallback_width, unsigned fallback_height,
    int override_width, int override_height,
    int physical_width, int physical_height, int density_override,
    unsigned configured_fps, struct carplay_display_plan *plan);

#endif
