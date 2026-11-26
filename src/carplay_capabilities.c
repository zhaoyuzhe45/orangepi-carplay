#include "carplay_capabilities.h"
#include "airplay_bplist.h"
#include "carplay_encoder.h"

#include <limits.h>
#include <string.h>

static int get_u64(const struct airplay_bplist_document *document,
                   const struct airplay_bplist_value *dictionary,
                   const char *key, uint64_t *number)
{
    struct airplay_bplist_value value;

    if (airplay_bplist_dict_get(document, dictionary, key, &value) != 1)
        return 0;
    return airplay_bplist_get_uint64(document, &value, number) == 0 ? 1 : -1;
}

/* Some receivers encode audio type/masks as ASCII decimal or 0x strings.
 * Keep this compatibility local to audio; display/HID parsing stays numeric.
 * Parse the whole counted string so signs, whitespace, embedded NUL, suffixes
 * and overflow cannot be accepted by a permissive strtoull conversion. */
static int get_audio_number(const struct airplay_bplist_document *document,
                             const struct airplay_bplist_value *dictionary,
                             const char *key, uint64_t maximum,
                             uint64_t *number)
{
    struct airplay_bplist_value value;
    uint64_t parsed = 0;
    int found = airplay_bplist_dict_get(document, dictionary, key, &value);
    if (found != 1)
        return found;
    if (value.type == AIRPLAY_BPLIST_INTEGER) {
        if (airplay_bplist_get_uint64(document, &value, &parsed) < 0 || parsed > maximum)
            return -1;
    } else {
        if (value.type != AIRPLAY_BPLIST_STRING || value.string_unit_size != 1 ||
            !value.count || value.count > SIZE_MAX ||
            value.data_offset > document->len || value.count > document->len - value.data_offset)
            return -1;
        const uint8_t *text = document->data + value.data_offset;
        size_t length = (size_t)value.count, index = 0;
        unsigned base = 10;
        if (length >= 2 && text[0] == '0' && (text[1] == 'x' || text[1] == 'X')) {
            base = 16;
            index = 2;
        }
        if (index == length)
            return -1;
        for (; index < length; index++) {
            unsigned digit;
            if (text[index] >= '0' && text[index] <= '9') digit = text[index] - '0';
            else if (text[index] >= 'a' && text[index] <= 'f') digit = text[index] - 'a' + 10u;
            else if (text[index] >= 'A' && text[index] <= 'F') digit = text[index] - 'A' + 10u;
            else return -1;
            if (digit >= base || digit > maximum || parsed > (maximum - digit) / base)
                return -1;
            parsed = parsed * base + digit;
        }
    }
    *number = parsed;
    return 1;
}

static int get_i32(const struct airplay_bplist_document *document,
                   const struct airplay_bplist_value *dictionary,
                   const char *key, int *number)
{
    uint64_t value;

    if (get_u64(document, dictionary, key, &value) != 1 ||
        value > INT_MAX)
        return 0;
    *number = (int)value;
    return 1;
}

static int copy_string(const struct airplay_bplist_document *document,
                       const struct airplay_bplist_value *dictionary,
                       const char *key, char *output, size_t capacity)
{
    struct airplay_bplist_value value;
    size_t length = 0;

    if (airplay_bplist_dict_get(document, dictionary, key, &value) != 1 ||
        airplay_bplist_string_copy(document, &value, output, capacity,
                                   &length) < 0)
        return 0;
    return 1;
}

static uint32_t descriptor_unsigned(const uint8_t *data, size_t length)
{
    uint32_t value = 0;
    size_t i;

    for (i = 0; i < length; i++)
        value |= (uint32_t)data[i] << (8u * i);
    return value;
}

int carplay_phone_hid_parse(const uint8_t *data, size_t len, struct carplay_phone_hid *out)
{
    struct globals { uint32_t page, size, count, id, min, max; } g={0}, stack[4];
    struct carplay_phone_hid p={0};
    uint32_t usages[64], range_min=0, range_max=0;
    unsigned usage_count=0, have_min=0, have_max=0, depth=0, collections=0;
    size_t offset=0;
    if(!out)return -1;
    memset(out,0,sizeof(*out));
    if(!data || !len || len>4096)return -1;
    while(offset<len) {
        uint8_t prefix=data[offset++];
        unsigned size=prefix&3u, type=(prefix>>2)&3u, tag=prefix>>4;
        if(prefix==0xfe)return -1; /* Long items cannot be interpreted safely. */
        if(size==3)size=4;
        if(size>len-offset)return -1;
        uint32_t value=descriptor_unsigned(data+offset,size);offset+=size;
        if(type==1) {
            switch(tag) {
            case 0:g.page=value;break;
            case 1:g.min=value;break;
            case 2:g.max=value;break;
            case 7:g.size=value;break;
            case 8:if(!value || value>255)return -1;g.id=value;break;
            case 9:g.count=value;break;
            case 10:if(depth==4)return -1;stack[depth++]=g;break;
            case 11:if(!depth)return -1;g=stack[--depth];break;
            default:break;
            }
        } else if(type==2) {
            uint32_t usage=size==4?value:(g.page<<16)|value;
            if(tag==0) { if(usage_count==64)return -1;usages[usage_count++]=usage; }
            else if(tag==1) { range_min=usage;have_min=1; }
            else if(tag==2) { range_max=usage;have_max=1; }
            else if(tag==10)return -1; /* Alternative usage sets. */
        } else if(type==0) {
            if(tag==10) { if(++collections>32)return -1; }
            else if(tag==12) { if(!collections)return -1;collections--; }
            else if(tag==8) {
                if(!g.size || g.size>32 || !g.count || g.count>512/g.size)return -1;
                unsigned r=0;
                while(r<p.report_count && p.reports[r].id!=g.id)r++;
                if(r==p.report_count) {
                    if(r==CARPLAY_PHONE_HID_REPORTS)return -1;
                    p.reports[r].id=(uint8_t)g.id;p.report_count++;
                }
                unsigned start=p.reports[r].bits, bits=g.size*g.count;
                if(start+bits>512)return -1;
                p.reports[r].bits=(uint16_t)(start+bits);
                if(have_min!=have_max || (have_min && (range_max<range_min || range_max-range_min>511)))return -1;
                if(!(value&1u))for(unsigned i=0;i<g.count;i++) {
                    uint32_t u=i<usage_count?usages[i]:have_min?
                        range_min+(i<range_max-range_min?i:range_max-range_min):
                        usage_count?usages[usage_count-1]:0;
                    unsigned action=u==0x000b0020?1:u==0x000b0026?2:0;
                    if(!action)continue;
                    if((value&7u)!=2 || g.min!=0 || g.max!=1 || g.size>16 ||
                        p.field_count==CARPLAY_PHONE_HID_FIELDS)return -1;
                    unsigned f=p.field_count++;
                    p.fields[f].offset=(uint16_t)(start+i*g.size);
                    p.fields[f].size=(uint8_t)g.size;p.fields[f].report=(uint8_t)r;
                    p.fields[f].action=(uint8_t)action;
                }
            }
            usage_count=have_min=have_max=0;
        }
    }
    if(depth || collections || !p.field_count)return -1;
    for(unsigned r=0;r<p.report_count;r++)
        if(!p.reports[r].id && p.report_count>1)return -1;
    p.valid=1;*out=p;return 0;
}

int carplay_phone_hid_owns_report(const struct carplay_phone_hid *p,const uint8_t *report,size_t len)
{
    if(!p || !p->valid || !report || !len)return 0;
    for(unsigned f=0;f<p->field_count;f++){
        uint8_t id=p->reports[p->fields[f].report].id;
        if(!id || report[0]==id)return 1;
    }
    return 0;
}

int carplay_phone_hid_press(const struct carplay_phone_hid *p,
    struct carplay_phone_hid_state *state, const uint8_t *report, size_t len)
{
    if(!p || !state || !report || !len || !p->valid)return -1;
    unsigned r=0;
    if(p->reports[0].id) {
        while(r<p->report_count && p->reports[r].id!=report[0])r++;
        if(r==p->report_count)return -1;
        report++;len--;
    }
    if(len!=(p->reports[r].bits+7u)/8u)return -1;
    unsigned pressed=0;
    for(unsigned f=0;f<p->field_count;f++) {
        if(p->fields[f].report!=r)continue;
        unsigned value=0;
        for(unsigned b=0;b<p->fields[f].size;b++) {
            unsigned bit=p->fields[f].offset+b;
            value|=((report[bit/8]>>(bit%8))&1u)<<b;
        }
        if(value>1)return -1;
        if(value)pressed|=p->fields[f].action;
    }
    unsigned edge=pressed&~state->pressed[r];state->pressed[r]=(uint8_t)pressed;
    return (int)edge;
}

static uint8_t consumer_keycode(uint32_t usage)
{
    switch (usage) {
    case 0x0c00b0: return 126; /* Play */
    case 0x0c00b1: return 127; /* Pause */
    case 0x0c00cd: return 85;  /* Play/Pause */
    case 0x0c00b5: return 87;  /* Scan Next Track */
    case 0x0c00b6: return 88;  /* Scan Previous Track */
    default: return 0;
    }
}

/* This path handles the single, unnumbered byte array used by Audi and
 * DiPlay. Do not apply the table to bitmaps, report IDs, or packed reports. */
static void parse_media_array(const uint8_t *data, size_t len,
                              struct carplay_hid_device_capabilities *device)
{
    uint8_t keys[256] = {0};
    size_t offset = 0, count = 0, depth = 0;
    uint32_t page = 0, minimum = 256, maximum = 256;
    uint32_t report_size = 0, report_count = 0;
    int have_input = 0;

    while (offset < len) {
        uint8_t prefix = data[offset++];
        unsigned size = prefix & 3u;
        unsigned type = (prefix >> 2) & 3u;
        unsigned tag = prefix >> 4;
        uint32_t value;

        if (size == 3) size = 4;
        if (prefix == 0xfe || size > len - offset) return;
        value = descriptor_unsigned(data + offset, size);
        offset += size;

        if (type == 1) {
            switch (tag) {
            case 0: page = value; break;
            case 1: minimum = value; break;
            case 2: maximum = value; break;
            case 7: report_size = value; break;
            case 9: report_count = value; break;
            case 8: case 10: case 11: return; /* Report ID, Push, Pop */
            }
        } else if (type == 2 && depth && !have_input) {
            if (tag == 1 || tag == 2 || tag == 10) return; /* Ranges/delimiters */
            if (tag != 0) continue;
            if (!size || count == sizeof(keys) || (size != 4 && page > UINT16_MAX))
                return;
            keys[count++] = consumer_keycode(size == 4 ? value : (page << 16) | value);
        } else if (type == 0) {
            if (tag == 8) {
                /* Data, Array, Absolute (optionally Null State). */
                if (have_input || !depth || !size || (value & ~0x40u) ||
                    report_size != 8 || report_count != 1 || minimum > maximum ||
                    maximum >= sizeof(keys) || count != maximum - minimum + 1u)
                    return;
                memcpy(device->media.keycodes + minimum, keys, count);
                have_input = 1;
            } else if (tag == 10) {
                if (!depth && (page != 0x0cu || value != 1)) return;
                if (count && !have_input) return;
                depth++;
            } else if (tag == 12) {
                if (!depth || (count && !have_input)) return;
                depth--;
            } else if (tag == 9 || tag == 11) {
                if (count && !have_input) return;
            }
            /* A reserved zero byte is present in observed descriptors. */
        }
    }
    if (!depth && have_input)
        device->media.valid = 1;
}

static void classify_descriptor(
    const uint8_t *data, size_t len,
    struct carplay_hid_device_capabilities *device)
{
    size_t offset = 0;
    unsigned usage_page = 0;
    unsigned usage = 0;
    unsigned logical_max = 0;
    int saw_keyboard = 0;
    int saw_touch = 0;
    int saw_knob = 0;
    int saw_media = 0;
    int saw_telephony = 0;

    while (data && offset < len) {
        uint8_t prefix = data[offset++];
        size_t value_size = (size_t)(prefix & 0x03);
        unsigned type = (prefix >> 2) & 0x03;
        unsigned tag = prefix >> 4;
        uint32_t value = 0;

        if (value_size == 3)
            value_size = 4;
        if (value_size > len - offset)
            break;
        if (value_size)
            value = descriptor_unsigned(data + offset, value_size);

        if (type == 2 && tag == 0 && value_size) {
            usage = value;
            if (usage_page == 0x01u && usage == 0x06u)
                saw_keyboard = 1;
            if (usage_page == 0x0du && usage == 0x04u)
                saw_touch = 1;
            if (usage_page == 0x01u && usage == 0x08u)
                saw_knob = 1;
            if (usage_page == 0x0cu && (usage == 0x01u || usage == 0x00u))
                saw_media = 1;
            if (usage_page == 0x0bu && (usage == 0x07u || usage == 0x11u))
                saw_telephony = 1;
        } else if (type == 1) {
            if (tag == 0 && value_size)
                usage_page = value;
            else if (tag == 2 && value_size)
                logical_max = value;
        } else if (type == 0 && tag == 8) {
            unsigned full_usage = usage;
            if (value_size == 4)
                full_usage = usage & 0xffffu;
            if (usage_page == 0x01u && full_usage == 0x06u)
                saw_keyboard = 1;
            if (usage_page == 0x0du && full_usage == 0x04u)
                saw_touch = 1;
            if (usage_page == 0x01u && full_usage == 0x08u)
                saw_knob = 1;
            if (usage_page == 0x0cu && (full_usage == 0x01u || full_usage == 0x00u))
                saw_media = 1;
            if (usage_page == 0x0bu && (full_usage == 0x07u || full_usage == 0x11u))
                saw_telephony = 1;
            if (usage_page == 0x01u && full_usage == 0x30u &&
                logical_max > 0 && logical_max <= UINT16_MAX)
                device->touch_x_max = (uint16_t)logical_max;
            if (usage_page == 0x01u && full_usage == 0x31u &&
                logical_max > 0 && logical_max <= UINT16_MAX)
                device->touch_y_max = (uint16_t)logical_max;
        }
        offset += value_size;
    }

    if (saw_touch)
        device->kind = CARPLAY_HID_TOUCH;
    else if (saw_keyboard)
        device->kind = CARPLAY_HID_KEYBOARD;
    else if (saw_knob)
        device->kind = CARPLAY_HID_KNOB;
    else if (saw_media)
        device->kind = CARPLAY_HID_MEDIA;
    else if (saw_telephony)
        device->kind = CARPLAY_HID_TELEPHONY;
    else
        device->kind = CARPLAY_HID_UNKNOWN;
}

static void parse_display(const struct airplay_bplist_document *document,
                          const struct airplay_bplist_value *root,
                          struct carplay_session_capabilities *capabilities)
{
    struct airplay_bplist_value displays;
    struct airplay_bplist_value display;
    uint64_t count = 0;
    uint64_t number = 0;
    int width = 0;
    int height = 0;

    if (airplay_bplist_dict_get(document, root, "displays", &displays) != 1 ||
        displays.type != AIRPLAY_BPLIST_ARRAY ||
        airplay_bplist_array_count(document, &displays, &count) != 0 ||
        count == 0 ||
        airplay_bplist_array_get(document, &displays, 0, &display) != 0 ||
        display.type != AIRPLAY_BPLIST_DICTIONARY ||
        get_i32(document, &display, "widthPixels", &width) != 1 ||
        get_i32(document, &display, "heightPixels", &height) != 1 ||
        width <= 0 || height <= 0)
        return;

    capabilities->display.present = 1;
    capabilities->display.width_pixels = width;
    capabilities->display.height_pixels = height;
    (void)copy_string(document, &display, "uuid",
                      capabilities->display.uuid,
                      sizeof(capabilities->display.uuid));
    (void)get_i32(document, &display, "widthPhysical",
                  &capabilities->display.width_physical_mm);
    (void)get_i32(document, &display, "heightPhysical",
                  &capabilities->display.height_physical_mm);
    (void)get_i32(document, &display, "maxFPS",
                  &capabilities->display.max_fps);
    (void)get_i32(document, &display, "primaryInputDevice",
                  &capabilities->display.primary_input_device);
    if (get_u64(document, &display, "features", &number) == 1)
        capabilities->display.features = number;
}

static void parse_hid_devices(const struct airplay_bplist_document *document,
                              const struct airplay_bplist_value *root,
                              struct carplay_session_capabilities *capabilities)
{
    struct airplay_bplist_value devices;
    uint64_t count = 0;
    uint64_t i;

    if (airplay_bplist_dict_get(document, root, "hidDevices", &devices) != 1 ||
        devices.type != AIRPLAY_BPLIST_ARRAY ||
        airplay_bplist_array_count(document, &devices, &count) != 0)
        return;

    for (i = 0; i < count && capabilities->hid_count < CARPLAY_MAX_HID_DEVICES; i++) {
        struct airplay_bplist_value dictionary;
        struct airplay_bplist_value descriptor;
        struct carplay_hid_device_capabilities *device =
            &capabilities->hid[capabilities->hid_count];
        uint64_t number = 0;

        if (airplay_bplist_array_get(document, &devices, (size_t)i,
                                     &dictionary) != 0 ||
            dictionary.type != AIRPLAY_BPLIST_DICTIONARY ||
            airplay_bplist_dict_get(document, &dictionary, "hidDescriptor",
                                    &descriptor) != 1 ||
            descriptor.type != AIRPLAY_BPLIST_DATA ||
            descriptor.count > SIZE_MAX ||
            descriptor.data_offset > document->len ||
            descriptor.count > document->len - descriptor.data_offset)
            continue;

        device->present = 1;
        device->descriptor_len = (size_t)descriptor.count;
        (void)copy_string(document, &dictionary, "uuid", device->uuid,
                          sizeof(device->uuid));
        if (get_u64(document, &dictionary, "hidVendorID", &number) == 1 &&
            number <= UINT16_MAX)
            device->vendor_id = (uint16_t)number;
        if (get_u64(document, &dictionary, "hidProductID", &number) == 1 &&
            number <= UINT16_MAX)
            device->product_id = (uint16_t)number;
        classify_descriptor(document->data + descriptor.data_offset,
                            (size_t)descriptor.count, device);
        if (device->kind == CARPLAY_HID_MEDIA)
            parse_media_array(document->data + descriptor.data_offset,
                              (size_t)descriptor.count, device);
        if(carplay_phone_hid_parse(document->data + descriptor.data_offset,
                (size_t)descriptor.count,&device->phone)==0)
            capabilities->has_telephony=1;
        capabilities->hid_count++;
        if (device->kind == CARPLAY_HID_TOUCH)
            capabilities->has_touch = 1;
        else if (device->kind == CARPLAY_HID_KEYBOARD)
            capabilities->has_keyboard = 1;
        else if (device->kind == CARPLAY_HID_KNOB)
            capabilities->has_knob = 1;
        else if (device->kind == CARPLAY_HID_MEDIA)
            capabilities->has_media = 1;
        else if (device->kind == CARPLAY_HID_TELEPHONY)
            capabilities->has_telephony = 1;
    }
}

static void parse_audio_array(const struct airplay_bplist_document *document,
                              const struct airplay_bplist_value *root,
                              const char *key,
                              struct carplay_session_capabilities *capabilities)
{
    struct airplay_bplist_value formats;
    uint64_t count = 0;
    uint64_t i;

    if (airplay_bplist_dict_get(document, root, key, &formats) != 1 ||
        formats.type != AIRPLAY_BPLIST_ARRAY ||
        airplay_bplist_array_count(document, &formats, &count) != 0)
        return;

    capabilities->audio.description_present = 1;
    for (i = 0; i < count; i++) {
        struct airplay_bplist_value entry;
        struct airplay_bplist_value value;
        struct carplay_audio_format_capabilities *format = NULL;
        uint64_t number = 0;
        size_t audio_type_len = 0;

        if (airplay_bplist_array_get(document, &formats, (size_t)i, &entry) != 0 ||
            entry.type != AIRPLAY_BPLIST_DICTIONARY)
            continue;
        if (strcmp(key, "audioFormats") == 0 &&
            capabilities->audio.format_count < CARPLAY_MAX_AUDIO_FORMATS) {
            format = &capabilities->audio.formats[
                capabilities->audio.format_count];
            memset(format, 0, sizeof(*format));
            format->present = 1;
            if (get_audio_number(document, &entry, "type", INT_MAX, &number) == 1)
                format->type = (int)number;
            int audio_type_found = airplay_bplist_dict_get(document, &entry, "audioType", &value);
            if (audio_type_found < 0 || (audio_type_found == 1 &&
                (airplay_bplist_string_copy(document, &value, format->audio_type,
                     sizeof(format->audio_type), &audio_type_len) < 0 ||
                 memchr(format->audio_type, '\0', audio_type_len) != NULL))) {
                /* Missing/empty classification is a legacy catch-all. A wrong
                 * type, truncated string or embedded NUL must not become one. */
                format->present = 0;
                format->audio_type[0] = '\0';
            }
            if (get_audio_number(document, &entry, "audioOutputFormats", UINT32_MAX, &number) == 1)
                format->output_formats = (uint32_t)number;
            if (get_audio_number(document, &entry, "audioInputFormats", UINT32_MAX, &number) == 1)
                format->input_formats = (uint32_t)number;
            capabilities->audio.format_count++;
        }
        if (airplay_bplist_dict_get(document, &entry,
                                    "audioOutputFormats", &value) == 1)
            capabilities->audio.media_output = 1;
        if (airplay_bplist_dict_get(document, &entry,
                                    "audioInputFormats", &value) == 1)
            capabilities->audio.microphone_input = 1;
    }
}

static void parse_audio(const struct airplay_bplist_document *document,
                        const struct airplay_bplist_value *root,
                        struct carplay_session_capabilities *capabilities)
{
    parse_audio_array(document, root, "audioLatencies", capabilities);
    parse_audio_array(document, root, "audioFormats", capabilities);
}

static void parse_appearance(const struct airplay_bplist_document *document,
                             const struct airplay_bplist_value *root,
                             struct carplay_session_capabilities *capabilities)
{
    struct airplay_bplist_value value;
    int night = 0;

    if (airplay_bplist_dict_get(document, root, "nightMode", &value) == 1 &&
        airplay_bplist_get_bool(document, &value, &night) == 0) {
        capabilities->appearance_known = 1;
        capabilities->night_mode = night != 0;
    }
}

static void parse_remote_ui(const struct airplay_bplist_document *document,
                            const struct airplay_bplist_value *root,
                            struct carplay_session_capabilities *capabilities)
{
    struct airplay_bplist_value value;
    uint64_t count = 0;
    uint64_t i;
    int visible = 0;

    /* A receiver with its own car button advertises at least one raster icon.
     * A receiver without one can still offer requestUI. */
    if (airplay_bplist_dict_get(document, root, "oemIcons", &value) == 1 &&
        value.type == AIRPLAY_BPLIST_ARRAY &&
        airplay_bplist_array_count(document, &value, &count) == 0 &&
        count > 0)
        capabilities->oem_icon_present = 1;
    if (airplay_bplist_dict_get(document, root, "oemIconVisible", &value) == 1 &&
        airplay_bplist_get_bool(document, &value, &visible) == 0 && visible)
        capabilities->oem_icon_present = 1;

    if (airplay_bplist_dict_get(document, root, "extendedFeatures", &value) != 1 ||
        value.type != AIRPLAY_BPLIST_ARRAY ||
        airplay_bplist_array_count(document, &value, &count) != 0)
        return;
    for (i = 0; i < count; i++) {
        struct airplay_bplist_value item;
        char feature[64];
        size_t feature_len = 0;
        if (airplay_bplist_array_get(document, &value, (size_t)i, &item) != 0 ||
            airplay_bplist_string_copy(document, &item, feature,
                                       sizeof(feature), &feature_len) < 0)
            continue;
        if (strcmp(feature, "enhancedRequestCarUI") == 0)
            capabilities->request_car_ui_supported = 1;
    }
}

int carplay_capabilities_parse(const uint8_t *data, size_t len,
                               struct carplay_session_capabilities *capabilities)
{
    struct airplay_bplist_document document;
    struct airplay_bplist_value root;

    if (!capabilities)
        return -1;
    memset(capabilities, 0, sizeof(*capabilities));
    if (!data || len == 0 ||
        airplay_bplist_init(&document, data, len) != 0 ||
        airplay_bplist_root(&document, &root) != 0 ||
        root.type != AIRPLAY_BPLIST_DICTIONARY)
        return -1;

    parse_display(&document, &root, capabilities);
    parse_hid_devices(&document, &root, capabilities);
    parse_audio(&document, &root, capabilities);
    parse_appearance(&document, &root, capabilities);
    parse_remote_ui(&document, &root, capabilities);
    return 0;
}

static uint32_t choose_audio_format(uint32_t mask, int type,
                                                  const char *audio_type, int input)
{
    static const uint32_t media_formats[] = {
        UINT32_C(0x00008000), UINT32_C(0x00000800),
        UINT32_C(0x00004000), UINT32_C(0x00000400)
    };
    static const uint32_t prompt_formats[] = {
        UINT32_C(0x00000020), UINT32_C(0x00000008),
        UINT32_C(0x00000200), UINT32_C(0x00000010)
    };
    static const uint32_t microphone_formats[] = {
        UINT32_C(0x00000010), UINT32_C(0x00000020),
        UINT32_C(0x00000004), UINT32_C(0x00000008),
        UINT32_C(0x00000200), UINT32_C(0x00000040)
    };
    static const uint32_t fallback_formats[] = {
        UINT32_C(0x00008000), UINT32_C(0x00000800),
        UINT32_C(0x00004000), UINT32_C(0x00000400),
        UINT32_C(0x00000200), UINT32_C(0x00000100),
        UINT32_C(0x00000080), UINT32_C(0x00000040),
        UINT32_C(0x00000020), UINT32_C(0x00000010),
        UINT32_C(0x00000008), UINT32_C(0x00000004)
    };
    const uint32_t *preferred = prompt_formats;
    size_t count = sizeof(prompt_formats) / sizeof(prompt_formats[0]);
    size_t i;

    (void)type;
    if (input) {
        preferred = microphone_formats;
        count = sizeof(microphone_formats) / sizeof(microphone_formats[0]);
    } else if (audio_type && (strcmp(audio_type, "media") == 0 ||
                       strcmp(audio_type, "compatibility") == 0 ||
                       strcmp(audio_type, "telephony") == 0)) {
        preferred = media_formats;
        count = sizeof(media_formats) / sizeof(media_formats[0]);
    }
    for (i = 0; i < count; i++) {
        if (mask & preferred[i])
            return preferred[i];
    }
    for (i = 0; i < sizeof(fallback_formats) / sizeof(fallback_formats[0]);
         i++) {
        if (mask & fallback_formats[i])
            return fallback_formats[i];
    }
    return 0;
}

static uint32_t find_audio_mask(
    const struct carplay_session_capabilities *capabilities,
    int type, const char *audio_type, int input)
{
    size_t i;
    if (!capabilities)
        return 0;
    for (i = 0; i < capabilities->audio.format_count; i++) {
        const struct carplay_audio_format_capabilities *entry =
            &capabilities->audio.formats[i];
        if (!entry->present || entry->type != type ||
            !audio_type || strcmp(entry->audio_type, audio_type) != 0)
            continue;
        return input ? entry->input_formats : entry->output_formats;
    }
    return 0;
}

uint32_t carplay_capabilities_select_audio_output(
    const struct carplay_session_capabilities *capabilities,
    int type, const char *audio_type)
{
    return choose_audio_format(find_audio_mask(capabilities, type,
                                                audio_type, 0),
                               type, audio_type, 0);
}

uint32_t carplay_capabilities_select_audio_input(
    const struct carplay_session_capabilities *capabilities,
    int type, const char *audio_type)
{
    return choose_audio_format(find_audio_mask(capabilities, type,
                                                audio_type, 1),
                               type, audio_type, 1);
}

const struct carplay_hid_device_capabilities *carplay_capabilities_find_hid(
    const struct carplay_session_capabilities *capabilities,
    const char *uuid)
{
    size_t i;

    if (!capabilities || !uuid)
        return NULL;
    for (i = 0; i < capabilities->hid_count; i++) {
        if (capabilities->hid[i].present &&
            strcmp(capabilities->hid[i].uuid, uuid) == 0)
            return &capabilities->hid[i];
    }
    return NULL;
}


int carplay_capabilities_output_size(
    const struct carplay_session_capabilities *capabilities,
    unsigned fallback_width, unsigned fallback_height,
    unsigned *width, unsigned *height)
{
    unsigned selected_width;
    unsigned selected_height;
    int negotiated;

    if (!width || !height)
        return -1;
    negotiated = capabilities && capabilities->display.present;
    selected_width = negotiated ? (unsigned)capabilities->display.width_pixels :
                                   fallback_width;
    selected_height = negotiated ? (unsigned)capabilities->display.height_pixels :
                                    fallback_height;
    selected_width &= ~1u;
    selected_height &= ~1u;
    if (!selected_width || !selected_height)
        return -1;
    *width = selected_width;
    *height = selected_height;
    return negotiated ? 1 : 0;
}


static unsigned even_dimension(unsigned value)
{
    return value & ~1u;
}

static unsigned automatic_density(unsigned width, unsigned height)
{
    unsigned density;

    if (width == 1920u && height == 1080u)
        return 280u;
    if (width == 800u && height == 480u)
        return 160u;
    density = (160u * height + 240u) / 480u;
    density = ((density + 5u) / 10u) * 10u;
    if (density < 120u)
        density = 120u;
    if (density > 480u)
        density = 480u;
    return density;
}

static int encoder_alignment(unsigned value)
{
    return value > 0 && (value & (value - 1u)) == 0;
}

static unsigned align_capture_dimension(unsigned value, unsigned alignment,
                                        unsigned maximum)
{
    unsigned lower = value - value % alignment;
    unsigned upper = lower;
    unsigned remainder = value % alignment;

    if (remainder && alignment - remainder <= remainder)
        upper = lower + alignment;
    if (upper > maximum || alignment - remainder > remainder)
        return lower;
    return upper;
}

int carplay_capabilities_capture_size(
    unsigned width, unsigned height,
    unsigned maximum_width, unsigned maximum_height,
    unsigned alignment_width, unsigned alignment_height,
    unsigned *capture_width, unsigned *capture_height)
{
    uint64_t scaled_width;
    uint64_t scaled_height;
    unsigned fitted_width;
    unsigned fitted_height;

    if (!capture_width || !capture_height ||
        width < 176u || height < 144u ||
        maximum_width < 176u || maximum_height < 144u ||
        !encoder_alignment(alignment_width) ||
        !encoder_alignment(alignment_height))
        return -1;

    scaled_width = width;
    scaled_height = height;
    if (width > maximum_width || height > maximum_height) {
        if ((uint64_t)maximum_width * height <=
            (uint64_t)maximum_height * width) {
            scaled_width = maximum_width;
            scaled_height = ((uint64_t)height * maximum_width) / width;
        } else {
            scaled_width = ((uint64_t)width * maximum_height) / height;
            scaled_height = maximum_height;
        }
    }
    if (!scaled_width || !scaled_height ||
        scaled_width > UINT_MAX || scaled_height > UINT_MAX)
        return -1;
    fitted_width = align_capture_dimension(
        (unsigned)scaled_width, alignment_width, maximum_width);
    fitted_height = align_capture_dimension(
        (unsigned)scaled_height, alignment_height, maximum_height);
    if (fitted_width < 176u || fitted_height < 144u)
        return -1;
    *capture_width = fitted_width;
    *capture_height = fitted_height;
    return 0;
}
int carplay_capabilities_display_plan(
    const struct carplay_session_capabilities *capabilities,
    unsigned fallback_width, unsigned fallback_height,
    int override_width, int override_height,
    int physical_width, int physical_height, int density_override,
    unsigned configured_fps, struct carplay_display_plan *plan)
{
    unsigned width;
    unsigned height;
    unsigned fps;
    int negotiated;

    if (!plan || override_width < 0 || override_height < 0 ||
        physical_width < 0 || physical_height < 0 ||
        (override_width == 0) != (override_height == 0) ||
        (physical_width == 0) != (physical_height == 0) ||
        density_override < 0 ||
        (density_override != 0 &&
         (density_override < 120 || density_override > 480)))
        return -1;
    negotiated = capabilities && capabilities->display.present;
    if (negotiated) {
        width = (unsigned)capabilities->display.width_pixels;
        height = (unsigned)capabilities->display.height_pixels;
    } else if (override_width != 0) {
        width = (unsigned)override_width;
        height = (unsigned)override_height;
    } else {
        width = fallback_width;
        height = fallback_height;
    }
    width = even_dimension(width);
    height = even_dimension(height);
    if (width < 320u || height < 240u ||
        width > CARPLAY_ENCODER_MAX_WIDTH || height > CARPLAY_ENCODER_MAX_HEIGHT)
        return -1;

    if (!configured_fps)
        configured_fps = 30u;
    if (configured_fps < 1u)
        configured_fps = 1u;
    if (configured_fps > 60u)
        configured_fps = 60u;
    fps = (unsigned)configured_fps;
    if (negotiated && capabilities->display.max_fps > 0 &&
        (unsigned)capabilities->display.max_fps < fps)
        fps = (unsigned)capabilities->display.max_fps;
    if (!fps)
        fps = 1u;

    memset(plan, 0, sizeof(*plan));
    plan->width = width;
    plan->height = height;
    /* Stretch the entire physical preset into the requested encoded canvas.
     * Do not substitute a smaller picture: receivers need not enlarge it.
     * Reject hardware-unsupported canvases before touching the OMX service. */
    plan->output_width = width;
    plan->output_height = height;
    plan->fps = fps;
    if (physical_width != 0) {
        unsigned checked_width, checked_height;
        if (carplay_capabilities_display_preset((unsigned)physical_width,
                (unsigned)physical_height, &checked_width, &checked_height) != 0)
            return -1;
        plan->physical_width = checked_width;
        plan->physical_height = checked_height;
    } else if (carplay_capabilities_display_preset(width, height,
            &plan->physical_width, &plan->physical_height) < 0) {
        return -1;
    }
    plan->physical_fps = 60u;
    plan->density_automatic = density_override == 0;
    plan->density = density_override ? (unsigned)density_override :
                     automatic_density(plan->physical_width, plan->physical_height);
    plan->force_physical = 1;
    plan->stretched = width != plan->physical_width || height != plan->physical_height;
    plan->negotiated = negotiated ? 1 : 0;
    return 0;
}

int carplay_capabilities_display_preset(unsigned width, unsigned height,
    unsigned *physical_width, unsigned *physical_height)
{
    static const unsigned presets[][2] = {
        {800,480}, {1024,480}, {1280,600}, {1280,720}, {1920,900}, {1920,1080}
    };
    uint64_t best = UINT64_MAX;
    size_t selected = 0;
    if (!physical_width || !physical_height || width < 320 || height < 240 ||
        width > 7680 || height > 4320)
        return -1;
    for (size_t i = 0; i < sizeof(presets) / sizeof(presets[0]); i++) {
        int64_t dx = (int64_t)presets[i][0] - width;
        int64_t dy = (int64_t)presets[i][1] - height;
        uint64_t score = (uint64_t)(dx * dx) * height * height +
                         (uint64_t)(dy * dy) * width * width;
        if (score < best) { best = score; selected = i; }
    }
    *physical_width = presets[selected][0];
    *physical_height = presets[selected][1];
    return best == 0 ? 0 : 1;
}
