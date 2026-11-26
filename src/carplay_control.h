#ifndef CARPLAY_CONTROL_H
#define CARPLAY_CONTROL_H

#include "carplay_config.h"
#include "carplay_vehicle_state.h"

#include <stddef.h>
#include <stdint.h>

#define CARPLAY_CONTROL_DEFAULT_PORT 49152
#define CARPLAY_CONTROL_MAX_REQUEST 65536u

struct carplay_control_status {
    char state[32];
    uint64_t generation;
    int usb_bound;
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
    int touch;
    int keyboard;
    int media_audio;
    int microphone;
    int gps;
    int appearance_known;
    int night_mode;
    struct carplay_vehicle_snapshot vehicle;
    int audio_streams;
    int media_available;
    int microphone_available;
    int microphone_ready;
    int microphone_demand;
    uint64_t microphone_packets_received;
    int oem_icon_present;
    int request_car_ui_supported;
    int car_ui_ready;
    char display_error[192];
    char last_error[192];
};

typedef void (*carplay_control_config_changed)(const struct carplay_config *config,
                                                void *context);
/* 0 sent; 1 session/event channel not ready; 2 unsupported; -1 transport error. */
typedef int (*carplay_control_car_ui_requested)(void *context);

int carplay_control_start(int port, const struct carplay_config *config,
                          carplay_control_config_changed changed,
                          void *changed_context,
                          carplay_control_car_ui_requested car_ui_requested,
                          void *car_ui_context);
void carplay_control_stop(void);
void carplay_control_set_status(const struct carplay_control_status *status);
void carplay_control_get_status(struct carplay_control_status *status);

#endif
