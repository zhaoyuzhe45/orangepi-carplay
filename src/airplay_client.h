#ifndef AIRPLAY_CLIENT_H
#define AIRPLAY_CLIENT_H

#include <stddef.h>
#include <stdint.h>
#include "airplay_crypto.h"

struct airplay_client;
enum airplay_profile {
    AIRPLAY_PROFILE_HAP = 0,
    AIRPLAY_PROFILE_LEGACY = 1
};
/* Select only before start. iAP2/4301 keeps the HAP default. */
void airplay_client_set_profile(struct airplay_client *, enum airplay_profile);
enum airplay_profile airplay_client_profile(const struct airplay_client *);
int airplay_client_wants_hap_fallback(const struct airplay_client *);
/* Drive deadlines and liveness from the control/event poll thread. */
int airplay_client_tick(struct airplay_client *);
struct carplay_session_capabilities;
struct carplay_display_plan;
struct carplay_vehicle_state;
struct carplay_audio_status;
void airplay_client_get_audio_status(const struct airplay_client *client,
                                     struct carplay_audio_status *status);
struct airplay_runtime_status {
    int display_width;
    int display_height;
    int requested_width;
    int requested_height;
    int physical_width;
    int physical_height;
    int display_density;
    int requested_fps;
    int actual_fps;
    int native_size;
    char display_error[192];
};
struct airplay_client *airplay_client_new(int fd);
struct airplay_client *airplay_client_new_with_audio(int fd, int enable_media,
                                                    int enable_microphone);
void airplay_client_set_vehicle_state(struct airplay_client *client,
                                    struct carplay_vehicle_state *state,
                                    uint64_t generation);
void airplay_client_set_video_options(struct airplay_client *client,
                                     int display_width, int display_height,
                                     int physical_width, int physical_height,
                                     int density, int configured_fps,
                                     int bitrate);
void airplay_client_get_runtime_status(
    const struct airplay_client *client,
    struct airplay_runtime_status *status);
void airplay_client_free(struct airplay_client *client);
/* Snapshot before free; bridge owns preparation after all sockets are closed. */
int airplay_client_get_display_preparation(const struct airplay_client *client,
                                          struct carplay_display_plan *plan);
int airplay_client_start(struct airplay_client *client);
int airplay_client_pairing_proven(const struct airplay_client *client);
/*
 * 1 once a handshake step was answered with a non-200 status.  Nothing further
 * will ever be sent on that session -- the ladder only advances from the
 * successful arm of the previous step -- so the caller must tear the session
 * down rather than keep polling.  airplay_client_failure() names the step, so
 * the log says "SETUP (session) refused (status=470)" instead of a bare CSeq.
 */
int airplay_client_failed(const struct airplay_client *client);
const char *airplay_client_failure(const struct airplay_client *client);
const struct carplay_session_capabilities *airplay_client_capabilities(
    const struct airplay_client *client);
int airplay_client_setup_complete(const struct airplay_client *client);
uint16_t airplay_client_event_port(const struct airplay_client *client);
int airplay_client_attach_event(struct airplay_client *client, int fd);
#define AIRPLAY_CAR_UI_SENT 0
#define AIRPLAY_CAR_UI_NOT_READY 1
#define AIRPLAY_CAR_UI_UNSUPPORTED 2
int airplay_client_car_ui_available(const struct airplay_client *client);
/* Sends requestUI on the existing event channel; never tears down the session. */
int airplay_client_request_car_ui(struct airplay_client *client);
/* Returns 1 while ownership/initial modes acknowledgement defers the SETUP. */
int airplay_client_send_screen_setup(struct airplay_client *client);
int airplay_client_screen_setup_complete(const struct airplay_client *client);
uint16_t airplay_client_screen_data_port(const struct airplay_client *client);
int airplay_client_attach_screen_data(struct airplay_client *client, int fd);
/* Starts negotiated LPCM streams, or retains host for a borrowed resource.
 * A deferred HAP audio-only SETUP resumes after release without resetting keys.
 */
int airplay_client_start_audio(struct airplay_client *client, const char *host);
int airplay_client_send_record(struct airplay_client *client);
int airplay_client_record_accepted(const struct airplay_client *client);
int airplay_client_audio_stream_count(const struct airplay_client *client);
int airplay_client_send_test_screen(struct airplay_client *client);
int airplay_client_start_live_screen(struct airplay_client *client);
void airplay_client_feed_event(struct airplay_client *client,
                               const uint8_t *data, size_t len);

void airplay_client_feed(struct airplay_client *client,
                         const uint8_t *data, size_t len);

#endif
