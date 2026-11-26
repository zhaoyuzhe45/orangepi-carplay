/* Isolated Android device probe. Loads the candidate without installing it. */
#include <dlfcn.h>
#include <errno.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <hardware/audio.h>

#define RECORDER_ROUTE "87650:ap:9mixr:0"
#define PLAYER_ROUTE "87650:ap:9mixp:1"
#define DEFAULT_RATE 16000u
#define FRAME_BYTES (2 * sizeof(int16_t))
#define WRAP 30000u
#define CHECK(expr) do { if (!(expr)) { fprintf(stderr, "probe:%d failed: %s\n", \
    __LINE__, #expr); return 1; } } while (0)

static void pause_ms(unsigned ms) {
    struct timespec delay = {ms / 1000, (long)(ms % 1000) * 1000000};
    while (nanosleep(&delay, &delay) && errno == EINTR) {}
}
static struct audio_config stereo_config(unsigned rate, audio_channel_mask_t mask) {
    return (struct audio_config){.sample_rate = rate, .channel_mask = mask,
        .format = AUDIO_FORMAT_PCM_16_BIT};
}
struct writer {
    struct audio_stream_out *stream;
    size_t period;
    bool inverted;
    atomic_bool stop;
    atomic_uint writes;
    int error;
};
struct reader {
    struct audio_stream_in *stream;
    const char *name;
    size_t period;
    unsigned periods;
    bool inverted;
    size_t total, silent, malformed, discontinuities;
    unsigned first, last, lost;
    int error;
};
static void *produce(void *arg) {
    struct writer *writer = arg;
    size_t period = writer->period;
    int16_t *pcm = malloc(period);
    if (!pcm) { writer->error = -ENOMEM; return NULL; }
    unsigned seq = 0;
    while (!writer->stop) {
        for (size_t f = 0; f < period / FRAME_BYTES; ++f) {
            int sample = (int)((seq + f) % WRAP + 1);
            pcm[f * 2] = pcm[f * 2 + 1] = (int16_t)(writer->inverted ? -sample : sample);
        }
        ssize_t bytes = writer->stream->write(writer->stream, pcm, period);
        if (!bytes && writer->stop) break; // Factory shutdown can wake a blocked write with zero.
        if (bytes <= 0 || (size_t)bytes > period || (size_t)bytes % FRAME_BYTES) {
            writer->error = bytes < 0 ? (int)bytes : -EIO;
            break;
        }
        seq = (seq + (unsigned)bytes / FRAME_BYTES) % WRAP;
        ++writer->writes;
    }
    free(pcm);
    return NULL;
}
static void *consume(void *arg) {
    struct reader *reader = arg;
    int16_t *pcm = malloc(reader->period);
    if (!pcm) { reader->error = -ENOMEM; return NULL; }
    for (unsigned i = 0; i < reader->periods; ++i) {
        ssize_t bytes = reader->stream->read(reader->stream, pcm, reader->period);
        if (bytes != (ssize_t)reader->period) {
            reader->error = bytes < 0 ? (int)bytes : -EIO;
            break;
        }
        for (size_t f = 0; f < (size_t)bytes / FRAME_BYTES; ++f) {
            ++reader->total;
            int left = pcm[f * 2];
            if (!left && !pcm[f * 2 + 1]) { ++reader->silent; continue; }
            unsigned sample = (unsigned)(reader->inverted ? -left : left);
            if (!sample || sample > WRAP || left != pcm[f * 2 + 1]) {
                ++reader->malformed;
                continue;
            }
            if (!reader->first) reader->first = sample;
            if (reader->last && sample != reader->last % WRAP + 1) ++reader->discontinuities;
            reader->last = sample;
        }
    }
    reader->lost = reader->stream->get_input_frames_lost(reader->stream);
    free(pcm);
    return NULL;
}
static void report(const struct reader *r) {
    printf("%s: frames=%zu silent=%zu malformed=%zu gaps=%zu first=%u last=%u lost=%u error=%d\n",
        r->name, r->total, r->silent, r->malformed, r->discontinuities, r->first, r->last,
        r->lost, r->error);
}
static bool good(const struct reader *r) {
    return !r->error && r->total == r->periods * r->period / FRAME_BYTES && r->first &&
        r->silent <= r->total / 100 && !r->malformed && !r->discontinuities && !r->lost;
}
static bool valid_period(size_t bytes, unsigned rate) {
    return bytes && bytes <= 65536 && bytes % FRAME_BYTES == 0 &&
        bytes / FRAME_BYTES < rate / 5;
}

/* A player mix must keep the original stream callbacks. Its negative tone
 * runs alongside the positive recorder tone and detects a crossed route. */
static int check_player_mix(struct audio_hw_device *dev, unsigned rate, const void *shim_base) {
    struct audio_config cfg = stereo_config(rate, AUDIO_CHANNEL_OUT_STEREO);
    struct audio_stream_out *out = NULL;
    CHECK(dev->open_output_stream(dev, 21, AUDIO_DEVICE_OUT_REMOTE_SUBMIX,
        AUDIO_OUTPUT_FLAG_NONE, &cfg, &out, PLAYER_ROUTE) == 0 && out && out->write);
    CHECK(cfg.sample_rate == rate && cfg.channel_mask == AUDIO_CHANNEL_OUT_STEREO &&
        cfg.format == AUDIO_FORMAT_PCM_16_BIT);
    cfg = stereo_config(rate, AUDIO_CHANNEL_IN_STEREO);
    struct audio_stream_in *in = NULL;
    CHECK(dev->open_input_stream(dev, 22, AUDIO_DEVICE_IN_REMOTE_SUBMIX, &cfg,
        &in, AUDIO_INPUT_FLAG_NONE, PLAYER_ROUTE, AUDIO_SOURCE_REMOTE_SUBMIX) == 0 && in && in->read);
    CHECK(cfg.sample_rate == rate && cfg.channel_mask == AUDIO_CHANNEL_IN_STEREO &&
        cfg.format == AUDIO_FORMAT_PCM_16_BIT);
    Dl_info input_method = {0}, output_method = {0};
    CHECK(dladdr((const void *)in->read, &input_method));
    CHECK(dladdr((const void *)out->write, &output_method));
    CHECK(input_method.dli_fbase == output_method.dli_fbase && input_method.dli_fbase != shim_base);

    size_t input_period = in->common.get_buffer_size(&in->common);
    size_t output_period = out->common.get_buffer_size(&out->common);
    CHECK(valid_period(input_period, rate) && valid_period(output_period, rate));
    struct writer w = {.stream = out, .period = output_period, .inverted = true};
    struct reader r = {.name = "mixp-reader", .stream = in, .period = input_period,
        .periods = rate / (unsigned)(input_period / FRAME_BYTES), .inverted = true};
    pthread_t thread;
    CHECK(!pthread_create(&thread, NULL, produce, &w));
    // Prime with one factory period so the raw stock read starts with PCM.
    for (unsigned ms = 0; !w.writes && ms < 1000; ms += 2) pause_ms(2);
    CHECK(w.writes > 0);
    consume(&r);
    w.stop = true;
    if (out->common.set_parameters) out->common.set_parameters(&out->common, "exiting=1");
    CHECK(!pthread_join(thread, NULL));
    dev->close_input_stream(dev, in);
    dev->close_output_stream(dev, out);
    report(&r);
    CHECK(!w.error && good(&r) && r.first == 1);
    printf("mixp route %s: factory input and output callbacks, exact initial sequence\n", PLAYER_ROUTE);
    return 0;
}

int main(int argc, char **argv) {
    setvbuf(stdout, NULL, _IONBF, 0);
    alarm(20); // Fail the disposable process instead of waiting on a stuck factory call.
    if (argc < 2 || argc > 3 ||
            (argc == 3 && strcmp(argv[2], "16000") && strcmp(argv[2], "48000"))) {
        fprintf(stderr, "Usage: %s WRAPPER_SO [16000|48000]\n", argv[0]);
        return 2;
    }
    const unsigned rate = argc == 3 && !strcmp(argv[2], "48000") ? 48000u : DEFAULT_RATE;
    printf("requested_rate=%u\n", rate);
    void *lib = dlopen(argv[1], RTLD_NOW | RTLD_LOCAL);
    if (!lib) fprintf(stderr, "%s\n", dlerror());
    CHECK(lib != NULL);
    struct audio_module *module = dlsym(lib, HAL_MODULE_INFO_SYM_AS_STR);
    CHECK(module && module->common.tag == HARDWARE_MODULE_TAG);
    Dl_info shim = {0};
    CHECK(dladdr(module, &shim));
    struct audio_hw_device *dev = NULL;
    int status = audio_hw_device_open(&module->common, &dev);
    printf("open=%d\n", status);
    CHECK(!status && dev && dev->init_check(dev) == 0);
    char *version = dev->get_parameters(dev, "carplay_shared_mic_version");
    CHECK(version && !strcmp(version, "carplay_shared_mic_version=1"));
    free(version);

    struct audio_stream_out *out = NULL;
    struct audio_config config = stereo_config(rate, AUDIO_CHANNEL_OUT_STEREO);
    CHECK(dev->open_output_stream(dev, 12, AUDIO_DEVICE_OUT_REMOTE_SUBMIX,
        AUDIO_OUTPUT_FLAG_NONE, &config, &out, RECORDER_ROUTE) == 0 && out);
    struct audio_stream_in *a = NULL, *b = NULL;
    config = stereo_config(rate, AUDIO_CHANNEL_IN_STEREO);
    CHECK(dev->open_input_stream(dev, 13, AUDIO_DEVICE_IN_REMOTE_SUBMIX, &config,
        &a, AUDIO_INPUT_FLAG_NONE, RECORDER_ROUTE, AUDIO_SOURCE_MIC) == 0 && a);
    config = stereo_config(rate, AUDIO_CHANNEL_IN_STEREO);
    CHECK(dev->open_input_stream(dev, 14, AUDIO_DEVICE_IN_REMOTE_SUBMIX, &config,
        &b, AUDIO_INPUT_FLAG_NONE, RECORDER_ROUTE, AUDIO_SOURCE_VOICE_RECOGNITION) == 0 && b);
    CHECK(a != b && config.sample_rate == rate &&
        config.channel_mask == AUDIO_CHANNEL_IN_STEREO && config.format == AUDIO_FORMAT_PCM_16_BIT);
    size_t period = a->common.get_buffer_size(&a->common);
    CHECK(valid_period(period, rate));
    const unsigned blocks_per_second = rate / (unsigned)(period / FRAME_BYTES);
    size_t output_period = out->common.get_buffer_size(&out->common);
    CHECK(valid_period(output_period, rate));
    printf("rate=%u input_frames_per_period=%zu output_frames_per_period=%zu\n",
        config.sample_rate, period / FRAME_BYTES, output_period / FRAME_BYTES);

    struct writer w = {.stream = out, .period = output_period};
    pthread_t tw, ta, tb;
    CHECK(!pthread_create(&tw, NULL, produce, &w));
    pause_ms(500);
    unsigned initial_writes = w.writes;
    pause_ms(500);
    CHECK(w.writes > initial_writes + 1); // Inputs are open; neither client has read yet.
    struct reader ra = {.name = "reader-a", .stream = a, .period = period,
        .periods = blocks_per_second * 4};
    struct reader rb = {.name = "reader-b", .stream = b, .period = period,
        .periods = blocks_per_second};
    struct reader resumed = {.name = "reader-b-resumed", .stream = b, .period = period,
        .periods = blocks_per_second};
    CHECK(!pthread_create(&ta, NULL, consume, &ra));
    pause_ms(250);
    CHECK(!pthread_create(&tb, NULL, consume, &rb));
    CHECK(!pthread_join(tb, NULL));
    CHECK(!b->common.standby(&b->common));
    CHECK(!check_player_mix(dev, rate, shim.dli_fbase));
    CHECK(!pthread_create(&tb, NULL, consume, &resumed));
    CHECK(!pthread_join(tb, NULL));
    dev->close_input_stream(dev, b);
    CHECK(!pthread_join(ta, NULL));
    dev->close_input_stream(dev, a);
    unsigned final_writes = w.writes;
    pause_ms(500);
    unsigned after_close = w.writes - final_writes;
    w.stop = true;
    if (out->common.set_parameters) out->common.set_parameters(&out->common, "exiting=1");
    CHECK(!pthread_join(tw, NULL));
    dev->close_output_stream(dev, out);
    CHECK(!audio_hw_device_close(dev));
    CHECK(!dlclose(lib));
    report(&ra); report(&rb); report(&resumed);
    printf("writer: after-last-reader=%u writes=%u error=%d\n", after_close, (unsigned)w.writes, w.error);
    CHECK(after_close > 1 && !w.error);
    CHECK(good(&ra) && good(&rb) && good(&resumed));
    puts("isolated factory submix fan-out passed");
    return 0;
}
