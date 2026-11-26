#define _GNU_SOURCE
#include "carplay_audio_bridge.h"
#include "airplay_bplist.h"

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netdb.h>
#include <net/if.h>
#include <netinet/in.h>
#include <poll.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>
#include <time.h>

#ifdef __ANDROID__
#include <SLES/OpenSLES.h>
#include <SLES/OpenSLES_Android.h>
#include <SLES/OpenSLES_AndroidConfiguration.h>
#endif

extern void iap2_log(const char *fmt, ...);

#define AUDIO_QUEUE_DEPTH 8u
#define AUDIO_BUFFER_BYTES 4096u
#define AUDIO_RTP_HEADER_BYTES 12u
#define AUDIO_RTP_TAIL_BYTES (AIRPLAY_CHACHA_TAG_BYTES + 8u)
#define AUDIO_ERROR_CAPACITY 192u
#define AUDIO_CAPTURE_FRAME_BYTES 3840u
#define AUDIO_CAPTURE_RATE 48000u
#define AUDIO_CAPTURE_CHANNELS 2u
#ifdef CARPLAY_AUDIO_TESTING
#define MICROPHONE_SOCKET "carplay_audio_microphone_test_v1"
#else
#define MICROPHONE_SOCKET "carplay_audio_microphone_v1"
#endif

struct carplay_audio_stream {
    struct carplay_audio_bridge *bridge;
    int configured;
    int active;
    int type;
    char audio_type[CARPLAY_UUID_CAPACITY];
    uint32_t format_mask;
    uint64_t connection_id;
    struct carplay_audio_pcm_format format;
    uint8_t output_key[AIRPLAY_CHACHA_KEY_BYTES];
    uint8_t input_key[AIRPLAY_CHACHA_KEY_BYTES];
    int microphone;
    int data_fd;
    int control_fd;
    int microphone_fd;
    uint16_t microphone_port;
    uint16_t local_control_port;
    uint8_t packet_pcm[1408];
    size_t packet_used;
    _Atomic int stop;
    pthread_t receive_thread;
    int receive_started;
    pthread_t control_thread;
    int control_started;
    pthread_t microphone_receive_thread;
    int microphone_receive_started;
    pthread_t microphone_uplink_thread;
    int microphone_uplink_started;
    pthread_mutex_t microphone_lock;
    pthread_cond_t microphone_ready;
    struct {
        uint8_t bytes[AUDIO_BUFFER_BYTES];
        size_t len;
    } microphone_queue[AUDIO_QUEUE_DEPTH];
    unsigned microphone_head;
    unsigned microphone_tail;
    unsigned microphone_count;
    struct carplay_audio_rtp_counters capture_counters;
    struct carplay_audio_rtp_counters microphone_counters;
    _Atomic unsigned long packets_received;
    _Atomic unsigned long packets_decrypted;
    _Atomic unsigned long packets_dropped;
    _Atomic unsigned long capture_packets_sent;
    _Atomic unsigned long capture_bytes_sent;
    _Atomic unsigned long capture_packets_failed;
#ifdef CARPLAY_AUDIO_TESTING
    int force_capture_send_failure;
#endif
    _Atomic unsigned long microphone_packets_sent;
#ifdef __ANDROID__
    SLObjectItf player_object;
    SLPlayItf player;
    SLAndroidSimpleBufferQueueItf player_queue;
    pthread_mutex_t player_lock;
    struct {
        uint8_t bytes[AUDIO_BUFFER_BYTES];
        int busy;
    } player_buffers[AUDIO_QUEUE_DEPTH];
    unsigned player_order[AUDIO_QUEUE_DEPTH];
    unsigned player_head;
    unsigned player_tail;
#endif
};

struct carplay_audio_bridge {
    pthread_mutex_t lock;
    pthread_mutex_t media_lock;
    _Atomic int paused;
    uint8_t shared_secret[32];
    int enable_media;
    int enable_microphone;
    int legacy;
    int legacy_call;
    uint32_t legacy_media_format;
    uint32_t legacy_call_format;
    uint32_t legacy_nav_format;
    int legacy_nav_failed;
    int legacy_separate;
    struct carplay_audio_pcm_format legacy_mic_format;
    int mic_listener;
    pthread_t mic_thread;
    int mic_thread_started;
    _Atomic int mic_stop;
    _Atomic int mic_ready;
    _Atomic int mic_demand;
    _Atomic uint64_t mic_heartbeat_ms;
    _Atomic uint64_t capture_input_ms;
    uint64_t capture_silence_ms;
    int prepared;
    struct carplay_audio_stream streams[CARPLAY_AUDIO_MAX_STREAMS];
    size_t stream_count;
    int capture_listener_fd;
    int capture_split_listener_fd;
    _Atomic int capture_split_ready;
    _Atomic int capture_stop;
    pthread_t capture_thread;
    int capture_started;
    _Atomic unsigned long capture_connections;
    char error[AUDIO_ERROR_CAPACITY];
#ifdef __ANDROID__
    SLObjectItf engine_object;
    SLEngineItf engine;
    SLObjectItf output_mix;
#endif
};

static void bridge_error(struct carplay_audio_bridge *bridge, const char *message)
{
    if (!bridge)
        return;
    pthread_mutex_lock(&bridge->lock);
    snprintf(bridge->error, sizeof(bridge->error), "%s", message ? message : "");
    pthread_mutex_unlock(&bridge->lock);
}

static void put_u16_be(uint8_t *out, uint16_t value)
{
    out[0] = (uint8_t)(value >> 8);
    out[1] = (uint8_t)value;
}

static void put_u32_be(uint8_t *out, uint32_t value)
{
    out[0] = (uint8_t)(value >> 24);
    out[1] = (uint8_t)(value >> 16);
    out[2] = (uint8_t)(value >> 8);
    out[3] = (uint8_t)value;
}

static unsigned long stream_stat_increment(_Atomic unsigned long *stat)
{
    return atomic_fetch_add_explicit(stat, 1ul, memory_order_relaxed) + 1ul;
}

static unsigned long stream_stat_get(const _Atomic unsigned long *stat)
{
    return atomic_load_explicit(stat, memory_order_relaxed);
}

static int stream_stop_requested(const struct carplay_audio_stream *stream)
{
    return atomic_load_explicit(&stream->stop, memory_order_acquire);
}

static void stream_request_stop(struct carplay_audio_stream *stream)
{
    atomic_store_explicit(&stream->stop, 1, memory_order_release);
}

static void *capture_receive_main(void *opaque);
static int legacy_prepare(struct carplay_audio_bridge *, const struct carplay_session_capabilities *);
static void legacy_ipc_stop(struct carplay_audio_bridge *);
static uint64_t audio_now_ms(void);
static void legacy_capture_silence(struct carplay_audio_bridge *);
static void legacy_stop_nav(struct carplay_audio_bridge *);
static void submit_split_frame(struct carplay_audio_bridge *, const uint8_t *);

#ifdef CARPLAY_AUDIO_TESTING
#define CAPTURE_SPLIT_SOCKET "carplay_audio_capture_test_v2"
#else
#define CAPTURE_SPLIT_SOCKET "carplay_audio_capture_v2"
#endif

static int capture_socket_open(const char *name)
{
    struct sockaddr_un address;
    size_t name_len = strlen(name);
    socklen_t address_len;
    int fd;

    if (name_len + 1u >= sizeof(address.sun_path))
        return -1;
    fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (fd < 0)
        return -1;
    memset(&address, 0, sizeof(address));
    address.sun_family = AF_UNIX;
    address.sun_path[0] = '\0';
    memcpy(address.sun_path + 1, name, name_len);
    address_len = (socklen_t)(offsetof(struct sockaddr_un, sun_path) +
                              1u + name_len);
    if (bind(fd, (const struct sockaddr *)&address, address_len) < 0 ||
        listen(fd, 1) < 0) {
        close(fd);
        return -1;
    }
    return fd;
}

static int capture_start(struct carplay_audio_bridge *bridge)
{
    if (!bridge || bridge->capture_started)
        return 0;
    bridge->capture_listener_fd = capture_socket_open(CARPLAY_AUDIO_CAPTURE_SOCKET);
    if (bridge->capture_listener_fd < 0) {
        iap2_log("audio: could not create Android capture socket: %s",
                 strerror(errno));
        return -1;
    }
    if (bridge->legacy && bridge->legacy_nav_format)
        bridge->capture_split_listener_fd = capture_socket_open(CAPTURE_SPLIT_SOCKET);
    atomic_store_explicit(&bridge->capture_stop, 0, memory_order_release);
    if (pthread_create(&bridge->capture_thread, NULL, capture_receive_main,
                       bridge) != 0) {
        close(bridge->capture_listener_fd);
        bridge->capture_listener_fd = -1;
        if (bridge->capture_split_listener_fd >= 0)
            close(bridge->capture_split_listener_fd);
        bridge->capture_split_listener_fd = -1;
        return -1;
    }
    bridge->capture_started = 1;
    iap2_log("audio: Android capture socket ready name=%s",
             CARPLAY_AUDIO_CAPTURE_SOCKET);
    return 0;
}

static void capture_stop(struct carplay_audio_bridge *bridge)
{
    if (!bridge || !bridge->capture_started)
        return;
    atomic_store_explicit(&bridge->capture_stop, 1, memory_order_release);
    if (bridge->capture_listener_fd >= 0)
        (void)shutdown(bridge->capture_listener_fd, SHUT_RDWR);
    if (bridge->capture_split_listener_fd >= 0)
        (void)shutdown(bridge->capture_split_listener_fd, SHUT_RDWR);
    pthread_join(bridge->capture_thread, NULL);
    if (bridge->capture_listener_fd >= 0)
        close(bridge->capture_listener_fd);
    if (bridge->capture_split_listener_fd >= 0)
        close(bridge->capture_split_listener_fd);
    bridge->capture_split_listener_fd = -1;
    atomic_store(&bridge->capture_split_ready, 0);
    bridge->capture_listener_fd = -1;
    bridge->capture_started = 0;
    atomic_store_explicit(&bridge->capture_stop, 0, memory_order_release);
}

static void *capture_receive_main(void *opaque)
{
    struct carplay_audio_bridge *bridge = opaque;
    uint8_t frame[3 * AUDIO_CAPTURE_FRAME_BYTES];
    size_t used = 0;
    size_t frame_len = AUDIO_CAPTURE_FRAME_BYTES;
    int client = -1;

    while (!atomic_load_explicit(&bridge->capture_stop,
                                 memory_order_acquire)) {
        struct pollfd pfd;
        int ready;
        if (bridge->legacy) legacy_capture_silence(bridge);

        if (client < 0) {
            struct pollfd listeners[] = {
                {bridge->capture_split_listener_fd, POLLIN, 0},
                {bridge->capture_listener_fd, POLLIN, 0}
            };
            ready = poll(listeners, 2, bridge->legacy ? 20 : 250);
            if (ready < 0 && errno == EINTR)
                continue;
            int split = !!(listeners[0].revents & POLLIN);
            if (ready <= 0 || (!split && !(listeners[1].revents & POLLIN)))
                continue;
            if (atomic_load_explicit(&bridge->capture_stop,
                                     memory_order_acquire))
                break;
            client = accept4(listeners[split ? 0 : 1].fd, NULL, NULL,
                             SOCK_CLOEXEC);
            if (client < 0) {
                if (errno != EINTR && errno != EAGAIN &&
                    errno != EWOULDBLOCK) {
                    iap2_log("audio: Android capture accept failed: %s",
                             strerror(errno));
                    break;
                }
                continue;
            }
            frame_len = split ? sizeof(frame) : AUDIO_CAPTURE_FRAME_BYTES;
            used = 0;
            if (atomic_fetch_add_explicit(&bridge->capture_connections,
                                          1ul, memory_order_relaxed) == 0ul)
                iap2_log("audio: Android capture connected");
            else
                iap2_log("audio: Android capture reconnected");
            continue;
        }

        pfd.fd = client;
        pfd.events = POLLIN;
        pfd.revents = 0;
        ready = poll(&pfd, 1, bridge->legacy ? 20 : 250);
        if (ready < 0 && errno == EINTR)
            continue;
        if (ready <= 0)
            continue;
        if (pfd.revents & (POLLHUP | POLLERR | POLLNVAL)) {
            goto disconnected;
        }
        if (pfd.revents & POLLIN) {
            ssize_t got = recv(client, frame + used, frame_len - used, MSG_DONTWAIT);
            if (got == 0) goto disconnected;
            if (got < 0) {
                if (errno == EINTR || errno == EAGAIN ||
                    errno == EWOULDBLOCK)
                    continue;
                goto disconnected;
            }
            used += (size_t)got;
            if (used == frame_len) {
                if (frame_len == sizeof(frame)) submit_split_frame(bridge, frame);
                else (void)carplay_audio_bridge_submit_capture_pcm(bridge, frame, frame_len);
                used = 0;
            }
        }
        continue;
disconnected:
        atomic_store(&bridge->capture_split_ready, 0);
        close(client);
        client = -1;
    }
    if (client >= 0) close(client);
    return NULL;
}

static void put_u64_le(uint8_t *out, uint64_t value)
{
    unsigned i;
    for (i = 0; i < 8; i++) {
        out[i] = (uint8_t)value;
        value >>= 8;
    }
}

static void put_s16_le(uint8_t *out, int value)
{
    if (value < -32768)
        value = -32768;
    else if (value > 32767)
        value = 32767;
    out[0] = (uint8_t)value;
    out[1] = (uint8_t)(value >> 8);
}

static int16_t read_s16_le(const uint8_t *in)
{
    return (int16_t)((uint16_t)in[0] | ((uint16_t)in[1] << 8));
}

/*
 * The Android capture endpoint is fixed at stereo 48 kHz. A car may offer a
 * different LPCM rate or channel count, so convert each 20 ms frame before
 * sealing it. Positional linear interpolation keeps packet duration stable.
 */
static int adapt_capture_pcm(const struct carplay_audio_stream *stream,
                             const uint8_t *pcm, size_t pcm_len,
                             uint8_t *out, size_t capacity,
                             size_t *out_len, size_t *frames_out)
{
    size_t source_frames;
    size_t target_frames;
    size_t target_channels;
    size_t frame_bytes;
    size_t i;

    if (!stream || !pcm || !out || !out_len || !frames_out ||
        stream->format.sample_rate == 0 ||
        (stream->format.channels != 1 && stream->format.channels != 2) ||
        pcm_len % (2u * AUDIO_CAPTURE_CHANNELS) != 0)
        return -1;
    source_frames = pcm_len / (2u * AUDIO_CAPTURE_CHANNELS);
    target_channels = stream->format.channels;
    target_frames = (source_frames * stream->format.sample_rate +
                     AUDIO_CAPTURE_RATE / 2u) / AUDIO_CAPTURE_RATE;
    frame_bytes = 2u * target_channels;
    if (!target_frames || target_frames > capacity / frame_bytes)
        return -1;
    for (i = 0; i < target_frames; i++) {
        uint64_t scaled = (uint64_t)i * AUDIO_CAPTURE_RATE;
        size_t first = (size_t)(scaled / stream->format.sample_rate);
        uint32_t fraction = (uint32_t)(scaled % stream->format.sample_rate);
        size_t second = first + 1u < source_frames ? first + 1u : first;
        int left_a = read_s16_le(pcm + first * 4u);
        int right_a = read_s16_le(pcm + first * 4u + 2u);
        int left_b = read_s16_le(pcm + second * 4u);
        int right_b = read_s16_le(pcm + second * 4u + 2u);
        int left = left_a + (int)(((int64_t)(left_b - left_a) * fraction) /
                                  stream->format.sample_rate);
        int right = right_a + (int)(((int64_t)(right_b - right_a) * fraction) /
                                    stream->format.sample_rate);
        if (target_channels == 1)
            put_s16_le(out + i * frame_bytes, (left + right) / 2);
        else {
            put_s16_le(out + i * frame_bytes, left);
            put_s16_le(out + i * frame_bytes + 2u, right);
        }
    }
    *out_len = target_frames * frame_bytes;
    *frames_out = target_frames;
    return 0;
}

int carplay_audio_lpcm_format(uint32_t mask,
                              struct carplay_audio_pcm_format *format)
{
    static const struct {
        uint32_t mask;
        unsigned sample_rate;
        unsigned channels;
    } supported[] = {
        { UINT32_C(0x00008000), 48000, 2 },
        { UINT32_C(0x00000800), 44100, 2 },
        { UINT32_C(0x00004000), 48000, 1 },
        { UINT32_C(0x00000400), 44100, 1 },
        { UINT32_C(0x00000200), 32000, 2 },
        { UINT32_C(0x00000100), 32000, 1 },
        { UINT32_C(0x00000080), 24000, 2 },
        { UINT32_C(0x00000040), 24000, 1 },
        { UINT32_C(0x00000020), 16000, 2 },
        { UINT32_C(0x00000010), 16000, 1 },
        { UINT32_C(0x00000008), 8000, 2 },
        { UINT32_C(0x00000004), 8000, 1 }
    };
    size_t i;
    if (!format)
        return -1;
    memset(format, 0, sizeof(*format));
    for (i = 0; i < sizeof(supported) / sizeof(supported[0]); i++) {
        if (!(mask & supported[i].mask))
            continue;
        format->codec = CARPLAY_AUDIO_CODEC_LPCM_S16BE;
        format->sample_rate = supported[i].sample_rate;
        format->channels = supported[i].channels;
        format->bits_per_sample = 16;
        return 0;
    }
    return -1;
}
int carplay_audio_decrypt_rtp(
    const uint8_t key[AIRPLAY_CHACHA_KEY_BYTES],
    const uint8_t *packet, size_t packet_len,
    uint8_t *payload, size_t payload_capacity, size_t *payload_len)
{
    uint8_t nonce[AIRPLAY_CHACHA_NONCE_BYTES] = { 0 };
    const uint8_t *tail;
    size_t sealed_len;
    size_t written = 0;

    if (!key || !packet || !payload || !payload_len ||
        packet_len < AUDIO_RTP_HEADER_BYTES + AUDIO_RTP_TAIL_BYTES)
        return -1;
    sealed_len = packet_len - AUDIO_RTP_HEADER_BYTES - 8u;
    if (sealed_len < AIRPLAY_CHACHA_TAG_BYTES ||
        sealed_len - AIRPLAY_CHACHA_TAG_BYTES > payload_capacity)
        return -1;
    tail = packet + packet_len - 8u;
    memcpy(nonce + 4, tail, 8u);
    if (airplay_chacha20_poly1305_open(
            key, nonce, packet + AUDIO_RTP_HEADER_BYTES, sealed_len,
            packet + 4u, 8u, payload, payload_capacity, &written) < 0)
        return -1;
    *payload_len = written;
    return 0;
}

int carplay_audio_seal_rtp_pcm(
    const uint8_t key[AIRPLAY_CHACHA_KEY_BYTES], uint8_t payload_type,
    struct carplay_audio_rtp_counters *counters,
    const void *pcm_little_endian, size_t pcm_len,
    uint8_t *packet, size_t packet_capacity, size_t *packet_len)
{
    uint8_t nonce[AIRPLAY_CHACHA_NONCE_BYTES] = { 0 };
    uint8_t *wire_pcm;
    size_t sealed_len = 0;
    size_t index;

    if (!key || !counters || (!pcm_little_endian && pcm_len) || !packet ||
        !packet_len || pcm_len > AUDIO_BUFFER_BYTES ||
        pcm_len + AUDIO_RTP_HEADER_BYTES + AUDIO_RTP_TAIL_BYTES >
            packet_capacity)
        return -1;
    wire_pcm = malloc(pcm_len ? pcm_len : 1u);
    if (!wire_pcm)
        return -1;
    if (pcm_len)
        memcpy(wire_pcm, pcm_little_endian, pcm_len);
    for (index = 0; index + 1 < pcm_len; index += 2) {
        uint8_t first = wire_pcm[index];
        wire_pcm[index] = wire_pcm[index + 1];
        wire_pcm[index + 1] = first;
    }
    packet[0] = 0x80;
    packet[1] = (uint8_t)(payload_type & 0x7fu);
    put_u16_be(packet + 2, counters->sequence);
    put_u32_be(packet + 4, counters->timestamp);
    put_u32_be(packet + 8, counters->ssrc);
    put_u64_le(nonce + 4, counters->nonce);
    if (airplay_chacha20_poly1305_seal(
            key, nonce, wire_pcm, pcm_len, packet + 4u, 8u,
            packet + AUDIO_RTP_HEADER_BYTES,
            packet_capacity - AUDIO_RTP_HEADER_BYTES - 8u,
            &sealed_len) < 0) {
        free(wire_pcm);
        return -1;
    }
    put_u64_le(packet + AUDIO_RTP_HEADER_BYTES + sealed_len, counters->nonce);
    *packet_len = AUDIO_RTP_HEADER_BYTES + sealed_len + 8u;
    counters->sequence++;
    counters->nonce++;
    free(wire_pcm);
    return 0;
}

static int stream_open_pcm(struct carplay_audio_stream *s, const uint8_t *p,
                           size_t n, uint8_t *out, size_t cap, size_t *written)
{
    if (!s->bridge->legacy)
        return carplay_audio_decrypt_rtp(s->input_key, p, n, out, cap, written);
    /* This negotiated stream uses fixed RTP v2 headers, without extensions. */
    if (n < 12 || p[0] != 0x80 || (p[1] & 0x7f) != 100 ||
        n - 12 > cap || (n - 12) % (s->format.channels * 2u))
        return -1;
    memcpy(out, p + 12, n - 12); *written = n - 12;
    return 0;
}

static int stream_seal_pcm(struct carplay_audio_stream *s, const uint8_t *pcm,
                           size_t n, uint8_t *out, size_t cap, size_t *written)
{
    struct carplay_audio_rtp_counters *c = &s->capture_counters;
    if (!s->bridge->legacy)
        return carplay_audio_seal_rtp_pcm(s->output_key, (uint8_t)s->type,
                                          c, pcm, n, out, cap, written);
    if (n + 12 > cap || n % (s->format.channels * 2u)) return -1;
    out[0] = 0x80; out[1] = (uint8_t)s->type;
    put_u16_be(out + 2, c->sequence++); put_u32_be(out + 4, c->timestamp);
    put_u32_be(out + 8, c->ssrc);
    for (size_t i = 0; i < n; i += 2) { out[12+i] = pcm[i+1]; out[13+i] = pcm[i]; }
    *written = n + 12; return 0;
}

#ifdef __ANDROID__
static SLuint32 sl_channels(unsigned channels)
{
    return channels == 2 ? (SL_SPEAKER_FRONT_LEFT | SL_SPEAKER_FRONT_RIGHT) :
                           SL_SPEAKER_FRONT_CENTER;
}
#endif

#ifdef __ANDROID__
static void player_queue_callback(SLAndroidSimpleBufferQueueItf queue,
                                  void *context);
#endif

static int output_start(struct carplay_audio_bridge *bridge,
                        struct carplay_audio_stream *stream)
{
#ifdef __ANDROID__
    SLDataLocator_AndroidSimpleBufferQueue locator = {
        SL_DATALOCATOR_ANDROIDSIMPLEBUFFERQUEUE, AUDIO_QUEUE_DEPTH
    };
    SLDataFormat_PCM pcm = {
        SL_DATAFORMAT_PCM, (SLuint32)stream->format.channels,
        stream->format.sample_rate * 1000u, SL_PCMSAMPLEFORMAT_FIXED_16,
        SL_PCMSAMPLEFORMAT_FIXED_16,
        sl_channels(stream->format.channels), SL_BYTEORDER_LITTLEENDIAN
    };
    SLDataSource source = { &locator, &pcm };
    SLDataLocator_OutputMix output_locator = {
        SL_DATALOCATOR_OUTPUTMIX, bridge->output_mix
    };
    SLDataSink sink = { &output_locator, NULL };
    SLInterfaceID ids[2] = { SL_IID_ANDROIDSIMPLEBUFFERQUEUE,
                            SL_IID_ANDROIDCONFIGURATION };
    SLboolean required[2] = { SL_BOOLEAN_TRUE, SL_BOOLEAN_FALSE };
    SLAndroidConfigurationItf configuration = NULL;
    SLint32 stream_type = SL_ANDROID_STREAM_MEDIA;

    if (!bridge->engine || !bridge->output_mix)
        return -1;
    if ((*bridge->engine)->CreateAudioPlayer(
            bridge->engine, &stream->player_object, &source, &sink, 2, ids,
            required) != SL_RESULT_SUCCESS) {
        iap2_log("audio: OpenSL could not create PCM player type=%d rate=%u channels=%u",
                 stream->type, stream->format.sample_rate,
                 stream->format.channels);
        return -1;
    }
    configuration = NULL;
    if ((*stream->player_object)->GetInterface(
            stream->player_object, SL_IID_ANDROIDCONFIGURATION,
            &configuration) != SL_RESULT_SUCCESS) {
        configuration = NULL;
        iap2_log("audio: OpenSL AndroidConfiguration unavailable; using default media output type=%d",
                 stream->type);
    }
    if (configuration) {
        SLresult configured = (*configuration)->SetConfiguration(
            configuration, SL_ANDROID_KEY_STREAM_TYPE, &stream_type,
            sizeof(stream_type));
        if (configured == SL_RESULT_SUCCESS)
            iap2_log("audio: OpenSL stream type configured as media type=%d",
                     stream->type);
        else
            iap2_log("audio: OpenSL media stream type rejected result=%d; using player default type=%d",
                     (int)configured, stream->type);
    }
    if ((*stream->player_object)->Realize(stream->player_object,

                                          SL_BOOLEAN_FALSE) !=
            SL_RESULT_SUCCESS ||
        (*stream->player_object)->GetInterface(
            stream->player_object, SL_IID_PLAY, &stream->player) !=
            SL_RESULT_SUCCESS ||
        (*stream->player_object)->GetInterface(
            stream->player_object, SL_IID_ANDROIDSIMPLEBUFFERQUEUE,
            &stream->player_queue) != SL_RESULT_SUCCESS ||
        (*stream->player_queue)->RegisterCallback(
            stream->player_queue, player_queue_callback, stream) !=
            SL_RESULT_SUCCESS ||
        (*stream->player)->SetPlayState(stream->player,
                                        SL_PLAYSTATE_PLAYING) !=
            SL_RESULT_SUCCESS)
        return -1;
    iap2_log("audio: media output ready type=%d audioType=%s rate=%u channels=%u",
             stream->type, stream->audio_type, stream->format.sample_rate,
             stream->format.channels);
    return 0;
#else
    (void)bridge;
    (void)stream;
    return -1;
#endif
}

static void output_stop(struct carplay_audio_stream *stream)
{
#ifdef __ANDROID__
    if (stream->player)
        (void)(*stream->player)->SetPlayState(stream->player,
                                              SL_PLAYSTATE_STOPPED);
    stream->player = NULL;
    stream->player_queue = NULL;
    if (stream->player_object) {
        (*stream->player_object)->Destroy(stream->player_object);
        stream->player_object = NULL;
    }
    pthread_mutex_lock(&stream->player_lock);
    memset(stream->player_buffers, 0, sizeof(stream->player_buffers));
    stream->player_head = 0;
    stream->player_tail = 0;
    pthread_mutex_unlock(&stream->player_lock);
#else
    (void)stream;
#endif
}

static int output_feed_unlocked(struct carplay_audio_stream *stream, uint8_t *payload,
                       size_t payload_len)
{
    size_t i;
    if (!payload_len)
        return 0;
    for (i = 0; i + 1 < payload_len; i += 2) {
        uint8_t first = payload[i];
        payload[i] = payload[i + 1];
        payload[i + 1] = first;
    }
#ifdef __ANDROID__
    {
        unsigned index = AUDIO_QUEUE_DEPTH;
        unsigned slot;
        if (!stream->player_queue)
            return -1;
        pthread_mutex_lock(&stream->player_lock);
        for (slot = 0; slot < AUDIO_QUEUE_DEPTH; slot++) {
            if (!stream->player_buffers[slot].busy) {
                index = slot;
                break;
            }
        }
        if (index == AUDIO_QUEUE_DEPTH || payload_len > AUDIO_BUFFER_BYTES) {
            (void)stream_stat_increment(&stream->packets_dropped);
            pthread_mutex_unlock(&stream->player_lock);
            return -1;
        }
        stream->player_buffers[index].busy = 1;
        memcpy(stream->player_buffers[index].bytes, payload, payload_len);
        stream->player_order[stream->player_tail++ % AUDIO_QUEUE_DEPTH] = index;
        pthread_mutex_unlock(&stream->player_lock);
        if ((*stream->player_queue)->Enqueue(
                stream->player_queue, stream->player_buffers[index].bytes,
                (SLuint32)payload_len) != SL_RESULT_SUCCESS) {
            pthread_mutex_lock(&stream->player_lock);
            stream->player_buffers[index].busy = 0;
            pthread_mutex_unlock(&stream->player_lock);
            return -1;
        }
        return 0;
    }
#else
    (void)stream;
    return -1;
#endif
}

static int output_feed(struct carplay_audio_stream *stream, uint8_t *payload,
                       size_t payload_len)
{
    struct carplay_audio_bridge *bridge = stream->bridge;
    int result;
    pthread_mutex_lock(&bridge->media_lock);
    result = atomic_load_explicit(&bridge->paused, memory_order_acquire) ? 1 :
        output_feed_unlocked(stream, payload, payload_len);
    pthread_mutex_unlock(&bridge->media_lock);
    return result;
}

void carplay_audio_bridge_set_paused(struct carplay_audio_bridge *bridge,
                                    int paused)
{
    if (!bridge)
        return;
    paused = !!paused;
    /* The lock makes return from this call a boundary: no earlier capture,
     * microphone send or playback enqueue can complete after the pause. */
    pthread_mutex_lock(&bridge->media_lock);
    if (atomic_load_explicit(&bridge->paused, memory_order_acquire) == paused) {
        pthread_mutex_unlock(&bridge->media_lock);
        return;
    }
    atomic_store_explicit(&bridge->paused, paused, memory_order_release);
    if (paused && bridge->legacy) bridge->streams[1].packet_used = 0;
    for (size_t i = 0; i < bridge->stream_count; i++) {
        struct carplay_audio_stream *stream = &bridge->streams[i];
        if (paused) {
            pthread_mutex_lock(&stream->microphone_lock);
            stream->microphone_head = 0;
            stream->microphone_tail = 0;
            stream->microphone_count = 0;
            stream->packet_used = 0;
            pthread_mutex_unlock(&stream->microphone_lock);
        }
#ifdef __ANDROID__
        if (stream->player)
            (void)(*stream->player)->SetPlayState(stream->player,
                paused ? SL_PLAYSTATE_PAUSED : SL_PLAYSTATE_PLAYING);
        if (paused && stream->player_queue) {
            (void)(*stream->player_queue)->Clear(stream->player_queue);
            pthread_mutex_lock(&stream->player_lock);
            memset(stream->player_buffers, 0, sizeof(stream->player_buffers));
            stream->player_head = 0;
            stream->player_tail = 0;
            pthread_mutex_unlock(&stream->player_lock);
        }
#endif
    }
    pthread_mutex_unlock(&bridge->media_lock);
}

#ifdef __ANDROID__
static void player_queue_callback(SLAndroidSimpleBufferQueueItf queue,
                                  void *context)
{
    struct carplay_audio_stream *stream = context;
    unsigned index;
    (void)queue;
    if (!stream)
        return;
    pthread_mutex_lock(&stream->player_lock);
    if (stream->player_head != stream->player_tail) {
        index = stream->player_order[stream->player_head++ % AUDIO_QUEUE_DEPTH];
        if (index < AUDIO_QUEUE_DEPTH)
            stream->player_buffers[index].busy = 0;
    }
    pthread_mutex_unlock(&stream->player_lock);
}
#endif
static void *audio_receive_main(void *opaque)
{
    struct carplay_audio_stream *stream = opaque;
    uint8_t wire[AUDIO_BUFFER_BYTES + AUDIO_RTP_HEADER_BYTES +
                 AUDIO_RTP_TAIL_BYTES];
    uint8_t payload[AUDIO_BUFFER_BYTES];

    while (!stream_stop_requested(stream)) {
        struct pollfd pfd = { .fd = stream->data_fd, .events = POLLIN };
        ssize_t n = poll(&pfd, 1, 100);
        if (n < 0 && errno == EINTR)
            continue;
        if (n <= 0 || !(pfd.revents & POLLIN))
            continue;
        n = recv(stream->data_fd, wire, sizeof(wire), 0);
        if (n < 0) {
            if (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK)
                continue;
            break;
        }
        (void)stream_stat_increment(&stream->packets_received);
        {
            size_t payload_len = 0;
            if (carplay_audio_decrypt_rtp(stream->output_key, wire, (size_t)n,
                                          payload, sizeof(payload),
                                          &payload_len) < 0) {
                if (stream_stat_increment(&stream->packets_dropped) == 1)
                    iap2_log("audio: first RTP authentication failure type=%d audioType=%s",
                             stream->type, stream->audio_type);
                continue;
            }
            if (output_feed(stream, payload, payload_len) != 0)
                continue;
        }
        if (stream_stat_increment(&stream->packets_decrypted) == 1)
            iap2_log("audio: first decrypted media packet type=%d audioType=%s",
                     stream->type, stream->audio_type);
        if (stream_stat_get(&stream->packets_decrypted) % 500u == 0)
            iap2_log("audio: type=%d audioType=%s decrypted=%lu dropped=%lu",
                     stream->type, stream->audio_type,
                     stream_stat_get(&stream->packets_decrypted),
                     stream_stat_get(&stream->packets_dropped));
    }
    return NULL;
}

static void *audio_control_main(void *opaque)
{
    struct carplay_audio_stream *stream = opaque;
    uint8_t buffer[2048];
    while (!stream_stop_requested(stream)) {
        struct pollfd pfd = { .fd = stream->control_fd, .events = POLLIN };
        int ready = poll(&pfd, 1, 100);
        if (ready > 0 && (pfd.revents & POLLIN))
            (void)recv(stream->control_fd, buffer, sizeof(buffer), 0);
        else if (ready < 0 && errno != EINTR)
            break;
    }
    return NULL;
}

static void microphone_queue_push(struct carplay_audio_stream *stream,
                                  const uint8_t *data, size_t len)
{
    if (!stream->microphone || !len || len > AUDIO_BUFFER_BYTES)
        return;
    pthread_mutex_lock(&stream->bridge->media_lock);
    if (atomic_load_explicit(&stream->bridge->paused, memory_order_acquire)) {
        pthread_mutex_unlock(&stream->bridge->media_lock);
        return;
    }
    pthread_mutex_lock(&stream->microphone_lock);
    if (stream->microphone_count == AUDIO_QUEUE_DEPTH) {
        stream->microphone_head = (stream->microphone_head + 1u) %
                                  AUDIO_QUEUE_DEPTH;
        stream->microphone_count--;
        (void)stream_stat_increment(&stream->packets_dropped);
    }
    {
        unsigned tail = stream->microphone_tail++ % AUDIO_QUEUE_DEPTH;
        memcpy(stream->microphone_queue[tail].bytes, data, len);
        stream->microphone_queue[tail].len = len;
        stream->microphone_count++;
        pthread_cond_signal(&stream->microphone_ready);
    }
    pthread_mutex_unlock(&stream->microphone_lock);
    pthread_mutex_unlock(&stream->bridge->media_lock);
}

static void *microphone_receive_main(void *opaque)
{
    struct carplay_audio_stream *stream = opaque;
    uint8_t wire[AUDIO_BUFFER_BYTES + AUDIO_RTP_HEADER_BYTES +
                 AUDIO_RTP_TAIL_BYTES];
    uint8_t pcm[AUDIO_BUFFER_BYTES];

    while (!stream_stop_requested(stream)) {
        struct pollfd pfd = { .fd = stream->microphone_fd, .events = POLLIN };
        int ready = poll(&pfd, 1, 100);
        ssize_t n;
        size_t pcm_len = 0;
        size_t i;

        if (ready < 0 && errno == EINTR)
            continue;
        if (ready <= 0 || !(pfd.revents & POLLIN))
            continue;
        n = recv(stream->microphone_fd, wire, sizeof(wire), MSG_DONTWAIT);
        if (n < 0) {
            if (errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR)
                break;
            continue;
        }
        if (stream_open_pcm(stream, wire, (size_t)n,
                             pcm, sizeof(pcm), &pcm_len) < 0) {
            (void)stream_stat_increment(&stream->packets_dropped);
            continue;
        }
        /* Car microphone RTP carries PCM16 big-endian. Android consumes LE. */
        if (pcm_len >= 2) {
            pcm_len &= ~(size_t)1;
            for (i = 0; i + 1 < pcm_len; i += 2) {
                uint8_t t = pcm[i];
                pcm[i] = pcm[i + 1];
                pcm[i + 1] = t;
            }
        }
        microphone_queue_push(stream, pcm, pcm_len);
        if (stream_stat_increment(&stream->packets_decrypted) == 1)
            iap2_log("audio: first car microphone packet received type=%d audioType=%s port=%u",
                     stream->type, stream->audio_type,
                     stream->microphone_port);
    }
    return NULL;
}

/*
 * Waits for PCM copied from the car-side microphone socket, seals it with the
 * stream input key, and sends it to the iPhone on the negotiated audio socket.
 * Keeping this separate from the receive thread lets each direction block
 * independently and makes teardown deterministic.
 */
static void *microphone_uplink_main(void *opaque)
{
    struct carplay_audio_stream *stream = opaque;
    uint8_t pcm[AUDIO_BUFFER_BYTES];
    uint8_t wire[AUDIO_BUFFER_BYTES + AUDIO_RTP_HEADER_BYTES +
                 AUDIO_RTP_TAIL_BYTES];

    while (!stream_stop_requested(stream)) {
        size_t pcm_len = 0;
        size_t wire_len = 0;
        ssize_t sent;
        unsigned head;

        pthread_mutex_lock(&stream->microphone_lock);
        while (!stream_stop_requested(stream) && !stream->microphone_count)
            pthread_cond_wait(&stream->microphone_ready,
                              &stream->microphone_lock);
        if (stream_stop_requested(stream)) {
            pthread_mutex_unlock(&stream->microphone_lock);
            break;
        }
        /* Never hold microphone_lock while waiting for media_lock: pause
         * takes those locks in the opposite direction to flush this queue. */
        pthread_mutex_unlock(&stream->microphone_lock);
        pthread_mutex_lock(&stream->bridge->media_lock);
        pthread_mutex_lock(&stream->microphone_lock);
        if (!stream->microphone_count ||
            atomic_load_explicit(&stream->bridge->paused, memory_order_acquire)) {
            pthread_mutex_unlock(&stream->microphone_lock);
            pthread_mutex_unlock(&stream->bridge->media_lock);
            continue;
        }
        head = stream->microphone_head++ % AUDIO_QUEUE_DEPTH;
        pcm_len = stream->microphone_queue[head].len;
        memcpy(pcm, stream->microphone_queue[head].bytes, pcm_len);
        stream->microphone_count--;
        pthread_mutex_unlock(&stream->microphone_lock);

        if (pcm_len < 2 ||
            carplay_audio_seal_rtp_pcm(
                stream->input_key, (uint8_t)stream->type,
                &stream->microphone_counters, pcm, pcm_len,
                wire, sizeof(wire), &wire_len) < 0) {
            (void)stream_stat_increment(&stream->packets_dropped);
            pthread_mutex_unlock(&stream->bridge->media_lock);
            continue;
        }
        sent = send(stream->data_fd, wire, wire_len,
                    MSG_DONTWAIT | MSG_NOSIGNAL);
        if (sent != (ssize_t)wire_len) {
            (void)stream_stat_increment(&stream->packets_dropped);
            pthread_mutex_unlock(&stream->bridge->media_lock);
            continue;
        }
        if (stream->format.channels) {
            stream->microphone_counters.timestamp +=
                (uint32_t)(pcm_len / 2u / stream->format.channels);
        }
        if (stream_stat_increment(&stream->microphone_packets_sent) == 1)
            iap2_log("audio: first car microphone packet forwarded type=%d audioType=%s",
                     stream->type, stream->audio_type);
        pthread_mutex_unlock(&stream->bridge->media_lock);
    }
    return NULL;
}

static int microphone_start(struct carplay_audio_bridge *bridge,
                            struct carplay_audio_stream *stream)
{
    (void)bridge;
    if (!stream->microphone || stream->microphone_fd < 0)
        return -1;
    iap2_log("audio: car microphone receive ready type=%d rate=%u channels=%u port=%u",
             stream->type, stream->format.sample_rate,
             stream->format.channels, stream->microphone_port);
    return 0;
}

static void microphone_stop(struct carplay_audio_stream *stream)
{
    if (!stream)
        return;
    if (stream->microphone_fd >= 0)
        close(stream->microphone_fd);
    stream->microphone_fd = -1;
    stream->microphone_port = 0;
}

static int bind_microphone_socket(struct carplay_audio_bridge *bridge,
                                  struct carplay_audio_stream *stream)
{
    struct sockaddr_in6 address;
    socklen_t address_len = sizeof(address);
    int fd;

    (void)bridge;
    if (!stream->microphone || stream->microphone_fd >= 0)
        return stream->microphone ? 0 : -1;
    fd = socket(AF_INET6, SOCK_DGRAM | SOCK_CLOEXEC, 0);
    if (fd < 0) {
        bridge_error(bridge, "cannot create car microphone socket");
        return -1;
    }
    {
        int enabled = 1;
        (void)setsockopt(fd, IPPROTO_IPV6, IPV6_V6ONLY, &enabled, sizeof(enabled));
        enabled = 0;
        (void)setsockopt(fd, IPPROTO_IPV6, IPV6_V6ONLY, &enabled, sizeof(enabled));
    }
    memset(&address, 0, sizeof(address));
    address.sin6_family = AF_INET6;
    address.sin6_addr = in6addr_any;
    address.sin6_port = 0;
    if (bind(fd, (const struct sockaddr *)&address, sizeof(address)) < 0 ||
        getsockname(fd, (struct sockaddr *)&address, &address_len) < 0 ||
        ntohs(address.sin6_port) == 0) {
        close(fd);
        bridge_error(bridge, "cannot bind car microphone UDP port");
        return -1;
    }
    stream->microphone_fd = fd;
    stream->microphone_port = ntohs(address.sin6_port);
    return 0;
}

int carplay_audio_bridge_init(struct carplay_audio_bridge **out,
                              const uint8_t shared_secret[32],
                              int enable_media, int enable_microphone)
{
    struct carplay_audio_bridge *bridge;
    size_t i;
    if (!out || !shared_secret)
        return -1;
    *out = NULL;
    bridge = calloc(1, sizeof(*bridge));
    if (!bridge)
        return -1;
    pthread_mutex_init(&bridge->lock, NULL);
    pthread_mutex_init(&bridge->media_lock, NULL);
    memcpy(bridge->shared_secret, shared_secret, sizeof(bridge->shared_secret));
    bridge->enable_media = !!enable_media;
    bridge->enable_microphone = !!enable_microphone;
    for (i = 0; i < CARPLAY_AUDIO_MAX_STREAMS; i++) {
        struct carplay_audio_stream *stream = &bridge->streams[i];
        stream->bridge = bridge;
        stream->data_fd = -1;
        stream->control_fd = -1;
        stream->microphone_fd = -1;
        pthread_mutex_init(&stream->microphone_lock, NULL);
        pthread_cond_init(&stream->microphone_ready, NULL);
#ifdef __ANDROID__
        pthread_mutex_init(&stream->player_lock, NULL);
#endif
    }
    bridge->capture_listener_fd = -1;
    bridge->capture_split_listener_fd = -1;
    bridge->mic_listener = -1;
#ifdef __ANDROID__
    {
        SLresult result = slCreateEngine(&bridge->engine_object, 0, NULL, 0,
                                         NULL, NULL);
        if (result == SL_RESULT_SUCCESS)
            result = (*bridge->engine_object)->Realize(
                bridge->engine_object, SL_BOOLEAN_FALSE);
        if (result == SL_RESULT_SUCCESS)
            result = (*bridge->engine_object)->GetInterface(
                bridge->engine_object, SL_IID_ENGINE, &bridge->engine);
        if (result == SL_RESULT_SUCCESS)
            result = (*bridge->engine)->CreateOutputMix(
                bridge->engine, &bridge->output_mix, 0, NULL, NULL);
        if (result == SL_RESULT_SUCCESS)
            result = (*bridge->output_mix)->Realize(
                bridge->output_mix, SL_BOOLEAN_FALSE);
        if (result != SL_RESULT_SUCCESS) {
            iap2_log("audio: OpenSL ES initialization failed result=%d", result);
            if (bridge->output_mix)
                (*bridge->output_mix)->Destroy(bridge->output_mix);
            bridge->output_mix = NULL;
            if (bridge->engine_object)
                (*bridge->engine_object)->Destroy(bridge->engine_object);
            bridge->engine_object = NULL;
            bridge->engine = NULL;
        }
    }
#endif
    *out = bridge;
    return 0;
}

void carplay_audio_bridge_stop(struct carplay_audio_bridge *bridge)
{
    size_t i;
    if (!bridge)
        return;
    if (bridge->legacy) {
        legacy_ipc_stop(bridge);
        capture_stop(bridge);
        carplay_audio_bridge_legacy_stop_stream(bridge);
        legacy_stop_nav(bridge);
        bridge->stream_count = 0;
        bridge->prepared = 0;
        return;
    }
    capture_stop(bridge);
    for (i = 0; i < bridge->stream_count; i++) {
        struct carplay_audio_stream *stream = &bridge->streams[i];
        stream_request_stop(stream);
        pthread_mutex_lock(&stream->microphone_lock);
        pthread_cond_broadcast(&stream->microphone_ready);
        pthread_mutex_unlock(&stream->microphone_lock);
        if (stream->active && stream->receive_started)
            pthread_join(stream->receive_thread, NULL);
        if (stream->active && stream->control_started)
            pthread_join(stream->control_thread, NULL);
        if (stream->active && stream->microphone_receive_started)
            pthread_join(stream->microphone_receive_thread, NULL);
        if (stream->active && stream->microphone_uplink_started)
            pthread_join(stream->microphone_uplink_thread, NULL);
        microphone_stop(stream);
        output_stop(stream);
        if (stream->data_fd >= 0)
            close(stream->data_fd);
        if (stream->control_fd >= 0)
            close(stream->control_fd);
        stream->data_fd = -1;
        stream->control_fd = -1;
        stream->receive_started = 0;
        stream->control_started = 0;
        stream->microphone_receive_started = 0;
        stream->microphone_uplink_started = 0;
        stream->active = 0;
        atomic_store_explicit(&stream->stop, 0, memory_order_release);
        atomic_store_explicit(&stream->packets_received, 0ul,
                              memory_order_relaxed);
        atomic_store_explicit(&stream->packets_decrypted, 0ul,
                              memory_order_relaxed);
        atomic_store_explicit(&stream->packets_dropped, 0ul,
                              memory_order_relaxed);
        atomic_store_explicit(&stream->capture_packets_sent, 0ul,
                              memory_order_relaxed);
        atomic_store_explicit(&stream->capture_bytes_sent, 0ul,
                              memory_order_relaxed);
        atomic_store_explicit(&stream->microphone_packets_sent, 0ul,
                              memory_order_relaxed);
        memset(&stream->capture_counters, 0,
               sizeof(stream->capture_counters));
        memset(&stream->microphone_counters, 0,
               sizeof(stream->microphone_counters));
    }
    bridge->prepared = 0;
    bridge->stream_count = 0;
}

void carplay_audio_bridge_destroy(struct carplay_audio_bridge **out)
{
    struct carplay_audio_bridge *bridge;
    size_t i;
    if (!out || !*out)
        return;
    bridge = *out;
    carplay_audio_bridge_stop(bridge);
#ifdef __ANDROID__
    if (bridge->output_mix)
        (*bridge->output_mix)->Destroy(bridge->output_mix);
    if (bridge->engine_object)
        (*bridge->engine_object)->Destroy(bridge->engine_object);
#endif
    for (i = 0; i < CARPLAY_AUDIO_MAX_STREAMS; i++) {
        pthread_mutex_destroy(&bridge->streams[i].microphone_lock);
        pthread_cond_destroy(&bridge->streams[i].microphone_ready);
#ifdef __ANDROID__
        pthread_mutex_destroy(&bridge->streams[i].player_lock);
#endif
    }
    pthread_mutex_destroy(&bridge->lock);
    pthread_mutex_destroy(&bridge->media_lock);
    free(bridge);
    *out = NULL;
}

int carplay_audio_bridge_prepare(
    struct carplay_audio_bridge *bridge,
    const struct carplay_session_capabilities *capabilities)
{
    size_t i;
    size_t input_index = SIZE_MAX;

    if (!bridge || !capabilities)
        return -1;
    if (bridge->legacy)
        return legacy_prepare(bridge, capabilities);
    carplay_audio_bridge_stop(bridge);
    if (!bridge->enable_media && !bridge->enable_microphone)
        return 0;
    if (bridge->enable_microphone) {
        int best_priority = 0;
        for (i = 0; i < capabilities->audio.format_count; i++) {
            const struct carplay_audio_format_capabilities *entry =
                &capabilities->audio.formats[i];
            int priority = 0;
            if (!entry->present || entry->type != 100 ||
                !carplay_capabilities_select_audio_input(
                    capabilities, entry->type, entry->audio_type))
                continue;
            if (strcasecmp(entry->audio_type, "media") == 0)
                priority = 3;
            else if (strcasecmp(entry->audio_type, "compatibility") == 0)
                priority = 2;
            else
                priority = 1;
            if (priority > best_priority) {
                best_priority = priority;
                input_index = i;
            }
        }
    }
    for (i = 0; i < capabilities->audio.format_count &&
                bridge->stream_count < CARPLAY_AUDIO_MAX_STREAMS; i++) {
        const struct carplay_audio_format_capabilities *candidate =
            &capabilities->audio.formats[i];
        struct carplay_audio_stream *stream;
        uint32_t output_mask = candidate->output_formats;
        uint32_t input_mask = i == input_index ? candidate->input_formats : 0;
        uint32_t output_format = 0;
        uint32_t input_format = 0;
        struct carplay_audio_pcm_format format;
        char salt[96];
        int salt_len;

        if (!candidate->present || !candidate->audio_type[0] ||
            (candidate->type != 100 && candidate->type != 101 &&
             candidate->type != 102))
            continue;
        if (!bridge->enable_media)
            output_mask = 0;
        if (!bridge->enable_microphone || candidate->type != 100)
            input_mask = 0;
        if (!output_mask && !input_mask)
            continue;
        if (output_mask)
            output_format = carplay_capabilities_select_audio_output(
                capabilities, candidate->type, candidate->audio_type);
        if (input_mask)
            input_format = carplay_capabilities_select_audio_input(
                capabilities, candidate->type, candidate->audio_type);
        if (output_format && carplay_audio_lpcm_format(output_format,
                                                       &format) < 0) {
            iap2_log("audio: declining unsupported compressed output type=%d audioType=%s format=0x%x",
                     candidate->type, candidate->audio_type,
                     output_format);
            output_format = 0;
        }
        if (input_format) {
            struct carplay_audio_pcm_format input_parsed;
            if (carplay_audio_lpcm_format(input_format, &input_parsed) < 0) {
                iap2_log("audio: declining unsupported compressed microphone type=%d audioType=%s format=0x%x",
                         candidate->type, candidate->audio_type,
                         input_format);
                input_format = 0;
            } else if (!output_format) {
                format = input_parsed;
            }
        }
        if (!output_format && !input_format)
            continue;
        stream = &bridge->streams[bridge->stream_count];
        stream->configured = 1;
        stream->type = candidate->type;
        snprintf(stream->audio_type, sizeof(stream->audio_type), "%s",
                 candidate->audio_type);
        stream->format_mask = output_format;
        stream->format = format;
        stream->microphone = input_format != 0;
        if (output_format)
            stream->capture_counters.ssrc = UINT32_C(0x43504c59);
        if (stream->microphone) {
            stream->microphone_counters.ssrc = UINT32_C(0x43504c59);
        }
        stream->connection_id = UINT64_C(0x0a0b0c0d00000000) |
            ((uint64_t)candidate->type << 16) |
            (uint64_t)bridge->stream_count;
        salt_len = snprintf(salt, sizeof(salt), "DataStream-Salt%llu",
                            (unsigned long long)stream->connection_id);
        if (salt_len <= 0 || (size_t)salt_len >= sizeof(salt) ||
            airplay_hkdf_sha512(
                bridge->shared_secret, sizeof(bridge->shared_secret),
                (const uint8_t *)salt, (size_t)salt_len,
                (const uint8_t *)"DataStream-Output-Encryption-Key",
                strlen("DataStream-Output-Encryption-Key"),
                stream->output_key, sizeof(stream->output_key)) < 0 ||
            airplay_hkdf_sha512(
                bridge->shared_secret, sizeof(bridge->shared_secret),
                (const uint8_t *)salt, (size_t)salt_len,
                (const uint8_t *)"DataStream-Input-Encryption-Key",
                strlen("DataStream-Input-Encryption-Key"),
                stream->input_key, sizeof(stream->input_key)) < 0) {
            bridge_error(bridge, "could not derive audio stream keys");
            return -1;
        }
        if (stream->microphone && bind_microphone_socket(bridge, stream) < 0)
            continue;
        iap2_log("audio: prepared type=%d audioType=%s format=0x%x microphone=%d localMicPort=%u",
                 stream->type, stream->audio_type, stream->format_mask,
                 stream->microphone, stream->microphone_port);
        bridge->stream_count++;
    }
    bridge->prepared = 1;
    if (bridge->enable_media && capture_start(bridge) < 0)
        iap2_log("audio: local capture endpoint unavailable");
    return 0;
}

size_t carplay_audio_bridge_setup_count(
    const struct carplay_audio_bridge *bridge)
{
    return bridge ? bridge->stream_count : 0;
}

int carplay_audio_bridge_resolve_response(
    const struct carplay_audio_bridge *bridge, int type,
    uint64_t connection_id, char *audio_type, size_t capacity,
    size_t *audio_type_len)
{
    size_t i;
    if (!bridge || !audio_type || !capacity || !audio_type_len ||
        (!connection_id && !bridge->legacy))
        return -1;
    for (i = 0; i < bridge->stream_count; i++) {
        const struct carplay_audio_stream *stream = &bridge->streams[i];
        size_t length;
        if (!stream->configured || stream->type != type ||
            stream->connection_id != connection_id)
            continue;
        length = strlen(stream->audio_type);
        if (length + 1u > capacity)
            return -1;
        memcpy(audio_type, stream->audio_type, length + 1u);
        *audio_type_len = length;
        return 0;
    }
    return -1;
}

int carplay_audio_bridge_fill_setups(
    const struct carplay_audio_bridge *bridge,
    struct airplay_stream_setup *setups, size_t capacity)
{
    size_t i;
    if (!bridge || !setups || capacity < bridge->stream_count)
        return -1;
    for (i = 0; i < bridge->stream_count; i++) {
        const struct carplay_audio_stream *stream = &bridge->streams[i];
        setups[i].type = stream->type;
        setups[i].stream_id = (int)(i + 2u);
        setups[i].connection_id = stream->connection_id;
        setups[i].audio_type = stream->audio_type;
        setups[i].audio_format = stream->format_mask;
        setups[i].data_port = stream->microphone ?
                             stream->microphone_port : 0;
        setups[i].input = stream->microphone;
        setups[i].legacy_audio = bridge->legacy;
        setups[i].control_port = stream->local_control_port;
    }
    return 0;
}
static int connect_udp(int family, const char *host, uint16_t port)
{
    char endpoint[INET6_ADDRSTRLEN + IFNAMSIZ + 4];
    const char *node = host;
    char *zone;
    size_t host_len;
    int fd;

    if (!host || !port)
        return -1;
    memset(endpoint, 0, sizeof(endpoint));
    host_len = strlen(host);
    if (family == AF_INET6 && host_len >= 2 && host[0] == '[' &&
        host[host_len - 1] == ']') {
        if (host_len - 2 >= sizeof(endpoint))
            return -1;
        memcpy(endpoint, host + 1, host_len - 2);
        node = endpoint;
    }

    if (family == AF_INET) {
        struct sockaddr_in target;
        struct sockaddr_in source;
        struct in_addr link_local;

        memset(&target, 0, sizeof(target));
        memset(&source, 0, sizeof(source));
        target.sin_family = AF_INET;
        target.sin_port = htons(port);
        if (inet_pton(AF_INET, node, &target.sin_addr) != 1) {
            iap2_log("audio: invalid IPv4 UDP endpoint %s port=%u", host,
                     port);
            return -1;
        }
        fd = socket(AF_INET, SOCK_DGRAM | SOCK_CLOEXEC, IPPROTO_UDP);
        if (fd < 0)
            return -1;
        if (inet_pton(AF_INET, "169.254.0.0", &link_local) == 1 &&
            (ntohl(target.sin_addr.s_addr) & UINT32_C(0xffff0000)) ==
                ntohl(link_local.s_addr)) {
            source.sin_family = AF_INET;
            if (inet_pton(AF_INET, "169.254.0.1",
                          &source.sin_addr) != 1 ||
                bind(fd, (const struct sockaddr *)&source,
                     sizeof(source)) < 0) {
                iap2_log("audio: bind IPv4 UDP endpoint %s port=%u failed: %s",
                         node, port, strerror(errno));
                close(fd);
                return -1;
            }
        }
        if (connect(fd, (const struct sockaddr *)&target,
                    sizeof(target)) < 0) {
            iap2_log("audio: connect IPv4 UDP endpoint %s port=%u failed: %s",
                     node, port, strerror(errno));
            close(fd);
            return -1;
        }
        iap2_log("audio: IPv4 UDP endpoint connected address=%s port=%u",
                 node, port);
        return fd;
    }

    if (family == AF_INET6) {
        char interface_name[IFNAMSIZ];
        struct sockaddr_in6 target;
        struct sockaddr_in6 source;
        unsigned interface_index = 0;

        memset(interface_name, 0, sizeof(interface_name));
        memset(&target, 0, sizeof(target));
        memset(&source, 0, sizeof(source));
        zone = strchr(endpoint, '%');
        if (zone) {
            *zone++ = '\0';
            snprintf(interface_name, sizeof(interface_name), "%s", zone);
            interface_index = if_nametoindex(interface_name);
            if (!interface_index) {
                iap2_log("audio: unknown IPv6 UDP interface %s endpoint=%s",
                         interface_name, host);
                return -1;
            }
        }
        target.sin6_family = AF_INET6;
        target.sin6_port = htons(port);
        target.sin6_scope_id = interface_index;
        if (inet_pton(AF_INET6, node, &target.sin6_addr) != 1) {
            iap2_log("audio: invalid IPv6 UDP endpoint %s port=%u", host,
                     port);
            return -1;
        }
        fd = socket(AF_INET6, SOCK_DGRAM | SOCK_CLOEXEC, IPPROTO_UDP);
        if (fd < 0)
            return -1;
        if (interface_index) {
#if defined(__ANDROID__) || defined(__linux__)
            if (setsockopt(fd, SOL_SOCKET, SO_BINDTODEVICE, interface_name,
                           strlen(interface_name) + 1) < 0) {
                iap2_log("audio: bind IPv6 UDP socket to %s failed: %s",
                         interface_name, strerror(errno));
                close(fd);
                return -1;
            }
#endif
            if (IN6_IS_ADDR_LINKLOCAL(&target.sin6_addr)) {
                source.sin6_family = AF_INET6;
                source.sin6_scope_id = interface_index;
                if (inet_pton(AF_INET6, "fe80::1",
                              &source.sin6_addr) != 1 ||
                    bind(fd, (const struct sockaddr *)&source,
                         sizeof(source)) < 0) {
                    iap2_log("audio: bind IPv6 UDP source on %s failed: %s",
                             interface_name, strerror(errno));
                    close(fd);
                    return -1;
                }
            }
        } else if (IN6_IS_ADDR_LINKLOCAL(&target.sin6_addr)) {
            iap2_log("audio: IPv6 UDP endpoint requires an interface scope: %s",
                     host);
            close(fd);
            return -1;
        }
        if (connect(fd, (const struct sockaddr *)&target,
                    sizeof(target)) < 0) {
            iap2_log("audio: connect IPv6 UDP endpoint %s port=%u failed: %s",
                     host, port, strerror(errno));
            close(fd);
            return -1;
        }
        iap2_log("audio: IPv6 UDP endpoint connected address=%s port=%u",
                 host, port);
        return fd;
    }

    return -1;
}
static int udp_host_is_ipv6(const char *host)
{
    if (!host)
        return 0;
    if (host[0] == '[')
        host++;
    return strchr(host, ':') != NULL;
}
int carplay_audio_bridge_start_stream(
    struct carplay_audio_bridge *bridge, int type, const char *audio_type,
    uint16_t data_port, uint16_t control_port, const char *host)
{
    struct carplay_audio_stream *stream = NULL;
    size_t i;

    if (!bridge || !audio_type || !data_port || !host)
        return -1;
    size_t count = bridge->legacy && type == 101 ? 2u : bridge->stream_count;
    for (i = 0; i < count; i++) {
        if (bridge->streams[i].configured &&
            bridge->streams[i].type == type &&
            strcasecmp(bridge->streams[i].audio_type, audio_type) == 0) {
            stream = &bridge->streams[i];
            break;
        }
    }
    if (!stream)
        return -1;
    if (stream->active)
        return 0;
    atomic_store_explicit(&stream->stop, 0, memory_order_release);
    stream->data_fd = udp_host_is_ipv6(host) ? -1 :
        connect_udp(AF_INET, host, data_port);
    if (stream->data_fd < 0)
        stream->data_fd = connect_udp(AF_INET6, host, data_port);
    if (stream->data_fd < 0)
        goto fail;
    if (control_port && bridge->legacy && stream->control_fd >= 0) {
        struct addrinfo hints = {0}, *addresses = NULL;
        char service[8];
        hints.ai_family = AF_INET6; hints.ai_socktype = SOCK_DGRAM;
        hints.ai_flags = AI_V4MAPPED;
        snprintf(service, sizeof(service), "%u", control_port);
        if (getaddrinfo(host, service, &hints, &addresses) == 0) {
            (void)connect(stream->control_fd, addresses->ai_addr, addresses->ai_addrlen);
            freeaddrinfo(addresses);
        }
    } else if (control_port) {
        stream->control_fd = udp_host_is_ipv6(host) ? -1 :
            connect_udp(AF_INET, host, control_port);
        if (stream->control_fd < 0)
            stream->control_fd = connect_udp(AF_INET6, host, control_port);
    }
    if (!bridge->legacy && stream->format_mask &&
        stream->format.codec == CARPLAY_AUDIO_CODEC_LPCM_S16BE &&
        output_start(bridge, stream) < 0)
        iap2_log("audio: media player unavailable; retaining stream for teardown safety type=%d",
                 stream->type);
    if (stream->microphone &&
        stream->format.codec == CARPLAY_AUDIO_CODEC_LPCM_S16BE) {
        if (microphone_start(bridge, stream) < 0) {
            iap2_log("audio: car-routed microphone unavailable type=%d",
                     stream->type);
        } else if (pthread_create(&stream->microphone_receive_thread, NULL,
                                  microphone_receive_main, stream) != 0) {
            iap2_log("audio: microphone receive thread creation failed");
            microphone_stop(stream);
        } else {
            stream->microphone_receive_started = 1;
            if (bridge->legacy) {
                /* Phone-side input terminates at Android; never echo it. */
            } else if (pthread_create(&stream->microphone_uplink_thread, NULL,
                               microphone_uplink_main, stream) != 0) {
                iap2_log("audio: microphone uplink thread creation failed");
                stream_request_stop(stream);
                pthread_mutex_lock(&stream->microphone_lock);
                pthread_cond_broadcast(&stream->microphone_ready);
                pthread_mutex_unlock(&stream->microphone_lock);
                pthread_join(stream->microphone_receive_thread, NULL);
                stream->microphone_receive_started = 0;
                microphone_stop(stream);
                atomic_store_explicit(&stream->stop, 0, memory_order_release);
                goto fail;
            } else {
                stream->microphone_uplink_started = 1;
            }
        }
    }
    if (!bridge->legacy) {
        if (pthread_create(&stream->receive_thread, NULL, audio_receive_main,
                           stream) != 0)
            goto fail;
        stream->receive_started = 1;
    }
    if (stream->control_fd >= 0 &&
        pthread_create(&stream->control_thread, NULL, audio_control_main,
                       stream) == 0)
        stream->control_started = 1;
    pthread_mutex_lock(&bridge->media_lock);
    stream->active = 1;
    atomic_store(&bridge->capture_input_ms, audio_now_ms());
    pthread_mutex_unlock(&bridge->media_lock);
    iap2_log("audio: stream attached type=%d audioType=%s dataPort=%u controlPort=%u transport=IPv4-first",
             type, audio_type, data_port, control_port);
    return 0;
fail:
    stream_request_stop(stream);
    pthread_mutex_lock(&stream->microphone_lock);
    pthread_cond_broadcast(&stream->microphone_ready);
    pthread_mutex_unlock(&stream->microphone_lock);
    if (stream->microphone_receive_started)
        pthread_join(stream->microphone_receive_thread, NULL);
    if (stream->microphone_uplink_started)
        pthread_join(stream->microphone_uplink_thread, NULL);
    stream->microphone_receive_started = 0;
    stream->microphone_uplink_started = 0;
    microphone_stop(stream);
    output_stop(stream);
    if (stream->data_fd >= 0)
        close(stream->data_fd);
    if (stream->control_fd >= 0)
        close(stream->control_fd);
    stream->data_fd = -1;
    stream->control_fd = -1;
    atomic_store_explicit(&stream->stop, 0, memory_order_release);
    bridge_error(bridge, "could not connect audio UDP streams");
    return -1;
}

static ssize_t capture_send_packet(
    struct carplay_audio_stream *stream, const uint8_t *wire,
    size_t wire_len)
{
#ifdef CARPLAY_AUDIO_TESTING
    if (stream->force_capture_send_failure) {
        stream->force_capture_send_failure = 0;
        errno = EIO;
        return -1;
    }
#endif
    return send(stream->data_fd, wire, wire_len,
                MSG_DONTWAIT | MSG_NOSIGNAL);
}

static int capture_stream_priority(
    const struct carplay_audio_stream *stream)
{
    if (!stream || !stream->active || !stream->format_mask ||
        stream->format.codec != CARPLAY_AUDIO_CODEC_LPCM_S16BE ||
        stream->data_fd < 0)
        return 0;
    if (stream->type == 100 &&
        strcasecmp(stream->audio_type, "media") == 0)
        return 6;
    if (stream->type == 101 &&
        strcasecmp(stream->audio_type, "default") == 0)
        return 5;
    if (stream->type == 102)
        return 4;
    if (strcasecmp(stream->audio_type, "compatibility") == 0)
        return 3;
    if (stream->type == 100)
        return 2;
    return 1;
}

static int legacy_send_pcm(struct carplay_audio_stream *stream,
                           const uint8_t *pcm, size_t pcm_len)
{
    uint8_t adapted[AUDIO_BUFFER_BYTES], wire[sizeof(stream->packet_pcm) + 12];
    size_t len, frames, offset = 0, wire_len;
    size_t packet_bytes = 352u * stream->format.channels * 2u;
    int failed = 0;
    if (adapt_capture_pcm(stream, pcm, pcm_len, adapted, sizeof(adapted), &len, &frames) < 0)
        return -1;
    while (offset < len) {
        size_t take = packet_bytes - stream->packet_used;
        if (take > len - offset) take = len - offset;
        memcpy(stream->packet_pcm + stream->packet_used, adapted + offset, take);
        offset += take;
        stream->packet_used += take;
        if (stream->packet_used != packet_bytes) continue;
        stream->packet_used = 0;
        if (stream_seal_pcm(stream, stream->packet_pcm, packet_bytes, wire, sizeof(wire), &wire_len) < 0)
            return -1;
        stream->capture_counters.timestamp += 352u;
        if (capture_send_packet(stream, wire, wire_len) != (ssize_t)wire_len) {
            (void)stream_stat_increment(&stream->capture_packets_failed);
            failed = 1;
        } else if (stream_stat_increment(&stream->capture_packets_sent) == 1 && stream->type == 101) {
            iap2_log("audio: first navigation RTP sent type=101 PCM16/16000/mono");
        }
    }
    return failed ? -1 : 0;
}

static int submit_capture_pcm_unlocked(
    struct carplay_audio_bridge *bridge, const uint8_t *pcm, size_t pcm_len)
{
    uint8_t adapted[AUDIO_BUFFER_BYTES];
    uint8_t wire[AUDIO_BUFFER_BYTES + AUDIO_RTP_HEADER_BYTES +
                 AUDIO_RTP_TAIL_BYTES];
    size_t i;
    int wanted_priority;

    if (!bridge || !pcm || !pcm_len || pcm_len > AUDIO_BUFFER_BYTES ||
        (pcm_len & 1u))
        return -1;
    /*
     * A car can advertise a different number and order of audio streams than
     * DiPlay. Pick by semantics, then try lower-priority negotiated outputs if
     * the preferred UDP socket fails. Media is always the first choice and the
     * normal fallback whenever the car offers it.
     */
    for (wanted_priority = 6; wanted_priority >= 1; wanted_priority--) {
        for (i = 0; i < bridge->stream_count; i++) {
            struct carplay_audio_stream *stream = &bridge->streams[i];
            size_t adapted_len = 0;
            size_t adapted_frames = 0;
            size_t wire_len = 0;
            ssize_t sent;
            int saved_errno;

            if (capture_stream_priority(stream) != wanted_priority)
                continue;
            if (bridge->legacy) {
                (void)legacy_send_pcm(stream, pcm, pcm_len);
                return 0;
            }
            if (adapt_capture_pcm(stream, pcm, pcm_len, adapted,
                                  sizeof(adapted), &adapted_len,
                                  &adapted_frames) < 0 ||
                carplay_audio_seal_rtp_pcm(
                    stream->output_key, (uint8_t)stream->type,
                    &stream->capture_counters, adapted, adapted_len,
                    wire, sizeof(wire), &wire_len) < 0) {
                iap2_log("audio: could not encode capture for type=%d audioType=%s; trying next output",
                         stream->type, stream->audio_type);
                continue;
            }
            sent = capture_send_packet(stream, wire, wire_len);
            if (sent != (ssize_t)wire_len) {
                saved_errno = errno;
                if (atomic_fetch_add_explicit(
                        &stream->capture_packets_failed, 1ul,
                        memory_order_relaxed) == 0ul)
                    iap2_log("audio: capture send failed type=%d audioType=%s errno=%d; trying next output",
                             stream->type, stream->audio_type, saved_errno);
                continue;
            }
            stream->capture_counters.timestamp +=
                (uint32_t)adapted_frames;
            if (atomic_fetch_add_explicit(&stream->capture_packets_sent, 1ul,
                                          memory_order_relaxed) == 0ul)
                iap2_log("audio: first captured RTP packet sent type=%d audioType=%s bytes=%zu priority=%d",
                         stream->type, stream->audio_type, adapted_len,
                         wanted_priority);
            atomic_fetch_add_explicit(&stream->capture_bytes_sent,
                                      (unsigned long)adapted_len,
                                      memory_order_relaxed);
            return 0;
        }
    }
    return -1;
}

static void legacy_split_transition(struct carplay_audio_bridge *b, int separate)
{
    if (b->legacy_separate == separate) return;
    b->legacy_separate = separate;
    b->streams[0].packet_used = 0;
    b->streams[1].packet_used = 0;
}

static void submit_split_frame(struct carplay_audio_bridge *b, const uint8_t *frame)
{
    pthread_mutex_lock(&b->media_lock);
    atomic_store(&b->capture_input_ms, audio_now_ms());
    atomic_store(&b->capture_split_ready, 1);
    if (!atomic_load(&b->paused) && b->streams[0].active) {
        struct carplay_audio_stream *nav = &b->streams[1];
        int separate = !b->legacy_call && nav->active && !b->legacy_nav_failed;
        legacy_split_transition(b, separate);
        const uint8_t *music = frame + (separate ? AUDIO_CAPTURE_FRAME_BYTES : 0);
        (void)legacy_send_pcm(&b->streams[0], music, AUDIO_CAPTURE_FRAME_BYTES);
        if (separate && legacy_send_pcm(nav, frame + 2u * AUDIO_CAPTURE_FRAME_BYTES,
                                       AUDIO_CAPTURE_FRAME_BYTES) < 0) {
            b->legacy_nav_failed = 1;
            legacy_split_transition(b, 0);
            iap2_log("audio: navigation 101 UDP send failed, restoring local mix");
        }
    }
    pthread_mutex_unlock(&b->media_lock);
}

int carplay_audio_bridge_submit_capture_pcm(
    struct carplay_audio_bridge *bridge, const uint8_t *pcm, size_t pcm_len)
{
    int result;
    if (!bridge || !pcm || !pcm_len || pcm_len > AUDIO_BUFFER_BYTES ||
        (pcm_len & 1u))
        return -1;
    pthread_mutex_lock(&bridge->media_lock);
    atomic_store(&bridge->capture_input_ms, audio_now_ms());
    if (bridge->legacy) legacy_split_transition(bridge, 0);
    result = atomic_load_explicit(&bridge->paused, memory_order_acquire) ? 0 :
        submit_capture_pcm_unlocked(bridge, pcm, pcm_len);
    pthread_mutex_unlock(&bridge->media_lock);
    return result;
}

static void legacy_capture_silence(struct carplay_audio_bridge *b)
{
    uint64_t now = audio_now_ms();
    if (now - atomic_load(&b->capture_input_ms) < 200u || now - b->capture_silence_ms < 20u) return;
    uint8_t silence[AUDIO_CAPTURE_FRAME_BYTES] = {0};
    pthread_mutex_lock(&b->media_lock);
    if (!atomic_load(&b->paused)) {
        legacy_split_transition(b, 0);
        (void)submit_capture_pcm_unlocked(b, silence, sizeof(silence));
    }
    b->capture_silence_ms = now;
    pthread_mutex_unlock(&b->media_lock);
}

#ifdef CARPLAY_AUDIO_TESTING
int carplay_audio_bridge_test_force_send_failure(
    struct carplay_audio_bridge *bridge, int type, const char *audio_type)
{
    if (!bridge || !audio_type)
        return -1;
    pthread_mutex_lock(&bridge->media_lock);
    size_t count = bridge->legacy && type == 101 ? 2u : bridge->stream_count;
    for (size_t i = 0; i < count; i++) {
        struct carplay_audio_stream *stream = &bridge->streams[i];
        if (stream->active && stream->type == type &&
            strcasecmp(stream->audio_type, audio_type) == 0) {
            stream->force_capture_send_failure = 1;
            pthread_mutex_unlock(&bridge->media_lock);
            return 0;
        }
    }
    pthread_mutex_unlock(&bridge->media_lock);
    return -1;
}
#endif

int carplay_audio_bridge_active_count(
    const struct carplay_audio_bridge *bridge)
{
    size_t i;
    int count = 0;
    if (!bridge)
        return 0;
    for (i = 0; i < bridge->stream_count; i++)
        if (bridge->streams[i].active)
            count++;
    return count;
}

static int read_microphone_unlocked(
    struct carplay_audio_bridge *bridge, uint8_t *pcm, size_t capacity,
    size_t *read_len)
{
    size_t i;
    if (read_len)
        *read_len = 0;
    if (!bridge || !pcm || !capacity)
        return -1;
    for (i = 0; i < bridge->stream_count; i++) {
        struct carplay_audio_stream *stream = &bridge->streams[i];
        size_t len;
        unsigned head;
        if (!stream->configured || !stream->microphone)
            continue;
        pthread_mutex_lock(&stream->microphone_lock);
        if (!stream->microphone_count) {
            pthread_mutex_unlock(&stream->microphone_lock);
            continue;
        }
        head = stream->microphone_head++ % AUDIO_QUEUE_DEPTH;
        len = stream->microphone_queue[head].len;
        if (len > capacity)
            len = capacity & ~(size_t)1;
        memcpy(pcm, stream->microphone_queue[head].bytes, len);
        stream->microphone_count--;
        pthread_mutex_unlock(&stream->microphone_lock);
        if (read_len)
            *read_len = len;
        return 0;
    }
    return -1;
}

int carplay_audio_bridge_read_microphone(
    struct carplay_audio_bridge *bridge, uint8_t *pcm, size_t capacity,
    size_t *read_len)
{
    int result;
    if (read_len)
        *read_len = 0;
    if (!bridge || !pcm || !capacity)
        return -1;
    pthread_mutex_lock(&bridge->media_lock);
    result = atomic_load_explicit(&bridge->paused, memory_order_acquire) ? -1 :
        read_microphone_unlocked(bridge, pcm, capacity, read_len);
    pthread_mutex_unlock(&bridge->media_lock);
    return result;
}

const char *carplay_audio_bridge_last_error(
    const struct carplay_audio_bridge *bridge)
{
    return bridge ? bridge->error : "audio bridge unavailable";
}

/* Traditional phone-side audio. The IPC and Android policy survive a stream
 * replacement; only the single network stream is rebuilt. */
static uint64_t audio_now_ms(void)
{
    struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t);
    return (uint64_t)t.tv_sec * 1000u + (uint64_t)t.tv_nsec / 1000000u;
}
static void put32le(uint8_t *p, uint32_t n)
{ for (unsigned i=0;i<4;i++) { p[i]=(uint8_t)n; n>>=8; } }
static uint32_t get32le(const uint8_t *p)
{ return p[0]|((uint32_t)p[1]<<8)|((uint32_t)p[2]<<16)|((uint32_t)p[3]<<24); }
static void mic_header(uint8_t *p, unsigned type, size_t n,
                        const struct carplay_audio_pcm_format *format)
{
    memcpy(p,"CPM1",4); put32le(p+4,type); put32le(p+8,(uint32_t)n);
    put32le(p+12,format ? format->sample_rate : 0);
    put32le(p+16,format ? format->channels : 0);
}
static int mic_send(struct carplay_audio_bridge *b, int fd, const uint8_t *p, size_t n)
{
    uint64_t deadline = audio_now_ms() + 250u;
    while (n && !atomic_load(&b->mic_stop)) {
        ssize_t sent = send(fd,p,n,MSG_DONTWAIT|MSG_NOSIGNAL);
        if (sent > 0) { p+=sent; n-=(size_t)sent; continue; }
        if (sent < 0 && errno == EINTR) continue;
        if (sent < 0 && (errno == EAGAIN || errno == EWOULDBLOCK) && audio_now_ms()<deadline) {
            struct pollfd f={fd,POLLOUT,0}; (void)poll(&f,1,20); continue;
        }
        return -1;
    }
    return n ? -1 : 0;
}
static void *legacy_mic_ipc(void *opaque)
{
    struct carplay_audio_bridge *b=opaque;
    int client=-1, command_wait_ms=10; size_t used=0;
    uint8_t command[20], packet[20+AUDIO_BUFFER_BYTES];
    while (!atomic_load(&b->mic_stop)) {
        if (client < 0) {
            struct pollfd f={b->mic_listener,POLLIN,0};
            if (poll(&f,1,100)<=0 || !(f.revents&POLLIN)) continue;
            client=accept4(b->mic_listener,NULL,NULL,SOCK_CLOEXEC|SOCK_NONBLOCK);
            if(client<0) continue;
            used=0; command_wait_ms=10;
            atomic_store(&b->mic_ready,0);atomic_store(&b->mic_demand,0);
            mic_header(packet,1,0,&b->legacy_mic_format);
            if(mic_send(b,client,packet,20)<0) goto disconnected;
        }
        {
            struct pollfd f={client,POLLIN,0};
            int r=poll(&f,1,command_wait_ms);
            if(r<0 && errno!=EINTR) goto disconnected;
            if(r>0 && (f.revents&(POLLHUP|POLLERR|POLLNVAL))) goto disconnected;
            if(r>0 && (f.revents&POLLIN)) {
                ssize_t n=recv(client,command+used,sizeof(command)-used,MSG_DONTWAIT);
                if(n==0) goto disconnected;
                if(n<0 && errno!=EINTR && errno!=EAGAIN && errno!=EWOULDBLOCK) goto disconnected;
                if(n>0) used+=(size_t)n;
                if(used==sizeof(command)) {
                    unsigned type=get32le(command+4); used=0;
                    if(memcmp(command,"CPM1",4) || get32le(command+8) ||
                       get32le(command+12) || get32le(command+16) || type<3 || type>6)
                        goto disconnected;
                    atomic_store(&b->mic_heartbeat_ms,audio_now_ms());
                    if(type==5) atomic_store(&b->mic_ready,1);
                    if(type==3 || type==4) atomic_store(&b->mic_demand,type==3);
                    if(type==6) { atomic_store(&b->mic_ready,0);atomic_store(&b->mic_demand,0); }
                }
            }
        }
        command_wait_ms=10;
        if(atomic_load(&b->mic_ready)) {
            size_t n=0; int result=0;
            /* Serialize the entire bounded IPC write with Borrow and stream
             * replacement so a copied old frame cannot escape after return. */
            pthread_mutex_lock(&b->media_lock);
            if(!atomic_load(&b->paused) && b->streams[0].active &&
               read_microphone_unlocked(b,packet+20,AUDIO_BUFFER_BYTES,&n)==0 && n) {
                mic_header(packet,2,n,&b->legacy_mic_format);
                result=mic_send(b,client,packet,20+n);
                /* Drain queued PCM without a rate-limiting sleep, while still
                 * checking commands and shutdown before every bounded write. */
                command_wait_ms=0;
            }
            pthread_mutex_unlock(&b->media_lock);
            if(result<0) goto disconnected;
        }
        continue;
disconnected:
        close(client); client=-1;
        atomic_store(&b->mic_ready,0);atomic_store(&b->mic_demand,0);
    }
    if(client>=0) close(client);
    return NULL;
}
static int legacy_ipc_start(struct carplay_audio_bridge *b)
{
    if(!b->legacy_call_format || !b->enable_microphone) return 0;
    struct sockaddr_un a={.sun_family=AF_UNIX};
    const char *name=MICROPHONE_SOCKET; size_t n=strlen(name);
    memcpy(a.sun_path+1,name,n);
    b->mic_listener=socket(AF_UNIX,SOCK_STREAM|SOCK_CLOEXEC|SOCK_NONBLOCK,0);
    if(b->mic_listener<0) return -1;
    if(bind(b->mic_listener,(void *)&a,(socklen_t)(offsetof(struct sockaddr_un,sun_path)+n+1))<0 ||
       listen(b->mic_listener,1)<0) goto fail;
    atomic_store(&b->mic_stop,0);
    if(pthread_create(&b->mic_thread,NULL,legacy_mic_ipc,b)!=0) goto fail;
    b->mic_thread_started=1;return 0;
fail:
    close(b->mic_listener);b->mic_listener=-1;return -1;
}
static void legacy_ipc_stop(struct carplay_audio_bridge *b)
{
    atomic_store(&b->mic_stop,1);
    if(b->mic_thread_started) pthread_join(b->mic_thread,NULL);
    b->mic_thread_started=0;
    if(b->mic_listener>=0) close(b->mic_listener);
    b->mic_listener=-1;atomic_store(&b->mic_ready,0);atomic_store(&b->mic_demand,0);
}
int carplay_audio_bridge_microphone_demand(struct carplay_audio_bridge *b)
{
    if(!b || !b->legacy || !b->legacy_call_format) return 0;
    return atomic_load(&b->mic_ready) && atomic_load(&b->mic_demand) &&
        audio_now_ms()-atomic_load(&b->mic_heartbeat_ms)<8000u;
}
int carplay_audio_bridge_legacy_mode(const struct carplay_audio_bridge *b)
{ return b && b->legacy ? b->legacy_call : 0; }
int carplay_audio_bridge_init_legacy(struct carplay_audio_bridge **b,int media,int microphone)
{
    uint8_t unused[32]={0};
    int result=carplay_audio_bridge_init(b,unused,media,microphone);
    if(result==0) (*b)->legacy=1;
    return result;
}
void carplay_audio_bridge_legacy_stop_stream(struct carplay_audio_bridge *b)
{
    if(!b || !b->legacy) return;
    struct carplay_audio_stream *s=&b->streams[0];
    pthread_mutex_lock(&b->media_lock);
    legacy_split_transition(b, 0);
    s->active=0;s->configured=0;stream_request_stop(s);s->packet_used=0;
    pthread_mutex_lock(&s->microphone_lock);
    s->microphone_count=s->microphone_head=s->microphone_tail=0;
    pthread_mutex_unlock(&s->microphone_lock);
    pthread_mutex_unlock(&b->media_lock);
    if(s->microphone_receive_started) pthread_join(s->microphone_receive_thread,NULL);
    if(s->control_started) pthread_join(s->control_thread,NULL);
    s->microphone_receive_started=0;s->control_started=0;
    pthread_mutex_lock(&b->media_lock);
    pthread_mutex_lock(&s->microphone_lock);
    s->microphone_count=s->microphone_head=s->microphone_tail=0;
    pthread_mutex_unlock(&s->microphone_lock);
    pthread_mutex_unlock(&b->media_lock);
    microphone_stop(s);
    if(s->data_fd>=0) close(s->data_fd);
    if(s->control_fd>=0) close(s->control_fd);
    s->data_fd=s->control_fd=-1;s->local_control_port=0;
    memset(&s->capture_counters,0,sizeof(s->capture_counters));
}

static void legacy_stop_nav(struct carplay_audio_bridge *b)
{
    struct carplay_audio_stream *s = &b->streams[1];
    pthread_mutex_lock(&b->media_lock);
    s->active = s->configured = 0;
    s->packet_used = 0;
    stream_request_stop(s);
    pthread_mutex_unlock(&b->media_lock);
    if (s->control_started) pthread_join(s->control_thread, NULL);
    s->control_started = 0;
    if (s->data_fd >= 0) close(s->data_fd);
    if (s->control_fd >= 0) close(s->control_fd);
    s->data_fd = s->control_fd = -1;
    memset(&s->capture_counters, 0, sizeof(s->capture_counters));
    atomic_store(&s->capture_packets_sent, 0);
}

int carplay_audio_bridge_legacy_nav_setup(struct carplay_audio_bridge *b,
                                         struct airplay_stream_setup *setup)
{
    if (!b || !b->legacy || !b->legacy_nav_format || !setup ||
        !atomic_load(&b->capture_split_ready)) return 0;
    pthread_mutex_lock(&b->media_lock);
    int ready = b->streams[0].active && !b->legacy_call && !atomic_load(&b->paused);
    pthread_mutex_unlock(&b->media_lock);
    if (!ready) return 0;

    struct carplay_audio_stream *s = &b->streams[1];
    if (!s->configured) {
        struct sockaddr_in6 a = {.sin6_family=AF_INET6};
        socklen_t length = sizeof(a);
        int zero = 0;
        s->control_fd = socket(AF_INET6, SOCK_DGRAM | SOCK_CLOEXEC, 0);
        if (s->control_fd < 0) return -1;
        (void)setsockopt(s->control_fd, IPPROTO_IPV6, IPV6_V6ONLY, &zero, sizeof(zero));
        if (bind(s->control_fd, (void *)&a, sizeof(a)) < 0 ||
            getsockname(s->control_fd, (void *)&a, &length) < 0 ||
            carplay_audio_lpcm_format(b->legacy_nav_format, &s->format) < 0) {
            legacy_stop_nav(b);
            return -1;
        }
        s->type = 101;
        s->format_mask = b->legacy_nav_format;
        s->local_control_port = ntohs(a.sin6_port);
        strcpy(s->audio_type, "default");
        s->configured = 1;
    }
    *setup = (struct airplay_stream_setup){.type=101, .audio_type=s->audio_type,
        .audio_format=s->format_mask, .control_port=s->local_control_port, .legacy_audio=1};
    return 1;
}
static uint32_t legacy_format(uint32_t mask, int mono)
{
    static const uint32_t media_formats[]={0x8000,0x800,0x4000,0x400,0x200,0x100,0x80,0x40,0x20,0x10,8,4};
    static const uint32_t call_formats[]={0x40,0x10,0x100,0x4000,0x400,4};
    const uint32_t *formats=mono ? call_formats : media_formats;
    size_t count=mono ? sizeof(call_formats)/sizeof(call_formats[0]) : sizeof(media_formats)/sizeof(media_formats[0]);
    for(size_t i=0;i<count;i++) {
        struct carplay_audio_pcm_format f;
        if((mask&formats[i]) && carplay_audio_lpcm_format(formats[i],&f)==0 && (!mono || f.channels==1))
            return formats[i];
    }
    return 0;
}
int carplay_audio_bridge_legacy_select(struct carplay_audio_bridge *b,int telephony)
{
    if(!b || !b->legacy) return -1;
    uint32_t mask=telephony ? b->legacy_call_format : b->legacy_media_format;
    if(!mask) return -1;
    carplay_audio_bridge_legacy_stop_stream(b);
    struct carplay_audio_stream *s=&b->streams[0];
    s->type=100;s->connection_id=0;s->format_mask=mask;s->microphone=!!telephony;
    strcpy(s->audio_type,telephony ? "telephony" : "media");
    if(carplay_audio_lpcm_format(mask,&s->format)<0) return -1;
    /* Bind RTCP before SETUP; a dual-stack socket accepts either peer family. */
    struct sockaddr_in6 a={.sin6_family=AF_INET6};socklen_t length=sizeof(a);int zero=0;
    s->control_fd=socket(AF_INET6,SOCK_DGRAM|SOCK_CLOEXEC,0);
    if(s->control_fd<0) return -1;
    (void)setsockopt(s->control_fd,IPPROTO_IPV6,IPV6_V6ONLY,&zero,sizeof(zero));
    if(bind(s->control_fd,(void *)&a,sizeof(a))<0 || getsockname(s->control_fd,(void *)&a,&length)<0)
        goto fail;
    s->local_control_port=ntohs(a.sin6_port);
    if(telephony && bind_microphone_socket(b,s)<0) goto fail;
    pthread_mutex_lock(&b->media_lock);
    s->configured=1;b->legacy_call=!!telephony;b->stream_count=1;
    pthread_mutex_unlock(&b->media_lock);
    return 0;
fail:
    carplay_audio_bridge_legacy_stop_stream(b);return -1;
}
static uint32_t legacy_capability_format(
    const struct carplay_session_capabilities *caps, const char *audio_type,
    int duplex)
{
    uint32_t mask = 0;
    int best_rank = 3;
    for (size_t i = 0; i < caps->audio.format_count; i++) {
        const struct carplay_audio_format_capabilities *f = &caps->audio.formats[i];
        int rank;
        if (!f->present || f->type != 100) continue;
        if (!strcasecmp(f->audio_type, audio_type)) rank = 0;
        else if (!f->audio_type[0]) rank = 1;
        else if (!strcasecmp(f->audio_type, "compatibility")) rank = 2;
        else continue;
        if (rank > best_rank) continue;
        if (rank < best_rank) {
            best_rank = rank;
            mask = 0;
        }
        /* Keep each duplex pair intact. Combining input/output separately
         * could invent a format that no individual entry supports both ways.
         * Explicit entries remain authoritative even if their mask is zero. */
        mask |= duplex ? f->output_formats & f->input_formats : f->output_formats;
    }
    return legacy_format(mask, duplex);
}

static int legacy_prepare(struct carplay_audio_bridge *b,const struct carplay_session_capabilities *caps)
{
    carplay_audio_bridge_stop(b);
    b->legacy_media_format = b->enable_media ? legacy_capability_format(caps, "media", 0) : 0;
    b->legacy_call_format = b->enable_microphone ? legacy_capability_format(caps, "telephony", 1) : 0;
    b->legacy_nav_format = 0;
    b->legacy_nav_failed = 0;
    if (b->legacy_media_format) {
        for (size_t i = 0; i < caps->audio.format_count; i++) {
            const struct carplay_audio_format_capabilities *f = &caps->audio.formats[i];
            if (f->present && f->type == 101 && (f->output_formats & 0x10) &&
                (!f->audio_type[0] || !strcasecmp(f->audio_type, "default")))
                b->legacy_nav_format = 0x10;
        }
    }
    iap2_log("audio: legacy selected media=0x%08x telephony=0x%08x (explicit, unlabelled, compatibility priority)",
             b->legacy_media_format, b->legacy_call_format);
    if(b->legacy_call_format)
        (void)carplay_audio_lpcm_format(b->legacy_call_format,&b->legacy_mic_format);
    if(b->legacy_media_format && carplay_audio_bridge_legacy_select(b,0)<0) return -1;
    if(legacy_ipc_start(b)<0) iap2_log("audio: Android microphone IPC unavailable");
    if((b->enable_media || b->legacy_call_format) && capture_start(b)<0)
        iap2_log("audio: Android media capture IPC unavailable");
    b->prepared=1;return 0;
}

void carplay_audio_bridge_get_status(const struct carplay_audio_bridge *bridge,
                                     struct carplay_audio_status *status)
{
    if (!status) return;
    memset(status, 0, sizeof(*status));
    if (!bridge) return;
    struct carplay_audio_bridge *b = (struct carplay_audio_bridge *)bridge;
    pthread_mutex_lock(&b->media_lock);
    int paused = atomic_load(&b->paused);
    if (b->legacy) {
        status->media_available = b->legacy_media_format != 0;
        status->microphone_available = b->legacy_call_format != 0;
        status->microphone_ready = atomic_load(&b->mic_ready);
        status->microphone_demand = carplay_audio_bridge_microphone_demand(b);
        status->media_active = b->streams[0].active && !paused;
        status->microphone_active = status->media_active && b->legacy_call &&
                                    status->microphone_ready && status->microphone_demand;
        if (b->legacy_call)
            status->microphone_packets_received = stream_stat_get(&b->streams[0].packets_decrypted);
    } else {
        for (size_t i = 0; i < b->stream_count; i++) {
            struct carplay_audio_stream *s = &b->streams[i];
            status->media_available |= s->format_mask != 0;
            status->microphone_available |= s->microphone;
            status->media_active |= s->active && s->format_mask && !paused;
            status->microphone_active |= s->active && s->microphone_receive_started && !paused;
        }
        status->microphone_ready = status->microphone_active;
    }
    pthread_mutex_unlock(&b->media_lock);
}
