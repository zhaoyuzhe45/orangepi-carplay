#define _GNU_SOURCE
#include "carplay_control.h"
#include "carplay_log.h"

#include <arpa/inet.h>
#include <errno.h>
#include <netinet/in.h>
#include <poll.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>

#define HEADER_MAX 16384u
#define SEND_CHUNK 32768u

static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_t g_thread;
static int g_thread_started;
static _Atomic int g_stop;
static int g_listen_fd = -1;
static _Atomic int g_client_fd = -1;
static struct carplay_config g_config;
static struct carplay_control_status g_status;
static carplay_control_config_changed g_changed;
static void *g_changed_context;
static carplay_control_car_ui_requested g_car_ui_requested;
static void *g_car_ui_context;

static int send_all(int fd, const void *data, size_t len)
{
    const uint8_t *p = data;
    while (len) {
        ssize_t n = send(fd, p, len, MSG_NOSIGNAL);
        if (n > 0) {
            p += n;
            len -= (size_t)n;
            continue;
        }
        if (n < 0 && errno == EINTR)
            continue;
        return -1;
    }
    return 0;
}

static int send_response(int fd, int status, const char *reason,
                         const char *content_type, const char *body,
                         size_t body_len, const char *extra)
{
    char header[1024];
    int n = snprintf(header, sizeof(header),
        "HTTP/1.1 %d %s\r\n"
        "Content-Type: %s\r\n"
        "Content-Length: %zu\r\n"
        "Cache-Control: no-store\r\n"
        "X-Content-Type-Options: nosniff\r\n"
        "X-Frame-Options: DENY\r\n"
        "Connection: close\r\n"
        "%s\r\n", status, reason, content_type, body_len,
        extra ? extra : "");
    if (n < 0 || (size_t)n >= sizeof(header))
        return -1;
    return send_all(fd, header, (size_t)n) < 0 ||
           (body_len && send_all(fd, body, body_len) < 0) ? -1 : 0;
}

static void send_json_error(int fd, int status, const char *reason,
                            const char *message)
{
    char body[512];
    int n = snprintf(body, sizeof(body), "{\"error\":\"%s\"}\n", message);
    if (n > 0)
        (void)send_response(fd, status, reason,
                            "application/json; charset=utf-8", body,
                            (size_t)n, NULL);
}

static int read_request(int fd, char **request)
{
    size_t capacity = 4096;
    size_t used = 0;
    char *buffer = malloc(capacity + 1);
    time_t deadline = time(NULL) + 5;

    if (!buffer)
        return -1;
    for (;;) {
        ssize_t n;
        char *end;
        if (used == capacity) {
            char *grown;
            if (capacity >= CARPLAY_CONTROL_MAX_REQUEST)
                break;
            capacity *= 2;
            if (capacity > CARPLAY_CONTROL_MAX_REQUEST)
                capacity = CARPLAY_CONTROL_MAX_REQUEST;
            grown = realloc(buffer, capacity + 1);
            if (!grown)
                break;
            buffer = grown;
        }
        n = recv(fd, buffer + used, capacity - used, MSG_DONTWAIT);
        if (n > 0) {
            used += (size_t)n;
            buffer[used] = '\0';
            end = strstr(buffer, "\r\n\r\n");
            if (end) {
                size_t header_len = (size_t)(end - buffer) + 4u;
                size_t content_len = 0;
                char *cl = strcasestr(buffer, "content-length:");
                if (header_len > HEADER_MAX)
                    break;
                if (cl && cl < end)
                    content_len = (size_t)strtoul(cl + 15, NULL, 10);
                if (content_len > CARPLAY_CONTROL_MAX_REQUEST - header_len)
                    break;
                if (used >= header_len + content_len) {
                    buffer[header_len + content_len] = '\0';
                    *request = buffer;
                    return (int)(header_len + content_len);
                }
            }
            if (used > HEADER_MAX && !strstr(buffer, "\r\n\r\n"))
                break;
            continue;
        }
        if (n == 0)
            break;
        if (errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR)
            break;
        if (time(NULL) >= deadline)
            break;
        {
            struct pollfd pfd = { .fd = fd, .events = POLLIN };
            (void)poll(&pfd, 1, 100);
        }
    }
    free(buffer);
    return -1;
}

static void split_request(char *request, char **method, char **path,
                          char **body, size_t *body_len)
{
    char *line = strstr(request, "\r\n");
    *body = request;
    *body_len = 0;
    if (line) {
        char *next = line + 2;
        *line = '\0';
        if (strncmp(next, "\r\n", 2) == 0) {
            *body = next + 2;
        } else {
            char *blank = strstr(next, "\r\n\r\n");
            if (blank)
                *body = blank + 4;
        }
        *body_len = strlen(*body);
    }
    *method = request;
    *path = strchr(request, ' ');
    if (*path) {
        *(*path)++ = '\0';
        line = strchr(*path, ' ');
        if (line)
            *line = '\0';
    } else {
        *path = request + strlen(request);
    }
}

static int append_json_escaped(char **out, size_t *used, size_t *capacity,
                               const char *text, size_t len)
{
    size_t i;
    for (i = 0; i < len; i++) {
        unsigned char c = (unsigned char)text[i];
        const char *escaped = NULL;
        char unicode[8];
        if (c == '"')
            escaped = "\\\"";
        else if (c == '\\')
            escaped = "\\\\";
        else if (c == '\n')
            escaped = "\\n";
        else if (c == '\r')
            escaped = "\\r";
        else if (c == '\t')
            escaped = "\\t";
        else if (c < 0x20) {
            snprintf(unicode, sizeof(unicode), "\\u%04x", c);
            escaped = unicode;
        }
        if (escaped) {
            size_t n = strlen(escaped);
            if (*used + n + 1 > *capacity) {
                size_t new_capacity = *capacity * 2u + n + 1u;
                char *grown = realloc(*out, new_capacity);
                if (!grown)
                    return -1;
                *out = grown;
                *capacity = new_capacity;
            }
            memcpy(*out + *used, escaped, n);
            *used += n;
        } else {
            if (*used + 2 > *capacity) {
                size_t new_capacity = *capacity * 2u + 2u;
                char *grown = realloc(*out, new_capacity);
                if (!grown)
                    return -1;
                *out = grown;
                *capacity = new_capacity;
            }
            (*out)[(*used)++] = (char)c;
        }
    }
    if (*out)
        (*out)[*used] = '\0';
    return 0;
}

static void handle_status(int fd)
{
    struct carplay_control_status status;
    char body[8192];
    char *rmc = calloc(1, 1), *gga = calloc(1, 1), *night_error = calloc(1, 1);
    size_t used = 0, capacity = 1;
    uint64_t now = carplay_vehicle_monotonic_ms(), elapsed;
    int n;
    carplay_control_get_status(&status);
    elapsed = now >= status.vehicle.sampled_ms ? now - status.vehicle.sampled_ms : UINT64_MAX;
    /* Time is reported for diagnostics only. The Android poller receives the
     * last successful car snapshot until the car or session replaces it. A
     * disconnected session is the one non-time event that clears it. */
    if (!status.vehicle.active) {
        status.vehicle.received = 0;
        status.vehicle.rmc[0] = status.vehicle.gga[0] = 0;
    }
    if (status.vehicle.age_ms < CARPLAY_LOCATION_STALE_MS) status.vehicle.age_ms += elapsed < CARPLAY_LOCATION_STALE_MS ? elapsed : CARPLAY_LOCATION_STALE_MS;
    if (status.vehicle.rmc_age_ms < CARPLAY_LOCATION_STALE_MS) status.vehicle.rmc_age_ms += elapsed < CARPLAY_LOCATION_STALE_MS ? elapsed : CARPLAY_LOCATION_STALE_MS;
    if (status.vehicle.gga_age_ms < CARPLAY_LOCATION_STALE_MS) status.vehicle.gga_age_ms += elapsed < CARPLAY_LOCATION_STALE_MS ? elapsed : CARPLAY_LOCATION_STALE_MS;
    if (!rmc || !gga || !night_error ||
        append_json_escaped(&rmc, &used, &capacity, status.vehicle.rmc, strlen(status.vehicle.rmc)) < 0) goto unavailable;
    used = 0; capacity = 1;
    if (append_json_escaped(&gga, &used, &capacity, status.vehicle.gga, strlen(status.vehicle.gga)) < 0) goto unavailable;
    used = 0; capacity = 1;
    if (append_json_escaped(&night_error, &used, &capacity, status.vehicle.night_error, strlen(status.vehicle.night_error)) < 0) goto unavailable;
    n = snprintf(body, sizeof(body),
        "{\"state\":\"%s\",\"generation\":%llu,\"usbBound\":%s,"
        "\"display\":{\"width\":%d,\"height\":%d,"
        "\"logicalWidth\":%d,\"logicalHeight\":%d,"
        "\"requestedWidth\":%d,\"requestedHeight\":%d,"
        "\"physicalWidth\":%d,\"physicalHeight\":%d,"
        "\"density\":%d,\"requestedFps\":%d,\"actualFps\":%d,"
        "\"nativeSize\":%s,\"error\":\"%s\"},"
        "\"input\":{\"touch\":%s,\"keyboard\":%s},"
        "\"audio\":{\"media\":%s,\"microphone\":%s,\"streams\":%d,"
        "\"mediaAvailable\":%s,\"microphoneAvailable\":%s,"
        "\"microphoneReady\":%s,\"microphoneDemand\":%s,\"microphonePacketsReceived\":%llu},"
        "\"vehicle\":{\"gps\":%s,\"appearanceKnown\":%s,\"night\":%s,"
        "\"appearanceApplied\":%s,\"appearanceError\":\"%s\","
        "\"location\":{\"source\":\"car-iap2\",\"instance\":%llu,\"generation\":%llu,\"sequence\":%llu,"
        "\"active\":%s,\"received\":%s,\"ageMs\":%llu,"
        "\"rmc\":\"%s\",\"rmcSequence\":%llu,\"rmcAgeMs\":%llu,"
        "\"gga\":\"%s\",\"ggaSequence\":%llu,\"ggaAgeMs\":%llu},"
        "\"oemIcon\":%s,\"carUiSupported\":%s,\"carUiReady\":%s},"
        "\"lastError\":\"%s\"}\n",
        status.state, (unsigned long long)status.generation,
        status.usb_bound ? "true" : "false", status.display_width,
        status.display_height, status.display_width, status.display_height,
        status.requested_width, status.requested_height,
        status.physical_width, status.physical_height,
        status.display_density, status.requested_fps,
        status.actual_fps, status.native_size ? "true" : "false",
        status.display_error, status.touch ? "true" : "false",
        status.keyboard ? "true" : "false",
        status.media_audio ? "true" : "false",
        status.microphone ? "true" : "false", status.audio_streams,
        status.media_available ? "true" : "false",
        status.microphone_available ? "true" : "false",
        status.microphone_ready ? "true" : "false",
        status.microphone_demand ? "true" : "false",
        (unsigned long long)status.microphone_packets_received,
        status.gps ? "true" : "false",
        status.appearance_known ? "true" : "false",
        status.night_mode ? "true" : "false",
        status.vehicle.night_applied ? "true" : "false", night_error,
        (unsigned long long)status.vehicle.instance,
        (unsigned long long)status.vehicle.generation,
        (unsigned long long)status.vehicle.sequence,
        status.vehicle.active ? "true" : "false",
        status.vehicle.received ? "true" : "false",
        (unsigned long long)(status.vehicle.age_ms < CARPLAY_LOCATION_STALE_MS ? status.vehicle.age_ms : CARPLAY_LOCATION_STALE_MS),
        rmc, (unsigned long long)status.vehicle.rmc_sequence,
        (unsigned long long)(status.vehicle.rmc_age_ms < CARPLAY_LOCATION_STALE_MS ? status.vehicle.rmc_age_ms : CARPLAY_LOCATION_STALE_MS),
        gga, (unsigned long long)status.vehicle.gga_sequence,
        (unsigned long long)(status.vehicle.gga_age_ms < CARPLAY_LOCATION_STALE_MS ? status.vehicle.gga_age_ms : CARPLAY_LOCATION_STALE_MS),
        status.oem_icon_present ? "true" : "false",
        status.request_car_ui_supported ? "true" : "false",
        status.car_ui_ready ? "true" : "false", status.last_error);
    if (n > 0 && (size_t)n < sizeof(body))
        (void)send_response(fd, 200, "OK",
                            "application/json; charset=utf-8", body,
                            (size_t)n, NULL);
    else send_json_error(fd, 500, "Internal Server Error", "vehicle status too large");
    free(rmc); free(gga); free(night_error);
    return;
unavailable:
    free(rmc); free(gga); free(night_error);
    send_json_error(fd, 500, "Internal Server Error", "vehicle status unavailable");
}

static void handle_config_get(int fd)
{
    char body[8192];
    size_t len = 0;
    if (carplay_config_to_json(&g_config, body, sizeof(body), &len) < 0) {
        send_json_error(fd, 500, "Internal Server Error",
                        "configuration unavailable");
        return;
    }
    (void)send_response(fd, 200, "OK", "application/json; charset=utf-8",
                        body, len, NULL);
}

static void handle_config_put(int fd, const char *body, size_t body_len,
                              const char *content_type)
{
    struct carplay_config candidate;
    char error[192];
    if (!content_type ||
        strncasecmp(content_type, "application/json", 16) != 0) {
        send_json_error(fd, 415, "Unsupported Media Type",
                        "Content-Type must be application/json");
        return;
    }
    if (!body_len) {
        send_json_error(fd, 400, "Bad Request", "empty configuration");
        return;
    }
    if (carplay_config_parse_json(body, body_len, &candidate, error,
                                  sizeof(error)) < 0 ||
        carplay_config_save(CARPLAY_CONFIG_PATH, &candidate, error,
                            sizeof(error)) < 0) {
        char response[320];
        snprintf(response, sizeof(response), "{\"error\":\"%s\"}\n", error);
        (void)send_response(fd, 400, "Bad Request",
                            "application/json; charset=utf-8", response,
                            strlen(response), NULL);
        return;
    }
    pthread_mutex_lock(&g_lock);
    g_config = candidate;
    pthread_mutex_unlock(&g_lock);
    (void)send_response(fd, 200, "OK", "application/json; charset=utf-8",
                        "{\"saved\":true,\"restartRequired\":true}\n",
                        strlen("{\"saved\":true,\"restartRequired\":true}\n"),
                        NULL);
    /* Deliver the acknowledgement before the supervisor-facing restart hook
     * can stop the control service and close this request. */
    if (g_changed)
        g_changed(&candidate, g_changed_context);
}

static void handle_logs(int fd)
{
    char *tail = NULL;
    char *escaped = malloc(16384u);
    size_t escaped_capacity = 16384u;
    size_t escaped_used = 0;
    size_t tail_len = 0;
    size_t used;
    struct carplay_log_metrics metrics;
    int prefix;

    memset(&metrics, 0, sizeof(metrics));
    (void)carplay_log_get_metrics(&metrics);
    if (!escaped || carplay_log_read_tail(131072u, &tail, &tail_len) < 0) {
        free(tail);
        free(escaped);
        send_json_error(fd, 503, "Service Unavailable", "logs unavailable");
        return;
    }
    if (append_json_escaped(&escaped, &escaped_used, &escaped_capacity,
                            tail, tail_len) < 0) {
        free(tail);
        free(escaped);
        send_json_error(fd, 500, "Internal Server Error", "out of memory");
        return;
    }
    free(tail);
    prefix = snprintf(escaped + escaped_used,
                      escaped_capacity - escaped_used,
                      "\",\"activeBytes\":%llu,\"totalBytes\":%llu,"
                      "\"rotatedFiles\":%llu}",
                      (unsigned long long)metrics.active_bytes,
                      (unsigned long long)metrics.total_bytes,
                      (unsigned long long)metrics.rotated_files);
    if (prefix < 0 || (size_t)prefix >= escaped_capacity - escaped_used) {
        free(escaped);
        send_json_error(fd, 500, "Internal Server Error", "log too large");
        return;
    }
    used = escaped_used + (size_t)prefix;
    {
        char header[] = "{\"text\":\"";
        size_t header_len = strlen(header);
        char *json = malloc(header_len + used + 1u);
        if (!json) {
            free(escaped);
            send_json_error(fd, 500, "Internal Server Error", "out of memory");
            return;
        }
        memcpy(json, header, header_len);
        memcpy(json + header_len, escaped, used);
        json[header_len + used] = '\0';
        (void)send_response(fd, 200, "OK",
                            "application/json; charset=utf-8", json,
                            header_len + used, NULL);
        free(json);
    }
    free(escaped);
}

static void handle_export(int fd)
{
    char *text = NULL;
    size_t len = 0;
    if (carplay_log_export_text(262144u, &text, &len) < 0) {
        send_json_error(fd, 503, "Service Unavailable", "logs unavailable");
        return;
    }
    (void)send_response(fd, 200, "OK", "text/plain; charset=utf-8",
                        text, len,
                        "Content-Disposition: attachment; filename=carplay-log.txt\r\n");
    free(text);
}

static void handle_car_ui(int fd)
{
    int result = g_car_ui_requested ?
        g_car_ui_requested(g_car_ui_context) : 1;
    if (result == 0) {
        (void)send_response(fd, 200, "OK",
            "application/json; charset=utf-8",
            "{\"sent\":true,\"carPlayActive\":true}\n",
            strlen("{\"sent\":true,\"carPlayActive\":true}\n"), NULL);
    } else if (result == 2) {
        send_json_error(fd, 409, "Conflict",
            "the connected car did not advertise enhancedRequestCarUI");
    } else if (result == 1) {
        send_json_error(fd, 409, "Conflict",
            "CarPlay is not active or its event channel is not ready");
    } else {
        send_json_error(fd, 502, "Bad Gateway",
            "requestUI could not be sent to the car");
    }
}

static void handle_client(int fd)
{
    char *request = NULL;
    char *method;
    char *path;
    char *body;
    size_t body_len;
    char content_type_storage[128];
    char *content_type = NULL;
    int rc = read_request(fd, &request);
    if (rc < 0 || !request) {
        send_json_error(fd, 400, "Bad Request", "request too large or malformed");
        free(request);
        return;
    }
    {
        char *ct = strcasestr(request, "content-type:");
        if (ct) {
            size_t length;
            ct += 13;
            while (*ct == ' ' || *ct == '\t') ct++;
            {
                char *line = strstr(ct, "\r\n");
                length = line ? (size_t)(line - ct) : strlen(ct);
            }
            if (length >= sizeof(content_type_storage))
                length = sizeof(content_type_storage) - 1u;
            memcpy(content_type_storage, ct, length);
            content_type_storage[length] = '\0';
            content_type = content_type_storage;
        }
    }
    split_request(request, &method, &path, &body, &body_len);
    if (strcmp(path, "/api/status") == 0 && strcmp(method, "GET") == 0)
        handle_status(fd);
    else if (strcmp(path, "/api/config") == 0 && strcmp(method, "GET") == 0)
        handle_config_get(fd);
    else if (strcmp(path, "/api/config") == 0 && strcmp(method, "PUT") == 0)
        handle_config_put(fd, body, body_len, content_type);
    else if (strcmp(path, "/api/logs") == 0 && strcmp(method, "GET") == 0)
        handle_logs(fd);
    else if (strcmp(path, "/api/logs/export") == 0 && strcmp(method, "POST") == 0)
        handle_export(fd);
    else if (strcmp(path, "/api/car-ui") == 0 && strcmp(method, "POST") == 0)
        handle_car_ui(fd);
    else
        send_json_error(fd, 404, "Not Found", "not found");
    free(request);
}

static void *control_main(void *unused)
{
    (void)unused;
    while (!atomic_load(&g_stop)) {
        struct pollfd pfd = { .fd = g_listen_fd, .events = POLLIN };
        int ready = poll(&pfd, 1, 250);
        if (atomic_load(&g_stop)) break;
        if (ready <= 0 || !(pfd.revents & POLLIN)) continue;
        {
            int fd = accept4(g_listen_fd, NULL, NULL, SOCK_CLOEXEC);
            if (fd < 0) continue;
            atomic_store(&g_client_fd, fd);
            {
                struct timeval timeout = { .tv_sec = 5, .tv_usec = 0 };
                (void)setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
                (void)setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout));
            }
            handle_client(fd);
            shutdown(fd, SHUT_RDWR);
            close(fd);
            atomic_store(&g_client_fd, -1);
        }
    }
    return NULL;
}

int carplay_control_start(int port, const struct carplay_config *config,
                          carplay_control_config_changed changed,
                          void *changed_context,
                          carplay_control_car_ui_requested car_ui_requested,
                          void *car_ui_context)
{
    struct sockaddr_in address;
    int fd;
    int enabled = 1;
    if (!config || port < 1024 || port > 65535)
        return -1;
    pthread_mutex_lock(&g_lock);
    if (g_thread_started) {
        pthread_mutex_unlock(&g_lock);
        return 0;
    }
    g_config = *config;
    g_changed = changed;
    g_changed_context = changed_context;
    g_car_ui_requested = car_ui_requested;
    g_car_ui_context = car_ui_context;
    memset(&g_status, 0, sizeof(g_status));
    snprintf(g_status.state, sizeof(g_status.state), "starting");
    g_status.display_width = config->fallback_width;
    g_status.display_height = config->fallback_height;
    g_status.display_density = config->display_density;
    g_status.requested_fps = config->video_fps;
    g_status.media_audio = config->media_audio;
    g_status.microphone = config->microphone;
    g_status.gps = config->car_gps;
    pthread_mutex_unlock(&g_lock);

    fd = socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (fd < 0) return -1;
    (void)setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &enabled, sizeof(enabled));
    memset(&address, 0, sizeof(address));
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    address.sin_port = htons((uint16_t)port);
    if (bind(fd, (const struct sockaddr *)&address, sizeof(address)) < 0 ||
        listen(fd, 8) < 0) {
        close(fd);
        return -1;
    }
    g_listen_fd = fd;
    atomic_store(&g_stop, 0);
    if (pthread_create(&g_thread, NULL, control_main, NULL) != 0) {
        close(fd); g_listen_fd = -1; return -1;
    }
    g_thread_started = 1;
    return 0;
}

void carplay_control_stop(void)
{
    if (!g_thread_started)
        return;
    atomic_store(&g_stop, 1);
    if (g_listen_fd >= 0)
        shutdown(g_listen_fd, SHUT_RDWR);
    {
        int client_fd = atomic_load(&g_client_fd);
        if (client_fd >= 0)
            shutdown(client_fd, SHUT_RDWR);
    }
    (void)pthread_join(g_thread, NULL);
    if (g_listen_fd >= 0)
        close(g_listen_fd);
    g_listen_fd = -1;
    g_thread_started = 0;
}

void carplay_control_set_status(const struct carplay_control_status *status)
{
    if (!status) return;
    pthread_mutex_lock(&g_lock);
    g_status = *status;
    g_status.state[sizeof(g_status.state) - 1] = '\0';
    g_status.last_error[sizeof(g_status.last_error) - 1] = '\0';
    pthread_mutex_unlock(&g_lock);
}

void carplay_control_get_status(struct carplay_control_status *status)
{
    if (!status) return;
    pthread_mutex_lock(&g_lock);
    *status = g_status;
    pthread_mutex_unlock(&g_lock);
}
