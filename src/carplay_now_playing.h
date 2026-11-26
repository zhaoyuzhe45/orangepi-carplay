#ifndef CARPLAY_NOW_PLAYING_H
#define CARPLAY_NOW_PLAYING_H
#include <stddef.h>
#include <stdint.h>

#define CARPLAY_NOW_MAX 2048u
struct carplay_now_playing {
    uint64_t sampled_ms;
    uint32_t duration, position;
    uint16_t speed;
    uint8_t status, can_seek; /* status 255 means Android has no mappable state. */
    uint32_t media_generation,art_size,queue_index,queue_count;
    uint8_t queue_index_known,queue_count_known;
    uint64_t queue_version;
    uint8_t queue_available;
    char title[256], album[256], artist[256], composer[256];
    char app_name[256], package[256];
};
struct carplay_now_subscription { uint32_t media, playback; };
struct carplay_communications {
    uint64_t sampled_ms;
    uint8_t airplane, cellular, telephony, can_call, muted;
};
int carplay_comm_decode(const uint8_t *wire,size_t len,uint64_t now_ms,
                       struct carplay_communications *out);
int carplay_comm_encode(uint32_t fields,const struct carplay_communications *p,
                       uint8_t *out,size_t cap,size_t *len);
int carplay_comm_read(uint64_t now_ms,struct carplay_communications *out);
/* All parsers leave the destination unchanged on failure. */
int carplay_now_decode(const uint8_t *wire, size_t len, uint64_t now_ms,
                       struct carplay_now_playing *out);
int carplay_now_subscribe(struct carplay_now_subscription *out,
                         const uint8_t *body, size_t len);
int carplay_now_encode(const struct carplay_now_subscription *sub,
                      const struct carplay_now_playing *p,
                      uint8_t *out, size_t cap, size_t *len);
int carplay_now_read(uint64_t now_ms, struct carplay_now_playing *out);
int carplay_art_read(uint32_t generation,uint32_t total,uint32_t offset,uint8_t *out,size_t cap,size_t *len);
/* op 1 = media key, 2 = voice action, 3 = seek milliseconds, 6 = phone key.
 * 0 queued, -1 unavailable/rejected, -2 delivery uncertain (never replay).
 * Command completion belongs to Android; queued is not proof of app behavior. */
int carplay_app_command(uint32_t op, uint32_t argument);
#endif
