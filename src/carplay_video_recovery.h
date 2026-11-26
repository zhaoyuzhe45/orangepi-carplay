#ifndef CARPLAY_VIDEO_RECOVERY_H
#define CARPLAY_VIDEO_RECOVERY_H
#include <stdint.h>

/* Owner-thread policy. No I/O or wall clock: cached screen replays never feed it. */
enum carplay_video_recovery_action {
    CARPLAY_VIDEO_WAIT, CARPLAY_VIDEO_KEYFRAME, CARPLAY_VIDEO_REFRESH,
    CARPLAY_VIDEO_RESTART
};

struct carplay_video_recovery {
    uint64_t started_ms, last_frame_ms, last_heartbeat_ms, probe_ms, last_pts_us;
    unsigned phase, resume_generation;
    int has_frame, suspended;
};

static inline void carplay_video_recovery_init(struct carplay_video_recovery *s,
                                               uint64_t now)
{
    *s=(struct carplay_video_recovery){.started_ms=now,.last_frame_ms=now,
                                     .last_heartbeat_ms=now};
}

static inline void carplay_video_recovery_heartbeat(struct carplay_video_recovery *s,
                                                    uint64_t now)
{
    s->last_heartbeat_ms=now;
}

/* Return the recovered probe phase, or zero for normal/duplicate output. */
static inline unsigned carplay_video_recovery_frame(struct carplay_video_recovery *s,
                                                     uint64_t now, uint64_t pts_us)
{
    if (s->has_frame && pts_us<=s->last_pts_us) return 0;
    unsigned recovered=s->phase;
    s->has_frame=1;s->last_pts_us=pts_us;s->last_frame_ms=now;s->phase=0;
    return recovered;
}

static inline enum carplay_video_recovery_action carplay_video_recovery_tick(
    struct carplay_video_recovery *s, uint64_t now, int suspended, unsigned resume_generation)
{
    if (suspended || s->suspended || s->resume_generation != resume_generation) {
        s->resume_generation=resume_generation;
        s->suspended=suspended;
        s->started_ms=s->last_frame_ms=s->last_heartbeat_ms=now;
        s->phase=0;
        return CARPLAY_VIDEO_WAIT;
    }
    if (!s->has_frame)
        return now-s->started_ms>=10000u ? CARPLAY_VIDEO_RESTART : CARPLAY_VIDEO_WAIT;
    if (!s->phase && now-s->last_frame_ms>=3000u) {
        s->phase=1;s->probe_ms=now;
        return CARPLAY_VIDEO_KEYFRAME;
    }
    if (s->phase && now-s->probe_ms>=5000u) {
        if (s->phase==2 || now-s->last_heartbeat_ms>=3000u)
            return CARPLAY_VIDEO_RESTART;
        /* A responsive codec can legitimately be idle on a still screen.
         * Refresh the capture display to request actual input before reset. */
        s->phase=2;s->probe_ms=now;
        return CARPLAY_VIDEO_REFRESH;
    }
    return CARPLAY_VIDEO_WAIT;
}

struct carplay_video_restart_budget {
    uint64_t window_ms;
    unsigned restarts;
};

static inline int carplay_video_restart_allowed(struct carplay_video_restart_budget *b,
                                                 uint64_t now)
{
    if (!b->restarts || now-b->window_ms>=60000u) {
        b->window_ms=now;b->restarts=0;
    }
    if (b->restarts>=2) return 0;
    b->restarts++;
    return 1;
}
#endif
