#define _GNU_SOURCE
#include "carplay_config.h"
#include "carplay_capabilities.h"
#include "carplay_log.h"
#include "carplay_time_sync.h"
#include "carplay_vehicle_state.h"
#include "usb_vendor.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <utime.h>
#include <unistd.h>

static int failures;

static void remove_tree(const char *path);

#define EXPECT(condition, message) do { \
    if (!(condition)) { \
        fprintf(stderr, "FAIL: %s (%s:%d)\n", message, __FILE__, __LINE__); \
        failures++; \
    } \
} while (0)

static int make_test_dir(char *path, size_t capacity)
{
    int n = snprintf(path, capacity, "/data/local/tmp/carplay-support-%ld",
                     (long)getpid());
    if (n < 0 || (size_t)n >= capacity)
        return -1;
    remove_tree(path);
    if (mkdir(path, 0700) < 0 && errno != EEXIST)
        return -1;
    return 0;
}

static void remove_tree(const char *path)
{
    int removed;

    /* readdir may skip entries that are unlinked underneath it, so repeat until
     * a complete scan finds no remaining file. */
    do {
        DIR *dir = opendir(path);
        struct dirent *entry;
        removed = 0;
        if (!dir)
            return;
        while ((entry = readdir(dir)) != NULL) {
            char file[1200];
            if (strcmp(entry->d_name, ".") == 0 ||
                strcmp(entry->d_name, "..") == 0)
                continue;
            snprintf(file, sizeof(file), "%s/%s", path, entry->d_name);
            if (unlink(file) == 0)
                removed++;
        }
        closedir(dir);
    } while (removed);
    rmdir(path);
}

static void test_config(void)
{
    char dir[512];
    char path[700];
    char error[256];
    char json[4096];
    struct carplay_config saved;
    struct carplay_config loaded;
    int saved_fd;

    EXPECT(make_test_dir(dir, sizeof(dir)) == 0, "config test directory");
    snprintf(path, sizeof(path), "%s/config.json", dir);
    carplay_config_defaults(&saved);
    EXPECT(saved.video_bitrate == 2000000,
           "default video bitrate is capped at 2 Mbps");
    EXPECT(saved.log_file_bytes == 8u * 1024u * 1024u,
           "default log file limit is 8 MiB");
    EXPECT(saved.log_retention_days == 7, "default retention is seven days");
    EXPECT(saved.log_total_bytes == 256u * 1024u * 1024u,
           "default log quota is 256 MiB");
    saved.fallback_width = 800;
    saved.fallback_height = 480;
    saved.display_width = 0;
    saved.display_height = 0;
    saved.display_density = 160;
    saved.physical_width = 800;
    saved.physical_height = 480;
    saved.physical_fps = 60;
    saved.control_port = 49200;
    EXPECT(carplay_config_to_json(&saved, json, sizeof(json), NULL) == 0,
           "configuration encodes");
    EXPECT(carplay_config_save(path, &saved, error, sizeof(error)) == 0,
           "configuration saves atomically");
    EXPECT(carplay_config_load(path, &loaded, error, sizeof(error)) ==
               CARPLAY_CONFIG_LOADED,
           "saved configuration loads");
    EXPECT(loaded.control_port == 49200 && loaded.fallback_width == 800 &&
               loaded.fallback_height == 480 && loaded.display_density == 160 &&
               loaded.physical_width == 800 && loaded.physical_height == 480 &&
               loaded.physical_fps == 60,
           "configuration values round-trip");
    EXPECT(strstr(json, "\"physicalMode\": \"800x480@60\"") != NULL,
           "physical mode is encoded in human-readable form");
    saved.physical_width = 0;
    saved.physical_height = 0;
    saved.physical_fps = 0;
    EXPECT(carplay_config_save(path, &saved, error, sizeof(error)) == 0 &&
           carplay_config_load(path, &loaded, error, sizeof(error)) ==
               CARPLAY_CONFIG_LOADED && loaded.physical_width == 0 &&
           loaded.physical_height == 0 && loaded.physical_fps == 0,
           "automatic physical mode round-trips");
    saved_fd = open(path, O_WRONLY | O_TRUNC);
    if (saved_fd >= 0) {
        const char legacy[] = "{\"preferredWidth\":800,\"preferredHeight\":480}";
        (void)write(saved_fd, legacy, sizeof(legacy) - 1u);
        close(saved_fd);
    }
    EXPECT(carplay_config_load(path, &loaded, error, sizeof(error)) ==
               CARPLAY_CONFIG_LOADED &&
               loaded.display_width == 800 && loaded.display_height == 480,
           "legacy preferred size maps to display override");
    saved_fd = open(path, O_WRONLY | O_TRUNC);
    if (saved_fd >= 0) {
        (void)write(saved_fd, "{\"enabled\":false,\"videoFps\":0}", 32);
        close(saved_fd);
    }
    EXPECT(carplay_config_load(path, &loaded, error, sizeof(error)) ==
               CARPLAY_CONFIG_DEFAULTED_INVALID,
           "invalid configuration falls back explicitly");
    EXPECT(loaded.enabled == 1 && loaded.video_fps == 30,
           "invalid configuration restores safe defaults");
    remove_tree(dir);
}

static void test_display_plan(void)
{
    struct carplay_session_capabilities caps;
    struct carplay_display_plan plan;

    memset(&caps, 0, sizeof(caps));
    caps.display.present = 1;
    caps.display.width_pixels = 1920;
    caps.display.height_pixels = 1080;
    caps.display.max_fps = 60;
    EXPECT(carplay_capabilities_display_plan(&caps, 800, 480, 0, 0, 0, 0, 0, 30,
                                             &plan) == 0,
           "display plan accepts valid capabilities");
    EXPECT(plan.width == 1920 && plan.height == 1080 &&
               plan.fps == 30 && plan.physical_fps == 60 &&
               plan.force_physical && plan.density == 280,
           "1080p defaults to 280 dpi, 30 fps stream and 60 Hz physical mode");
    caps.display.width_pixels = 800;
    caps.display.height_pixels = 480;
    caps.display.max_fps = 30;
    EXPECT(carplay_capabilities_display_plan(&caps, 1920, 1080, 0, 0, 0, 0, 0, 30,
                                             &plan) == 0 &&
               plan.width == 800 && plan.height == 480 &&
               plan.density == 160,
           "800x480 defaults to 160 dpi");
    EXPECT(carplay_capabilities_display_plan(&caps, 1920, 1080, 0, 0, 0, 0, 240, 30,
                                             &plan) == 0 && plan.density == 240,
           "manual dpi overrides automatic mapping");
    caps.display.max_fps = 24;
    EXPECT(carplay_capabilities_display_plan(&caps, 1920, 1080, 0, 0, 0, 0, 0, 30,
                                             &plan) == 0 && plan.fps == 24,
           "negotiated maximum limits configured fps");
    caps.display.max_fps = 0;
    EXPECT(carplay_capabilities_display_plan(&caps, 1920, 1080, 0, 0, 0, 0, 0, 30,
                                             &plan) == 0 && plan.fps == 30,
           "missing negotiated fps falls back to configured fps");
    caps.display.present = 0;
    EXPECT(carplay_capabilities_display_plan(&caps, 1280, 720, 0, 0, 0, 0, 0, 30,
                                             &plan) == 0 && !plan.negotiated &&
               plan.width == 1280 && plan.height == 720 && plan.density == 240,
           "fallback dimensions retain automatic dpi mapping");
    caps.display.present = 1;
    caps.display.width_pixels = 801;
    caps.display.height_pixels = 481;
    EXPECT(carplay_capabilities_display_plan(&caps, 800, 480, 0, 0, 0, 0, 0, 30,
                                             &plan) == 0 && plan.width == 800 &&
               plan.height == 480,
           "display output dimensions are even");
    EXPECT(carplay_capabilities_display_plan(&caps, 800, 480, 0, 0, 0, 0, 119, 30,
                                             &plan) < 0,
           "invalid dpi override is rejected");
}

static void test_capture_size(void)
{
    unsigned width = 0;
    unsigned height = 0;

    EXPECT(carplay_capabilities_capture_size(
               2310, 1080, 1920, 1088, 16, 8, &width, &height) == 0 &&
               width == 1920 && height == 896,
           "wide display is fitted to the AVC encoder limit");
    EXPECT(carplay_capabilities_capture_size(
               1080, 2310, 1920, 1088, 16, 8, &width, &height) == 0 &&
               width == 512 && height == 1088,
           "portrait display preserves aspect ratio within AVC limits");
    EXPECT(carplay_capabilities_capture_size(
               800, 480, 1920, 1088, 16, 8, &width, &height) == 0 &&
               width == 800 && height == 480,
           "compatible display keeps its native capture size");
    EXPECT(carplay_capabilities_capture_size(
               800, 480, 1920, 1088, 15, 8, &width, &height) < 0,
           "invalid encoder alignment is rejected");
}

static void test_vehicle_state(void)
{
    struct carplay_vehicle_state state;
    uint8_t start[128];
    uint8_t stop[16];
    uint8_t response[160];
    size_t start_len;
    size_t stop_len;
    size_t off = 0;
    const char *nmea =
        "$GPGGA,120000.00,2233.1000,N,11356.2000,E,1,08,1.0,0.0,M,0.0,M,,*00\r\n";
    size_t nmea_len = strlen(nmea);
    int known = -1;
    int night = -1;
    char stored[128];
    size_t stored_len = 0;

    carplay_vehicle_state_init(&state);
    carplay_vehicle_state_begin_session(&state, 77);
    EXPECT(carplay_vehicle_state_set_night(&state, 77, 1, 1) == 1,
           "first night update changes state");
    EXPECT(carplay_vehicle_state_set_night(&state, 77, 1, 1) == 0,
           "duplicate night update is idempotent");
    EXPECT(carplay_vehicle_state_get_night(&state, &known, &night) == 0 &&
               known == 1 && night == 1,
           "night state persists");
    carplay_vehicle_state_begin_session(&state, 78);
    EXPECT(carplay_vehicle_state_get_night(&state, &known, &night) == 0 &&
               known == 0,
           "new session waits for a fresh car appearance report");

    start_len = carplay_vehicle_state_encode_location_start(
        start, sizeof(start), 1000);
    EXPECT(start_len == 18 && start[2] == 0 && start[3] == 18 && start[4] == 0xff && start[5] == 0xfa,
           "location start is a 0xFFFA request");
    EXPECT(start_len == 18 && start[6] == 0x00 && start[7] == 0x04 &&
               start[8] == 0x00 && start[9] == 0x01 &&
               start[12] == 0x00 && start[13] == 0x02,
           "location start requests NMEA components");
    response[off++] = 0x00; response[off++] = 0x04 + (uint8_t)(nmea_len + 1);
    response[off++] = 0x00; response[off++] = 0x00;
    memcpy(response + off, nmea, nmea_len + 1);
    off += nmea_len + 1;
    EXPECT(carplay_vehicle_state_decode_location_information(
               &state, 78, response, off, NULL, 0) == 1,
           "0xFFFB location information decodes");
    EXPECT(carplay_vehicle_state_get_nmea(&state, stored, sizeof(stored),
                                          &stored_len) == 0 &&
               stored_len == nmea_len && strstr(stored, "$GPGGA") != NULL &&
               strstr(stored, "$GPRMC") == NULL,
           "location commits the requested NMEA sentence");
    stop_len = carplay_vehicle_state_encode_location_stop(stop, sizeof(stop));
    EXPECT(stop_len == 6 && stop[4] == 0xff && stop[5] == 0xfc,
           "location stop is a 0xFFFC request");
    carplay_vehicle_state_destroy(&state);
}

static void test_time_sync(void)
{
    struct carplay_nmea_time parsed;
    char error[192];
    const char *rmc =
        "$GPRMC,123519,A,4807.038,N,01131.000,E,022.4,084.4,221026,003.1,W*60\r\n";
    const char *zda =
        "$GPZDA,201530.00,22,10,2026,00,00*64\r\n";
    const char *combined =
        "$GPGGA,123519.00,2233.1000,N,11356.2000,E,1,08,1.0,0.0,M,0.0,M,,*5B\r\n"
        "$GPRMC,123519,A,2233.1000,N,11356.2000,E,0.0,,221026,,*38\r\n";
    struct timespec now;

    const char *receiver_epoch = "$GPRMC,000000.00,A,4807.038,N,01131.000,E,0.0,0.0,010170,,,A*5B";
    const char *different_time = "$GPRMC,000001.00,A,4807.038,N,01131.000,E,0.0,0.0,010170,,,A*5A";
    EXPECT(carplay_time_sync_parse_nmea(receiver_epoch, strlen(receiver_epoch), &parsed,
                                        error, sizeof(error)) == 0,
           "receiver Unix epoch placeholder must not set CLOCK_REALTIME to 2070");
    EXPECT(carplay_time_sync_parse_nmea(different_time, strlen(different_time), &parsed,
                                        error, sizeof(error)) == 1,
           "a real time outside the placeholder is still accepted without a jump limit");
    EXPECT(carplay_time_sync_parse_nmea(rmc, strlen(rmc), &parsed,
                                        error, sizeof(error)) == 1 &&
               parsed.epoch_seconds == 1792672519LL &&
               strcmp(parsed.sentence, "RMC") == 0,
           "RMC date and UTC time convert to Unix time");
    EXPECT(carplay_time_sync_parse_nmea(zda, strlen(zda), &parsed,
                                        error, sizeof(error)) == 1 &&
               parsed.epoch_seconds == 1792700130LL &&
               strcmp(parsed.sentence, "ZDA") == 0,
           "ZDA date and UTC time convert to Unix time");
    EXPECT(carplay_time_sync_parse_nmea(combined, strlen(combined), &parsed,
                                        error, sizeof(error)) == 1 &&
               parsed.epoch_seconds == 1792672519LL,
           "absolute time is found after unrelated NMEA sentences");
    EXPECT(carplay_time_sync_parse_nmea(
               "$GPRMC,123519,A,4807.038,N,01131.000,E,022.4,084.4,221026,003.1,W*00\r\n",
               74, &parsed, error, sizeof(error)) < 0,
           "bad NMEA checksum cannot change the clock");
    EXPECT(carplay_time_sync_parse_nmea(
               "$GPRMC,123519,V,4807.038,N,01131.000,E,022.4,084.4,221026,003.1,W*77\r\n",
               74, &parsed, error, sizeof(error)) == 0,
           "RMC without an active fix is ignored");
    EXPECT(carplay_time_sync_parse_nmea(
               "$GPGGA,123519.00,2233.1000,N,11356.2000,E,1,08,1.0,0.0,M,0.0,M,,*00\r\n",
               73, &parsed, error, sizeof(error)) == 0,
           "GGA time without a date is not sufficient");
    EXPECT(carplay_time_sync_from_apple_epoch(814365319u,
                                              &parsed.epoch_seconds) == 0 &&
               parsed.epoch_seconds == 1792672519LL,
           "iAP2 Apple reference time converts to Unix time");
    EXPECT(carplay_time_sync_apply_epoch(1, error, sizeof(error)) < 0,
           "implausible Unix time is rejected");
    EXPECT(carplay_time_sync_jump_allowed(1794950000LL,
                                           1795036400LL) == 1,
           "a sub-day RTC correction is accepted");
    EXPECT(carplay_time_sync_jump_allowed(1794950000LL,
                                           1795554800LL) == 1,
           "a seven-day RTC correction is accepted without an age limit");
    EXPECT(carplay_time_sync_jump_allowed(1794950000LL,
                                           3155760000LL) == 1,
           "a multi-decade correction inside the supported date range is accepted");
    EXPECT(carplay_time_sync_jump_allowed(946684800LL,
                                           1794950000LL) == 1,
           "an uninitialised RTC accepts the first plausible time");
    EXPECT(clock_gettime(CLOCK_REALTIME, &now) == 0 &&
               carplay_time_sync_apply_epoch((int64_t)now.tv_sec,
                                             error, sizeof(error)) == 0,
           "a current clock value is accepted without a needless update");
}

struct writer_args {
    int id;
};

static void *log_writer(void *opaque)
{
    struct writer_args *args = opaque;
    int i;
    for (i = 0; i < 80; i++) {
        char message[128];
        snprintf(message, sizeof(message),
                 "writer=%d sequence=%d payload=abcdefghijklmnopqrstuvwxyz",
                 args->id, i);
        carplay_log_write(CARPLAY_LOG_INFO, message);
    }
    return NULL;
}

static void test_logs(void)
{
    char dir[512];
    char stale[700];
    char path[700];
    struct carplay_log_policy policy;
    struct carplay_log_metrics metrics;
    pthread_t threads[4];
    struct writer_args args[4];
    char *tail = NULL;
    size_t tail_len = 0;
    int i;
    int fd;
    DIR *scan;
    struct dirent *entry;

    EXPECT(make_test_dir(dir, sizeof(dir)) == 0, "log test directory");
    snprintf(stale, sizeof(stale), "%s/carplay-20200101-000000-1-1.log", dir);
    fd = open(stale, O_WRONLY | O_CREAT | O_TRUNC, 0600);
    if (fd >= 0) {
        (void)write(fd, "expired\n", 8);
        close(fd);
        {
            struct utimbuf times;
            times.actime = time(NULL) - 10 * 86400;
            times.modtime = times.actime;
            EXPECT(utime(stale, &times) == 0,
                   "expired log timestamp can be prepared");
        }
    }
    memset(&policy, 0, sizeof(policy));
    policy.directory = dir;
    policy.file_bytes = 16384;
    policy.retention_days = 7;
    policy.total_bytes = 32768;
    policy.minimum_level = CARPLAY_LOG_DEBUG;
    EXPECT(carplay_log_init(&policy) == 0, "log system initializes");
    EXPECT(access(stale, F_OK) < 0, "expired logs are removed");
    for (i = 0; i < 4; i++) {
        args[i].id = i;
        EXPECT(pthread_create(&threads[i], NULL, log_writer, &args[i]) == 0,
               "log writer thread starts");
    }
    for (i = 0; i < 4; i++)
        (void)pthread_join(threads[i], NULL);
    EXPECT(carplay_log_get_metrics(&metrics) == 0, "log metrics available");
    EXPECT(metrics.rotated_files > 0, "log rotates by size");
    EXPECT(metrics.total_bytes <= policy.total_bytes,
           "log total quota is enforced");
    EXPECT(carplay_log_read_tail(8192, &tail, &tail_len) == 0,
           "active log tail is readable");
    EXPECT(tail_len > 0 && tail != NULL, "log tail has content");
    free(tail);
    scan = opendir(dir);
    if (scan) {
        while ((entry = readdir(scan)) != NULL) {
            struct stat st;
            if (strcmp(entry->d_name, ".") == 0 ||
                strcmp(entry->d_name, "..") == 0)
                continue;
            snprintf(path, sizeof(path), "%s/%s", dir, entry->d_name);
            if (stat(path, &st) == 0)
                EXPECT((uint64_t)st.st_size <= policy.file_bytes,
                       "individual log stays under its file limit");
        }
        closedir(scan);
    }
    carplay_log_shutdown();
    remove_tree(dir);
}

static uint64_t directory_size(const char *directory)
{
    DIR *dir = opendir(directory);
    struct dirent *entry;
    uint64_t total = 0;

    if (!dir)
        return 0;
    while ((entry = readdir(dir)) != NULL) {
        char path[1200];
        struct stat st;
        if (strcmp(entry->d_name, ".") == 0 ||
            strcmp(entry->d_name, "..") == 0)
            continue;
        snprintf(path, sizeof(path), "%s/%s", directory, entry->d_name);
        if (stat(path, &st) == 0 && S_ISREG(st.st_mode))
            total += (uint64_t)st.st_size;
    }
    closedir(dir);
    return total;
}

static int write_fill(const char *path, size_t bytes)
{
    char block[1024];
    int fd;
    size_t remaining = bytes;

    fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0600);
    if (fd < 0)
        return -1;
    memset(block, 'x', sizeof(block));
    while (remaining > 0) {
        size_t chunk = remaining < sizeof(block) ? remaining : sizeof(block);
        ssize_t written = write(fd, block, chunk);
        if (written <= 0) {
            close(fd);
            return -1;
        }
        remaining -= (size_t)written;
    }
    return close(fd);
}

static void test_log_downsize_and_unbounded_scan(void)
{
    char dir[512];
    char path[700];
    struct carplay_log_policy policy;
    struct carplay_log_metrics metrics;
    DIR *scan;
    struct dirent *entry;
    int i;

    EXPECT(make_test_dir(dir, sizeof(dir)) == 0,
           "log downsize test directory");
    snprintf(path, sizeof(path), "%s/carplay-active.log", dir);
    EXPECT(write_fill(path, 40000) == 0, "legacy active log is prepared");
    snprintf(path, sizeof(path), "%s/carplay-20261022-000000-1-0.log", dir);
    EXPECT(write_fill(path, 20000) == 0,
           "legacy oversized rotated log is prepared");
    memset(&policy, 0, sizeof(policy));
    policy.directory = dir;
    policy.file_bytes = 16384;
    policy.retention_days = 7;
    policy.total_bytes = 1048576;
    policy.minimum_level = CARPLAY_LOG_DEBUG;
    EXPECT(carplay_log_init(&policy) == 0,
           "log system starts after file limit is reduced");
    scan = opendir(dir);
    if (scan) {
        while ((entry = readdir(scan)) != NULL) {
            struct stat st;
            if (strcmp(entry->d_name, ".") == 0 ||
                strcmp(entry->d_name, "..") == 0)
                continue;
            snprintf(path, sizeof(path), "%s/%s", dir, entry->d_name);
            if (stat(path, &st) == 0)
                EXPECT((uint64_t)st.st_size <= policy.file_bytes,
                       "reduced file limit splits the legacy active log");
        }
        closedir(scan);
    }
    carplay_log_shutdown();
    remove_tree(dir);

    EXPECT(make_test_dir(dir, sizeof(dir)) == 0,
           "many rotated log test directory");
    for (i = 0; i < 600; i++) {
        snprintf(path, sizeof(path), "%s/carplay-20261022-%06d-%d-0.log",
                 dir, i, i + 1);
        EXPECT(write_fill(path, 100) == 0, "rotated log is prepared");
    }
    policy.total_bytes = 32768;
    EXPECT(carplay_log_init(&policy) == 0,
           "log system scans a large rotation history");
    EXPECT(directory_size(dir) <= policy.total_bytes,
           "total quota covers every rotated log file");
    EXPECT(carplay_log_get_metrics(&metrics) == 0 &&
               metrics.total_bytes <= policy.total_bytes,
           "reported total quota covers every rotated log file");
    carplay_log_shutdown();
    remove_tree(dir);
}

static void test_usb_vendor_requests(void)
{
    struct usb_vendor_request request;
    uint8_t response[8];
    int len;

    /* 0xC0/0x53 is the first thing a real head unit asks: without four bytes
     * of 0x00000001 it decides the accessory is not CarPlay capable and never
     * opens iAP2. */
    memset(&request, 0, sizeof(request));
    request.bmRequestType = 0xC0;
    request.bRequest = 0x53;
    request.wLength = 4;
    memset(response, 0xff, sizeof(response));
    len = usb_vendor_response(&request, response, sizeof(response));
    EXPECT(len == 4, "0x53 answers four bytes");
    EXPECT(response[0] == 0x01 && response[1] == 0x00 &&
               response[2] == 0x00 && response[3] == 0x00,
           "0x53 reports CarPlay support as 0x00000001");

    /* Too small a scratch buffer must fail rather than truncate the answer. */
    memset(response, 0xff, sizeof(response));
    EXPECT(usb_vendor_response(&request, response, 2) < 0,
           "0x53 refuses a buffer that cannot hold the answer");

    /* 0xC0/0x52 keeps its one-byte acknowledgement. */
    memset(&request, 0, sizeof(request));
    request.bmRequestType = 0xC0;
    request.bRequest = 0x52;
    request.wIndex = 0x0004;
    request.wLength = 1;
    EXPECT(usb_vendor_response(&request, response, sizeof(response)) == 1,
           "0x52 answers one byte");
    EXPECT(response[0] == 0x00, "0x52 acknowledges with 0x00");

    /* 0xC0/0x45 stays 3:3:3:0. */
    memset(&request, 0, sizeof(request));
    request.bmRequestType = 0xC0;
    request.bRequest = 0x45;
    request.wLength = 4;
    EXPECT(usb_vendor_response(&request, response, sizeof(response)) == 4,
           "0x45 answers four bytes");
    EXPECT(response[0] == 0x03 && response[1] == 0x03 &&
               response[2] == 0x03 && response[3] == 0x00,
           "0x45 reports the unswitched 3:3:3:0 mode");

    /*
     * 0x40/0x51 is *not* answered from the table any more: it carries a
     * 4-byte OUT data stage, and acknowledging it here would leave the
     * payload unread and the control pipe hung.  iap2d intercepts it in
     * handle_setup() and consumes the data stage itself.
     */
    memset(&request, 0, sizeof(request));
    request.bmRequestType = 0x40;
    request.bRequest = 0x51;
    request.wValue = 1;
    request.wLength = 4;
    EXPECT(usb_vendor_response(&request, response, sizeof(response)) < 0,
           "0x51 is left to handle_setup, not answered from the table");

    /* 0x40/0x40 has no data stage: handled, nothing to write. */

    memset(&request, 0, sizeof(request));
    request.bmRequestType = 0x40;
    request.bRequest = 0x40;
    request.wValue = 500;
    request.wIndex = 1600;
    EXPECT(usb_vendor_response(&request, response, sizeof(response)) == 0,
           "0x40 is acknowledged with no data stage");

    /* Anything else must be left to FunctionFS. */
    memset(&request, 0, sizeof(request));
    request.bmRequestType = 0xC0;
    request.bRequest = 0x7f;
    EXPECT(usb_vendor_response(&request, response, sizeof(response)) < 0,
           "an unknown vendor request is not answered");
    request.bmRequestType = 0x00;
    request.bRequest = 0x09;
    EXPECT(usb_vendor_response(&request, response, sizeof(response)) < 0,
           "a standard SET_CONFIGURATION is not answered");
    EXPECT(usb_vendor_response(NULL, response, sizeof(response)) < 0,
           "a null request is rejected");

    EXPECT(strcmp(usb_vendor_request_name(0x53), "carplay-support") == 0,
           "0x53 has a diagnostic name");
}

int main(void)
{
    test_config();
    test_display_plan();
    test_capture_size();
    test_vehicle_state();
    test_time_sync();
    test_usb_vendor_requests();
    test_logs();
    test_log_downsize_and_unbounded_scan();
    if (failures) {
        fprintf(stderr, "carplay support tests failed: %d\n", failures);
        return 1;
    }
    puts("carplay support tests passed");
    return 0;
}
