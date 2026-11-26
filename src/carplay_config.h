#ifndef CARPLAY_CONFIG_H
#define CARPLAY_CONFIG_H

#include <stddef.h>
#include <stdint.h>

#define CARPLAY_CONFIG_PATH "/data/carplay/config.json"
#define CARPLAY_CONFIG_MAX_BYTES 65536u
#define CARPLAY_VIDEO_BITRATE_MAX 2000000

enum carplay_config_load_result {
    CARPLAY_CONFIG_LOADED = 0,
    CARPLAY_CONFIG_DEFAULTED_MISSING = 1,
    CARPLAY_CONFIG_DEFAULTED_INVALID = 2
};

struct carplay_config {
    int enabled;
    int boot_start;
    int display_width;
    int display_height;
    int display_density;
    int physical_width;
    int physical_height;
    int physical_fps;
    int fallback_width;
    int fallback_height;
    int video_bitrate;
    int video_fps;
    int media_audio;
    int microphone;
    int car_gps;
    int car_appearance;
    int reconnect_initial_ms;
    int reconnect_max_ms;
    int control_port;
    int log_level;
    uint64_t log_file_bytes;
    int log_retention_days;
    uint64_t log_total_bytes;
};

void carplay_config_defaults(struct carplay_config *config);
int carplay_config_validate(const struct carplay_config *config,
                            char *error, size_t error_capacity);
enum carplay_config_load_result carplay_config_load(
    const char *path, struct carplay_config *config,
    char *error, size_t error_capacity);
int carplay_config_save(const char *path, const struct carplay_config *config,
                        char *error, size_t error_capacity);
int carplay_config_parse_json(const char *json, size_t len,
                             struct carplay_config *config,
                             char *error, size_t error_capacity);
int carplay_config_to_json(const struct carplay_config *config,
                           char *out, size_t capacity, size_t *written);

#endif
