#define _GNU_SOURCE
#include "carplay_log.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#ifndef CARPLAY_LOG_TEST_NO_ANDROID
#include <android/log.h>
#endif

#define ACTIVE_NAME "carplay-active.log"
#define MAX_LINE_BYTES 8192u
#define ACTIVE_BUFFER_BYTES (64u * 1024u)
#define CLEANUP_INTERVAL_SECONDS 3600

struct rotated_file {
    char *path;
    uint64_t size;
    time_t mtime;
    int removed;
};

static pthread_mutex_t log_mutex = PTHREAD_MUTEX_INITIALIZER;
static char active_buffer[ACTIVE_BUFFER_BYTES];
static char active_path[1024];
static char log_directory[1024];
static FILE *active_file;
static uint64_t active_bytes;
static struct carplay_log_policy configured_policy;
static struct carplay_log_metrics metrics;
static int initialized;
static unsigned rotation_sequence;
static struct timespec last_cleanup_monotonic;
static int cleanup_has_run;

static const char *level_name(enum carplay_log_level level)
{
    switch (level) {
    case CARPLAY_LOG_DEBUG: return "DEBUG";
    case CARPLAY_LOG_INFO: return "INFO";
    case CARPLAY_LOG_WARN: return "WARN";
    case CARPLAY_LOG_ERROR: return "ERROR";
    default: return "INFO";
    }
}

static int path_is_rotated(const char *name)
{
    return strncmp(name, "carplay-", 8) == 0 &&
           strcmp(name, ACTIVE_NAME) != 0 && strstr(name, ".log") != NULL;
}

static void rotated_name(char *out, size_t capacity)
{
    struct timespec ts;
    struct tm tm_now;

    clock_gettime(CLOCK_REALTIME, &ts);
    localtime_r(&ts.tv_sec, &tm_now);
    snprintf(out, capacity,
             "%s/carplay-%04d%02d%02d-%02d%02d%02d-%ld-%u.log",
             log_directory, tm_now.tm_year + 1900, tm_now.tm_mon + 1,
             tm_now.tm_mday, tm_now.tm_hour, tm_now.tm_min,
             tm_now.tm_sec, (long)getpid(), rotation_sequence++);
}

static int split_file_locked(const char *source_path, uint64_t size)
{
    static const size_t buffer_size = 16384u;
    char *buffer = malloc(buffer_size);
    FILE *source = NULL;
    uint64_t remaining = size;
    int result = -1;

    if (!buffer)
        return -1;
    source = fopen(source_path, "rb");
    if (!source)
        goto out;
    while (remaining > 0) {
        char rotated[1280];
        FILE *destination;
        size_t wanted = (size_t)(remaining < configured_policy.file_bytes ?
                                 remaining : configured_policy.file_bytes);
        size_t left = wanted;

        rotated_name(rotated, sizeof(rotated));
        destination = fopen(rotated, "wb");
        if (!destination)
            goto out;
        (void)fchmod(fileno(destination), 0600);
        while (left > 0) {
            size_t chunk = left < buffer_size ? left : buffer_size;
            size_t got = fread(buffer, 1, chunk, source);
            size_t wrote;
            if (got != chunk) {
                fclose(destination);
                unlink(rotated);
                goto out;
            }
            wrote = fwrite(buffer, 1, got, destination);
            if (wrote != got) {
                fclose(destination);
                unlink(rotated);
                goto out;
            }
            left -= got;
        }
        fflush(destination);
        (void)fsync(fileno(destination));
        if (fclose(destination) != 0) {
            unlink(rotated);
            goto out;
        }
        remaining -= wanted;
    }
    if (unlink(source_path) < 0)
        goto out;
    result = 0;
out:
    if (source)
        fclose(source);
    free(buffer);
    return result;
}

static int open_active_locked(void)
{
    struct stat st;

    if (active_file)
        return 0;
    if (stat(active_path, &st) == 0) {
        active_bytes = (uint64_t)st.st_size;
        if (active_bytes > configured_policy.file_bytes) {
            if (split_file_locked(active_path, active_bytes) < 0)
                return -1;
            active_bytes = 0;
        }
    }
    active_file = fopen(active_path, "a");
    if (!active_file)
        return -1;
    (void)fchmod(fileno(active_file), 0600);
    (void)setvbuf(active_file, active_buffer, _IOFBF,
                  sizeof(active_buffer));
    return 0;
}

static void free_rotated_files(struct rotated_file *files, size_t count)
{
    size_t i;
    for (i = 0; i < count; i++)
        free(files[i].path);
}

static int cleanup_due_locked(void)
{
    struct timespec now;

    if (!cleanup_has_run)
        return 1;
    if (clock_gettime(CLOCK_MONOTONIC, &now) < 0)
        return 1;
    if (now.tv_sec < last_cleanup_monotonic.tv_sec)
        return 0;
    return now.tv_sec - last_cleanup_monotonic.tv_sec >=
           CLEANUP_INTERVAL_SECONDS;
}

static void mark_cleanup_locked(void)
{
    if (clock_gettime(CLOCK_MONOTONIC, &last_cleanup_monotonic) == 0)
        cleanup_has_run = 1;
}

static int cleanup_locked(void)
{
    DIR *dir;
    struct dirent *entry;
    struct rotated_file *files = NULL;
    size_t count = 0;
    size_t capacity = 0;
    size_t i;
    uint64_t total = active_bytes;
    time_t now = time(NULL);

    dir = opendir(log_directory);
    if (!dir)
        return -1;
    while ((entry = readdir(dir)) != NULL) {
        char path[1280];
        struct stat st;
        if (!path_is_rotated(entry->d_name))
            continue;
        snprintf(path, sizeof(path), "%s/%s", log_directory, entry->d_name);
        if (lstat(path, &st) == 0 && S_ISREG(st.st_mode) &&
            (uint64_t)st.st_size > configured_policy.file_bytes)
            (void)split_file_locked(path, (uint64_t)st.st_size);
    }
    closedir(dir);

    dir = opendir(log_directory);
    if (!dir)
        return -1;
    while ((entry = readdir(dir)) != NULL) {
        char path[1280];
        struct stat st;
        if (!path_is_rotated(entry->d_name))
            continue;
        snprintf(path, sizeof(path), "%s/%s", log_directory, entry->d_name);
        if (lstat(path, &st) < 0 || !S_ISREG(st.st_mode))
            continue;
        /* A reboot without a valid RTC can move the clock behind saved
         * logs. Do not turn a negative age into a huge unsigned age and
         * delete the evidence. The byte quota still bounds disk usage. */
        if (now >= st.st_mtime && (uint64_t)(now - st.st_mtime) >
            (uint64_t)configured_policy.retention_days * 86400u) {
            if (unlink(path) == 0) {
                metrics.expired_files_removed++;
                continue;
            }
        }
        if (count == capacity) {
            size_t next_capacity = capacity ? capacity * 2u : 64u;
            struct rotated_file *grown = realloc(
                files, next_capacity * sizeof(*files));
            if (!grown)
                break;
            files = grown;
            capacity = next_capacity;
        }
        files[count].path = strdup(path);
        if (!files[count].path)
            break;
        files[count].size = (uint64_t)st.st_size;
        files[count].mtime = st.st_mtime;
        files[count].removed = 0;
        total += files[count].size;
        count++;
    }
    closedir(dir);

    while (total > configured_policy.total_bytes) {
        size_t oldest = count;
        for (i = 0; i < count; i++) {
            if (!files[i].removed &&
                (oldest == count || files[i].mtime < files[oldest].mtime))
                oldest = i;
        }
        if (oldest == count)
            break;
        if (unlink(files[oldest].path) == 0) {
            total -= files[oldest].size;
            metrics.quota_files_removed++;
            files[oldest].removed = 1;
        } else {
            files[oldest].removed = 1;
        }
    }
    metrics.total_bytes = total;
    metrics.active_bytes = active_bytes;
    metrics.rotated_files = 0;
    for (i = 0; i < count; i++) {
        if (!files[i].removed)
            metrics.rotated_files++;
    }
    free_rotated_files(files, count);
    mark_cleanup_locked();
    return 0;
}

static int rotate_locked(void)
{
    char rotated[1280];

    if (active_file) {
        fflush(active_file);
        (void)fsync(fileno(active_file));
        fclose(active_file);
        active_file = NULL;
    }
    rotated_name(rotated, sizeof(rotated));
    if (rename(active_path, rotated) < 0 && errno != ENOENT)
        return -1;
    active_bytes = 0;
    metrics.rotated_files++;
    return open_active_locked();
}

int carplay_log_init(const struct carplay_log_policy *policy)
{
    const char *directory;

    if (!policy)
        return -1;
    directory = policy->directory ? policy->directory :
                CARPLAY_LOG_DEFAULT_DIRECTORY;
    if (policy->file_bytes < 16384u ||
        policy->retention_days == 0 || policy->retention_days > 3650u ||
        policy->total_bytes < policy->file_bytes ||
        policy->minimum_level < CARPLAY_LOG_DEBUG ||
        policy->minimum_level > CARPLAY_LOG_ERROR)
        return -1;
    pthread_mutex_lock(&log_mutex);
    if (initialized) {
        pthread_mutex_unlock(&log_mutex);
        return 0;
    }
    configured_policy = *policy;
    configured_policy.directory = log_directory;
    snprintf(log_directory, sizeof(log_directory), "%s", directory);
    if (mkdir(log_directory, 0700) < 0 && errno != EEXIST) {
        pthread_mutex_unlock(&log_mutex);
        return -1;
    }
    (void)chmod(log_directory, 0700);
    snprintf(active_path, sizeof(active_path), "%s/%s", log_directory,
             ACTIVE_NAME);
    memset(&metrics, 0, sizeof(metrics));
    active_bytes = 0;
    cleanup_has_run = 0;
    initialized = 1;
    if (open_active_locked() < 0 || cleanup_locked() < 0) {
        if (active_file) {
            fclose(active_file);
            active_file = NULL;
        }
        initialized = 0;
        pthread_mutex_unlock(&log_mutex);
        return -1;
    }
    pthread_mutex_unlock(&log_mutex);
    return 0;
}

void carplay_log_shutdown(void)
{
    pthread_mutex_lock(&log_mutex);
    if (active_file) {
        fflush(active_file);
        (void)fsync(fileno(active_file));
        fclose(active_file);
        active_file = NULL;
    }
    initialized = 0;
    pthread_mutex_unlock(&log_mutex);
}

/*
 * Make everything written so far durable.  See carplay_log.h for why the two
 * windows that matter (0x51 -> configuration switch, and the host-mode bus
 * dump) get one of these instead of relying on writeback.
 */
void carplay_log_sync(void)
{
    pthread_mutex_lock(&log_mutex);
    if (initialized && active_file) {
        fflush(active_file);
        (void)fsync(fileno(active_file));
    }
    pthread_mutex_unlock(&log_mutex);
}

static int format_prefix_locked(enum carplay_log_level level, char *line,
                                size_t capacity)
{
    char stamp[40];
    struct timespec ts;
    struct tm tm_now;
    int len;

    clock_gettime(CLOCK_REALTIME, &ts);
    localtime_r(&ts.tv_sec, &tm_now);
    strftime(stamp, sizeof(stamp), "%Y-%m-%dT%H:%M:%S", &tm_now);
    len = snprintf(line, capacity, "%s.%03ldZ level=%s pid=%ld ", stamp,
                   ts.tv_nsec / 1000000, level_name(level), (long)getpid());
    return len >= 0 && (size_t)len < capacity ? len : -1;
}

static void write_line(enum carplay_log_level level, char *line, int len)
{
    if (len < 0)
        return;
    if ((size_t)len >= MAX_LINE_BYTES) {
        len = (int)MAX_LINE_BYTES - 2;
        line[len - 1] = '\n';
        line[len] = '\0';
    }

    pthread_mutex_lock(&log_mutex);
    if (!initialized || level < configured_policy.minimum_level) {
        pthread_mutex_unlock(&log_mutex);
        return;
    }
#ifndef CARPLAY_LOG_TEST_NO_ANDROID
    /* High-volume INFO traffic already goes to the rotating file. Mirror only
     * exceptional records to logd, whose per-record transport is expensive. */
    if (level >= CARPLAY_LOG_WARN)
        __android_log_write(level == CARPLAY_LOG_ERROR ? ANDROID_LOG_ERROR :
                            ANDROID_LOG_WARN, "CarPlayBox", line);
#endif
    if (active_bytes &&
        (uint64_t)len > configured_policy.file_bytes - active_bytes) {
        if (rotate_locked() < 0) {
            pthread_mutex_unlock(&log_mutex);
            return;
        }
        (void)cleanup_locked();
    }
    if (active_file && fputs(line, active_file) >= 0) {
        active_bytes += (uint64_t)len;
        metrics.active_bytes = active_bytes;
        metrics.total_bytes += (uint64_t)len;
        if (cleanup_due_locked())
            (void)cleanup_locked();
    }
    pthread_mutex_unlock(&log_mutex);
}

void carplay_log_write(enum carplay_log_level level, const char *message)
{
    char line[MAX_LINE_BYTES];
    int prefix;
    int len;

    if (!message)
        return;
    prefix = format_prefix_locked(level, line, sizeof(line));
    if (prefix < 0)
        return;
    len = prefix + snprintf(line + prefix, sizeof(line) - (size_t)prefix,
                            "%s\n", message);
    write_line(level, line, len);
}

void carplay_log_vwrite(enum carplay_log_level level, const char *fmt,
                        va_list ap)
{
    char line[MAX_LINE_BYTES];
    int prefix;
    int len;

    if (!fmt)
        return;
    prefix = format_prefix_locked(level, line, sizeof(line));
    if (prefix < 0)
        return;
    len = prefix + vsnprintf(line + prefix, sizeof(line) - (size_t)prefix,
                             fmt, ap);
    if (len >= 0 && (size_t)len < sizeof(line)) {
        int suffix = snprintf(line + len, sizeof(line) - (size_t)len, "\n");
        if (suffix >= 0)
            len += suffix;
    }
    write_line(level, line, len);
}

int carplay_log_get_metrics(struct carplay_log_metrics *out)
{
    if (!out)
        return -1;
    pthread_mutex_lock(&log_mutex);
    *out = metrics;
    out->active_bytes = active_bytes;
    pthread_mutex_unlock(&log_mutex);
    return 0;
}

int carplay_log_read_tail(size_t max_bytes, char **out, size_t *out_len)
{
    char *buffer;
    struct stat st;
    size_t read_len;
    size_t start;
    ssize_t n;
    int fd;

    if (!out || !out_len || !max_bytes)
        return -1;
    *out = NULL;
    *out_len = 0;
    pthread_mutex_lock(&log_mutex);
    if (!initialized) {
        pthread_mutex_unlock(&log_mutex);
        return -1;
    }
    if (active_file)
        fflush(active_file);
    fd = open(active_path, O_RDONLY | O_CLOEXEC);
    if (fd < 0 || fstat(fd, &st) < 0) {
        if (fd >= 0)
            close(fd);
        pthread_mutex_unlock(&log_mutex);
        return -1;
    }
    read_len = (size_t)st.st_size;
    if (read_len > max_bytes)
        read_len = max_bytes;
    start = (size_t)st.st_size - read_len;
    buffer = malloc(read_len + 1);
    if (!buffer) {
        close(fd);
        pthread_mutex_unlock(&log_mutex);
        return -1;
    }
    if (lseek(fd, (off_t)start, SEEK_SET) < 0) {
        free(buffer);
        close(fd);
        pthread_mutex_unlock(&log_mutex);
        return -1;
    }
    n = read(fd, buffer, read_len);
    close(fd);
    pthread_mutex_unlock(&log_mutex);
    if (n < 0) {
        free(buffer);
        return -1;
    }
    buffer[n] = '\0';
    *out = buffer;
    *out_len = (size_t)n;
    return 0;
}

int carplay_log_export_text(size_t max_bytes, char **out, size_t *out_len)
{
    char *tail = NULL;
    size_t tail_len = 0;
    char *buffer;
    int n;

    if (!out || !out_len || carplay_log_read_tail(max_bytes, &tail,
                                                    &tail_len) < 0)
        return -1;
    buffer = malloc(tail_len + 128);
    if (!buffer) {
        free(tail);
        return -1;
    }
    n = snprintf(buffer, tail_len + 128,
                 "# CarPlay box log export generated by pid %ld\n"
                 "# Authentication material is intentionally omitted.\n",
                 (long)getpid());
    if (n < 0) {
        free(tail);
        free(buffer);
        return -1;
    }
    memcpy(buffer + n, tail, tail_len);
    free(tail);
    *out = buffer;
    *out_len = (size_t)n + tail_len;
    return 0;
}
