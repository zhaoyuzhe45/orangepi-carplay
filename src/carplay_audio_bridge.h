#ifndef CARPLAY_AUDIO_BRIDGE_H
#define CARPLAY_AUDIO_BRIDGE_H

#include "airplay_crypto.h"
#include "carplay_capabilities.h"

#include <pthread.h>
#include <stddef.h>
#include <stdint.h>

#define CARPLAY_AUDIO_MAX_STREAMS CARPLAY_MAX_AUDIO_FORMATS
#define CARPLAY_AUDIO_MAX_PACKET 4096u
#ifndef CARPLAY_AUDIO_CAPTURE_SOCKET
#ifdef CARPLAY_AUDIO_TESTING
#define CARPLAY_AUDIO_CAPTURE_SOCKET "carplay_audio_capture_test_v1"
#else
#define CARPLAY_AUDIO_CAPTURE_SOCKET "carplay_audio_capture_v1"
#endif
#endif

struct airplay_stream_setup;

enum carplay_audio_codec {
    CARPLAY_AUDIO_CODEC_NONE = 0,
    CARPLAY_AUDIO_CODEC_LPCM_S16BE
};

struct carplay_audio_pcm_format {
    enum carplay_audio_codec codec;
    unsigned sample_rate;
    unsigned channels;
    unsigned bits_per_sample;
};

struct carplay_audio_rtp_counters {
    uint16_t sequence;
    uint32_t timestamp;
    uint64_t nonce;
    uint32_t ssrc;
};

struct carplay_audio_bridge;

/*
 * The bridge emits only formats it can play without a platform codec:
 * wired LPCM (16-bit, big-endian on the wire). Compressed formats are left
 * unavailable until an encoder/decoder is present.
 */
int carplay_audio_lpcm_format(uint32_t format_mask,
                              struct carplay_audio_pcm_format *format);
int carplay_audio_decrypt_rtp(
    const uint8_t key[AIRPLAY_CHACHA_KEY_BYTES],
    const uint8_t *packet, size_t packet_len,
    uint8_t *payload, size_t payload_capacity, size_t *payload_len);
int carplay_audio_seal_rtp_pcm(
    const uint8_t key[AIRPLAY_CHACHA_KEY_BYTES], uint8_t payload_type,
    struct carplay_audio_rtp_counters *counters,
    const void *pcm_little_endian, size_t pcm_len,
    uint8_t *packet, size_t packet_capacity, size_t *packet_len);

int carplay_audio_bridge_init(struct carplay_audio_bridge **bridge,
                              const uint8_t shared_secret[32],
                              int enable_media, int enable_microphone);
/* Explicit et=NONE phone-side path. Existing initializer remains HAP. */
int carplay_audio_bridge_init_legacy(struct carplay_audio_bridge **bridge,
                                    int enable_media, int enable_microphone);
/* Lifecycle calls are serialized by the control owner; IPC stays alive. */
int carplay_audio_bridge_legacy_select(struct carplay_audio_bridge *bridge,
                                      int telephony);
void carplay_audio_bridge_legacy_stop_stream(struct carplay_audio_bridge *bridge);
/* Returns 1 when V2 capture and media are ready, 0 while waiting, or -1 on error. */
int carplay_audio_bridge_legacy_nav_setup(struct carplay_audio_bridge *bridge,
                                         struct airplay_stream_setup *setup);
int carplay_audio_bridge_microphone_demand(struct carplay_audio_bridge *bridge);
int carplay_audio_bridge_legacy_mode(const struct carplay_audio_bridge *bridge);
struct carplay_audio_status {
    int media_available, microphone_available;
    int media_active, microphone_active, microphone_ready, microphone_demand;
    uint64_t microphone_packets_received;
};
void carplay_audio_bridge_get_status(const struct carplay_audio_bridge *bridge,
                                     struct carplay_audio_status *status);
void carplay_audio_bridge_destroy(struct carplay_audio_bridge **bridge);
int carplay_audio_bridge_prepare(
    struct carplay_audio_bridge *bridge,
    const struct carplay_session_capabilities *capabilities);
size_t carplay_audio_bridge_setup_count(
    const struct carplay_audio_bridge *bridge);
int carplay_audio_bridge_fill_setups(
    const struct carplay_audio_bridge *bridge,
    struct airplay_stream_setup *setups, size_t capacity);
/* Resolve receiver SETUP responses, which omit audioType, by connection ID. */
int carplay_audio_bridge_resolve_response(
    const struct carplay_audio_bridge *bridge, int type,
    uint64_t connection_id, char *audio_type, size_t capacity,
    size_t *audio_type_len);
/*
 * Ports are the receiver's advertised UDP endpoints. Host may be an IPv4
 * literal or hostname; IPv4 is attempted before IPv6. Calls are idempotent for
 * a (type, audioType) pair and never block protocol teardown.
 */
int carplay_audio_bridge_start_stream(
    struct carplay_audio_bridge *bridge, int type, const char *audio_type,
    uint16_t data_port, uint16_t control_port, const char *host);
/*
 * Submits PCM captured from the local Android media mix. Data is sealed with
 * the active media stream's output key and sent to its negotiated data port.
 * PCM is little-endian 16-bit at the stream's negotiated rate and channels.
 */
int carplay_audio_bridge_submit_capture_pcm(
    struct carplay_audio_bridge *bridge, const uint8_t *pcm, size_t pcm_len);
#ifdef CARPLAY_AUDIO_TESTING
/* Protocol tests make the next send fail to exercise runtime fallback. */
int carplay_audio_bridge_test_force_send_failure(
    struct carplay_audio_bridge *bridge, int type, const char *audio_type);
#endif
void carplay_audio_bridge_stop(struct carplay_audio_bridge *bridge);
/* Resource Borrow pause: drain incoming sockets, discard queued PCM and stop
 * audio I/O without resetting negotiated streams or encryption counters.
 * Serializes with in-flight media I/O; caller serializes lifecycle operations.
 * submit_capture_pcm returns 0 when paused PCM is intentionally discarded.
 */
void carplay_audio_bridge_set_paused(struct carplay_audio_bridge *bridge,
                                    int paused);
int carplay_audio_bridge_active_count(
    const struct carplay_audio_bridge *bridge);
const char *carplay_audio_bridge_last_error(
    const struct carplay_audio_bridge *bridge);
/* Copies the newest decrypted car-microphone PCM without blocking. */
int carplay_audio_bridge_read_microphone(
    struct carplay_audio_bridge *bridge, uint8_t *pcm, size_t capacity,
    size_t *read_len);

#endif
