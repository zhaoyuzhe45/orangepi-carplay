#include "airplay_input.h"
#include "airplay_bplist.h"

#include <string.h>

int airplay_input_parse(const uint8_t *data, size_t len,
                        struct airplay_input_command *command)
{
    struct airplay_bplist_document document;
    struct airplay_bplist_value root;
    struct airplay_bplist_value value;
    struct airplay_bplist_value params;
    size_t type_len = 0;

    if (!command)
        return 0;
    memset(command, 0, sizeof(*command));
    if (!data || !len ||
        airplay_bplist_init(&document, data, len) < 0 ||
        airplay_bplist_root(&document, &root) < 0 ||
        root.type != AIRPLAY_BPLIST_DICTIONARY ||
        airplay_bplist_dict_get(&document, &root, "type", &value) != 1 ||
        value.type != AIRPLAY_BPLIST_STRING ||
        airplay_bplist_string_copy(&document, &value, command->uuid,
                                   sizeof(command->uuid), &type_len) < 0)
        return 0;

    if (airplay_bplist_string_equals(&document, &value, "hidSendReport")) {
        command->kind = AIRPLAY_INPUT_HID_REPORT;
        if (airplay_bplist_dict_get(&document, &root, "uuid", &value) != 1 ||
            value.type != AIRPLAY_BPLIST_STRING ||
            airplay_bplist_string_copy(&document, &value, command->uuid,
                                       sizeof(command->uuid), &type_len) < 0 ||
            airplay_bplist_dict_get(&document, &root, "hidReport", &value) != 1 ||
            value.type != AIRPLAY_BPLIST_DATA || value.count == 0 ||
            value.count > SIZE_MAX || value.data_offset > len ||
            value.count > len - value.data_offset)
            return 0;
        command->report = data + value.data_offset;
        command->report_len = (size_t)value.count;
        return 1;
    }
    if (airplay_bplist_string_equals(&document, &value, "setNightMode")) {
        command->kind = AIRPLAY_INPUT_NIGHT_MODE;
        if (airplay_bplist_dict_get(&document, &root, "params", &params) != 1 ||
            params.type != AIRPLAY_BPLIST_DICTIONARY ||
            airplay_bplist_dict_get(&document, &params, "nightMode", &value) != 1 ||
            airplay_bplist_get_bool(&document, &value,
                                    &command->boolean_value) < 0)
            return 0;
        return 1;
    }
    if (airplay_bplist_string_equals(&document, &value, "requestSiri")) {
        uint64_t action;
        if(airplay_bplist_dict_get(&document,&root,"params",&params)!=1 ||
           params.type!=AIRPLAY_BPLIST_DICTIONARY ||
           airplay_bplist_dict_get(&document,&params,"siriAction",&value)!=1 ||
           value.type!=AIRPLAY_BPLIST_INTEGER ||
           airplay_bplist_get_uint64(&document,&value,&action)<0 || action<1 || action>3)return 0;
        command->kind=AIRPLAY_INPUT_SIRI;command->siri_action=(unsigned)action;return 1;
    }
    if (airplay_bplist_string_equals(&document, &value, "forceKeyFrame")) {
        command->kind = AIRPLAY_INPUT_FORCE_KEY_FRAME;
        return 1;
    }

    command->kind = AIRPLAY_INPUT_OTHER;
    return 1;
}int airplay_input_touch_point(const uint8_t *report, size_t report_len,
                              struct airplay_input_touch *touch)
{
    if (!report || report_len < 6 || !touch)
        return 0;
    touch->down = (report[1] & 0x01) != 0;
    touch->x = (uint16_t)(report[2] | ((uint16_t)report[3] << 8));
    touch->y = (uint16_t)(report[4] | ((uint16_t)report[5] << 8));
    return 1;
}


static int keyboard_key_was_down(const uint8_t *report, unsigned usage)
{
    size_t i;

    for (i = 2; i < 8; i++) {
        if (report[i] == usage)
            return 1;
    }
    return 0;
}

static int keyboard_shift(const uint8_t *report)
{
    return (report[0] & 0x22u) != 0;
}

static int keyboard_caps_lock(const uint8_t *report)
{
    (void)report;
    return 0;
}

static int keyboard_character(unsigned usage, int shift, int caps_lock)
{
    static const char letters[] = "abcdefghijklmnopqrstuvwxyz";
    static const char unshifted_digits[] = "1234567890";
    static const char shifted_digits[] = "!@#$%^&*()";
    int index;

    if (usage >= 0x04u && usage <= 0x1du) {
        int upper = shift ^ caps_lock;
        char letter = letters[usage - 0x04u];
        return upper ? letter - 'a' + 'A' : letter;
    }
    if (usage >= 0x1eu && usage <= 0x27u) {
        index = (int)(usage - 0x1eu);
        return (unsigned char)(shift ? shifted_digits[index] :
                                          unshifted_digits[index]);
    }
    switch (usage) {
    case 0x2c: return ' ';
    case 0x2d: return shift ? '_' : '-';
    case 0x2e: return shift ? '+' : '=';
    case 0x2f: return shift ? '{' : '[';
    case 0x30: return shift ? '}' : ']';
    case 0x31: return shift ? '|' : 0x5c;
    case 0x33: return shift ? ':' : ';';
    case 0x34: return shift ? '\"' : '\'';
    case 0x35: return shift ? '~' : 0x60;
    case 0x36: return shift ? '<' : ',';
    case 0x37: return shift ? '>' : '.';
    case 0x38: return shift ? '?' : '/';
    default: return 0;
    }
}

static int keyboard_android_keycode(unsigned usage)
{
    if (usage >= 0x04u && usage <= 0x1du)
        return 29 + (int)(usage - 0x04u);
    if (usage >= 0x1eu && usage <= 0x27u) {
        int index = (int)(usage - 0x1eu);
        return index == 9 ? 7 : 8 + index;
    }
    if (usage >= 0x3au && usage <= 0x45u)
        return 131 + (int)(usage - 0x3au);
    switch (usage) {
    case 0x28: return 66;
    case 0x29: return 111;
    case 0x2a: return 67;
    case 0x2b: return 61;
    case 0x2c: return 62;
    case 0x2du: return 69;
    case 0x2eu: return 70;
    case 0x2fu: return 71;
    case 0x30: return 72;
    case 0x31: return 73;
    case 0x33: return 74;
    case 0x34: return 75;
    case 0x35: return 68;
    case 0x36: return 55;
    case 0x37: return 56;
    case 0x38: return 76;
    case 0x39: return 115;
    case 0x46: return 120;
    case 0x47: return 116;
    case 0x48: return 121;
    case 0x49: return 124;
    case 0x4a: return 122;
    case 0x4b: return 92;
    case 0x4c: return 112;
    case 0x4d: return 123;
    case 0x4e: return 93;
    case 0x4f: return 22;
    case 0x50: return 21;
    case 0x51: return 20;
    case 0x52: return 19;
    case 0x53: return 143;
    case 0xe0: return 113;
    case 0xe1: return 59;
    case 0xe2: return 57;
    case 0xe3: return 117;
    case 0xe4: return 114;
    case 0xe5: return 60;
    case 0xe6: return 58;
    case 0xe7: return 118;
    default: return 0;
    }
}

int airplay_input_keyboard_changed(
    const uint8_t *previous, size_t previous_len,
    const uint8_t *current, size_t current_len,
    struct airplay_input_keyboard_change *changes, size_t capacity)
{
    size_t i;
    size_t count = 0;
    int shift;
    int caps_lock;

    if (!previous || previous_len < 8 || !current || current_len < 8 ||
        (!changes && capacity))
        return -1;
    shift = keyboard_shift(current);
    caps_lock = keyboard_caps_lock(current);
    for (i = 2; i < 8; i++) {
        unsigned usage = current[i];
        int character;
        int keycode;

        if (!usage || keyboard_key_was_down(previous, usage))
            continue;
        character = keyboard_character(usage, shift, caps_lock);
        keycode = character ? 0 : keyboard_android_keycode(usage);
        if (!character && !keycode)
            continue;
        if (count >= capacity)
            return (int)count;
        changes[count].character = character;
        changes[count].keycode = keycode;
        count++;
    }
    return (int)count;
}

/* Compatibility fallback; descriptor-backed Consumer arrays are decoded in
 * carplay_capabilities.c so each receiver keeps its declared Usage order. */
int airplay_input_media_keycode(unsigned report_id)
{
    switch (report_id) {
    case 1: return 126; /* Android KEYCODE_MEDIA_PLAY */
    case 2: return 127; /* Android KEYCODE_MEDIA_PAUSE */
    case 3: return 85;  /* Android KEYCODE_MEDIA_PLAY_PAUSE */
    case 4: return 87;  /* Android KEYCODE_MEDIA_NEXT */
    case 5: return 88;  /* Android KEYCODE_MEDIA_PREVIOUS */
    default: return 0;
    }
}int airplay_input_knob_keycode(unsigned buttons, signed char x,
                               signed char y, signed char wheel)
{
    if (buttons & 0x01)
        return 66; /* Android KEYCODE_ENTER */
    if (buttons & 0x02)
        return 3;  /* Android KEYCODE_HOME */
    if (buttons & 0x04)
        return 4;  /* Android KEYCODE_BACK */
    if (x < 0)
        return 21; /* Android KEYCODE_DPAD_LEFT */
    if (x > 0)
        return 22; /* Android KEYCODE_DPAD_RIGHT */
    if (y < 0)
        return 19; /* Android KEYCODE_DPAD_UP */
    if (y > 0)
        return 20; /* Android KEYCODE_DPAD_DOWN */
    if (wheel < 0)
        return 21; /* Android KEYCODE_DPAD_LEFT */
    if (wheel > 0)
        return 22; /* Android KEYCODE_DPAD_RIGHT */
    return 0;
}
