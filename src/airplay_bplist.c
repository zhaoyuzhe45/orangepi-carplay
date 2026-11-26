#include "airplay_bplist.h"
#include "carplay_identity.h"
#include "carplay_modes.h"

#include <limits.h>
#include <string.h>

#define BPLIST_HEADER_BYTES 8u
#define BPLIST_TRAILER_BYTES 32u
#define BPLIST_MAX_ARRAY_REFERENCES 12u
#define BPLIST_MAX_NODES 192u

static int read_uint_be(const uint8_t *data, size_t len, size_t offset,
                        size_t size, uint64_t *value)
{
    uint64_t result = 0;
    size_t i;

    if (!data || !value || size == 0 || size > 8 || offset > len ||
        size > len - offset)
        return -1;
    for (i = 0; i < size; i++)
        result = (result << 8) | data[offset + i];
    *value = result;
    return 0;
}

static int data_available(const struct airplay_bplist_document *document,
                          size_t offset, size_t amount)
{
    return document && offset <= document->len &&
           amount <= document->len - offset;
}

int airplay_bplist_init(struct airplay_bplist_document *document,
                        const uint8_t *data, size_t len)
{
    size_t trailer;
    size_t i;
    uint64_t object_count;
    uint64_t top_object;
    uint64_t offset_table;

    if (!document || !data || len < BPLIST_HEADER_BYTES + BPLIST_TRAILER_BYTES)
        return -1;
    if (memcmp(data, "bplist00", BPLIST_HEADER_BYTES) != 0)
        return -1;
    trailer = len - BPLIST_TRAILER_BYTES;
    if (data[trailer + 6] == 0 || data[trailer + 6] > 8 ||
        data[trailer + 7] == 0 || data[trailer + 7] > 8)
        return -1;
    if (read_uint_be(data, len, trailer + 8, 8, &object_count) < 0 ||
        read_uint_be(data, len, trailer + 16, 8, &top_object) < 0 ||
        read_uint_be(data, len, trailer + 24, 8, &offset_table) < 0)
        return -1;
    if (object_count == 0 || object_count > UINT32_MAX ||
        top_object >= object_count || offset_table < BPLIST_HEADER_BYTES ||
        offset_table > trailer ||
        object_count > (uint64_t)(trailer - (size_t)offset_table) /
                           data[trailer + 6])
        return -1;

    for (i = 0; i < (size_t)object_count; i++) {
        uint64_t object_offset;

        if (read_uint_be(data, len,
                         (size_t)offset_table + i * data[trailer + 6],
                         data[trailer + 6], &object_offset) < 0 ||
            object_offset < BPLIST_HEADER_BYTES ||
            object_offset >= offset_table)
            return -1;
    }

    memset(document, 0, sizeof(*document));
    document->data = data;
    document->len = len;
    document->offset_table = (size_t)offset_table;
    document->object_count = object_count;
    document->top_object = (uint32_t)top_object;
    document->offset_size = data[trailer + 6];
    document->reference_size = data[trailer + 7];
    return 0;
}

static int read_count(const struct airplay_bplist_document *document,
                      size_t *position, uint8_t nibble, uint64_t *count)
{
    uint8_t marker;

    if (nibble < 15) {
        *count = nibble;
        return 0;
    }
    if (!data_available(document, *position, 2))
        return -1;
    marker = document->data[*position];
    (*position)++;
    if ((marker >> 4) != 1 || (marker & 0x0f) > 3)
        return -1;
    if (read_uint_be(document->data, document->len, *position,
                     (size_t)1u << (marker & 0x0f), count) < 0)
        return -1;
    *position += (size_t)1u << (marker & 0x0f);
    return 0;
}

static int read_object(const struct airplay_bplist_document *document,
                       uint32_t object_index,
                       struct airplay_bplist_value *value)
{
    uint64_t object_offset64;
    size_t object_offset;
    uint8_t marker;
    uint8_t type;
    uint8_t nibble;
    size_t position;
    uint64_t count;
    size_t bytes;

    if (!document || !value || object_index >= document->object_count)
        return -1;
    if (read_uint_be(document->data, document->len,
                     document->offset_table +
                         (size_t)object_index * document->offset_size,
                     document->offset_size, &object_offset64) < 0 ||
        object_offset64 >= document->len)
        return -1;
    object_offset = (size_t)object_offset64;
    marker = document->data[object_offset];
    type = marker >> 4;
    nibble = marker & 0x0f;
    position = object_offset + 1;
    memset(value, 0, sizeof(*value));
    value->object_index = object_index;

    switch (type) {
    case 0x0:
        if (nibble == 8 || nibble == 9) {
            value->type = AIRPLAY_BPLIST_BOOLEAN;
            value->boolean_value = nibble == 9;
            return 0;
        }
        if (marker != 0x00)
            return -1;
        value->type = AIRPLAY_BPLIST_NULL;
        return 0;
    case 0x1:
        if (nibble > 3)
            return -1;
        value->type = AIRPLAY_BPLIST_INTEGER;
        value->integer_size = (uint8_t)(1u << nibble);
        value->data_offset = position;
        if (!data_available(document, position, value->integer_size))
            return -1;
        return 0;
    case 0x4:
    case 0x5:
    case 0x6:
    case 0xa:
    case 0xc:
    case 0xd:
        if (read_count(document, &position, nibble, &count) < 0)
            return -1;
        value->count = count;
        if (type == 0x4) {
            value->type = AIRPLAY_BPLIST_DATA;
            value->data_offset = position;
            if (count > SIZE_MAX ||
                !data_available(document, position, (size_t)count))
                return -1;
        } else if (type == 0x5 || type == 0x6) {
            value->type = AIRPLAY_BPLIST_STRING;
            value->string_unit_size = type == 0x6 ? 2u : 1u;
            value->data_offset = position;
            if (count > SIZE_MAX / (type == 0x6 ? 2u : 1u))
                return -1;
            bytes = (size_t)count * (type == 0x6 ? 2u : 1u);
            if (!data_available(document, position, bytes))
                return -1;
        } else {
            value->type = type == 0xd ? AIRPLAY_BPLIST_DICTIONARY :
                                        AIRPLAY_BPLIST_ARRAY;
            value->data_offset = position;
            if (type == 0xd && count > UINT64_MAX / 2)
                return -1;
            bytes = type == 0xd ? (size_t)count * 2 : (size_t)count;
            if (bytes > SIZE_MAX / document->reference_size)
                return -1;
            bytes *= document->reference_size;
            if (!data_available(document, position, bytes))
                return -1;
        }
        return 0;
    case 0x8:
        if (nibble != 8 && nibble != 9)
            return -1;
        value->type = AIRPLAY_BPLIST_BOOLEAN;
        value->boolean_value = nibble == 9;
        return 0;
    default:
        return -1;
    }
}

int airplay_bplist_root(const struct airplay_bplist_document *document,
                        struct airplay_bplist_value *value)
{
    if (!document)
        return -1;
    return read_object(document, document->top_object, value);
}

static int read_reference(const struct airplay_bplist_document *document,
                          size_t offset, uint32_t *object_index)
{
    uint64_t reference;

    if (!document || !object_index ||
        read_uint_be(document->data, document->len, offset,
                     document->reference_size, &reference) < 0 ||
        reference >= document->object_count || reference > UINT32_MAX)
        return -1;
    *object_index = (uint32_t)reference;
    return 0;
}

int airplay_bplist_dict_get(const struct airplay_bplist_document *document,
                            const struct airplay_bplist_value *dictionary,
                            const char *key,
                            struct airplay_bplist_value *value)
{
    uint64_t i;
    int found = 0;

    if (!document || !dictionary || !key || !value ||
        dictionary->type != AIRPLAY_BPLIST_DICTIONARY ||
        dictionary->count > SIZE_MAX / document->reference_size)
        return -1;
    for (i = 0; i < dictionary->count; i++) {
        struct airplay_bplist_value key_value;
        uint32_t key_index;
        uint32_t value_index;
        size_t key_ref;
        size_t value_ref;

        key_ref = dictionary->data_offset +
                  (size_t)i * document->reference_size;
        value_ref = dictionary->data_offset +
                    ((size_t)dictionary->count + (size_t)i) *
                        document->reference_size;
        if (read_reference(document, key_ref, &key_index) < 0 ||
            read_reference(document, value_ref, &value_index) < 0 ||
            read_object(document, key_index, &key_value) < 0 ||
            key_value.type != AIRPLAY_BPLIST_STRING)
            return -1;
        if (airplay_bplist_string_equals(document, &key_value, key)) {
            /* Duplicate command fields are ambiguous: never accept the first
             * transfer and silently ignore a conflicting later transfer. */
            if (found || read_object(document, value_index, value) < 0)
                return -1;
            found = 1;
        }
    }
    return found;
}

int airplay_bplist_array_count(
    const struct airplay_bplist_document *document,
    const struct airplay_bplist_value *array, uint64_t *count)
{
    if (!document || !array || !count || array->type != AIRPLAY_BPLIST_ARRAY ||
        array->count > UINT64_MAX)
        return -1;
    *count = array->count;
    return 0;
}

int airplay_bplist_array_get(const struct airplay_bplist_document *document,
                             const struct airplay_bplist_value *array,
                             size_t index,
                             struct airplay_bplist_value *value)
{
    uint32_t object_index;
    size_t offset;

    if (!document || !array || !value ||
        array->type != AIRPLAY_BPLIST_ARRAY || index >= array->count ||
        index > (SIZE_MAX - array->data_offset) / document->reference_size)
        return -1;
    offset = array->data_offset + index * document->reference_size;
    if (read_reference(document, offset, &object_index) < 0)
        return -1;
    return read_object(document, object_index, value);
}

int airplay_bplist_get_uint64(const struct airplay_bplist_document *document,
                              const struct airplay_bplist_value *value,
                              uint64_t *number)
{
    if (!document || !value || !number ||
        value->type != AIRPLAY_BPLIST_INTEGER ||
        !data_available(document, value->data_offset, value->integer_size))
        return -1;
    return read_uint_be(document->data, document->len, value->data_offset,
                        value->integer_size, number);
}

int airplay_bplist_get_bool(const struct airplay_bplist_document *document,
                            const struct airplay_bplist_value *value,
                            int *boolean_value)
{
    if (!document || !value || !boolean_value ||
        value->type != AIRPLAY_BPLIST_BOOLEAN)
        return -1;
    *boolean_value = value->boolean_value;
    return 0;
}

int airplay_bplist_string_equals(
    const struct airplay_bplist_document *document,
    const struct airplay_bplist_value *value, const char *expected)
{
    size_t expected_len;

    if (!document || !value || !expected ||
        value->type != AIRPLAY_BPLIST_STRING || value->string_unit_size != 1 ||
        value->count > SIZE_MAX)
        return 0;
    expected_len = strlen(expected);
    if ((size_t)value->count != expected_len ||
        !data_available(document, value->data_offset, expected_len))
        return 0;
    return memcmp(document->data + value->data_offset, expected,
                  expected_len) == 0;
}

int airplay_bplist_string_copy(
    const struct airplay_bplist_document *document,
    const struct airplay_bplist_value *value,
    char *output, size_t capacity, size_t *output_len)
{
    if (!document || !value || !output || !output_len ||
        value->type != AIRPLAY_BPLIST_STRING || value->string_unit_size != 1 ||
        value->count >= capacity || value->count > SIZE_MAX ||
        !data_available(document, value->data_offset, (size_t)value->count))
        return -1;
    memcpy(output, document->data + value->data_offset, (size_t)value->count);
    output[value->count] = '\0';
    *output_len = (size_t)value->count;
    return 0;
}

enum node_kind {
    NODE_STRING,
    NODE_INTEGER,
    NODE_BOOLEAN,
    NODE_ARRAY,
    NODE_DICTIONARY
};

struct plist_node {
    enum node_kind kind;
    const char *string;
    uint64_t number;
    int boolean_value;
    int refs[BPLIST_MAX_ARRAY_REFERENCES];
    size_t reference_count;
    int key_refs[12];
    int value_refs[12];
    size_t entry_count;
};

struct node_builder {
    struct plist_node nodes[BPLIST_MAX_NODES];
    size_t count;
    int failed;
};

static int add_node(struct node_builder *builder, enum node_kind kind)
{
    int index;

    if (builder->failed || builder->count >=
                              sizeof(builder->nodes) /
                                  sizeof(builder->nodes[0])) {
        builder->failed = 1;
        return -1;
    }
    index = (int)builder->count++;
    memset(&builder->nodes[index], 0, sizeof(builder->nodes[index]));
    builder->nodes[index].kind = kind;
    return index;
}

static int add_string(struct node_builder *builder, const char *string)
{
    int index = add_node(builder, NODE_STRING);

    if (index >= 0)
        builder->nodes[index].string = string;
    return index;
}

static int add_integer(struct node_builder *builder, uint64_t number)
{
    int index = add_node(builder, NODE_INTEGER);

    if (index >= 0)
        builder->nodes[index].number = number;
    return index;
}

static int add_boolean(struct node_builder *builder, int value)
{
    int index = add_node(builder, NODE_BOOLEAN);

    if (index >= 0)
        builder->nodes[index].boolean_value = !!value;
    return index;
}

static void add_dictionary_entry(struct node_builder *builder, int dictionary,
                                 int key, int value)
{
    struct plist_node *node;

    if (builder->failed || dictionary < 0 || key < 0 || value < 0 ||
        (size_t)dictionary >= builder->count) {
        builder->failed = 1;
        return;
    }
    node = &builder->nodes[dictionary];
    if (node->kind != NODE_DICTIONARY ||
        node->entry_count >= sizeof(node->key_refs) /
                                 sizeof(node->key_refs[0])) {
        builder->failed = 1;
        return;
    }
    node->key_refs[node->entry_count] = key;
    node->value_refs[node->entry_count] = value;
    node->entry_count++;
}

static int put_byte(uint8_t *output, size_t capacity, size_t *position,
                    uint8_t byte)
{
    if (*position >= capacity)
        return -1;
    output[(*position)++] = byte;
    return 0;
}

static int put_integer_marker(uint8_t *output, size_t capacity,
                              size_t *position, uint64_t number)
{
    size_t size = number > UINT32_MAX ? 8 :
                  number > UINT16_MAX ? 4 :
                  number > UINT8_MAX ? 2 : 1;
    uint8_t log = size == 8 ? 3 : size == 4 ? 2 : size == 2 ? 1 : 0;
    size_t i;

    if (put_byte(output, capacity, position, (uint8_t)(0x10u | log)) < 0)
        return -1;
    for (i = size; i > 0; i--) {
        if (put_byte(output, capacity, position,
                     (uint8_t)(number >> ((i - 1) * 8))) < 0)
            return -1;
    }
    return 0;
}

static int put_marker(uint8_t *output, size_t capacity, size_t *position,
                      uint8_t type, size_t count)
{
    if (count < 15)
        return put_byte(output, capacity, position,
                        (uint8_t)((type << 4) | count));
    if (put_byte(output, capacity, position,
                 (uint8_t)((type << 4) | 0x0f)) < 0)
        return -1;
    return put_integer_marker(output, capacity, position, count);
}

static int put_reference(uint8_t *output, size_t capacity, size_t *position,
                         int reference)
{
    if (reference < 0 || reference > UINT8_MAX)
        return -1;
    return put_byte(output, capacity, position, (uint8_t)reference);
}

static int encode_node(const struct plist_node *node, uint8_t *output,
                       size_t capacity, size_t *position)
{
    size_t i;
    size_t text_len;

    switch (node->kind) {
    case NODE_STRING:
        text_len = strlen(node->string);
        if (put_marker(output, capacity, position, 0x5, text_len) < 0 ||
            text_len > capacity - (*position > capacity ? capacity : *position))
            return -1;
        memcpy(output + *position, node->string, text_len);
        *position += text_len;
        return 0;
    case NODE_INTEGER:
        return put_integer_marker(output, capacity, position, node->number);
    case NODE_BOOLEAN:
        return put_byte(output, capacity, position,
                        node->boolean_value ? 0x09 : 0x08);
    case NODE_ARRAY:
        if (put_marker(output, capacity, position, 0xa,
                       node->reference_count) < 0)
            return -1;
        for (i = 0; i < node->reference_count; i++)
            if (put_reference(output, capacity, position,
                              node->refs[i]) < 0)
                return -1;
        return 0;
    case NODE_DICTIONARY:
        if (put_marker(output, capacity, position, 0xd,
                       node->entry_count) < 0)
            return -1;
        for (i = 0; i < node->entry_count; i++)
            if (put_reference(output, capacity, position,
                              node->key_refs[i]) < 0)
                return -1;
        for (i = 0; i < node->entry_count; i++)
            if (put_reference(output, capacity, position,
                              node->value_refs[i]) < 0)
                return -1;
        return 0;
    default:
        return -1;
    }
}

static int put_be(uint8_t *output, size_t capacity, size_t *position,
                  uint64_t value, size_t size)
{
    size_t i;

    if (size > 8 || size > capacity - (*position > capacity ? capacity : *position))
        return -1;
    for (i = size; i > 0; i--)
        output[(*position)++] = (uint8_t)(value >> ((i - 1) * 8));
    return 0;
}

static void add_setup_entry(struct node_builder *builder, int dictionary,
                            const char *key, int value)
{
    int key_ref = add_string(builder, key);

    add_dictionary_entry(builder, dictionary, key_ref, value);
}

static int finish_encode(const struct node_builder *builder, int root,
                         uint8_t *output, size_t capacity, size_t *output_len)
{
    size_t positions[BPLIST_MAX_NODES];
    size_t position = 0;
    size_t offset_size;
    size_t offset_table_offset;
    size_t i;

    if (!builder || !output || !output_len || root < 0 ||
        (size_t)root >= builder->count ||
        builder->count > BPLIST_MAX_NODES)
        return -1;
    if (put_be(output, capacity, &position, 0x62706c6973743030ULL, 8) < 0)
        return -1;
    for (i = 0; i < builder->count; i++) {
        positions[i] = position;
        if (encode_node(&builder->nodes[i], output, capacity, &position) < 0)
            return -1;
    }
    offset_table_offset = position;
    offset_size = offset_table_offset > UINT16_MAX ? 4 :
                  offset_table_offset > UINT8_MAX ? 2 : 1;
    for (i = 0; i < builder->count; i++)
        if (put_be(output, capacity, &position, positions[i], offset_size) < 0)
            return -1;
    for (i = 0; i < 6; i++)
        if (put_byte(output, capacity, &position, 0) < 0)
            return -1;
    if (put_byte(output, capacity, &position, (uint8_t)offset_size) < 0 ||
        put_byte(output, capacity, &position, 1) < 0 ||
        put_be(output, capacity, &position, builder->count, 8) < 0 ||
        put_be(output, capacity, &position, (uint64_t)root, 8) < 0 ||
        put_be(output, capacity, &position, offset_table_offset, 8) < 0)
        return -1;
    *output_len = position;
    return 0;
}
int airplay_bplist_encode_setup(uint8_t *output, size_t capacity,
                                size_t *output_len, uint16_t timing_port,
                                int keep_alive_low_power, int encrypted)
{
    struct node_builder builder;
    int root;
    int features;

    if (!output || !output_len)
        return -1;
    memset(&builder, 0, sizeof(builder));
    root = add_node(&builder, NODE_DICTIONARY);
    features = add_node(&builder, NODE_ARRAY);
    if (features >= 0 && encrypted) {
        int feature_iap = add_string(&builder, "iAPChannel");
        int feature_view_areas = add_string(&builder, "viewAreas");
        builder.nodes[features].reference_count = 2;
        builder.nodes[features].refs[0] = feature_iap;
        builder.nodes[features].refs[1] = feature_view_areas;
    }
    add_setup_entry(&builder, root, "name",
                    add_string(&builder, "iPhone"));
    add_setup_entry(&builder, root, "deviceID",
                    add_string(&builder, CARPLAY_DEVICE_ID));
    add_setup_entry(&builder, root, "macAddress",
                    add_string(&builder, CARPLAY_DEVICE_ID));
    add_setup_entry(&builder, root, "model",
                    add_string(&builder, "iPhone8,1"));
    add_setup_entry(&builder, root, "osVersion",
                    add_string(&builder, "8.1.0"));
    add_setup_entry(&builder, root, "sourceVersion",
                    add_string(&builder, CARPLAY_SOURCE_VERSION));
    add_setup_entry(&builder, root, "timingPort",
                    add_integer(&builder, timing_port));
    /*
     * `et` belongs to the *unpaired* session only, and getting it backwards
     * in either direction ruins the session:
     *
     *  - Unpaired (no HAP shared secret): the key MUST be present and MUST be
     *    the integer 1 (kAirPlayEncryptionType_None). An absent key reads back
     *    as kAirPlayEncryptionType_Undefined, the receiver goes looking for a
     *    FairPlay `ekey` we never send and answers 466 Key Management Error --
     *    indistinguishable on the wire from "this head unit wants pairing".
     *
     *  - Paired (pair-setup + pair-verify done): the key MUST be absent. The
     *    streams are ChaCha20-Poly1305 keyed from the pair-verify shared
     *    secret (see airplay_screen_encode_frame()); advertising et = 1 there
     *    would tell the receiver to expect plaintext and it would fail to open
     *    every frame. catplay's do_initial_setup() sets req.et = None once a
     *    shared secret exists, which is why real iPhones never carry it here.
     *
     * The pairing path is the one this box actually runs (RECORD accepted on
     * the F-PACE head unit with no `et` in the plist), so `encrypted` is 1 in
     * production and only the bench/plaintext path sets it to 0.
     */
    if (!encrypted)
        add_setup_entry(&builder, root, "et", add_integer(&builder, 1));
    add_setup_entry(&builder, root, "keepAliveLowPower",
                    add_boolean(&builder, keep_alive_low_power));
    add_setup_entry(&builder, root, "features", features);
    if (builder.failed || root != 0)
        return -1;
    return finish_encode(&builder, root, output, capacity, output_len);
}

static int add_stream_entry(struct node_builder *builder, int stream,
                            const struct airplay_stream_setup *setup)
{
    int value;

    if (!setup)
        return -1;
    add_setup_entry(builder, stream, "type", add_integer(builder, setup->type));
    if (!setup->legacy_audio)
        add_setup_entry(builder, stream, "streamID",
                        add_integer(builder, setup->stream_id));
    if (!setup->legacy_audio || setup->audio_type)
        add_setup_entry(builder, stream, "streamConnectionID",
                        add_integer(builder, setup->connection_id));
    if (setup->audio_type) {
        add_setup_entry(builder, stream, "audioType",
                        add_string(builder, setup->audio_type));
        add_setup_entry(builder, stream, "audioFormat",
                        add_integer(builder, setup->audio_format));
        if (setup->input && setup->data_port) {
            add_setup_entry(builder, stream, "dataPort",
                            add_integer(builder, setup->data_port));
            value = add_boolean(builder, 1);
        } else {
            value = add_boolean(builder, 0);
        }
        add_setup_entry(builder, stream, "input", value);
        if (setup->legacy_audio) {
            add_setup_entry(builder, stream, "controlPort", add_integer(builder, setup->control_port));
            add_setup_entry(builder, stream, "audioLatencyMs", add_integer(builder, 100));
            add_setup_entry(builder, stream, "supportsRTPPacketRedundancy", add_boolean(builder, 0));
            add_setup_entry(builder, stream, "supportsHighAccuracyTimestamps", add_boolean(builder, 1));
        }
    }
    return builder->failed ? -1 : 0;
}

int airplay_bplist_encode_stream_setup(
    uint8_t *output, size_t capacity, size_t *output_len,
    const struct airplay_stream_setup *setups, size_t stream_count)
{
    struct node_builder builder;
    int root;
    int streams;
    size_t i;

    if (!output || !output_len || !setups || stream_count == 0 ||
        stream_count > BPLIST_MAX_ARRAY_REFERENCES)
        return -1;
    memset(&builder, 0, sizeof(builder));
    root = add_node(&builder, NODE_DICTIONARY);
    streams = add_node(&builder, NODE_ARRAY);
    if (streams >= 0) {
        builder.nodes[streams].reference_count = stream_count;
        for (i = 0; i < stream_count; i++) {
            int stream = add_node(&builder, NODE_DICTIONARY);
            builder.nodes[streams].refs[i] = stream;
            if (add_stream_entry(&builder, stream, &setups[i]) < 0)
                return -1;
        }
    }
    add_setup_entry(&builder, root, "streams", streams);
    if (builder.failed || root != 0)
        return -1;
    return finish_encode(&builder, root, output, capacity, output_len);
}

int airplay_bplist_encode_screen_setup(uint8_t *output, size_t capacity,
                                       size_t *output_len,
                                       uint64_t stream_connection_id)
{
    struct airplay_stream_setup setup = {
        .type = 110,
        .stream_id = 1,
        .connection_id = stream_connection_id,
        .audio_type = NULL,
        .audio_format = 0,
        .data_port = 0,
        .input = 0
    };
    return airplay_bplist_encode_stream_setup(output, capacity, output_len,
                                              &setup, 1);
}
int airplay_bplist_encode_legacy_screen_setup(uint8_t *output, size_t capacity,
                                              size_t *output_len,
                                              uint64_t stream_connection_id,
                                              const char *display_uuid)
{
    struct node_builder builder;
    int root, streams, stream;

    if (!output || !output_len)
        return -1;
    memset(&builder, 0, sizeof(builder));
    root = add_node(&builder, NODE_DICTIONARY);
    streams = add_node(&builder, NODE_ARRAY);
    stream = add_node(&builder, NODE_DICTIONARY);
    if (streams < 0 || stream < 0)
        return -1;
    builder.nodes[streams].reference_count = 1;
    builder.nodes[streams].refs[0] = stream;
    add_setup_entry(&builder, stream, "type", add_integer(&builder, 110));
    add_setup_entry(&builder, stream, "streamConnectionID",
                    add_integer(&builder, stream_connection_id));
    add_setup_entry(&builder, stream, "latencyMs", add_integer(&builder, 25));
    if (display_uuid && display_uuid[0])
        add_setup_entry(&builder, stream, "uuid", add_string(&builder, display_uuid));
    add_setup_entry(&builder, root, "streams", streams);
    if (builder.failed)
        return -1;
    return finish_encode(&builder, root, output, capacity, output_len);
}

int airplay_bplist_encode_car_ui_request(uint8_t *output, size_t capacity,
                                         size_t *output_len)
{
    struct node_builder builder;
    int root;
    int params;

    if (!output || !output_len)
        return -1;
    memset(&builder, 0, sizeof(builder));
    root = add_node(&builder, NODE_DICTIONARY);
    params = add_node(&builder, NODE_DICTIONARY);
    add_setup_entry(&builder, root, "type",
                    add_string(&builder, "requestUI"));
    add_setup_entry(&builder, root, "params", params);
    if (builder.failed || root != 0)
        return -1;
    return finish_encode(&builder, root, output, capacity, output_len);
}

static int add_modes_resource(struct node_builder *builder, int resource_id,
                              int entity, int permanent_entity)
{
    int resource = add_node(builder, NODE_DICTIONARY);
    add_setup_entry(builder, resource, "resourceID",
                    add_integer(builder, (uint64_t)resource_id));
    add_setup_entry(builder, resource, "entity",
                    add_integer(builder, (uint64_t)entity));
    add_setup_entry(builder, resource, "permanentEntity",
                    add_integer(builder, (uint64_t)permanent_entity));
    return resource;
}

static int add_modes_app_state(struct node_builder *builder, int app_state_id, int entity)
{
    int app = add_node(builder, NODE_DICTIONARY);
    add_setup_entry(builder, app, "appStateID",
                    add_integer(builder, (uint64_t)app_state_id));
    add_setup_entry(builder, app, "entity",
                    add_integer(builder, entity));
    if (app_state_id == 1)
        add_setup_entry(builder, app, "speechMode",
                        add_integer(builder, UINT64_MAX)); /* signed -1 */
    return app;
}

static int add_modes_params(struct node_builder *builder,
                            const struct carplay_modes *state)
{
    int params = add_node(builder, NODE_DICTIONARY);
    int resources = add_node(builder, NODE_ARRAY);
    int apps = add_node(builder, NODE_ARRAY);
    if (params < 0 || resources < 0 || apps < 0)
        return -1;
    builder->nodes[resources].reference_count = 2;
    builder->nodes[resources].refs[0] = add_modes_resource(
        builder, CARPLAY_RESOURCE_MAIN_SCREEN,
        state->screen_entity, state->screen_permanent_entity);
    builder->nodes[resources].refs[1] = add_modes_resource(
        builder, CARPLAY_RESOURCE_MAIN_AUDIO,
        state->audio_entity, state->audio_permanent_entity);
    builder->nodes[apps].reference_count = 3;
    builder->nodes[apps].refs[0] = add_modes_app_state(builder, 2,
        state->phone_call ? CARPLAY_ENTITY_CONTROLLER : CARPLAY_ENTITY_NONE);
    builder->nodes[apps].refs[1] = add_modes_app_state(builder, 3, CARPLAY_ENTITY_NONE);
    builder->nodes[apps].refs[2] = add_modes_app_state(builder, 1, CARPLAY_ENTITY_NONE);
    add_setup_entry(builder, params, "resources", resources);
    add_setup_entry(builder, params, "appStates", apps);
    return builder->failed ? -1 : params;
}

int airplay_bplist_encode_modes_response(uint8_t *output, size_t capacity,
                                         size_t *output_len,
                                         const struct carplay_modes *state,
                                         int status)
{
    struct node_builder builder;
    int root;
    if (!output || !output_len || (!status && !state))
        return -1;
    memset(&builder, 0, sizeof(builder));
    root = add_node(&builder, NODE_DICTIONARY);
    add_setup_entry(&builder, root, "status", add_integer(&builder, !!status));
    if (!status)
        add_setup_entry(&builder, root, "params", add_modes_params(&builder, state));
    if (builder.failed)
        return -1;
    return finish_encode(&builder, root, output, capacity, output_len);
}

int airplay_bplist_encode_modes_changed(uint8_t *output, size_t capacity,
                                        size_t *output_len,
                                        const struct carplay_modes *state)
{
    struct node_builder builder;
    int root;
    if (!output || !output_len || !state)
        return -1;
    memset(&builder, 0, sizeof(builder));
    root = add_node(&builder, NODE_DICTIONARY);
    add_setup_entry(&builder, root, "type", add_string(&builder, "modesChanged"));
    add_setup_entry(&builder, root, "params", add_modes_params(&builder, state));
    if (builder.failed)
        return -1;
    return finish_encode(&builder, root, output, capacity, output_len);
}
#if 0
int airplay_bplist_encode_screen_setup_DISABLED(uint8_t *output, size_t capacity,
                                       size_t *output_len,
                                       uint64_t stream_connection_id)
{
    struct node_builder builder;
    int root;
    int streams;
    int stream;
    int type;
    int stream_id;
    int connection_id;

    if (!output || !output_len)
        return -1;
    memset(&builder, 0, sizeof(builder));
    root = add_node(&builder, NODE_DICTIONARY);
    streams = add_node(&builder, NODE_ARRAY);
    stream = add_node(&builder, NODE_DICTIONARY);
    type = add_integer(&builder, 110);
    stream_id = add_integer(&builder, 1);
    connection_id = add_integer(&builder, stream_connection_id);
    add_setup_entry(&builder, stream, "type", type);
    add_setup_entry(&builder, stream, "streamID", stream_id);
    add_setup_entry(&builder, stream, "streamConnectionID", connection_id);
    if (streams >= 0) {
        builder.nodes[streams].reference_count = 1;
        builder.nodes[streams].refs[0] = stream;
    }
    add_setup_entry(&builder, root, "streams", streams);
    if (builder.failed || root != 0)
        return -1;
    return finish_encode(&builder, root, output, capacity, output_len);
}
#endif
