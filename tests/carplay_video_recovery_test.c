#include "carplay_video_recovery.h"
#include <assert.h>
#include <stdio.h>

static void startup(void)
{
    struct carplay_video_recovery s;
    carplay_video_recovery_init(&s,100);
    carplay_video_recovery_heartbeat(&s,10099);
    assert(carplay_video_recovery_tick(&s,10099,0,0)==CARPLAY_VIDEO_WAIT);
    assert(carplay_video_recovery_tick(&s,10100,0,0)==CARPLAY_VIDEO_RESTART);
}

static void static_capture_and_hung_encoder(void)
{
    struct carplay_video_recovery s;
    carplay_video_recovery_init(&s,100);
    assert(carplay_video_recovery_frame(&s,200,10)==0);
    assert(carplay_video_recovery_tick(&s,3199,0,0)==CARPLAY_VIDEO_WAIT);
    assert(carplay_video_recovery_tick(&s,3200,0,0)==CARPLAY_VIDEO_KEYFRAME);
    assert(carplay_video_recovery_tick(&s,8199,0,0)==CARPLAY_VIDEO_WAIT);
    carplay_video_recovery_heartbeat(&s,8100);
    assert(carplay_video_recovery_tick(&s,8200,0,0)==CARPLAY_VIDEO_REFRESH);
    assert(carplay_video_recovery_frame(&s,8300,11)==2);
    assert(carplay_video_recovery_tick(&s,8300,0,0)==CARPLAY_VIDEO_WAIT);
    /* A native replay does not call frame(); heartbeat alone does not heal. */
    assert(carplay_video_recovery_tick(&s,11300,0,0)==CARPLAY_VIDEO_KEYFRAME);
    carplay_video_recovery_heartbeat(&s,16000);
    assert(carplay_video_recovery_tick(&s,16300,0,0)==CARPLAY_VIDEO_REFRESH);
    carplay_video_recovery_heartbeat(&s,21000);
    assert(carplay_video_recovery_tick(&s,21300,0,0)==CARPLAY_VIDEO_RESTART);

    carplay_video_recovery_init(&s,100);
    carplay_video_recovery_frame(&s,200,10);
    assert(carplay_video_recovery_tick(&s,3200,0,0)==CARPLAY_VIDEO_KEYFRAME);
    assert(carplay_video_recovery_tick(&s,8200,0,0)==CARPLAY_VIDEO_RESTART);
}

static void duplicate_pts_and_suspension(void)
{
    struct carplay_video_recovery s;
    carplay_video_recovery_init(&s,100);
    carplay_video_recovery_frame(&s,200,10);
    carplay_video_recovery_frame(&s,3000,10);
    assert(carplay_video_recovery_tick(&s,3200,0,0)==CARPLAY_VIDEO_KEYFRAME);
    assert(carplay_video_recovery_tick(&s,50000,1,0)==CARPLAY_VIDEO_WAIT);
    assert(carplay_video_recovery_tick(&s,50001,0,0)==CARPLAY_VIDEO_WAIT);
    assert(carplay_video_recovery_tick(&s,53000,0,0)==CARPLAY_VIDEO_WAIT);
    assert(carplay_video_recovery_tick(&s,53001,0,0)==CARPLAY_VIDEO_KEYFRAME);
    assert(carplay_video_recovery_frame(&s,53010,11)==1);
    assert(carplay_video_recovery_tick(&s,53020,0,0)==CARPLAY_VIDEO_WAIT);
}

static void healthy_moving_or_repeated_picture(void)
{
    struct carplay_video_recovery s;
    carplay_video_recovery_init(&s,0);
    for(unsigned i=0;i<300;i++) {
        carplay_video_recovery_frame(&s,100+i*1000,i);
        carplay_video_recovery_heartbeat(&s,100+i*1000);
        assert(carplay_video_recovery_tick(&s,100+i*1000,0,0)==CARPLAY_VIDEO_WAIT);
    }
}

static void bounded_local_restart_budget(void)
{
    struct carplay_video_restart_budget b={0};
    assert(carplay_video_restart_allowed(&b,100));
    assert(carplay_video_restart_allowed(&b,200));
    assert(!carplay_video_restart_allowed(&b,300));
    assert(!carplay_video_restart_allowed(&b,60099));
    assert(carplay_video_restart_allowed(&b,60100));
}

int main(void)
{
    startup();static_capture_and_hung_encoder();duplicate_pts_and_suspension();
    healthy_moving_or_repeated_picture();bounded_local_restart_budget();
    puts("PASS video recovery: startup, static probe, heartbeat, capture refresh, hang, PTS, suspension, restart budget");
}
