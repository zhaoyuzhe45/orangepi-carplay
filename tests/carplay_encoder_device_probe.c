#define _POSIX_C_SOURCE 200809L
#include "carplay_encoder.h"
#include <errno.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>
static uint64_t now_ms(void) {
    struct timespec t; clock_gettime(CLOCK_MONOTONIC,&t);
    return (uint64_t)t.tv_sec*1000u+t.tv_nsec/1000000u;
}
int main(int argc,char **argv) {
    struct carplay_encoder e; struct carplay_encoder_packet p;
    unsigned seconds=argc>1?(unsigned)atoi(argv[1]):5u;
    unsigned cycles=argc>2?(unsigned)atoi(argv[2]):1u;
    for(unsigned cycle=0;cycle<cycles;cycle++) {
        carplay_encoder_init(&e);
        FILE *output=fopen("/data/local/tmp/mediacodec-probe.h264","wb");
        if(!output || carplay_encoder_start(&e,1920,1080,8000000,30)<0) {perror("start");return 1;}
        pid_t original=e.pid;
        uint64_t start=now_ms(),last_pts=0,request_ms=0;
        unsigned configs=0,frames=0,keys=0,requested=0,after_request=0;
        while(now_ms()-start<seconds*1000u) {
            uint64_t elapsed=now_ms()-start;
            if(elapsed>=1500 && !requested) {
                if(carplay_encoder_request_keyframe(&e)<0) {perror("keyframe");return 2;}
                requested=1;request_ms=now_ms();
            }
            int result=carplay_encoder_reader_read(&e.reader,e.fd,&p,now_ms());
            if(result<0) {perror("read");carplay_encoder_stop(&e);return 3;}
            if(result==1) {
                if(p.flags==CARPLAY_ENCODER_HEARTBEAT) continue;
                if(p.flags&CARPLAY_ENCODER_EOS) {fprintf(stderr,"early EOS\n");return 4;}
                if(p.flags&CARPLAY_ENCODER_CONFIG) configs++;
                else { if(p.pts_us<last_pts) {fprintf(stderr,"PTS reversed\n");return 5;} last_pts=p.pts_us;frames++; }
                if(p.flags&CARPLAY_ENCODER_KEYFRAME) {keys++; if(requested) after_request++;}
                if(elapsed<3000) fwrite(p.data,1,p.size,output);
                continue;
            }
            struct pollfd pollfd={e.fd,POLLIN,0}; poll(&pollfd,1,50);
        }
        int alive=kill(original,0)==0;
        if(carplay_encoder_stop(&e)<0) {perror("stop");return 6;}
        fclose(output);
        int status, reaped=waitpid(original,&status,WNOHANG)<0 && errno==ECHILD;
        printf("cycle=%u pid=%d elapsed_ms=%llu config=%u frames=%u keys=%u key_after_request=%u same_pid_alive=%d reaped=%d request_ms=%llu\n",
            cycle,(int)original,(unsigned long long)(now_ms()-start),configs,frames,keys,after_request,alive,reaped,(unsigned long long)(request_ms-start));
        fflush(stdout);
        if(!configs || frames<2 || !after_request || !alive || !reaped) return 7;
    }
    puts("PASS: continuous hardware capture, keyframe without restart, bounded cleanup");
    return 0;
}
