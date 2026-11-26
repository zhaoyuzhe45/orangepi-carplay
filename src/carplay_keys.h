#ifndef CARPLAY_KEYS_H
#define CARPLAY_KEYS_H
#include "carplay_knob.h"
#include <stdint.h>
#define CARPLAY_KEYS_QUEUE_ENTRIES 128u
#define CARPLAY_KEYS_QUEUE_ACTIONS 4096u
struct carplay_keys;
/* accepted/emitted/cancelled count actions; rejected counts all rejected submit
 * attempts (including invalid and full queue); failures counts sink I/O failures. */
struct carplay_keys_stats { uint64_t accepted, emitted, rejected, cancelled, failures; };
/* NULL path tries /dev/uinput and /dev/input/uinput. Returns 0 or -1. */
int carplay_keys_create(struct carplay_keys **, const char *path);
/* Inject a pipe/socket sink for the same worker; duplicates fd, caller owns original.
 * O_NONBLOCK is set on the shared open-file description (including original fd). */
int carplay_keys_create_fd(struct carplay_keys **, int fd);
/* Whole-batch FIFO admission under a short bookkeeping mutex; never waits for
 * device IO. 0 accepted, -1 invalid/stopped/suspended, -2 queue full.
 * No events from a rejected batch are admitted. */
int carplay_keys_submit(struct carplay_keys *, const struct carplay_key_batch *);
/* Cancel queued work; suspend additionally rejects submissions until resumed.
 * A tap already being written may finish. A failed partial write removes the
 * physical uinput device to release held keys and marks the worker unavailable.
 * An arbitrary injected sink cannot promise delivery after peer failure.
 * These are lifecycle operations, not input-report callbacks. */
void carplay_keys_clear(struct carplay_keys *);
void carplay_keys_suspend(struct carplay_keys *, int suspended);
void carplay_keys_get_stats(struct carplay_keys *, struct carplay_keys_stats *);
/* Lifecycle calls must be serialized by the owner. Stop report producers before
 * destroy; it wakes and joins the worker, then destroys the device. */
void carplay_keys_destroy(struct carplay_keys *);
#endif
