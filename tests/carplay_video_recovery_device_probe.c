#define _POSIX_C_SOURCE 200809L
#include "carplay_encoder.h"
#include "carplay_video_recovery.h"
#include <errno.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#include <unistd.h>

static uint64_t now_ms(void)
{
    struct timespec t;clock_gettime(CLOCK_MONOTONIC,&t);
    return (uint64_t)t.tv_sec*1000u+t.tv_nsec/1000000u;
}

int main(int argc,char **argv)
{
    int mode=argc>1?atoi(argv[1]):0;
    int inject=mode==1, failed=0, injected=0, refreshed=0;
    struct carplay_encoder e;
    struct carplay_video_recovery recovery;
    carplay_encoder_init(&e);
    unsigned frames=0,heartbeats=0,refreshes=0,restarts=0,recovered_frames=0;
    uint64_t begin=now_ms(),run_ms=inject?22000u:(mode==2?12000u:35000u);
    if(carplay_encoder_start(&e,800,480,2000000,30)<0){perror("start");return 1;}
    carplay_video_recovery_init(&recovery,now_ms());
    pid_t stopped_pid=-1;
    while(now_ms()-begin<run_ms) {
        uint64_t now=now_ms();
        if(mode==2 && !refreshed && frames && now-begin>=3000u) {
            if(carplay_encoder_request_refresh(&e)<0){perror("manual refresh");failed=1;break;}
            refreshed=1;refreshes++;
            printf("requested capture refresh pid=%d\n",(int)e.pid);fflush(stdout);
        }
        if(inject && !injected && frames && now-begin>=5000u) {
            stopped_pid=e.pid;
            if(kill(stopped_pid,SIGSTOP)<0){perror("inject");failed=1;break;}
            injected=1;printf("injected stop pid=%d at_ms=%llu\n",(int)stopped_pid,(unsigned long long)(now-begin));fflush(stdout);
        }
        enum carplay_video_recovery_action action=carplay_video_recovery_tick(&recovery,now,0,0);
        if(action==CARPLAY_VIDEO_KEYFRAME) {
            if(carplay_encoder_request_keyframe(&e)<0){perror("key");failed=1;break;}
        } else if(action==CARPLAY_VIDEO_REFRESH) {
            refreshes++;
            if(carplay_encoder_request_refresh(&e)<0){perror("refresh");failed=1;break;}
        } else if(action==CARPLAY_VIDEO_RESTART) {
            if(!injected || restarts){fprintf(stderr,"unexpected restart phase=%u frames=%u beats=%u\n",recovery.phase,frames,heartbeats);failed=1;break;}
            carplay_encoder_stop(&e);restarts++;
            if(carplay_encoder_start(&e,800,480,2000000,30)<0){perror("restart");failed=1;break;}
            carplay_video_recovery_init(&recovery,now_ms());
            printf("restarted pid=%d at_ms=%llu\n",(int)e.pid,(unsigned long long)(now_ms()-begin));fflush(stdout);
        }
        struct carplay_encoder_packet p;
        int got=carplay_encoder_reader_read(&e.reader,e.fd,&p,now_ms());
        if(got<0){perror("reader");failed=1;break;}
        if(got>0) {
            if(p.flags==CARPLAY_ENCODER_HEARTBEAT){heartbeats++;carplay_video_recovery_heartbeat(&recovery,now_ms());}
            else if(p.flags&CARPLAY_ENCODER_EOS){fprintf(stderr,"unexpected EOS\n");failed=1;break;}
            else if(!(p.flags&CARPLAY_ENCODER_CONFIG)) {
                frames++;if(restarts || refreshed)recovered_frames++;
                carplay_video_recovery_frame(&recovery,now_ms(),p.pts_us);
            }
        } else {struct pollfd f={e.fd,POLLIN,0};poll(&f,1,50);}
    }
    if(carplay_encoder_stop(&e)<0)failed=1;
    if(stopped_pid>0 && (kill(stopped_pid,0)==0 || errno!=ESRCH))failed=1;
    if(!frames || heartbeats<2 || (inject && (!injected || restarts!=1 || !recovered_frames)) ||
       (mode==2 && (!refreshed || !recovered_frames || restarts)))failed=1;
    printf("%s inject=%d frames=%u heartbeats=%u refreshes=%u restarts=%u recovered_frames=%u elapsed_ms=%llu\n",
           failed?"FAIL":"PASS",inject,frames,heartbeats,refreshes,restarts,recovered_frames,(unsigned long long)(now_ms()-begin));
    return failed?1:0;
}
