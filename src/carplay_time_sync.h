#ifndef CARPLAY_TIME_SYNC_H
#define CARPLAY_TIME_SYNC_H

#include <stddef.h>
#include <stdint.h>

struct carplay_nmea_time {
    int64_t epoch_seconds;
    char sentence[8];
};

/*
 * Extracts an absolute UTC time from a bounded buffer containing one or more
 * checksum-valid NMEA RMC/ZDA sentences.
 *
 * Returns 1 when time was found, 0 when no usable absolute time is present,
 * and -1 when a time-bearing sentence is malformed or fails validation.
 */
int carplay_time_sync_parse_nmea(const char *nmea, size_t len,
                                 struct carplay_nmea_time *result,
                                 char *error, size_t error_capacity);

/* Checks only the supported date range; RTC drift never rejects car time. */
int carplay_time_sync_jump_allowed(int64_t current_epoch_seconds,
                                    int64_t candidate_epoch_seconds);

/* Converts iAP2 DeviceTimeUpdate seconds since 2001-01-01T00:00:00Z. */
int carplay_time_sync_from_apple_epoch(uint64_t seconds,
                                       int64_t *utc_epoch_seconds);

/*
 * Sets CLOCK_REALTIME when it differs from utc_epoch_seconds by at least two
 * seconds. Returns 1 after an update, 0 when already aligned, and -1 on an
 * invalid value or clock_settime()/verification failure.
 */
int carplay_time_sync_apply_epoch(int64_t utc_epoch_seconds,
                                  char *detail, size_t detail_capacity);

/* Convenience wrapper used by the iAP2 LocationInformation receive path. */
int carplay_time_sync_apply_nmea(const char *nmea, size_t len,
                                 char *detail, size_t detail_capacity);

#endif /* CARPLAY_TIME_SYNC_H */
