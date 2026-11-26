#include <errno.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <hardware/audio.h>

#define RECORDER "-601694664:ap:1mixr:0"
#define PLAYER "legacy:ap:1mixp:0"
#define PERIOD 64
static void wait_ms(long ms) {
    struct timespec delay = {ms / 1000, ms % 1000 * 1000000};
    while (nanosleep(&delay, &delay) && errno == EINTR) {}
}

#ifdef FAKE_HAL
#include <pthread.h>
static struct audio_hw_device original;
static struct audio_stream_in passthrough, recorder;
static struct audio_stream_out output;
static atomic_uint reads, opened, closed, standby_calls, sequence, read_ms = 4;
static atomic_uint refs;
static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_mutex_t read_gate = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t read_gate_changed = PTHREAD_COND_INITIALIZER;
static bool hold_reads, hold_closes;
static atomic_bool read_blocked, close_blocked;

void fake_hold_reads(bool hold) {
    pthread_mutex_lock(&read_gate);
    hold_reads = hold;
    pthread_cond_broadcast(&read_gate_changed);
    pthread_mutex_unlock(&read_gate);
}
bool fake_read_blocked(void) { return read_blocked; }

void fake_hold_closes(bool hold) {
    pthread_mutex_lock(&read_gate);
    hold_closes = hold;
    pthread_cond_broadcast(&read_gate_changed);
    pthread_mutex_unlock(&read_gate);
}
bool fake_close_blocked(void) { return close_blocked; }

unsigned fake_reads(void) { return reads; }
unsigned fake_opens(void) { return opened; }
unsigned fake_closes(void) { return closed; }
unsigned fake_standbys(void) { return standby_calls; }
void fake_delay(unsigned ms) { read_ms = ms; }
const void *fake_input(void) { return &passthrough; }
const void *fake_output(void) { return &output; }

#define CHECK_REAL(dev) do {if ((dev) != &original) abort();} while (0)
static uint32_t rate(const struct audio_stream *stream) { (void)stream; return 16000; }
static size_t size(const struct audio_stream *stream) { (void)stream; return PERIOD * 2; }
static audio_channel_mask_t channels(const struct audio_stream *stream) {
    (void)stream; return AUDIO_CHANNEL_IN_MONO;
}
static audio_format_t format(const struct audio_stream *stream) {
    (void)stream; return AUDIO_FORMAT_PCM_16_BIT;
}
static int standby(struct audio_stream *stream) {
    if (stream != &recorder.common) abort();
    standby_calls++;
    return 0;
}
static ssize_t read_stream(struct audio_stream_in *stream, void *buffer, size_t bytes) {
    if (stream != &recorder || bytes != PERIOD * 2) abort();
    reads++;
    pthread_mutex_lock(&read_gate);
    read_blocked = hold_reads;
    while (hold_reads) pthread_cond_wait(&read_gate_changed, &read_gate);
    read_blocked = false;
    pthread_mutex_unlock(&read_gate);
    wait_ms(read_ms);
    unsigned start = atomic_fetch_add(&sequence, PERIOD);
    for (unsigned i = 0; i < PERIOD; ++i) ((uint16_t *)buffer)[i] = (start + i) % 65536;
    return (ssize_t)bytes;
}
static int init(const struct audio_hw_device *dev) { CHECK_REAL(dev); return 0; }
static int volume(struct audio_hw_device *dev, float value) {
    CHECK_REAL(dev); return (int)(value * 8);
}
static int patch(struct audio_hw_device *dev, unsigned nsrc,
    const struct audio_port_config *src, unsigned nsink,
    const struct audio_port_config *sink, audio_patch_handle_t *handle) {
    CHECK_REAL(dev); (void)src; (void)sink;
    *handle = (audio_patch_handle_t)(nsrc + nsink);
    return 13;
}
static int input_open(struct audio_hw_device *dev, audio_io_handle_t handle,
    audio_devices_t device, struct audio_config *cfg, struct audio_stream_in **stream,
    audio_input_flags_t flags, const char *address, audio_source_t source) {
    CHECK_REAL(dev);
    if (device != AUDIO_DEVICE_IN_REMOTE_SUBMIX || flags != AUDIO_INPUT_FLAG_NONE ||
        source != AUDIO_SOURCE_REMOTE_SUBMIX || handle != 11) abort();
    if (!address || strcmp(address, RECORDER)) { *stream = &passthrough; return 0; }
    if (cfg->sample_rate != 16000 || cfg->format != AUDIO_FORMAT_PCM_16_BIT ||
        cfg->channel_mask != AUDIO_CHANNEL_IN_MONO) return -EINVAL;
    pthread_mutex_lock(&lock);
    ++refs;
    opened++;
    pthread_mutex_unlock(&lock);
    *stream = &recorder; // What the stock Android 8.1 library does for duplicate opens.
    return 0;
}
static void input_close(struct audio_hw_device *dev, struct audio_stream_in *stream) {
    CHECK_REAL(dev);
    if (stream == &passthrough) return;
    if (stream != &recorder) abort();
    pthread_mutex_lock(&read_gate);
    close_blocked = hold_closes;
    while (hold_closes) pthread_cond_wait(&read_gate_changed, &read_gate);
    close_blocked = false;
    pthread_mutex_unlock(&read_gate);
    pthread_mutex_lock(&lock);
    if (!refs) abort();
    --refs;
    closed++;
    pthread_mutex_unlock(&lock);
}
static char *get_parameters(const struct audio_hw_device *dev, const char *keys) {
    CHECK_REAL(dev);
    if (strcmp(keys, "test_factory_parameter")) abort();
    return strdup("test_factory_parameter=original");
}
static ssize_t output_write(struct audio_stream_out *stream, const void *buffer, size_t bytes) {
    if (stream != &output) abort();
    (void)buffer;
    // Simulate a full MonoPipe: when input is open the writer needs it to drain.
    unsigned start = reads;
    for (unsigned i = 0; refs && reads == start; ++i) {
        if (i == 100) return -ETIMEDOUT;
        wait_ms(1);
    }
    return (ssize_t)bytes;
}
static struct audio_stream_out output = {.write = output_write};
static int output_open(struct audio_hw_device *dev, audio_io_handle_t handle,
    audio_devices_t device, audio_output_flags_t flags, struct audio_config *cfg,
    struct audio_stream_out **stream, const char *address) {
    CHECK_REAL(dev);
    if (handle != 12 || device != AUDIO_DEVICE_OUT_REMOTE_SUBMIX ||
        flags != AUDIO_OUTPUT_FLAG_NONE || cfg->sample_rate != 16000 ||
        (strcmp(address, "0") && strcmp(address, RECORDER) && strcmp(address, PLAYER))) abort();
    *stream = &output;
    return 0;
}
static void output_close(struct audio_hw_device *dev, struct audio_stream_out *stream) {
    CHECK_REAL(dev);
    if (stream != &output) abort();
}
static int device_close(struct hw_device_t *dev) {
    if (dev != &original.common || refs) abort();
    return 0;
}
static int module_open(const struct hw_module_t *module, const char *name,
    struct hw_device_t **dev) {
    if (strcmp(name, AUDIO_HARDWARE_INTERFACE)) return -EINVAL;
    original.common.module = (struct hw_module_t *)module;
    *dev = &original.common;
    return 0;
}
static struct audio_stream_in recorder = {
    .common = {.get_sample_rate = rate, .get_buffer_size = size, .get_channels = channels,
               .get_format = format, .standby = standby},
    .read = read_stream,
};
static struct audio_hw_device original = {
    .common = {.tag = HARDWARE_DEVICE_TAG, .version = AUDIO_DEVICE_API_VERSION_3_0,
               .close = device_close},
    .init_check = init, .set_master_volume = volume, .create_audio_patch = patch,
    .get_parameters = get_parameters,
    .open_input_stream = input_open, .close_input_stream = input_close,
    .open_output_stream = output_open, .close_output_stream = output_close,
};
static struct hw_module_methods_t methods = {.open = module_open};
struct audio_module HAL_MODULE_INFO_SYM = {.common = {
    .tag = HARDWARE_MODULE_TAG, .module_api_version = AUDIO_MODULE_API_VERSION_0_1,
    .hal_api_version = HARDWARE_HAL_API_VERSION, .id = AUDIO_HARDWARE_MODULE_ID,
    .name = "Submix factory test double", .methods = &methods,
}};
#else
#include <dlfcn.h>
#include <pthread.h>
#include <stdio.h>

#define CHECK(condition) do {if (!(condition)) { \
    fprintf(stderr, "%s:%d %s\n", __FILE__, __LINE__, #condition); exit(1); }} while (0)
static struct audio_config config(void) {
    return (struct audio_config){ .sample_rate = 16000, .channel_mask = AUDIO_CHANNEL_IN_MONO,
        .format = AUDIO_FORMAT_PCM_16_BIT };
}
static int open_in(struct audio_hw_device *dev, const char *route,
    struct audio_config *cfg, struct audio_stream_in **stream) {
    return dev->open_input_stream(dev, 11, AUDIO_DEVICE_IN_REMOTE_SUBMIX, cfg, stream,
        AUDIO_INPUT_FLAG_NONE, route, AUDIO_SOURCE_REMOTE_SUBMIX);
}
static void read_period(struct audio_stream_in *stream, uint16_t *samples) {
    CHECK(stream->read(stream, samples, PERIOD * sizeof(*samples)) == PERIOD * sizeof(*samples));
    for (size_t i = 1; i < PERIOD; ++i) CHECK(samples[i] == (uint16_t)(samples[i - 1] + 1));
}
static void read_next(struct audio_stream_in *stream, uint16_t *previous) {
    uint16_t block[PERIOD];
    read_period(stream, block);
    CHECK(block[0] == (uint16_t)(*previous + 1));
    *previous = block[PERIOD - 1];
}
static void *pending_read(void *arg) {
    struct audio_stream_in *stream = arg;
    uint16_t samples[PERIOD * 128];
    ssize_t status = stream->read(stream, samples, sizeof(samples));
    CHECK(status == -EINTR || (status > 0 && status < (ssize_t)sizeof(samples)));
    return NULL;
}
struct closing_route_probe {
    struct audio_hw_device *dev;
    struct audio_stream_in *a;
    unsigned before;
    atomic_bool close_started, close_finished, reopen_started, reopen_finished;
    struct audio_stream_in *reopened;
};
static void *close_while_factory_read_is_blocked(void *arg) {
    struct closing_route_probe *p = arg;
    p->close_started = true;
    p->dev->close_input_stream(p->dev, p->a);
    p->close_finished = true;
    return NULL;
}
static void *reopen_while_factory_read_is_blocked(void *arg) {
    struct closing_route_probe *p = arg;
    struct audio_config cfg = config();
    p->reopen_started = true;
    CHECK(open_in(p->dev, RECORDER, &cfg, &p->reopened) == 0 && p->reopened);
    p->reopen_finished = true;
    return NULL;
}
static void check_independent_routes(struct audio_hw_device *dev) {
    struct audio_config cfg = config();
    struct audio_stream_in *input = NULL;
    CHECK(open_in(dev, PLAYER, &cfg, &input) == 0 && input);
    dev->close_input_stream(dev, input);
    struct audio_stream_out *out = NULL;
    CHECK(dev->open_output_stream(dev, 12, AUDIO_DEVICE_OUT_REMOTE_SUBMIX,
        AUDIO_OUTPUT_FLAG_NONE, &cfg, &out, PLAYER) == 0 && out);
    dev->close_output_stream(dev, out);
}
static int64_t monotonic_ms(void) {
    struct timespec ts;
    CHECK(!clock_gettime(CLOCK_MONOTONIC, &ts));
    return (int64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}
static void *open_lib(const char *lib) {
    void *dso = dlopen(lib, RTLD_NOW | RTLD_LOCAL);
    if (dso == NULL) fprintf(stderr, "%s\n", dlerror());
    CHECK(dso != NULL);
    return dso;
}
int main(int argc, char **argv) {
    CHECK(argc == 3);
    void *fake = open_lib(argv[1]);
    unsigned (*reads)(void) = dlsym(fake, "fake_reads");
    unsigned (*opens)(void) = dlsym(fake, "fake_opens");
    unsigned (*closes)(void) = dlsym(fake, "fake_closes");
    void (*delay)(unsigned) = dlsym(fake, "fake_delay");
    void (*hold)(bool) = dlsym(fake, "fake_hold_reads");
    bool (*blocked)(void) = dlsym(fake, "fake_read_blocked");
    void (*hold_closes)(bool) = dlsym(fake, "fake_hold_closes");
    bool (*close_blocked)(void) = dlsym(fake, "fake_close_blocked");
    const void *(*fake_input)(void) = dlsym(fake, "fake_input");
    const void *(*fake_output)(void) = dlsym(fake, "fake_output");
    CHECK(reads && opens && closes && delay && hold && blocked && fake_input && fake_output);
    void *module_lib = open_lib(argv[2]);
    struct audio_module *module = dlsym(module_lib, HAL_MODULE_INFO_SYM_AS_STR);
    CHECK(module && module->common.tag == HARDWARE_MODULE_TAG);
    struct audio_hw_device *dev = NULL;
    CHECK(audio_hw_device_open(&module->common, &dev) == 0);

    struct audio_stream_in *a = NULL, *b = NULL;
    struct audio_config cfg = config();
    CHECK(open_in(dev, RECORDER, &cfg, &a) == 0);
    CHECK(open_in(dev, RECORDER, &cfg, &b) == 0);
    CHECK(a != b); // RED with the unwrapped HAL.
    CHECK(opens() - closes() <= 1);
    CHECK(dev->init_check(dev) == 0 && dev->set_master_volume(dev, 0.5f) == 4);
    CHECK(dev->get_mic_mute == NULL && dev->release_audio_patch == NULL);
    audio_patch_handle_t patch = 0;
    CHECK(dev->create_audio_patch(dev, 1, NULL, 2, NULL, &patch) == 13 && patch == 3);
    CHECK(audio_hw_device_close(dev) == -EBUSY);
    char *value = dev->get_parameters(dev, "carplay_shared_mic_version");
    CHECK(value && !strcmp(value, "carplay_shared_mic_version=1"));
    free(value);
    value = dev->get_parameters(dev, "test_factory_parameter");
    CHECK(value && !strcmp(value, "test_factory_parameter=original"));
    free(value);
    struct audio_stream_out *out = NULL;
    CHECK(dev->open_output_stream(dev, 12, AUDIO_DEVICE_OUT_REMOTE_SUBMIX,
        AUDIO_OUTPUT_FLAG_NONE, &cfg, &out, RECORDER) == 0 && out == fake_output());
    int16_t sent[PERIOD] = {0};
    CHECK(out->write(out, sent, sizeof(sent)) == sizeof(sent));
    dev->close_output_stream(dev, out);

    struct audio_config other = cfg;
    other.sample_rate = 48000;
    struct audio_stream_in *bad = NULL;
    CHECK(open_in(dev, RECORDER, &other, &bad) == -EINVAL && !bad);
    other = cfg; other.channel_mask = AUDIO_CHANNEL_IN_STEREO;
    CHECK(open_in(dev, RECORDER, &other, &bad) == -EINVAL && !bad);
    other = cfg; other.format = AUDIO_FORMAT_PCM_FLOAT;
    CHECK(open_in(dev, RECORDER, &other, &bad) == -EINVAL && !bad);
    CHECK(a->common.get_sample_rate(&a->common) == 16000);
    CHECK(a->common.get_channels(&a->common) == AUDIO_CHANNEL_IN_MONO);
    CHECK(a->common.get_format(&a->common) == AUDIO_FORMAT_PCM_16_BIT);
    CHECK(a->common.get_buffer_size(&a->common) == PERIOD * 2);
    CHECK(a->common.set_sample_rate(&a->common, 48000) == -ENOSYS);
    CHECK(a->common.set_format(&a->common, AUDIO_FORMAT_PCM_FLOAT) == -ENOSYS);

    uint16_t buf[PERIOD];
    read_period(a, buf);
    uint16_t last_a = buf[PERIOD - 1];
    for (int i = 0; i < 4; ++i) read_next(a, &last_a);
    read_period(b, buf);
    uint16_t last_b = buf[PERIOD - 1];
    CHECK(last_b > last_a && last_b - last_a <= PERIOD * 3);
    for (int i = 0; i < 16; ++i) {read_next(a, &last_a); read_next(b, &last_b);}
    CHECK(opens() - closes() == 1);
    CHECK(a->get_input_frames_lost(a) == 0 && b->get_input_frames_lost(b) == 0);

    CHECK(b->common.standby(&b->common) == 0);
    for (int i = 0; i < 12; ++i) read_next(a, &last_a);
    read_period(b, buf);
    CHECK(buf[0] > last_b + PERIOD * 6);
    last_b = buf[PERIOD - 1];
    for (int i = 0; i < 8; ++i) {read_next(a, &last_a); read_next(b, &last_b);}
    CHECK(b->get_input_frames_lost(b) == 0);

    for (int i = 0; i < 125; ++i) read_next(a, &last_a);
    read_period(b, buf);
    CHECK(buf[0] > last_b + PERIOD * 2);
    CHECK(b->get_input_frames_lost(b) > 0 && b->get_input_frames_lost(b) == 0);
    CHECK(a->get_input_frames_lost(a) == 0);
    dev->close_input_stream(dev, b);
    CHECK(opens() - closes() == 1);
    CHECK(open_in(dev, RECORDER, &cfg, &b) == 0);
    read_period(b, buf); last_b = buf[PERIOD - 1];
    for (int i = 0; i < 8; ++i) {read_next(a, &last_a); read_next(b, &last_b);}
    CHECK(opens() - closes() == 1);

    CHECK(a->common.standby(&a->common) == 0);
    CHECK(b->common.standby(&b->common) == 0);
    // Even during logical standby, the real input is open and must drain
    // the factory pipe until close. A new reader must skip that standby data.
    unsigned idle = reads();
    wait_ms(35);
    CHECK(reads() > idle);
    read_period(a, buf);
    CHECK(buf[0] > last_a);
    dev->close_input_stream(dev, b);
    delay(80);
    unsigned starting_reads = reads();
    pthread_t pending;
    CHECK(pthread_create(&pending, NULL, pending_read, a) == 0);
    for (int i = 0; i < 100 && reads() == starting_reads; ++i) wait_ms(1);
    CHECK(reads() > starting_reads);
    dev->close_input_stream(dev, a);
    CHECK(pthread_join(pending, NULL) == 0 && opens() == closes());
    idle = reads(); wait_ms(35); CHECK(reads() == idle);

    delay(4);
    hold(true);
    CHECK(open_in(dev, RECORDER, &cfg, &a) == 0);
    for (unsigned i = 0; !blocked() && i < 1000; ++i) wait_ms(1);
    CHECK(blocked());
    struct closing_route_probe probe = {.dev = dev, .a = a, .before = opens()};
    pthread_t closer, reopener;
    CHECK(!pthread_create(&closer, NULL, close_while_factory_read_is_blocked, &probe));
    while (!probe.close_started) wait_ms(1);

    // A deliberately incompatible request can observe when close has marked
    // the route without accidentally joining it before the closer has run.
    int status;
    int64_t began;
    struct audio_stream_in *input = NULL;
    do {
        struct audio_config incompatible = config();
        incompatible.sample_rate = 48000;
        began = monotonic_ms();
        status = open_in(dev, RECORDER, &incompatible, &input);
        if (status == -EINVAL) wait_ms(1);
    } while (status == -EINVAL);
    int64_t waited = monotonic_ms() - began;
    CHECK(status == -ETIMEDOUT && !input);
    CHECK(waited >= 250 && waited < 2500);
    for (unsigned i = 0; !probe.close_finished && i < 750; ++i) wait_ms(1);
    CHECK(blocked() && probe.close_finished && opens() == probe.before);
    CHECK(audio_hw_device_close(dev) == -EBUSY);
    check_independent_routes(dev);
    CHECK(!pthread_join(closer, NULL));

    // Now hold the original close itself. The same address must stay
    // reserved, but an unrelated input/output must not wait on the shim.
    hold_closes(true);
    hold(false);
    for (unsigned i = 0; !close_blocked() && i < 1000; ++i) wait_ms(1);
    CHECK(close_blocked() && !blocked());
    CHECK(audio_hw_device_close(dev) == -EBUSY);
    check_independent_routes(dev);
    began = monotonic_ms();
    CHECK(open_in(dev, RECORDER, &cfg, &input) == -ETIMEDOUT && !input);
    CHECK(monotonic_ms() - began >= 250 && opens() == probe.before);
    CHECK(!pthread_create(&reopener, NULL, reopen_while_factory_read_is_blocked, &probe));
    while (!probe.reopen_started) wait_ms(1);
    wait_ms(30);
    CHECK(!probe.reopen_finished && opens() == probe.before);
    CHECK(audio_hw_device_close(dev) == -EBUSY);
    check_independent_routes(dev);
    CHECK(close_blocked() && !probe.reopen_finished);
    hold_closes(false);
    CHECK(!pthread_join(reopener, NULL));
    CHECK(probe.reopen_finished && opens() == probe.before + 1 && opens() == closes() + 1);
    read_period(probe.reopened, buf);
    dev->close_input_stream(dev, probe.reopened);
    CHECK(opens() == closes());

    // If the original read did finish but its close blocks, the caller still
    // has the same deadline. No second worker may open the address early.
    hold_closes(true);
    CHECK(open_in(dev, RECORDER, &cfg, &a) == 0);
    struct closing_route_probe close_only = {.dev = dev, .a = a, .before = opens()};
    CHECK(!pthread_create(&closer, NULL, close_while_factory_read_is_blocked, &close_only));
    for (unsigned i = 0; !close_blocked() && i < 1000; ++i) wait_ms(1);
    CHECK(close_blocked());
    check_independent_routes(dev);
    for (unsigned i = 0; !close_only.close_finished && i < 1750; ++i) wait_ms(1);
    CHECK(close_only.close_finished && close_blocked() && opens() == close_only.before);
    CHECK(!pthread_join(closer, NULL));
    hold_closes(false);
    CHECK(open_in(dev, RECORDER, &cfg, &a) == 0);
    dev->close_input_stream(dev, a);
    CHECK(opens() == closes());

    const char *routes[] = { NULL, "0", PLAYER, "invalidmixr:not-a-number" };
    for (unsigned i = 0; i < sizeof(routes) / sizeof(routes[0]); ++i) {
        CHECK(open_in(dev, routes[i], &cfg, &a) == 0 && a == fake_input());
        dev->close_input_stream(dev, a);
    }
    out = NULL;
    CHECK(dev->open_output_stream(dev, 12, AUDIO_DEVICE_OUT_REMOTE_SUBMIX,
        AUDIO_OUTPUT_FLAG_NONE, &cfg, &out, "0") == 0 && out == fake_output());
    dev->close_output_stream(dev, out);
    CHECK(audio_hw_device_close(dev) == 0);
    CHECK(dlclose(module_lib) == 0 && dlclose(fake) == 0);
    puts("shared microphone HAL: fan-out, lifecycle, bounded loss, config and forwarding passed");
    return 0;
}
#endif
