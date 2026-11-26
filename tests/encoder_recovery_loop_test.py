#!/usr/bin/env python3
"""Run the production encoder loop against controllable child/transport boundaries."""
from pathlib import Path
import os
import subprocess
import tempfile
from airplay_connection_regression_test import function
ROOT=Path(__file__).resolve().parents[1]
source=Path(os.environ.get("CARPLAY_CLIENT_SOURCE",ROOT/"src/airplay_client.c")).resolve()
body=function(source.read_text(),"static void *encoder_main(")
code=r'''
#define _GNU_SOURCE
#include <assert.h>
#include <errno.h>
#include <poll.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdatomic.h>
#include <unistd.h>
#include "carplay_encoder.h"
#include "carplay_video_recovery.h"
#define ENCODER_RESTART_MIN_MS 2000u
#define FORCE_KEY_FRAME_MIN_MS 200u
#define STILL_FRAME_REFRESH_MS 1000u
#define PLACEHOLDER_FIRST_MS 500u
struct airplay_h264_parser {int unused;};
struct airplay_client {
    struct carplay_encoder encoder;
    atomic_int live_screen_stop,force_key_frame,screen_suspended,video_failure;
    atomic_uint screen_resume_generation;
    int display_profile_eligible,display_memory,physical_width,physical_height,actual_density;
    unsigned capture_width,capture_height,output_width,output_height,placeholder_frames;
    int has_real_frame;
    unsigned long live_frame_count;
    uint64_t last_encoder_spawn_ms,last_force_key_frame_ms,last_capture_read_ms,last_screen_frame_ms;
    void *video_batch;size_t video_batch_capacity;
};
struct h264_send_context {struct airplay_client *client;};
static struct airplay_client *active;
static uint64_t clock_ms,last_beat,pts,first_probe_ms,resume_ms;
static unsigned starts,stops,key_requests,refreshes,consumed;
static int scenario,emitted,refresh_pending;
static void iap2_log(const char *fmt,...) {(void)fmt;}
static uint64_t video_monotonic_ms(void) {
    if(scenario==6 && first_probe_ms && !resume_ms && clock_ms>=first_probe_ms+5000) {
        /* The owner handles borrow and unborrow between encoder samples, right
         * at the old probe deadline. The resume IDR has not gone out yet. */
        clock_ms=resume_ms=first_probe_ms+5000;
        atomic_store(&active->screen_suspended,1);
        atomic_store(&active->force_key_frame,1);
        atomic_fetch_add(&active->screen_resume_generation,1);
        atomic_store(&active->screen_suspended,0);
    }
    if(clock_ms>50000) atomic_store(&active->live_screen_stop,1);
    return clock_ms;
}
static int fake_usleep(unsigned us) {clock_ms+=us/1000;return 0;}
static int fake_poll(struct pollfd *p,nfds_t n,int ms) {(void)p;(void)n;clock_ms+=(unsigned)ms;return 0;}
#define usleep fake_usleep
#define poll fake_poll
#define carplay_display_remember_step(a,b,c,d,e) ((void)0)
#define airplay_h264_parser_init(a,b,c,d,e,f) ((void)(a),(void)(b),(void)(c),(void)(f))
#define airplay_h264_parser_set_stream_crop(a,b,c) ((void)(a),(void)(b),(void)(c))
static void reset_encoder_picture(struct airplay_client *c) {(void)c;}
static int start_encoder(struct airplay_client *c) {
    starts++;emitted=0;refresh_pending=0;
    c->encoder.fd=9;c->encoder.pid=10;c->last_encoder_spawn_ms=clock_ms;
    if((scenario==0 || scenario==3 || scenario==5 || scenario==6) && starts==2)atomic_store(&c->live_screen_stop,1);
    return 0;
}
int carplay_encoder_request_keyframe(struct carplay_encoder *e) {
    (void)e;key_requests++;
    if(scenario==6 && key_requests==1)first_probe_ms=clock_ms;
    return 0;
}
int carplay_encoder_request_refresh(struct carplay_encoder *e) {(void)e;refreshes++;refresh_pending=1;return 0;}
int carplay_encoder_stop(struct carplay_encoder *e) {if(e->fd>=0)stops++;e->fd=-1;return 0;}
int carplay_encoder_reader_read(struct carplay_encoder_reader *r,int fd,struct carplay_encoder_packet *p,uint64_t now) {
    (void)r;(void)fd;(void)now;clock_ms+=100;
    if(scenario==4){errno=EIO;return -1;}
    if(scenario==5) {
        p->flags=0;p->pts_us=++pts;return 1;
    }
    if((!emitted && scenario!=3) || (scenario==1 && refresh_pending) ||
       (scenario==6 && key_requests==2)) {
        emitted=1;refresh_pending=0;p->flags=CARPLAY_ENCODER_KEYFRAME;p->pts_us=++pts;
        return 1;
    }
    if((scenario==1 || scenario==3) && clock_ms-last_beat>=1000) {
        last_beat=clock_ms;p->flags=CARPLAY_ENCODER_HEARTBEAT;p->pts_us=pts;return 1;
    }
    return 0;
}
static int consume_encoder_packet(struct airplay_client *c,struct airplay_h264_parser *p,const struct carplay_encoder_packet *packet) {
    (void)p;
    if(packet->flags==CARPLAY_ENCODER_HEARTBEAT) {
        fprintf(stderr,"FAIL heartbeat forwarded into video parser\n");exit(2);
    }
    consumed++;
    /* Fresh P-frames discarded while waiting for decoder configuration/IDR
     * must not count as progress toward a usable picture. */
    if(scenario==5)return 0;
    c->live_frame_count++;c->has_real_frame=1;c->last_screen_frame_ms=clock_ms;
    if(scenario==6 && consumed==2)atomic_store(&c->live_screen_stop,1);
    return 0;
}
static int resend_last_keyframe(struct airplay_client *c) {c->last_screen_frame_ms=clock_ms;return 0;}
static uint64_t screen_sender_ntp(void) {return 0;}
static int send_placeholder_frame(struct airplay_client *c,uint64_t ntp) {(void)ntp;c->last_screen_frame_ms=clock_ms;return 0;}
'''+body+r'''
int main(void) {
    for(scenario=0;scenario<7;scenario++) {
        struct airplay_client c={0};active=&c;c.encoder.fd=-1;
        starts=stops=key_requests=refreshes=consumed=0;clock_ms=100;last_beat=0;pts=0;first_probe_ms=resume_ms=0;
        if(scenario==2)atomic_store(&c.screen_suspended,1);
        encoder_main(&c);
        if(scenario==0 || scenario==3 || scenario==5) {
            if(starts!=2 || stops!=2 || clock_ms>15000) {
                fprintf(stderr,"FAIL stall recovery scenario=%d starts=%u stops=%u elapsed=%llu\n",
                    scenario,starts,stops,(unsigned long long)clock_ms);return 1;
            }
        } else if(scenario==1) {
            assert(starts==1 && stops==1 && refreshes>0 && consumed>1);
        } else if(scenario==2) {
            assert(starts==1 && stops==1 && !key_requests && !refreshes);
        } else if(scenario==6) {
            if(starts!=1 || stops!=1 || key_requests!=2 || refreshes || consumed!=2 || !resume_ms) {
                fprintf(stderr,"FAIL coalesced resume at probe deadline: starts=%u stops=%u key_requests=%u refreshes=%u frames=%u\n",
                        starts,stops,key_requests,refreshes,consumed);return 1;
            }
        } else {
            assert(starts==3 && stops==3 && atomic_load(&c.video_failure));
        }
    }
    puts("PASS production encoder loop: silent child, healthy idle, suspension, heartbeat-only startup, failure escalation, discarded frames, coalesced resume at probe timeout");
}
'''
with tempfile.TemporaryDirectory() as temp:
    cfile=Path(temp)/"test.c";binary=Path(temp)/"test"
    cfile.write_text(code)
    subprocess.run(["cc","-std=c11","-Wall","-Wextra","-Werror","-I"+str(source.parent),"-I"+str(ROOT/"src"),str(cfile),"-o",str(binary)],check=True)
    subprocess.run([str(binary)],check=True,timeout=5)
