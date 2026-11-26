#define _GNU_SOURCE
#include "carplay_config.h"

#include <errno.h>
#include <fcntl.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static void set_error(char *error, size_t capacity, const char *message)
{
    if (error && capacity)
        snprintf(error, capacity, "%s", message);
}

void carplay_config_defaults(struct carplay_config *config)
{
    if (!config)
        return;
    memset(config, 0, sizeof(*config));
    config->enabled = 1;
    config->boot_start = 1;
    config->display_width = 0;
    config->display_height = 0;
    config->display_density = 0;
    config->physical_width = 0;
    config->physical_height = 0;
    config->physical_fps = 0;
    config->fallback_width = 800;
    config->fallback_height = 480;
    config->video_bitrate = CARPLAY_VIDEO_BITRATE_MAX;
    config->video_fps = 30;
    config->media_audio = 1;
    config->microphone = 1;
    config->car_gps = 1;
    config->car_appearance = 1;
    config->reconnect_initial_ms = 1000;
    config->reconnect_max_ms = 30000;
    config->control_port = 49152;
    config->log_level = 1;
    config->log_file_bytes = 8u * 1024u * 1024u;
    config->log_retention_days = 7;
    config->log_total_bytes = 256u * 1024u * 1024u;
}

static int physical_mode_is_builtin(int width, int height, int fps)
{
    static const int modes[][2] = {
        { 800, 480 }, { 1024, 480 }, { 1280, 600 },
        { 1280, 720 }, { 1920, 900 }, { 1920, 1080 }
    };
    size_t i;

    if (fps != 60)
        return 0;
    for (i = 0; i < sizeof(modes) / sizeof(modes[0]); i++) {
        if (modes[i][0] == width && modes[i][1] == height)
            return 1;
    }
    return 0;
}

int carplay_config_validate(const struct carplay_config *config,
                            char *error, size_t error_capacity)
{
    if (!config) {
        set_error(error, error_capacity, "configuration is missing");
        return -1;
    }
    if (config->enabled < 0 || config->enabled > 1 ||
        config->boot_start < 0 || config->boot_start > 1 ||
        config->media_audio < 0 || config->media_audio > 1 ||
        config->microphone < 0 || config->microphone > 1 ||
        config->car_gps < 0 || config->car_gps > 1 ||
        config->car_appearance < 0 || config->car_appearance > 1) {
        set_error(error, error_capacity, "boolean setting is out of range");
        return -1;
    }
    if (config->display_width < 0 || config->display_height < 0 ||
        (config->display_width == 0) != (config->display_height == 0) ||
        (config->display_width != 0 &&
         ((config->display_width & 1) || (config->display_height & 1) ||
          config->display_width < 320 || config->display_height < 240 ||
          config->display_width > 7680 || config->display_height > 4320))) {
        set_error(error, error_capacity, "display override is invalid");
        return -1;
    }
    if (config->display_density != 0 &&
        (config->display_density < 120 || config->display_density > 480)) {
        set_error(error, error_capacity, "display density is invalid");
        return -1;
    }
    if ((config->physical_width == 0) != (config->physical_height == 0) ||
        (config->physical_width == 0) != (config->physical_fps == 0) ||
        (config->physical_width != 0 &&
         !physical_mode_is_builtin(config->physical_width,
                                  config->physical_height,
                                  config->physical_fps))) {
        set_error(error, error_capacity, "physical display mode is invalid");
        return -1;
    }
    if (config->fallback_width < 320 || config->fallback_height < 240 ||
        config->fallback_width > 7680 || config->fallback_height > 4320) {
        set_error(error, error_capacity, "fallback resolution is invalid");
        return -1;
    }
    if (config->video_bitrate < 100000 ||
        config->video_bitrate > CARPLAY_VIDEO_BITRATE_MAX ||
        config->video_fps < 1 || config->video_fps > 60) {
        set_error(error, error_capacity, "video settings are out of range");
        return -1;
    }
    if (config->reconnect_initial_ms < 100 ||
        config->reconnect_max_ms < config->reconnect_initial_ms ||
        config->reconnect_max_ms > 300000) {
        set_error(error, error_capacity, "reconnect delay is invalid");
        return -1;
    }
    if (config->control_port < 1024 || config->control_port > 65535) {
        set_error(error, error_capacity, "control port is invalid");
        return -1;
    }
    if (config->log_level < 0 || config->log_level > 3) {
        set_error(error, error_capacity, "log level is invalid");
        return -1;
    }
    if (config->log_file_bytes < 16384u ||
        config->log_file_bytes > UINT64_C(1073741824) ||
        config->log_retention_days < 1 ||
        config->log_retention_days > 3650 ||
        config->log_total_bytes < config->log_file_bytes ||
        config->log_total_bytes > UINT64_C(4294967296)) {
        set_error(error, error_capacity, "log policy is invalid");
        return -1;
    }
    return 0;
}

static const char *find_value(const char *json, const char *key)
{
    char pattern[96];
    const char *p;

    if (snprintf(pattern, sizeof(pattern), "\"%s\"", key) >=
        (int)sizeof(pattern))
        return NULL;
    p = strstr(json, pattern);
    if (!p)
        return NULL;
    p += strlen(pattern);
    while (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n')
        p++;
    if (*p++ != ':')
        return NULL;
    while (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n')
        p++;
    return p;
}

static int parse_physical_mode(const char *json, struct carplay_config *config)
{
    const char *p;
    char mode[64];
    char tail;
    int offset = 0;
    int width;
    int height;
    int fps;

    p = find_value(json, "physicalMode");
    if (!p)
        return 0;
    if (sscanf(p, " \"%63[^\"]\"%n", mode, &offset) != 1 || offset <= 0)
        return -1;
    p += offset;
    while (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n')
        p++;
    if (*p != ',' && *p != '}')
        return -1;
    if (strcmp(mode, "auto") == 0) {
        config->physical_width = 0;
        config->physical_height = 0;
        config->physical_fps = 0;
        return 1;
    }
    if (sscanf(mode, "%dx%d@%d%c", &width, &height, &fps, &tail) != 3)
        return -1;
    config->physical_width = width;
    config->physical_height = height;
    config->physical_fps = fps;
    return 1;
}

static int parse_bool(const char *json, const char *key, int *value)
{
    const char *p = find_value(json, key);
    if (!p)
        return 0;
    if (strncmp(p, "true", 4) == 0) {
        *value = 1;
        return 1;
    }
    if (strncmp(p, "false", 5) == 0) {
        *value = 0;
        return 1;
    }
    return -1;
}

static int parse_int(const char *json, const char *key, int *value)
{
    char *end;
    long parsed;
    const char *p = find_value(json, key);
    if (!p)
        return 0;
    errno = 0;
    parsed = strtol(p, &end, 10);
    if (errno || end == p || parsed < INT32_MIN || parsed > INT32_MAX)
        return -1;
    *value = (int)parsed;
    return 1;
}

static int parse_u64(const char *json, const char *key, uint64_t *value)
{
    char *end;
    unsigned long long parsed;
    const char *p = find_value(json, key);
    if (!p)
        return 0;
    errno = 0;
    parsed = strtoull(p, &end, 10);
    if (errno || end == p)
        return -1;
    *value = (uint64_t)parsed;
    return 1;
}

static int read_file(const char *path, char **out)
{
    struct stat st;
    char *buffer;
    ssize_t n;
    int fd;

    *out = NULL;
    fd = open(path, O_RDONLY | O_CLOEXEC);
    if (fd < 0)
        return errno == ENOENT ? 1 : -1;
    if (fstat(fd, &st) < 0 || st.st_size < 0 ||
        st.st_size > (off_t)CARPLAY_CONFIG_MAX_BYTES) {
        close(fd);
        return -1;
    }
    buffer = malloc((size_t)st.st_size + 1);
    if (!buffer) {
        close(fd);
        return -1;
    }
    n = read(fd, buffer, (size_t)st.st_size);
    close(fd);
    if (n != st.st_size) {
        free(buffer);
        return -1;
    }
    buffer[n] = '\0';
    *out = buffer;
    return 0;
}

enum carplay_config_load_result carplay_config_load(
    const char *path, struct carplay_config *config,
    char *error, size_t error_capacity)
{
    char *json = NULL;
    int rc;
    int invalid = 0;
    char local_error[160] = { 0 };

    if (!path || !config) {
        set_error(error, error_capacity, "invalid configuration arguments");
        carplay_config_defaults(config);
        return CARPLAY_CONFIG_DEFAULTED_INVALID;
    }
    carplay_config_defaults(config);
    rc = read_file(path, &json);
    if (rc == 1) {
        set_error(error, error_capacity, "configuration missing; defaults used");
        return CARPLAY_CONFIG_DEFAULTED_MISSING;
    }
    if (rc < 0) {
        set_error(error, error_capacity, "configuration unreadable; defaults used");
        return CARPLAY_CONFIG_DEFAULTED_INVALID;
    }

#define PARSE_INT_FIELD(field, key) do { \
    int parse_rc = parse_int(json, key, &config->field); \
    if (parse_rc < 0) invalid = 1; \
} while (0)
#define PARSE_BOOL_FIELD(field, key) do { \
    int parse_rc = parse_bool(json, key, &config->field); \
    if (parse_rc < 0) invalid = 1; \
} while (0)
#define PARSE_U64_FIELD(field, key) do { \
    int parse_rc = parse_u64(json, key, &config->field); \
    if (parse_rc < 0) invalid = 1; \
} while (0)
    PARSE_BOOL_FIELD(enabled, "enabled");
    PARSE_BOOL_FIELD(boot_start, "bootStart");
    PARSE_INT_FIELD(display_width, "displayWidth");
    PARSE_INT_FIELD(display_height, "displayHeight");
    if (!find_value(json, "displayWidth"))
        PARSE_INT_FIELD(display_width, "preferredWidth");
    if (!find_value(json, "displayHeight"))
        PARSE_INT_FIELD(display_height, "preferredHeight");
    PARSE_INT_FIELD(display_density, "displayDensity");
    if (parse_physical_mode(json, config) < 0) invalid = 1;
    PARSE_INT_FIELD(fallback_width, "fallbackWidth");
    PARSE_INT_FIELD(fallback_height, "fallbackHeight");
    PARSE_INT_FIELD(video_bitrate, "videoBitrate");
    PARSE_INT_FIELD(video_fps, "videoFps");
    PARSE_BOOL_FIELD(media_audio, "mediaAudio");
    PARSE_BOOL_FIELD(microphone, "microphone");
    PARSE_BOOL_FIELD(car_gps, "carGps");
    PARSE_BOOL_FIELD(car_appearance, "carAppearance");
    PARSE_INT_FIELD(reconnect_initial_ms, "reconnectInitialMs");
    PARSE_INT_FIELD(reconnect_max_ms, "reconnectMaxMs");
    PARSE_INT_FIELD(control_port, "controlPort");
    PARSE_INT_FIELD(log_level, "logLevel");
    PARSE_U64_FIELD(log_file_bytes, "logFileBytes");
    PARSE_INT_FIELD(log_retention_days, "logRetentionDays");
    PARSE_U64_FIELD(log_total_bytes, "logTotalBytes");
#undef PARSE_INT_FIELD
#undef PARSE_BOOL_FIELD
#undef PARSE_U64_FIELD
    free(json);
    if (invalid) {
        carplay_config_defaults(config);
        set_error(error, error_capacity, "configuration has invalid values; defaults used");
        return CARPLAY_CONFIG_DEFAULTED_INVALID;
    }
    if (config->video_bitrate > CARPLAY_VIDEO_BITRATE_MAX)
        config->video_bitrate = CARPLAY_VIDEO_BITRATE_MAX;
    if (carplay_config_validate(config, local_error, sizeof(local_error)) < 0) {
        carplay_config_defaults(config);
        set_error(error, error_capacity, local_error);
        return CARPLAY_CONFIG_DEFAULTED_INVALID;
    }
    if (error && error_capacity)
        error[0] = '\0';
    return CARPLAY_CONFIG_LOADED;
}

int carplay_config_parse_json(const char *json, size_t len,
                             struct carplay_config *config,
                             char *error, size_t error_capacity)
{
    char *copy;
    char local_error[160] = { 0 };
    int invalid = 0;

    if (!json || !len || len > CARPLAY_CONFIG_MAX_BYTES || !config) {
        set_error(error, error_capacity, "invalid configuration payload");
        return -1;
    }
    copy = malloc(len + 1);
    if (!copy) {
        set_error(error, error_capacity, "out of memory");
        return -1;
    }
    memcpy(copy, json, len);
    copy[len] = '\0';
    carplay_config_defaults(config);
#define PARSE_INT_FIELD(field, key) do {     int parse_rc = parse_int(copy, key, &config->field);     if (parse_rc < 0) invalid = 1; } while (0)
#define PARSE_BOOL_FIELD(field, key) do {     int parse_rc = parse_bool(copy, key, &config->field);     if (parse_rc < 0) invalid = 1; } while (0)
#define PARSE_U64_FIELD(field, key) do {     int parse_rc = parse_u64(copy, key, &config->field);     if (parse_rc < 0) invalid = 1; } while (0)
    PARSE_BOOL_FIELD(enabled, "enabled");
    PARSE_BOOL_FIELD(boot_start, "bootStart");
    PARSE_INT_FIELD(display_width, "displayWidth");
    PARSE_INT_FIELD(display_height, "displayHeight");
    PARSE_INT_FIELD(display_density, "displayDensity");
    if (parse_physical_mode(copy, config) < 0) invalid = 1;
    if (!find_value(copy, "displayWidth"))
        PARSE_INT_FIELD(display_width, "preferredWidth");
    if (!find_value(copy, "displayHeight"))
        PARSE_INT_FIELD(display_height, "preferredHeight");
    PARSE_INT_FIELD(fallback_width, "fallbackWidth");
    PARSE_INT_FIELD(fallback_height, "fallbackHeight");
    PARSE_INT_FIELD(video_bitrate, "videoBitrate");
    PARSE_INT_FIELD(video_fps, "videoFps");
    PARSE_BOOL_FIELD(media_audio, "mediaAudio");
    PARSE_BOOL_FIELD(microphone, "microphone");
    PARSE_BOOL_FIELD(car_gps, "carGps");
    PARSE_BOOL_FIELD(car_appearance, "carAppearance");
    PARSE_INT_FIELD(reconnect_initial_ms, "reconnectInitialMs");
    PARSE_INT_FIELD(reconnect_max_ms, "reconnectMaxMs");
    PARSE_INT_FIELD(control_port, "controlPort");
    PARSE_INT_FIELD(log_level, "logLevel");
    PARSE_U64_FIELD(log_file_bytes, "logFileBytes");
    PARSE_INT_FIELD(log_retention_days, "logRetentionDays");
    PARSE_U64_FIELD(log_total_bytes, "logTotalBytes");
#undef PARSE_INT_FIELD
#undef PARSE_BOOL_FIELD
#undef PARSE_U64_FIELD
    free(copy);
    if (invalid) {
        set_error(error, error_capacity, "configuration has invalid types");
        return -1;
    }
    if (config->video_bitrate > CARPLAY_VIDEO_BITRATE_MAX)
        config->video_bitrate = CARPLAY_VIDEO_BITRATE_MAX;
    if (carplay_config_validate(config, local_error, sizeof(local_error)) < 0) {
        set_error(error, error_capacity, local_error);
        return -1;
    }
    if (error && error_capacity)
        error[0] = '\0';
    return 0;
}

int carplay_config_to_json(const struct carplay_config *config,
                           char *out, size_t capacity, size_t *written)
{
    struct carplay_config bounded;
    int n;
    char physical[40];

    if (!config || !out || !capacity)
        return -1;
    bounded = *config;
    if (bounded.video_bitrate > CARPLAY_VIDEO_BITRATE_MAX)
        bounded.video_bitrate = CARPLAY_VIDEO_BITRATE_MAX;
    config = &bounded;
    if (carplay_config_validate(config, NULL, 0) < 0)
        return -1;
    if (config->physical_width) {
        n = snprintf(physical, sizeof(physical), "%dx%d@%d",
                     config->physical_width, config->physical_height,
                     config->physical_fps);
        if (n < 0 || (size_t)n >= sizeof(physical))
            return -1;
    } else {
        snprintf(physical, sizeof(physical), "auto");
    }
    n = snprintf(out, capacity,
        "{\n"
        "  \"enabled\": %s,\n"
        "  \"bootStart\": %s,\n"
        "  \"displayWidth\": %d,\n"
        "  \"displayHeight\": %d,\n"
        "  \"displayDensity\": %d,\n"
        "  \"physicalMode\": \"%s\",\n"
        "  \"fallbackWidth\": %d,\n"
        "  \"fallbackHeight\": %d,\n"
        "  \"videoBitrate\": %d,\n"
        "  \"videoFps\": %d,\n"
        "  \"mediaAudio\": %s,\n"
        "  \"microphone\": %s,\n"
        "  \"carGps\": %s,\n"
        "  \"carAppearance\": %s,\n"
        "  \"reconnectInitialMs\": %d,\n"
        "  \"reconnectMaxMs\": %d,\n"
        "  \"controlPort\": %d,\n"
        "  \"logLevel\": %d,\n"
        "  \"logFileBytes\": %llu,\n"
        "  \"logRetentionDays\": %d,\n"
        "  \"logTotalBytes\": %llu\n"
        "}\n",
        config->enabled ? "true" : "false",
        config->boot_start ? "true" : "false",
        config->display_width, config->display_height,
        config->display_density, physical,
        config->fallback_width, config->fallback_height,
        config->video_bitrate, config->video_fps,
        config->media_audio ? "true" : "false",
        config->microphone ? "true" : "false",
        config->car_gps ? "true" : "false",
        config->car_appearance ? "true" : "false",
        config->reconnect_initial_ms, config->reconnect_max_ms,
        config->control_port, config->log_level,
        (unsigned long long)config->log_file_bytes,
        config->log_retention_days,
        (unsigned long long)config->log_total_bytes);
    if (n < 0 || (size_t)n >= capacity)
        return -1;
    if (written)
        *written = (size_t)n;
    return 0;
}

int carplay_config_save(const char *path, const struct carplay_config *config,
                        char *error, size_t error_capacity)
{
    char json[4096];
    char temp[1200];
    size_t len = 0;
    int fd;
    size_t offset = 0;
    char directory[1024];
    char *slash;

    if (!path || carplay_config_to_json(config, json, sizeof(json), &len) < 0) {
        set_error(error, error_capacity, "configuration is invalid");
        return -1;
    }
    snprintf(temp, sizeof(temp), "%s.tmp.%ld", path, (long)getpid());
    snprintf(directory, sizeof(directory), "%s", path);
    slash = strrchr(directory, '/');
    if (slash) {
        *slash = '\0';
        if (mkdir(directory, 0700) < 0 && errno != EEXIST) {
            set_error(error, error_capacity, "cannot create configuration directory");
            return -1;
        }
    }
    fd = open(temp, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0600);
    if (fd < 0) {
        set_error(error, error_capacity, "cannot open temporary configuration");
        return -1;
    }
    while (offset < len) {
        ssize_t n = write(fd, json + offset, len - offset);
        if (n > 0) {
            offset += (size_t)n;
            continue;
        }
        if (n < 0 && errno == EINTR)
            continue;
        close(fd);
        unlink(temp);
        set_error(error, error_capacity, "cannot write configuration");
        return -1;
    }
    if (fsync(fd) < 0 || close(fd) < 0 || rename(temp, path) < 0) {
        unlink(temp);
        set_error(error, error_capacity, "cannot commit configuration");
        return -1;
    }
    (void)fsync(open(directory, O_RDONLY | O_DIRECTORY | O_CLOEXEC));
    if (error && error_capacity)
        error[0] = '\0';
    return 0;
}
