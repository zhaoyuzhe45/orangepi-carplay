#define _GNU_SOURCE
#include "carplay_vehicle_state.h"

#include <stdio.h>
#include <string.h>
#include <time.h>
#include <errno.h>
#ifdef __ANDROID__
#include <signal.h>
#include <sys/wait.h>
#include <unistd.h>
#endif

static void set_error(char *error, size_t capacity, const char *message)
{
    if (error && capacity)
        snprintf(error, capacity, "%s", message);
}

int carplay_vehicle_state_set_night_executor(struct carplay_vehicle_state *state,
                                            carplay_night_executor executor,
                                            void *context)
{
    if (!state) return -1;
    pthread_rwlock_wrlock(&state->lock);
    if (state->night_worker_started) { pthread_rwlock_unlock(&state->lock); return -1; }
    state->night_executor = executor;
    state->night_executor_context = context;
    pthread_rwlock_unlock(&state->lock);
    return 0;
}

static void night_sleep_ms(unsigned ms)
{
    struct timespec delay = {(time_t)(ms / 1000u), (long)(ms % 1000u) * 1000000L};
    while (nanosleep(&delay, &delay) < 0 && errno == EINTR) {}
}

static int android_night_execute(int night, void *context)
{
    (void)context;
#ifdef __ANDROID__
    pid_t pid = fork();
    uint64_t deadline = carplay_vehicle_monotonic_ms() + 2000;
    int status = 0;
    if (pid == 0) {
        execl("/system/bin/cmd", "cmd", "uimode", "night", night ? "yes" : "no", (char *)NULL);
        _exit(127);
    }
    if (pid < 0) return -1;
    for (;;) {
        pid_t result = waitpid(pid, &status, WNOHANG);
        if (result == pid) return WIFEXITED(status) && WEXITSTATUS(status) == 0 ? 0 : -1;
        if (result < 0 && errno != EINTR) return -1;
        if (carplay_vehicle_monotonic_ms() >= deadline) {
            (void)kill(pid, SIGKILL);
            while (waitpid(pid, &status, 0) < 0 && errno == EINTR) {}
            return -1;
        }
        night_sleep_ms(10);
    }
#else
    (void)night;
    return -1;
#endif
}

static void *night_worker_main(void *context)
{
    struct carplay_vehicle_state *state = context;
    uint64_t generation = 0, next_attempt = 0;
    int mode = -1;
    for (;;) {
        struct carplay_vehicle_snapshot snapshot;
        carplay_night_executor execute;
        void *execute_context;
        int stop;
        pthread_rwlock_rdlock(&state->lock);
        stop = state->night_worker_stop;
        execute = state->night_executor ? state->night_executor : android_night_execute;
        execute_context = state->night_executor_context;
        pthread_rwlock_unlock(&state->lock);
        if (stop) break;
        uint64_t now = carplay_vehicle_monotonic_ms();
        carplay_vehicle_state_snapshot(state, now, &snapshot);
        if (snapshot.generation != generation || snapshot.night_mode != mode) {
            generation = snapshot.generation; mode = snapshot.night_mode; next_attempt = 0;
        }
        if (snapshot.night_known && !snapshot.night_applied && now >= next_attempt) {
            /* An external command can supersede the last successful mode even
             * when desired state changes back while it is in flight. Invalidate
             * that historical success before execution, and recheck ownership
             * under the same lock before issuing the command. */
            int current;
            pthread_rwlock_wrlock(&state->lock);
            current = !state->night_worker_stop && state->active && state->night_known &&
                state->session_generation == snapshot.generation &&
                state->night_mode == snapshot.night_mode;
            if (current) state->night_applied_known = 0;
            pthread_rwlock_unlock(&state->lock);
            if (!current) continue;
            int result = execute(snapshot.night_mode, execute_context);
            (void)carplay_vehicle_state_night_applied(state, snapshot.generation, snapshot.night_mode,
                result == 0 ? "" : "Android uimode failed; retrying");
            next_attempt = carplay_vehicle_monotonic_ms() + 1000;
        }
        night_sleep_ms(100);
    }
    return NULL;
}

int carplay_vehicle_state_request_night(struct carplay_vehicle_state *state,
                                       uint64_t generation, int night)
{
    if (carplay_vehicle_state_set_night(state, generation, 1, night) < 0) return -1;
    pthread_rwlock_wrlock(&state->lock);
    if (!state->night_worker_started) {
        state->night_worker_stop = 0;
        if (pthread_create(&state->night_worker, NULL, night_worker_main, state) != 0) {
            snprintf(state->night_error, sizeof(state->night_error), "could not start Android UI worker");
            pthread_rwlock_unlock(&state->lock);
            return -1;
        }
        state->night_worker_started = 1;
    }
    pthread_rwlock_unlock(&state->lock);
    return 0;
}

static void put_u16_be(uint8_t *out, uint16_t value)
{
    out[0] = (uint8_t)(value >> 8);
    out[1] = (uint8_t)value;
}

static uint16_t get_u16_be(const uint8_t *in)
{
    return (uint16_t)(((uint16_t)in[0] << 8) | in[1]);
}

static size_t append_param(uint8_t *out, size_t capacity, size_t used,
                           uint16_t id, const uint8_t *payload,
                           size_t payload_len)
{
    size_t total = 4u + payload_len;

    if (total > UINT16_MAX || used > capacity || total > capacity - used)
        return 0;
    put_u16_be(out + used, (uint16_t)total);
    put_u16_be(out + used + 2, id);
    if (payload_len)
        memcpy(out + used + 4, payload, payload_len);
    return total;
}

static size_t begin_csm(uint8_t *out, size_t capacity, uint16_t message_id)
{
    if (!out || capacity < 6)
        return 0;
    out[0] = 0x40;
    out[1] = 0x40;
    put_u16_be(out + 4, message_id);
    return 6;
}

static void finish_csm(uint8_t *out, size_t used)
{
    put_u16_be(out + 2, (uint16_t)used);
}

uint64_t carplay_vehicle_monotonic_ms(void)
{
    struct timespec now;
    if (clock_gettime(CLOCK_MONOTONIC, &now) != 0) return 0;
    return (uint64_t)now.tv_sec * 1000u + (uint64_t)now.tv_nsec / 1000000u;
}

static void clear_location_locked(struct carplay_vehicle_state *state)
{
    state->nmea[0] = state->rmc[0] = state->gga[0] = 0;
    state->nmea_len = 0;
    state->location_updated_ms = state->rmc_updated_ms = state->gga_updated_ms = 0;
    state->rmc_sequence = state->gga_sequence = 0;
    state->location_sequence++;
}

void carplay_vehicle_state_init(struct carplay_vehicle_state *state)
{
    if (!state)
        return;
    memset(state, 0, sizeof(*state));
    pthread_rwlock_init(&state->lock, NULL);
    state->instance = carplay_vehicle_monotonic_ms();
}

void carplay_vehicle_state_destroy(struct carplay_vehicle_state *state)
{
    if (state) {
        int started;
        pthread_rwlock_wrlock(&state->lock);
        state->night_worker_stop = 1;
        started = state->night_worker_started;
        pthread_rwlock_unlock(&state->lock);
        if (started) pthread_join(state->night_worker, NULL);
        pthread_rwlock_destroy(&state->lock);
    }
}

void carplay_vehicle_state_begin_session(struct carplay_vehicle_state *state,
                                         uint64_t generation)
{
    if (!state)
        return;
    pthread_rwlock_wrlock(&state->lock);
    state->session_generation = generation;
    state->active = 1;
    state->night_known = 0;
    state->night_error[0] = 0;
    clear_location_locked(state);
    pthread_rwlock_unlock(&state->lock);
}

void carplay_vehicle_state_end_session(struct carplay_vehicle_state *state,
                                      uint64_t generation)
{
    if (!state) return;
    pthread_rwlock_wrlock(&state->lock);
    if (state->session_generation == generation) {
        state->active = 0;
        state->night_known = 0;
        clear_location_locked(state);
    }
    pthread_rwlock_unlock(&state->lock);
}

void carplay_vehicle_state_clear_location(struct carplay_vehicle_state *state,
                                         uint64_t generation)
{
    if (!state) return;
    pthread_rwlock_wrlock(&state->lock);
    if (state->session_generation == generation) clear_location_locked(state);
    pthread_rwlock_unlock(&state->lock);
}

static uint64_t sample_age(uint64_t now, uint64_t then)
{
    return then && now >= then ? now - then : UINT64_MAX;
}

void carplay_vehicle_state_snapshot(const struct carplay_vehicle_state *state,
                                   uint64_t now_ms,
                                   struct carplay_vehicle_snapshot *out)
{
    if (!out) return;
    memset(out, 0, sizeof(*out));
    out->sampled_ms = now_ms;
    if (!state) return;
    pthread_rwlock_rdlock((pthread_rwlock_t *)&state->lock);
    out->instance = state->instance;
    out->generation = state->session_generation;
    out->sequence = state->location_sequence;
    out->active = state->active;
    out->age_ms = sample_age(now_ms, state->location_updated_ms);
    /* Ages remain diagnostics only. A successfully received car snapshot is
     * kept until the car replaces it or the session is explicitly cleared. */
    out->received = state->active && state->nmea_len;
    out->rmc_age_ms = sample_age(now_ms, state->rmc_updated_ms);
    out->gga_age_ms = sample_age(now_ms, state->gga_updated_ms);
    if (state->active) {
        memcpy(out->rmc, state->rmc, sizeof(out->rmc));
        memcpy(out->gga, state->gga, sizeof(out->gga));
        out->rmc_sequence = state->rmc_sequence;
        out->gga_sequence = state->gga_sequence;
    }
    out->night_known = state->active && state->night_known;
    out->night_mode = state->night_mode;
    out->night_applied = out->night_known && state->night_applied_known &&
        state->night_applied_generation == state->session_generation &&
        state->night_applied_mode == state->night_mode;
    memcpy(out->night_error, state->night_error, sizeof(out->night_error));
    pthread_rwlock_unlock((pthread_rwlock_t *)&state->lock);
}

int carplay_vehicle_state_set_night(struct carplay_vehicle_state *state,
                                    uint64_t generation, int known,
                                    int night_mode)
{
    int changed;

    if (!state)
        return -1;
    pthread_rwlock_wrlock(&state->lock);
    if (!state->active || generation != state->session_generation) {
        pthread_rwlock_unlock(&state->lock);
        return -1;
    }
    changed = state->night_known != !!known ||
              (known && state->night_mode != !!night_mode);
    state->night_known = !!known;
    state->night_mode = known ? !!night_mode : 0;
    state->night_generation = generation;
    pthread_rwlock_unlock(&state->lock);
    return changed;
}

int carplay_vehicle_state_night_applied(struct carplay_vehicle_state *state,
                                      uint64_t generation, int night,
                                      const char *error)
{
    if (!state) return -1;
    pthread_rwlock_wrlock(&state->lock);
    if (!state->active || generation != state->session_generation ||
        !state->night_known || state->night_mode != !!night) {
        pthread_rwlock_unlock(&state->lock);
        return -1;
    }
    state->night_applied_known = !error || !*error;
    state->night_applied_generation = generation;
    state->night_applied_mode = !!night;
    snprintf(state->night_error, sizeof(state->night_error), "%s", error ? error : "");
    pthread_rwlock_unlock(&state->lock);
    return 0;
}

int carplay_vehicle_state_get_night(const struct carplay_vehicle_state *state,
                                    int *known, int *night_mode)
{
    if (!state)
        return -1;
    pthread_rwlock_rdlock((pthread_rwlock_t *)&state->lock);
    if (known)
        *known = state->night_known;
    if (night_mode)
        *night_mode = state->night_mode;
    pthread_rwlock_unlock((pthread_rwlock_t *)&state->lock);
    return 0;
}

int carplay_vehicle_state_set_nmea(struct carplay_vehicle_state *state,
                                   uint64_t generation,
                                   const char *nmea, size_t len)
{
    size_t i;
    if (!state || (!nmea && len) || len >= sizeof(state->nmea))
        return -1;
    /* The API serializes C strings. Reject interior NUL/non-ASCII rather than
     * turning a malformed bounded value into an apparently valid prefix. */
    for (i = 0; i < len; i++) {
        unsigned char c = (unsigned char)nmea[i];
        if ((c < 32 && c != '\r' && c != '\n') || c > 126) return -1;
    }
    pthread_rwlock_wrlock(&state->lock);
    if (!state->active || generation != state->session_generation) {
        pthread_rwlock_unlock(&state->lock);
        return -1;
    }
    if (!len) {
        clear_location_locked(state);
        pthread_rwlock_unlock(&state->lock);
        return 0;
    }
    if (len)
        memcpy(state->nmea, nmea, len);
    state->nmea[len] = '\0';
    state->nmea_len = len;
    state->location_generation = generation;
    state->location_updated_ms = carplay_vehicle_monotonic_ms();
    state->location_sequence++;
    /* Keep position-bearing sentences independently: a following PASCD must
     * not erase the GGA/RMC before the Android worker polls this snapshot. */
    if (len > 6 && len < CARPLAY_POSITION_NMEA_CAPACITY && nmea[0] == '$' && nmea[6] == ',') {
        char *slot = NULL;
        uint64_t *stamp = NULL, *sequence = NULL;
        if (!memcmp(nmea + 3, "RMC", 3)) {
            slot = state->rmc; stamp = &state->rmc_updated_ms; sequence = &state->rmc_sequence;
        } else if (!memcmp(nmea + 3, "GGA", 3)) {
            slot = state->gga; stamp = &state->gga_updated_ms; sequence = &state->gga_sequence;
        }
        if (slot) {
            memcpy(slot, nmea, len);
            slot[len] = 0;
            *stamp = state->location_updated_ms;
            *sequence = state->location_sequence;
        }
    }
    pthread_rwlock_unlock(&state->lock);
    return 0;
}

int carplay_vehicle_state_get_nmea(const struct carplay_vehicle_state *state,
                                   char *out, size_t capacity,
                                   size_t *written)
{
    size_t needed;

    if (!state || !out || !capacity)
        return -1;
    pthread_rwlock_rdlock((pthread_rwlock_t *)&state->lock);
    needed = state->nmea_len;
    if (needed >= capacity) {
        pthread_rwlock_unlock((pthread_rwlock_t *)&state->lock);
        return -1;
    }
    if (needed)
        memcpy(out, state->nmea, needed);
    out[needed] = '\0';
    if (written)
        *written = needed;
    pthread_rwlock_unlock((pthread_rwlock_t *)&state->lock);
    return 0;
}

size_t carplay_vehicle_state_encode_location_start(uint8_t *out,
                                                   size_t capacity,
                                                   uint16_t interval_ms)
{
    static const uint16_t components[] = { 1, 2, 4 };
    size_t used = begin_csm(out, capacity, 0xfffa);
    size_t i;
    (void)interval_ms;

    if (!used)
        return 0;
    for (i = 0; i < sizeof(components) / sizeof(components[0]); i++) {
        size_t added = append_param(out, capacity, used, components[i],
                                    NULL, 0);
        if (!added)
            return 0;
        used += added;
    }
    finish_csm(out, used);
    return used;
}

size_t carplay_vehicle_state_encode_location_stop(uint8_t *out,
                                                  size_t capacity)
{
    size_t used = begin_csm(out, capacity, 0xfffc);
    if (!used)
        return 0;
    finish_csm(out, used);
    return used;
}

int carplay_vehicle_state_decode_location_information(
    struct carplay_vehicle_state *state, uint64_t generation,
    const uint8_t *body, size_t body_len,
    char *error, size_t error_capacity)
{
    size_t off = 0;
    const char *nmea = NULL;
    size_t nmea_len = 0;
    int found_nmea = 0;

    if (!state || (!body && body_len)) {
        set_error(error, error_capacity, "invalid location payload");
        return -1;
    }
    while (off < body_len) {
        uint16_t total;
        uint16_t id;
        if (body_len - off < 4) {
            set_error(error, error_capacity, "truncated iAP2 parameter");
            return -1;
        }
        total = get_u16_be(body + off);
        id = get_u16_be(body + off + 2);
        if (total < 4 || total > body_len - off) {
            set_error(error, error_capacity, "invalid iAP2 parameter length");
            return -1;
        }
        if (id == 0) {
            if (nmea) {
                set_error(error, error_capacity, "duplicate NMEA parameter");
                return -1;
            }
            nmea = (const char *)(body + off + 4);
            nmea_len = total - 4;
            found_nmea = 1;
            while (nmea_len && nmea[nmea_len - 1] == '\0')
                nmea_len--;
            if (nmea_len >= CARPLAY_NMEA_CAPACITY) {
                set_error(error, error_capacity, "NMEA value is too long");
                return -1;
            }
        }
        off += total;
    }
    if (!found_nmea) {
        if (body_len == 0) {
            if (carplay_vehicle_state_set_nmea(state, generation, "", 0) < 0)
                return -1;
            return 0;
        }
        set_error(error, error_capacity, "location payload has no NMEA parameter");
        return -1;
    }
    if (carplay_vehicle_state_set_nmea(state, generation, nmea,
                                       nmea_len) < 0) {
        set_error(error, error_capacity, "could not store NMEA");
        return -1;
    }
    return 1;
}
