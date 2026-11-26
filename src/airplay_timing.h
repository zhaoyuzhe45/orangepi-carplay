#ifndef AIRPLAY_TIMING_H
#define AIRPLAY_TIMING_H

#include <stddef.h>
#include <stdint.h>

#define AIRPLAY_TIMING_PACKET_BYTES 32u
struct airplay_timing;

/* NTP 32.32 in host integer order; packets serialize it big endian. */
int airplay_timing_ntp_from_unix(int64_t seconds, long nanoseconds,
                               uint64_t *ntp);
int airplay_timing_now(uint64_t *ntp);
int airplay_timing_reply(const uint8_t *request, size_t request_len,
                         uint64_t received, uint64_t transmitted,
                         uint8_t reply[AIRPLAY_TIMING_PACKET_BYTES]);
/* Bind a UDP port to the connected TCP socket's local address and peer IP. */
int airplay_timing_start(struct airplay_timing **out, int control_fd);
uint16_t airplay_timing_port(const struct airplay_timing *timing);
int airplay_timing_error(const struct airplay_timing *timing);
void airplay_timing_stop(struct airplay_timing **timing);

#endif
