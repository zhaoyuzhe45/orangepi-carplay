/* Android 8.1 r_submix shim: one factory reader per recorder-mix address. */
#include <dlfcn.h>
#include <errno.h>
#include <inttypes.h>
#include <limits.h>
#include <pthread.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <hardware/audio.h>
#ifdef SHARED_MIC_OPEN_DIAGNOSTICS
#include <android/log.h>
#endif

#ifndef ORIGINAL_SUBMIX_PATH
#error Set ORIGINAL_SUBMIX_PATH to the absolute path of the unmodified factory HAL
#endif

#define VERSION_KEY "carplay_shared_mic_version"
#define MAX_PERIOD_BYTES (64u * 1024u)
#define RING_MS 200u
#define CLOSE_WAIT_MS 500L

struct shared_device;
struct shared_reader;

struct shared_route {
    /* The device lock protects list membership, readers and close state. */
    struct shared_route *next;
    struct shared_reader *readers;
    struct shared_device *device;
    bool closing, close_waiting, factory_closed;
    struct audio_stream_in *factory;
    char address[AUDIO_DEVICE_MAX_ADDRESS_LEN];
    struct audio_config config;
    size_t frame_bytes, period_bytes, capacity;
    uint8_t *ring, *period;
    pthread_mutex_t lock;
    pthread_cond_t change;
    pthread_t pump;
    /* Frame counters are absolute; only the most recent capacity are kept. */
    uint64_t head, errors, source_lost;
    int read_error;
    bool stopping;
};

struct shared_reader {
    struct audio_stream_in stream;
    struct shared_reader *next;
    struct shared_route *route;
    uint64_t cursor, generation, errors, source_lost;
    uint32_t lost;
    bool active, reading, closing;
};

struct shared_device {
    struct audio_hw_device dev;
    struct audio_hw_device *factory;
    void *factory_lib;
    pthread_mutex_t lock;
    pthread_cond_t route_closed;
    struct shared_route *routes;
    size_t foreign_inputs, outputs, reopen_waiters;
};

static struct audio_hw_device *factory_device(const struct audio_hw_device *dev) {
    return ((const struct shared_device *)dev)->factory;
}

static bool recorder_address(const char *address) {
    if (!address || strnlen(address, AUDIO_DEVICE_MAX_ADDRESS_LEN) == AUDIO_DEVICE_MAX_ADDRESS_LEN)
        return false;
    const char *suffix = strstr(address, "mixr:");
    if (!suffix) return false;
    suffix += sizeof("mixr:") - 1;
    if (!*suffix) return false;
    for (; *suffix; ++suffix)
        if (*suffix < '0' || *suffix > '9') return false;
    return true;
}

static uint64_t oldest_frame(const struct shared_route *route) {
    return route->head > route->capacity ? route->head - route->capacity : 0;
}

static void count_loss(struct shared_reader *reader, uint64_t frames) {
    reader->lost += frames > UINT32_MAX - reader->lost ? UINT32_MAX - reader->lost : (uint32_t)frames;
}

static void catch_up(struct shared_reader *reader) {
    if (!reader->active) return;
    struct shared_route *route = reader->route;
    uint64_t oldest = oldest_frame(route);
    if (reader->cursor < oldest) {
        count_loss(reader, oldest - reader->cursor);
        reader->cursor = oldest;
    }
    count_loss(reader, route->source_lost - reader->source_lost);
    reader->source_lost = route->source_lost;
}

static void ring_put(struct shared_route *route, size_t frames) {
    const size_t index = route->head % route->capacity;
    const size_t tail = route->capacity - index;
    const size_t first = frames < tail ? frames : tail;
    const size_t frame_bytes = route->frame_bytes;
    memcpy(route->ring + index * frame_bytes, route->period, first * frame_bytes);
    memcpy(route->ring, route->period + first * frame_bytes, (frames - first) * frame_bytes);
    route->head += frames;
}

static void ring_get(struct shared_reader *reader, uint8_t *buffer, size_t frames) {
    const struct shared_route *route = reader->route;
    const size_t index = reader->cursor % route->capacity;
    const size_t tail = route->capacity - index;
    const size_t first = frames < tail ? frames : tail;
    const size_t frame_bytes = route->frame_bytes;
    memcpy(buffer, route->ring + index * frame_bytes, first * frame_bytes);
    memcpy(buffer + first * frame_bytes, route->ring, (frames - first) * frame_bytes);
    reader->cursor += frames;
}

static void *pump_input(void *opaque) {
    struct shared_route *route = opaque;
    struct audio_stream_in *input = route->factory;
    pthread_mutex_lock(&route->lock);
    while (!route->stopping) {
        pthread_mutex_unlock(&route->lock);
        /* The stock sink may block while an input exists, even in standby.
         * Drain for the lifetime of that input, including before the first
         * logical read. The factory read already clocks playback in real time.
         */
        ssize_t bytes = input->read(input, route->period, route->period_bytes);
        uint32_t lost = input->get_input_frames_lost ? input->get_input_frames_lost(input) : 0;
        pthread_mutex_lock(&route->lock);
        if (route->stopping) break;
        route->source_lost += lost;
        if (bytes > 0 && (size_t)bytes <= route->period_bytes &&
                (size_t)bytes % route->frame_bytes == 0) {
            ring_put(route, (size_t)bytes / route->frame_bytes);
            pthread_cond_broadcast(&route->change);
            continue;
        }
        route->read_error = bytes < 0 && bytes >= INT_MIN ? (int)bytes : -EIO;
        ++route->errors;
        pthread_cond_broadcast(&route->change);
        pthread_mutex_unlock(&route->lock);
        struct timespec pause = {0, 10000000};
        nanosleep(&pause, NULL); // A failed factory HAL may return immediately.
        pthread_mutex_lock(&route->lock);
    }
    pthread_mutex_unlock(&route->lock);

    /* No factory close concurrent with the pump's read. A stalled vendor
     * close must not hold the shim's device lock or the framework caller. */
    struct shared_device *device = route->device;
    device->factory->close_input_stream(device->factory, input);
    pthread_mutex_lock(&device->lock);
    route->factory_closed = true;
    pthread_cond_broadcast(&device->route_closed);
    pthread_mutex_unlock(&device->lock);
    return NULL;
}

static ssize_t reader_read(struct audio_stream_in *stream, void *buffer, size_t bytes) {
    struct shared_reader *reader = (struct shared_reader *)stream;
    struct shared_route *route = reader->route;
    const size_t frame_bytes = route->frame_bytes;
    if (!bytes) return 0;
    if (!buffer || bytes > SSIZE_MAX || bytes % frame_bytes) return -EINVAL;
    pthread_mutex_lock(&route->lock);
    if (reader->closing || reader->reading) {
        const int status = reader->closing ? -EINTR : -EBUSY;
        pthread_mutex_unlock(&route->lock);
        return status;
    }
    if (!reader->active) {
        reader->active = true;
        reader->cursor = route->head;
        reader->errors = route->errors;
        reader->source_lost = route->source_lost;
    }
    const uint64_t generation = reader->generation;
    reader->reading = true;
    size_t done = 0, requested = bytes / frame_bytes;
    int error = -EINTR;
    while (done < requested && !reader->closing && generation == reader->generation) {
        catch_up(reader);
        /* When a read returns partial data, expose a pending error on the
         * next read, as required by audio_stream_in::read. */
        if (reader->errors != route->errors) {
            if (!done) {
                reader->errors = route->errors;
                error = route->read_error;
            }
            break;
        }
        const uint64_t available = route->head - reader->cursor;
        if (!available) {
            pthread_cond_wait(&route->change, &route->lock);
            continue;
        }
        const size_t count = available < requested - done ? (size_t)available : requested - done;
        ring_get(reader, (uint8_t *)buffer + done * frame_bytes, count);
        done += count;
    }
    reader->reading = false;
    if (reader->closing) pthread_cond_broadcast(&route->change);
    pthread_mutex_unlock(&route->lock);
    return done ? (ssize_t)(done * frame_bytes) : error;
}

static int reader_standby(struct audio_stream *stream) {
    struct shared_reader *reader = (struct shared_reader *)stream;
    struct shared_route *route = reader->route;
    pthread_mutex_lock(&route->lock);
    catch_up(reader);
    reader->active = false;
    ++reader->generation;
    pthread_cond_broadcast(&route->change);
    pthread_mutex_unlock(&route->lock);
    return 0;
}

static uint32_t reader_lost(struct audio_stream_in *stream) {
    struct shared_reader *reader = (struct shared_reader *)stream;
    pthread_mutex_lock(&reader->route->lock);
    catch_up(reader);
    uint32_t lost = reader->lost;
    reader->lost = 0;
    pthread_mutex_unlock(&reader->route->lock);
    return lost;
}

static const struct shared_route *stream_route(const struct audio_stream *stream) {
    return ((const struct shared_reader *)stream)->route;
}
static uint32_t stream_rate(const struct audio_stream *stream) {
    return stream_route(stream)->config.sample_rate;
}
static size_t stream_buffer(const struct audio_stream *stream) {
    return stream_route(stream)->period_bytes;
}
static audio_channel_mask_t stream_channels(const struct audio_stream *stream) {
    return stream_route(stream)->config.channel_mask;
}
static audio_format_t stream_format(const struct audio_stream *stream) {
    return stream_route(stream)->config.format;
}
static int stream_set_rate(struct audio_stream *stream, uint32_t rate) {
    return rate == stream_rate(stream) ? 0 : -ENOSYS;
}
static int stream_set_format(struct audio_stream *stream, audio_format_t format) {
    return format == stream_format(stream) ? 0 : -ENOSYS;
}
static int stream_dump(const struct audio_stream *stream, int fd) {
    (void)stream;
    (void)fd;
    return 0;
}
/* Match the stock virtual input's no-op controls without letting any logical
 * reader mutate the single factory input or another reader's route. */
static int stream_set_parameters(struct audio_stream *stream, const char *pairs) {
    (void)stream;
    (void)pairs;
    return 0;
}
static char *stream_get_parameters(const struct audio_stream *stream, const char *keys) {
    (void)stream;
    (void)keys;
    return strdup("");
}
static int stream_effect(const struct audio_stream *stream, effect_handle_t effect) {
    (void)stream;
    (void)effect;
    return 0;
}
static int reader_gain(struct audio_stream_in *stream, float gain) {
    (void)stream;
    (void)gain;
    return 0;
}

static const struct audio_stream_in fanout_stream = {
    .common = {.get_sample_rate = stream_rate, .set_sample_rate = stream_set_rate,
        .get_buffer_size = stream_buffer, .get_channels = stream_channels,
        .get_format = stream_format, .set_format = stream_set_format,
        .standby = reader_standby, .dump = stream_dump,
        .set_parameters = stream_set_parameters, .get_parameters = stream_get_parameters,
        .add_audio_effect = stream_effect, .remove_audio_effect = stream_effect},
    .set_gain = reader_gain, .read = reader_read, .get_input_frames_lost = reader_lost,
};

static bool same_config(const struct audio_config *a, const struct audio_config *b) {
    return a->sample_rate == b->sample_rate && a->channel_mask == b->channel_mask &&
           a->format == b->format;
}

/* Called with the device lock held. Factory output calls remain untouched. */
static int create_route(struct shared_device *device, audio_io_handle_t handle,
    audio_devices_t devices, struct audio_config *config, audio_input_flags_t flags,
    const char *address, audio_source_t source, struct shared_route **result,
    const char **stage) {
    *stage = "allocate-route";
    struct shared_route *route = calloc(1, sizeof(*route));
    if (!route) return -ENOMEM;
    route->device = device;
    memcpy(route->address, address, strlen(address) + 1);
    *stage = "factory-open";
    int status = device->factory->open_input_stream(device->factory, handle, devices,
        config, &route->factory, flags, address, source);
    if (status) goto fail_route;
    if (!route->factory) { status = -EIO; goto fail_route; }
    *stage = "factory-properties";
    const struct audio_stream_in *input = route->factory;
    const struct audio_stream *stream = &input->common;
    if (!stream->get_buffer_size || !stream->get_sample_rate || !stream->get_channels ||
            !stream->get_format || !input->read) { status = -ENOSYS; goto fail_input; }
    route->config = *config;
    route->config.sample_rate = stream->get_sample_rate(stream);
    route->config.channel_mask = stream->get_channels(stream);
    route->config.format = stream->get_format(stream);
    const uint32_t channels = audio_channel_count_from_in_mask(route->config.channel_mask);
    if (!route->config.sample_rate || route->config.sample_rate > 192000 ||
            !channels || channels > 2 || route->config.format != AUDIO_FORMAT_PCM_16_BIT) {
        status = -EINVAL;
        goto fail_input;
    }
    route->frame_bytes = channels * sizeof(int16_t);
    route->period_bytes = stream->get_buffer_size(stream);
    *stage = "factory-period";
    if (!route->period_bytes || route->period_bytes > MAX_PERIOD_BYTES ||
            route->period_bytes % route->frame_bytes) { status = -EINVAL; goto fail_input; }
    const size_t min_frames = 3 * (route->period_bytes / route->frame_bytes);
    route->capacity = route->config.sample_rate * RING_MS / 1000;
    if (route->capacity < min_frames) route->capacity = min_frames;
    *stage = "allocate-pump-buffers";
    route->period = malloc(route->period_bytes);
    route->ring = malloc(route->capacity * route->frame_bytes);
    if (!route->period || !route->ring) { status = -ENOMEM; goto fail_buffers; }
    *stage = "start-pump";
    status = -pthread_mutex_init(&route->lock, NULL);
    if (status) goto fail_buffers;
    status = -pthread_cond_init(&route->change, NULL);
    if (status) goto fail_lock;
    status = -pthread_create(&route->pump, NULL, pump_input, route);
    if (status) goto fail_change;
    *config = route->config;
    route->next = device->routes;
    device->routes = route;
    *result = route;
    return 0;

fail_change:
    pthread_cond_destroy(&route->change);
fail_lock:
    pthread_mutex_destroy(&route->lock);
fail_buffers:
    free(route->ring);
    free(route->period);
fail_input:
    device->factory->close_input_stream(device->factory, route->factory);
fail_route:
    free(route);
    return status;
}

/* Called under device->lock. Once signalled, the pump no longer needs any
 * lock or vendor call. Join before freeing the route or unloading the DSO. */
static void reap_closed_routes(struct shared_device *device) {
    for (struct shared_route **link = &device->routes; *link;) {
        struct shared_route *route = *link;
        if (!route->factory_closed || route->close_waiting) {
            link = &route->next;
            continue;
        }
        pthread_join(route->pump, NULL);
        *link = route->next;
        pthread_cond_destroy(&route->change);
        pthread_mutex_destroy(&route->lock);
        free(route->period);
        free(route->ring);
        free(route);
    }
}

static struct shared_route *find_route(struct shared_device *device, const char *address) {
    reap_closed_routes(device);
    struct shared_route *route = device->routes;
    while (route && strcmp(route->address, address)) route = route->next;
    return route;
}

static int close_deadline(struct timespec *deadline) {
    if (clock_gettime(CLOCK_MONOTONIC, deadline)) return errno;
    deadline->tv_nsec += CLOSE_WAIT_MS * 1000000L;
    if (deadline->tv_nsec >= 1000000000L) {
        ++deadline->tv_sec;
        deadline->tv_nsec -= 1000000000L;
    }
    return 0;
}

/* Enter and leave with the device lock held. A waiter counts as an active
 * user of the device even after the closing route is removed from the list. */
static int wait_for_route_close(struct shared_device *device, const char *address,
                                struct shared_route **route) {
    struct timespec deadline;
    int status = close_deadline(&deadline);
    if (status) return -status;
    ++device->reopen_waiters;
    do {
        status = pthread_cond_timedwait(&device->route_closed, &device->lock, &deadline);
        // The original route may already have been freed while we slept.
        *route = find_route(device, address);
    } while (!status && *route && (*route)->closing);
    --device->reopen_waiters;
    // A close can finish at the deadline. The current route state decides
    // whether it is safe to open, even if the condition reported timeout.
    if (status == ETIMEDOUT && (!*route || !(*route)->closing)) return 0;
    return -status;
}

static int open_input(struct audio_hw_device *dev, audio_io_handle_t handle,
    audio_devices_t devices, struct audio_config *config, struct audio_stream_in **input,
    audio_input_flags_t flags, const char *address, audio_source_t source) {
    struct shared_device *device = (struct shared_device *)dev;
    pthread_mutex_lock(&device->lock);
    int status;
    const char *stage = "passthrough-open";
#ifdef SHARED_MIC_OPEN_DIAGNOSTICS
    const struct audio_config requested = config ? *config : (struct audio_config){0};
#endif
    if (!recorder_address(address)) {
        status = device->factory->open_input_stream(device->factory, handle, devices,
            config, input, flags, address, source);
        if (!status) ++device->foreign_inputs;
        goto done;
    }
    stage = "invalid-arguments";
    if (!config || !input) { status = -EINVAL; goto done; }
    *input = NULL;
    if (devices != AUDIO_DEVICE_IN_REMOTE_SUBMIX) { status = -EINVAL; goto done; }
    if (flags & AUDIO_INPUT_FLAG_MMAP_NOIRQ) { status = -ENOSYS; goto done; }
    struct shared_route *route = find_route(device, address);
    if (route && route->closing) {
        status = wait_for_route_close(device, address, &route);
        if (status) { stage = "route-close-wait"; goto done; }
    }
    if (route && !same_config(config, &route->config)) {
        stage = "route-config-mismatch";
        config->sample_rate = route->config.sample_rate;
        config->channel_mask = route->config.channel_mask;
        config->format = route->config.format;
        status = -EINVAL;
        goto done;
    }
    stage = "allocate-reader";
    struct shared_reader *reader = calloc(1, sizeof(*reader));
    if (!reader) { status = -ENOMEM; goto done; }
    if (!route) {
        status = create_route(device, handle, devices, config, flags, address, source, &route, &stage);
        if (status) { free(reader); goto done; }
    }
    reader->stream = fanout_stream;
    reader->route = route;
    reader->next = route->readers;
    route->readers = reader;
    *input = &reader->stream;
    stage = "shared-input-open";
    status = 0;
done:
    pthread_mutex_unlock(&device->lock);
#ifdef SHARED_MIC_OPEN_DIAGNOSTICS
    const struct audio_config returned = config ? *config : (struct audio_config){0};
    if (status || !same_config(&requested, &returned)) {
        __android_log_print(ANDROID_LOG_WARN, "SharedMicHal",
            "input io=%d device=%#x route=%.32s source=%u flags=%#x stage=%s status=%d "
            "requested=%u/%#x/%#x returned=%u/%#x/%#x",
            handle, (unsigned)devices, address ? address : "", (unsigned)source,
            (unsigned)flags, stage, status,
            requested.sample_rate, (unsigned)requested.channel_mask, (unsigned)requested.format,
            returned.sample_rate, (unsigned)returned.channel_mask, (unsigned)returned.format);
    }
#endif
    return status;
}

static void close_input(struct audio_hw_device *dev, struct audio_stream_in *input) {
    struct shared_device *device = (struct shared_device *)dev;
    pthread_mutex_lock(&device->lock);
    for (struct shared_route **link = &device->routes; *link; link = &(*link)->next) {
        struct shared_route *route = *link;
        for (struct shared_reader **entry = &route->readers; *entry; entry = &(*entry)->next) {
            struct shared_reader *reader = *entry;
            if (&reader->stream != input) continue;
            *entry = reader->next;
            const bool last = !route->readers;
            if (last) {
                route->closing = true;
                route->close_waiting = true;
            }
            pthread_mutex_lock(&route->lock);
            reader->closing = true;
            if (last) route->stopping = true;
            pthread_cond_broadcast(&route->change);
            while (reader->reading) pthread_cond_wait(&route->change, &route->lock);
            pthread_mutex_unlock(&route->lock);
            free(reader);
            if (last) {
                struct timespec deadline;
                int status = close_deadline(&deadline);
                while (!status && !route->factory_closed)
                    status = pthread_cond_timedwait(&device->route_closed, &device->lock, &deadline);
                route->close_waiting = false;
                if (route->factory_closed) {
                    reap_closed_routes(device);
                    pthread_cond_broadcast(&device->route_closed);
                }
            }
            pthread_mutex_unlock(&device->lock);
            return;
        }
    }
    device->factory->close_input_stream(device->factory, input);
    if (device->foreign_inputs) --device->foreign_inputs;
    pthread_mutex_unlock(&device->lock);
}

static int open_output(struct audio_hw_device *dev, audio_io_handle_t handle,
    audio_devices_t devices, audio_output_flags_t flags, struct audio_config *config,
    struct audio_stream_out **output, const char *address) {
    struct shared_device *device = (struct shared_device *)dev;
    pthread_mutex_lock(&device->lock);
    int status = device->factory->open_output_stream(device->factory, handle, devices,
        flags, config, output, address);
    if (!status) ++device->outputs;
    pthread_mutex_unlock(&device->lock);
    return status;
}

static void close_output(struct audio_hw_device *dev, struct audio_stream_out *output) {
    struct shared_device *device = (struct shared_device *)dev;
    pthread_mutex_lock(&device->lock);
    device->factory->close_output_stream(device->factory, output);
    if (device->outputs) --device->outputs;
    pthread_mutex_unlock(&device->lock);
}

static char *get_parameters(const struct audio_hw_device *dev, const char *keys) {
    if (keys && !strcmp(keys, VERSION_KEY)) return strdup(VERSION_KEY "=1");
    struct audio_hw_device *original = factory_device(dev);
    return original->get_parameters ? original->get_parameters(original, keys) : strdup("");
}

/* Passing a proxy device to these methods would be an invalid vendor downcast. */
#define FWD0(ret, name, cv) \
    static ret forward_##name(cv struct audio_hw_device *dev) { \
        struct audio_hw_device *factory = factory_device(dev); return factory->name(factory); }
#define FWD1(ret, name, cv, type) \
    static ret forward_##name(cv struct audio_hw_device *dev, type value) { \
        struct audio_hw_device *factory = factory_device(dev); return factory->name(factory, value); }
FWD0(uint32_t, get_supported_devices, const)
FWD0(int, init_check, const)
FWD1(int, set_voice_volume, , float)
FWD1(int, set_master_volume, , float)
FWD1(int, get_master_volume, , float *)
FWD1(int, set_mode, , audio_mode_t)
FWD1(int, set_mic_mute, , bool)
FWD1(int, get_mic_mute, const, bool *)
FWD1(int, set_parameters, , const char *)
FWD1(size_t, get_input_buffer_size, const, const struct audio_config *)
FWD1(int, dump, const, int)
FWD1(int, set_master_mute, , bool)
FWD1(int, get_master_mute, , bool *)
FWD1(int, release_audio_patch, , audio_patch_handle_t)
FWD1(int, get_audio_port, , struct audio_port *)
FWD1(int, set_audio_port_config, , const struct audio_port_config *)
#undef FWD0
#undef FWD1

static int forward_create_audio_patch(struct audio_hw_device *dev,
    unsigned nsrc, const struct audio_port_config *src,
    unsigned nsink, const struct audio_port_config *sink, audio_patch_handle_t *handle) {
    struct audio_hw_device *factory = factory_device(dev);
    return factory->create_audio_patch(factory, nsrc, src, nsink, sink, handle);
}

static int close_device(struct hw_device_t *hw) {
    struct shared_device *device = (struct shared_device *)hw;
    pthread_mutex_lock(&device->lock);
    reap_closed_routes(device);
    /* The framework owns output and ordinary input lifetimes. Keep the
     * original image loaded until it has closed all of those streams. */
    if (device->routes || device->foreign_inputs || device->outputs || device->reopen_waiters) {
        pthread_mutex_unlock(&device->lock);
        return -EBUSY;
    }
    int status = audio_hw_device_close(device->factory);
    pthread_mutex_unlock(&device->lock);
    if (!status) {
        pthread_cond_destroy(&device->route_closed);
        pthread_mutex_destroy(&device->lock);
        dlclose(device->factory_lib);
        free(device);
    }
    return status;
}

static int open_device(const struct hw_module_t *module, const char *name, struct hw_device_t **hw) {
    if (!hw || !name || strcmp(name, AUDIO_HARDWARE_INTERFACE)) return -EINVAL;
    *hw = NULL;
    void *lib = dlopen(ORIGINAL_SUBMIX_PATH, RTLD_NOW | RTLD_LOCAL);
    if (!lib) return -ENOENT;
    struct audio_module *original = dlsym(lib, HAL_MODULE_INFO_SYM_AS_STR);
    int status = -EINVAL;
    if (!original || &original->common == module || original->common.tag != HARDWARE_MODULE_TAG ||
            !original->common.id || strcmp(original->common.id, AUDIO_HARDWARE_MODULE_ID) ||
            !original->common.methods || !original->common.methods->open) goto fail_lib;
    struct shared_device *device = calloc(1, sizeof(*device));
    if (!device) { status = -ENOMEM; goto fail_lib; }
    status = -pthread_mutex_init(&device->lock, NULL);
    if (status) goto fail_device;
    pthread_condattr_t condition_attr;
    status = -pthread_condattr_init(&condition_attr);
    if (status) goto fail_lock;
    status = -pthread_condattr_setclock(&condition_attr, CLOCK_MONOTONIC);
    if (!status) status = -pthread_cond_init(&device->route_closed, &condition_attr);
    pthread_condattr_destroy(&condition_attr);
    if (status) goto fail_lock;
    status = audio_hw_device_open(&original->common, &device->factory);
    if (status) goto fail_condition;
    struct audio_hw_device *factory = device->factory;
    status = -ENOSYS;
    if (!factory || !factory->common.close) goto fail_condition;
    const uint32_t version = factory->common.version;
    if (factory->common.tag != HARDWARE_DEVICE_TAG || version < AUDIO_DEVICE_API_VERSION_2_0 ||
            version > AUDIO_DEVICE_API_VERSION_CURRENT) goto fail_factory;
    if (!factory->open_input_stream || !factory->close_input_stream) goto fail_factory;
    device->factory_lib = lib;
    device->dev.common = factory->common;
    device->dev.common.module = (struct hw_module_t *)module;
    device->dev.common.close = close_device;
    device->dev.open_input_stream = open_input;
    device->dev.close_input_stream = close_input;
    device->dev.open_output_stream = factory->open_output_stream ? open_output : NULL;
    device->dev.close_output_stream = factory->close_output_stream ? close_output : NULL;
    device->dev.get_parameters = get_parameters;
#define SET(name) device->dev.name = factory->name ? forward_##name : NULL
    SET(get_supported_devices);
    SET(init_check);
    SET(set_voice_volume);
    SET(set_master_volume);
    SET(get_master_volume);
    SET(set_mode);
    SET(set_mic_mute);
    SET(get_mic_mute);
    SET(set_parameters);
    SET(get_input_buffer_size);
    SET(dump);
    SET(set_master_mute);
    SET(get_master_mute);
    if (version >= AUDIO_DEVICE_API_VERSION_3_0) {
        SET(create_audio_patch);
        SET(release_audio_patch);
        SET(get_audio_port);
        SET(set_audio_port_config);
    }
#undef SET
    *hw = &device->dev.common;
    return 0;

fail_factory:
    audio_hw_device_close(device->factory);
fail_condition:
    pthread_cond_destroy(&device->route_closed);
fail_lock:
    pthread_mutex_destroy(&device->lock);
fail_device:
    free(device);
fail_lib:
    dlclose(lib);
    return status;
}

static struct hw_module_methods_t methods = {.open = open_device};
__attribute__((visibility("default"))) struct audio_module HAL_MODULE_INFO_SYM = {.common = {
    .tag = HARDWARE_MODULE_TAG, .module_api_version = AUDIO_MODULE_API_VERSION_0_1,
    .hal_api_version = HARDWARE_HAL_API_VERSION, .id = AUDIO_HARDWARE_MODULE_ID,
    .name = "Shared recorder remote submix", .author = "CarPlayAudio", .methods = &methods,
}};
