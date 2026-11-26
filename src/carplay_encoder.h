#ifndef CARPLAY_ENCODER_H
#define CARPLAY_ENCODER_H
#include <stddef.h>
#include <stdint.h>
#include <sys/types.h>

#define CARPLAY_ENCODER_MAX_PACKET (4u * 1024u * 1024u)
/* Installed RK3399 OMX encoder's advertised coded-surface limits. */
#define CARPLAY_ENCODER_MAX_WIDTH 1920u
#define CARPLAY_ENCODER_MAX_HEIGHT 1088u
#define CARPLAY_ENCODER_CONFIG 1u
#define CARPLAY_ENCODER_KEYFRAME 2u
#define CARPLAY_ENCODER_EOS 4u
/* Empty encoder-thread heartbeat; pts_us carries the completed frame count. */
#define CARPLAY_ENCODER_HEARTBEAT 8u

struct carplay_encoder_packet {
    const uint8_t *data;
    size_t size;
    unsigned flags;
    uint64_t pts_us;
};

struct carplay_encoder_reader {
    uint8_t header[24];
    size_t header_used, payload_used, payload_size, capacity;
    uint8_t *payload;
    unsigned flags;
    uint64_t pts_us, partial_since_ms;
    int delivered;
};

struct carplay_encoder {
    pid_t pid;
    int fd;
    struct carplay_encoder_reader reader;
};

void carplay_encoder_init(struct carplay_encoder *encoder);
int carplay_encoder_start(struct carplay_encoder *encoder, unsigned width,
                          unsigned height, unsigned bitrate, unsigned fps);
int carplay_encoder_start_scaled(struct carplay_encoder *encoder,
    unsigned width, unsigned height, unsigned bitrate, unsigned fps,
    unsigned source_width, unsigned source_height);
int carplay_encoder_request_keyframe(struct carplay_encoder *encoder);
int carplay_encoder_request_refresh(struct carplay_encoder *encoder);
/* Graceful close followed by bounded TERM/KILL and waitpid. Owner thread only. */
int carplay_encoder_stop(struct carplay_encoder *encoder);
/* 1 complete packet, 0 temporarily idle, -1 fatal/EOF. Data valid until next read. */
int carplay_encoder_reader_read(struct carplay_encoder_reader *reader, int fd,
                                struct carplay_encoder_packet *packet,
                                uint64_t now_ms);
void carplay_encoder_reader_destroy(struct carplay_encoder_reader *reader);
#endif
