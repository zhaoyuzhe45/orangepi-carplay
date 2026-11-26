#ifndef CARPLAY_LOG_H
#define CARPLAY_LOG_H

#include <stdarg.h>
#include <stddef.h>
#include <stdint.h>

#define CARPLAY_LOG_DEFAULT_FILE_BYTES (8u * 1024u * 1024u)
#define CARPLAY_LOG_DEFAULT_RETENTION_DAYS 7u
#define CARPLAY_LOG_DEFAULT_QUOTA_BYTES (256u * 1024u * 1024u)
#define CARPLAY_LOG_DEFAULT_DIRECTORY "/data/carplay/logs"

enum carplay_log_level {
    CARPLAY_LOG_DEBUG = 0,
    CARPLAY_LOG_INFO,
    CARPLAY_LOG_WARN,
    CARPLAY_LOG_ERROR
};

struct carplay_log_policy {
    const char *directory;
    uint64_t file_bytes;
    unsigned retention_days;
    uint64_t total_bytes;
    enum carplay_log_level minimum_level;
};

struct carplay_log_metrics {
    uint64_t active_bytes;
    uint64_t total_bytes;
    uint64_t rotated_files;
    uint64_t expired_files_removed;
    uint64_t quota_files_removed;
};

int carplay_log_init(const struct carplay_log_policy *policy);
void carplay_log_shutdown(void);
void carplay_log_write(enum carplay_log_level level, const char *message);
/* Writes a formatted line without adding a second timestamp. */
void carplay_log_vwrite(enum carplay_log_level level, const char *fmt,
                        va_list ap);
/* Pushes everything written so far to flash.
 *
 * The active file is block buffered to avoid one write syscall per protocol
 * frame. Explicit sync points still make handshakes and role switches durable.
 * The flash itself is reached on the kernel's dirty writeback, which is ~30 s
 * here.  The
 * head unit does everything interesting in bursts of a few seconds (0x51, the
 * configuration switch, the first iAP2 frames) and this box has been
 * power-cycled inside exactly those bursts, which is why two live attempts
 * left no trace at all.  One fsync at each end of such a burst is cheap.
 */
void carplay_log_sync(void);
int carplay_log_get_metrics(struct carplay_log_metrics *metrics);
int carplay_log_read_tail(size_t max_bytes, char **out, size_t *out_len);
int carplay_log_export_text(size_t max_bytes, char **out, size_t *out_len);

#endif
