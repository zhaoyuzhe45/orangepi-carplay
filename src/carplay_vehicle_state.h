#ifndef CARPLAY_VEHICLE_STATE_H
#define CARPLAY_VEHICLE_STATE_H

#include <pthread.h>
#include <stddef.h>
#include <stdint.h>

#define CARPLAY_NMEA_CAPACITY 1024u
#define CARPLAY_POSITION_NMEA_CAPACITY 256u
#define CARPLAY_LOCATION_STALE_MS 5000u

struct carplay_vehicle_snapshot {
    uint64_t sampled_ms;
    uint64_t instance;
    uint64_t generation;
    uint64_t sequence;
    int active;
    int received;
    uint64_t age_ms;
    char rmc[CARPLAY_POSITION_NMEA_CAPACITY];
    char gga[CARPLAY_POSITION_NMEA_CAPACITY];
    uint64_t rmc_sequence, gga_sequence;
    uint64_t rmc_age_ms, gga_age_ms;
    int night_known, night_mode, night_applied;
    char night_error[128];
};

typedef int (*carplay_night_executor)(int night, void *context);

struct carplay_vehicle_state {
    pthread_rwlock_t lock;
    uint64_t session_generation;
    uint64_t night_generation;
    uint64_t location_generation;
    int night_known;
    int night_mode;
    char nmea[CARPLAY_NMEA_CAPACITY];
    size_t nmea_len;
    int active;
    uint64_t instance, location_sequence, location_updated_ms;
    char rmc[CARPLAY_POSITION_NMEA_CAPACITY];
    char gga[CARPLAY_POSITION_NMEA_CAPACITY];
    uint64_t rmc_sequence, gga_sequence, rmc_updated_ms, gga_updated_ms;
    uint64_t night_applied_generation;
    int night_applied_known, night_applied_mode;
    char night_error[128];
    pthread_t night_worker;
    int night_worker_started, night_worker_stop;
    carplay_night_executor night_executor;
    void *night_executor_context;
};

void carplay_vehicle_state_init(struct carplay_vehicle_state *state);
void carplay_vehicle_state_destroy(struct carplay_vehicle_state *state);
void carplay_vehicle_state_begin_session(struct carplay_vehicle_state *state,
                                         uint64_t generation);
void carplay_vehicle_state_end_session(struct carplay_vehicle_state *state,
                                      uint64_t generation);
void carplay_vehicle_state_clear_location(struct carplay_vehicle_state *state,
                                         uint64_t generation);
uint64_t carplay_vehicle_monotonic_ms(void);
void carplay_vehicle_state_snapshot(const struct carplay_vehicle_state *state,
                                   uint64_t now_ms,
                                   struct carplay_vehicle_snapshot *snapshot);
int carplay_vehicle_state_night_applied(struct carplay_vehicle_state *state,
                                      uint64_t generation, int night,
                                      const char *error);
int carplay_vehicle_state_request_night(struct carplay_vehicle_state *state,
                                       uint64_t generation, int night);
/* Optional platform adapter; configure before the first request. NULL selects
 * the Android cmd adapter. Useful to hosts without Android UI services. */
int carplay_vehicle_state_set_night_executor(struct carplay_vehicle_state *state,
                                            carplay_night_executor executor,
                                            void *context);
int carplay_vehicle_state_set_night(struct carplay_vehicle_state *state,
                                    uint64_t generation, int known,
                                    int night_mode);
int carplay_vehicle_state_get_night(const struct carplay_vehicle_state *state,
                                    int *known, int *night_mode);
int carplay_vehicle_state_set_nmea(struct carplay_vehicle_state *state,
                                   uint64_t generation,
                                   const char *nmea, size_t len);
int carplay_vehicle_state_get_nmea(const struct carplay_vehicle_state *state,
                                   char *out, size_t capacity,
                                   size_t *written);

/*
 * Builds the iPhone-to-accessory location subscription messages. The caller
 * passes the result to iap2_link_send_control().
 *  - 0xFFFA requests the established GPGGA, GPRMC and PASCD flags.
 *    interval_ms is retained for ABI compatibility; the standard flags do not
 *    carry a sampling interval.
 *  - 0xFFFC stops the subscription during teardown.
 */
size_t carplay_vehicle_state_encode_location_start(uint8_t *out,
                                                   size_t capacity,
                                                   uint16_t interval_ms);
size_t carplay_vehicle_state_encode_location_stop(uint8_t *out,
                                                  size_t capacity);
/*
 * Parses an accessory-to-iPhone 0xFFFB LocationInformation CSM body and stores
 * its NMEA parameter. A valid body without NMEA clears stale location data.
 * Returns 1 when stored, 0 for a valid empty update, and -1 on malformed input.
 */
int carplay_vehicle_state_decode_location_information(
    struct carplay_vehicle_state *state, uint64_t generation,
    const uint8_t *body, size_t body_len,
    char *error, size_t error_capacity);

#endif
