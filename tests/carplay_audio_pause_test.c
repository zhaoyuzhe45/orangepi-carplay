/* Host harness: real UDP/worker lifecycle and RTP counters, mocked OpenSL and
 * crypto boundaries. Crypto correctness belongs to airplay_crypto_selftest.
 * Compile with __ANDROID__ and NDK headers via -idirafter, never link OpenSL.
 */
#define SL_API_DEPRECATED(level)
#define CARPLAY_AUDIO_CAPTURE_SOCKET "carplay_audio_pause_test_v1"
#include "../src/carplay_audio_bridge.c"
#include <assert.h>
#include <time.h>

const SLInterfaceID SL_IID_ANDROIDCONFIGURATION = NULL;
const SLInterfaceID SL_IID_ANDROIDSIMPLEBUFFERQUEUE = NULL;
const SLInterfaceID SL_IID_ENGINE = NULL;
const SLInterfaceID SL_IID_PLAY = NULL;
static _Atomic unsigned enqueues;
static _Atomic unsigned clears;
static _Atomic unsigned play_state;

SLresult slCreateEngine(SLObjectItf *object, SLuint32 options_count,
    const SLEngineOption *options, SLuint32 interface_count,
    const SLInterfaceID *ids, const SLboolean *required)
{
    (void)object; (void)options_count; (void)options; (void)interface_count;
    (void)ids; (void)required;
    return SL_RESULT_RESOURCE_ERROR;
}
static SLresult mock_enqueue(SLAndroidSimpleBufferQueueItf queue,
                              const void *buffer, SLuint32 size)
{
    (void)queue; (void)buffer;
    assert(size > 0);
    atomic_fetch_add(&enqueues, 1);
    return SL_RESULT_SUCCESS;
}
static SLresult mock_clear(SLAndroidSimpleBufferQueueItf queue)
{
    (void)queue;
    atomic_fetch_add(&clears, 1);
    return SL_RESULT_SUCCESS;
}
static SLresult mock_play_state(SLPlayItf player, SLuint32 state)
{
    (void)player;
    atomic_store(&play_state, state);
    return SL_RESULT_SUCCESS;
}
static const struct SLAndroidSimpleBufferQueueItf_ queue_vtable = {
    .Enqueue = mock_enqueue, .Clear = mock_clear
};
static const struct SLAndroidSimpleBufferQueueItf_ *queue = &queue_vtable;
static const struct SLPlayItf_ player_vtable = {.SetPlayState = mock_play_state};
static const struct SLPlayItf_ *player = &player_vtable;

void iap2_log(const char *format, ...) { (void)format; }
int airplay_hkdf_sha512(const uint8_t *ikm, size_t ikm_len,
    const uint8_t *salt, size_t salt_len, const uint8_t *info, size_t info_len,
    uint8_t *out, size_t out_len)
{
    (void)ikm; (void)ikm_len; (void)salt; (void)salt_len;
    (void)info; (void)info_len; memset(out, 0x42, out_len); return 0;
}
int airplay_chacha20_poly1305_seal(const uint8_t key[32], const uint8_t nonce[12],
    const uint8_t *plain, size_t len, const uint8_t *aad, size_t aad_len,
    uint8_t *out, size_t cap, size_t *out_len)
{
    (void)key; (void)nonce; (void)aad; (void)aad_len;
    if (cap < len + 16) return -1;
    memcpy(out, plain, len); memset(out + len, 0, 16); *out_len = len + 16;
    return 0;
}
int airplay_chacha20_poly1305_open(const uint8_t key[32], const uint8_t nonce[12],
    const uint8_t *sealed, size_t len, const uint8_t *aad, size_t aad_len,
    uint8_t *out, size_t cap, size_t *out_len)
{
    (void)key; (void)nonce; (void)aad; (void)aad_len;
    if (len < 16 || cap < len - 16) return -1;
    memcpy(out, sealed, len - 16); *out_len = len - 16; return 0;
}

#ifdef PAUSE_RED_STUB
void carplay_audio_bridge_set_paused(struct carplay_audio_bridge *b, int p)
{ (void)b; (void)p; }
#endif

static int receive_packet(int fd, uint8_t *packet, struct sockaddr_in *peer)
{
    struct pollfd pfd = {.fd = fd, .events = POLLIN};
    socklen_t peer_len = sizeof(*peer);
    assert(poll(&pfd, 1, 1000) == 1);
    int len = (int)recvfrom(fd, packet, 4096, 0,
                          (struct sockaddr *)peer, &peer_len);
    assert(len >= 36);
    return len;
}

static void no_packet(int fd)
{
    struct pollfd pfd = {.fd = fd, .events = POLLIN};
    assert(poll(&pfd, 1, 150) == 0);
}

static void wait_enqueues(unsigned wanted)
{
    for (unsigned i = 0; i < 1000 && atomic_load(&enqueues) != wanted; i++)
        usleep(1000);
    assert(atomic_load(&enqueues) == wanted);
}

static uint64_t packet_nonce(const uint8_t *packet, size_t len)
{
    uint64_t value = 0;
    for (size_t i = 0; i < 8; i++) value |= (uint64_t)packet[len - 8 + i] << (8 * i);
    return value;
}

int main(void)
{
    struct carplay_audio_bridge *bridge = NULL;
    struct carplay_session_capabilities caps = {0};
    struct sockaddr_in receiver = {0}, stream_peer, mic_peer;
    socklen_t addr_len = sizeof(receiver);
    uint8_t secret[32] = {0}, pcm[16] = {1, 2, 3, 4}, wire[4096], scratch[4096];
    size_t wire_len, read_len = 99;
    struct carplay_audio_rtp_counters car_counters = {0};
    int fd = socket(AF_INET, SOCK_DGRAM, 0), len;
    assert(fd >= 0);
    receiver.sin_family = AF_INET;
    receiver.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    assert(bind(fd, (struct sockaddr *)&receiver, sizeof(receiver)) == 0);
    assert(getsockname(fd, (struct sockaddr *)&receiver, &addr_len) == 0);
    caps.audio.description_present = caps.audio.media_output = 1;
    caps.audio.microphone_input = caps.audio.format_count = 1;
    caps.audio.formats[0].present = 1;
    caps.audio.formats[0].type = 100;
    strcpy(caps.audio.formats[0].audio_type, "media");
    caps.audio.formats[0].output_formats = 0x8000;
    caps.audio.formats[0].input_formats = 0x10;
    assert(carplay_audio_bridge_init(&bridge, secret, 1, 1) == 0);
    assert(carplay_audio_bridge_prepare(bridge, &caps) == 0);
    assert(carplay_audio_bridge_start_stream(bridge, 100, "media",
             ntohs(receiver.sin_port), 0, "127.0.0.1") == 0);
    struct carplay_audio_stream *stream = &bridge->streams[0];
    stream->player = &player;
    stream->player_queue = &queue;
    assert(carplay_audio_bridge_submit_capture_pcm(bridge, pcm, sizeof(pcm)) == 0);
    len = receive_packet(fd, scratch, &stream_peer);
    assert(packet_nonce(scratch, (size_t)len) == 0 && scratch[3] == 0);
    assert(carplay_audio_seal_rtp_pcm(stream->output_key, 100, &car_counters,
             pcm, sizeof(pcm), wire, sizeof(wire), &wire_len) == 0);
    assert(sendto(fd, wire, wire_len, 0, (struct sockaddr *)&stream_peer,
                  sizeof(stream_peer)) == (ssize_t)wire_len);
    wait_enqueues(1);

    mic_peer = receiver;
    mic_peer.sin_port = htons(stream->microphone_port);
    assert(sendto(fd, wire, wire_len, 0, (struct sockaddr *)&mic_peer,
                  sizeof(mic_peer)) == (ssize_t)wire_len);
    len = receive_packet(fd, scratch, &receiver);
    assert(packet_nonce(scratch, (size_t)len) == 0);

    carplay_audio_bridge_set_paused(bridge, 1);
    assert(carplay_audio_bridge_submit_capture_pcm(bridge, pcm, sizeof(pcm)) == 0);
    no_packet(fd); /* RED: no-op pause sends a captured packet. */
    assert(atomic_load(&play_state) == SL_PLAYSTATE_PAUSED);
    assert(atomic_load(&clears) == 1);
    assert(carplay_audio_bridge_setup_count(bridge) == 1);
    assert(carplay_audio_bridge_active_count(bridge) == 1);
    assert(stream->capture_counters.nonce == 1);
    assert(stream->microphone_counters.nonce == 1);
    assert(sendto(fd, wire, wire_len, 0, (struct sockaddr *)&stream_peer,
                  sizeof(stream_peer)) == (ssize_t)wire_len);
    assert(sendto(fd, wire, wire_len, 0, (struct sockaddr *)&mic_peer,
                  sizeof(mic_peer)) == (ssize_t)wire_len);
    no_packet(fd);
    assert(atomic_load(&enqueues) == 1);
    microphone_queue_push(stream, pcm, sizeof(pcm));
    assert(stream->microphone_count == 0);
    assert(carplay_audio_bridge_read_microphone(bridge, scratch, sizeof(scratch),
                                                 &read_len) == -1 && read_len == 0);
    carplay_audio_bridge_set_paused(bridge, 1); /* Idempotent. */
    carplay_audio_bridge_set_paused(bridge, 0);
    assert(atomic_load(&play_state) == SL_PLAYSTATE_PLAYING);
    assert(carplay_audio_bridge_submit_capture_pcm(bridge, pcm, sizeof(pcm)) == 0);
    len = receive_packet(fd, scratch, &receiver);
    assert(packet_nonce(scratch, (size_t)len) == 1 && scratch[3] == 1);
    assert(sendto(fd, wire, wire_len, 0, (struct sockaddr *)&stream_peer,
                  sizeof(stream_peer)) == (ssize_t)wire_len);
    wait_enqueues(2);
    assert(sendto(fd, wire, wire_len, 0, (struct sockaddr *)&mic_peer,
                  sizeof(mic_peer)) == (ssize_t)wire_len);
    len = receive_packet(fd, scratch, &receiver);
    assert(packet_nonce(scratch, (size_t)len) == 1 && scratch[3] == 1);
    carplay_audio_bridge_destroy(&bridge);
    close(fd);
    puts("audio pause: capture/playback/microphone suppressed, streams and nonces retained");
    return 0;
}
