#!/usr/bin/env python3
"""Execute production session ladder with only transport/platform boundaries stubbed."""
import os
from pathlib import Path
import plistlib
import shutil
import subprocess
from airplay_connection_regression_test import function

ROOT = Path(__file__).resolve().parents[1]
OUT = ROOT / "build/compat-20261111/profile-test"
OUT.mkdir(parents=True, exist_ok=True)
source = Path(os.environ.get("CARPLAY_CLIENT_SOURCE", ROOT / "src/airplay_client.c")).read_text()
fixtures = {
    "legacy_setup": {"eventPort": 7001, "timingPort": 0, "keepAlivePort": 7002},
    "hap_setup": {"eventPort": 7001, "timingPort": 7003, "enabledFeatures": ["iAPChannel"]},
    "info": {"displays": [{"widthPixels": 1280, "heightPixels": 720}]},
    "modes_ok": {"status": 0},
    "modes_refused": {"status": 1},
}
arrays = "\n".join("static const uint8_t %s[]={%s};" % (name, ",".join(map(str, plistlib.dumps(obj, fmt=plistlib.FMT_BINARY)))) for name, obj in fixtures.items())
prelude = r'''
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <limits.h>
#include <stdatomic.h>
#include <pthread.h>
#include <errno.h>
#include <unistd.h>
#include "airplay_bplist.h"
#include "carplay_capabilities.h"
#include "airplay_client.h"
#include "airplay_video.h"
static unsigned unexpected_logs;
static void iap2_log(const char *fmt,...){if(strstr(fmt,"unexpected RTSP response"))unexpected_logs++;}
struct airplay_timing { int unused; };
struct airplay_client {
 int fd, profile, wants_hap_fallback, info_requested, info_complete;
 int setup_sent, setup_complete, failed, keep_alive_port_present;
 int pair_setup_requested, pairing_proven, pair_setup_m5_sent, pair_setup_complete;
 int pair_verify_requested, pair_verify_m2_verified, pair_verify_m3_sent;
 int pair_verify_complete, auth_setup_sent, auth_setup_complete;
 int screen_setup_sent, screen_setup_complete, record_sent, record_accepted;
 int screen_data_fd, event_fd, keepalive_stats_body;
 int keepalive_started,initial_modes_accepted,audio_setup_pending,audio_setup_failed;
 unsigned pending_initial_modes_cseq,next_control_cseq,legacy_audio_cseq,legacy_nav_cseq;
 unsigned post_handshake_refusals;
 uint64_t initial_modes_deadline_ms,audio_setup_deadline_ms;
 char audio_host[128];
 pthread_mutex_t video_send_mutex;atomic_int screen_suspended,video_failure,live_screen_stop;
 pthread_t encoder_thread;
 int live_screen_attempted,live_screen_started,has_real_frame;
 unsigned placeholder_frames,still_refreshes;
 unsigned requested_width, requested_height;
 uint16_t timing_port,event_port,keep_alive_port;
 uint64_t handshake_deadline_ms,last_feedback_ms,feedback_deadline_ms;
 unsigned pending_handshake_cseq,pending_feedback_cseq;
 char failure[256],display_error[192];
 struct airplay_timing *timing;
 struct carplay_session_capabilities capabilities;
};
static unsigned encoder_thread_creates;
static int fail_encoder_thread_create(pthread_t *thread,const pthread_attr_t *attr,
        void *(*worker)(void *),void *context) {
 (void)thread;(void)attr;(void)worker;(void)context;encoder_thread_creates++;return EAGAIN;
}
#define pthread_create fail_encoder_thread_create
static void *encoder_main(void *arg){(void)arg;abort();}
static void reset_encoder_picture(struct airplay_client *c){(void)c;}
static int sent_count; static unsigned sent[32]; static char sent_path[32][64];
static uint64_t now_ms=1000;static int audio_started,audio_completed,audio_failure;
static int audio_prepared,legacy_responses,legacy_drives,legacy_result;
static unsigned legacy_status,legacy_response_cseq;
static const uint8_t *legacy_body;static size_t legacy_body_len;
static uint8_t last_body[8192]; static size_t last_body_len;
static int send_request(struct airplay_client*c,const char*m,const char*p,const char*t,unsigned seq,const uint8_t*b,size_t n) {
 (void)c;(void)m;(void)t; sent[sent_count]=seq; snprintf(sent_path[sent_count++],64,"%s",p);
 if(c->profile==AIRPLAY_PROFILE_LEGACY&&seq==1&&!c->keepalive_started)return -1;
 if(n>sizeof(last_body))return -1; if(n)memcpy(last_body,b,n);last_body_len=n;return 0;
}
static uint64_t video_monotonic_ms(void){return now_ms;}
static void fail_session(struct airplay_client*c,const char*s){c->failed=1;snprintf(c->failure,sizeof(c->failure),"%s",s);}
static int airplay_timing_start(struct airplay_timing**t,int fd){static struct airplay_timing timer;(void)fd;*t=&timer;return 0;}
static uint16_t airplay_timing_port(struct airplay_timing*t){(void)t;return 7004;}
static int airplay_timing_error(struct airplay_timing*t){(void)t;return 0;}
static void apply_car_night_mode(struct airplay_client*c,int n){(void)c;(void)n;}
static int display_result;
static int apply_native_display(struct airplay_client*c){(void)c;return display_result;}
static void prepare_knob_layouts(struct airplay_client*c,const uint8_t*b,size_t n){(void)c;(void)b;(void)n;}
static int send_modes_changed(struct airplay_client*c){c->next_control_cseq++;return 0;}
static int start_keepalive_worker(struct airplay_client*c){c->keepalive_started=1;return 0;}
/* The profile ladder dispatches into these independently tested audio modules.
 * Record the calls/results so the seam cannot hide a wrong profile or CSeq. */
static void prepare_audio_bridge(struct airplay_client*c){(void)c;audio_prepared++;}
static int legacy_audio_response(struct airplay_client*c,unsigned status,unsigned cseq,const uint8_t*b,size_t n){
 (void)c;legacy_responses++;legacy_status=status;legacy_response_cseq=cseq;legacy_body=b;legacy_body_len=n;return legacy_result;
}
static int legacy_audio_drive(struct airplay_client*c){(void)c;legacy_drives++;return legacy_result;}
static int legacy_nav_response(struct airplay_client*c,unsigned status,unsigned cseq,const uint8_t*b,size_t n){
 (void)c;(void)status;(void)cseq;(void)b;(void)n;return 0;
}
static int complete_stream_setup(struct airplay_client*c,const uint8_t*b,size_t n,int a){(void)c;(void)b;(void)n;(void)a;audio_completed++;return 0;}
int airplay_client_start_audio(struct airplay_client*c,const char*h){(void)h;audio_started++;if(audio_failure==2)c->failed=1;return audio_failure?-1:0;}
static int send_all(int fd,const void*b,size_t n){(void)fd;(void)b;(void)n;return 0;}
int airplay_screen_encode_keepalive(uint8_t*b,size_t c,size_t*n,int stats){(void)b;(void)c;(void)stats;*n=0;return 0;}
#define STEP_STUB(name) static int name(struct airplay_client*c,const uint8_t*b,size_t n){(void)c;(void)b;(void)n;return 0;}
STEP_STUB(parse_state_two)
STEP_STUB(verify_state_four)
STEP_STUB(complete_pair_setup_state_six)
STEP_STUB(complete_pair_verify_state_two)
STEP_STUB(complete_pair_verify_state_four)
STEP_STUB(complete_auth_setup)
STEP_STUB(complete_screen_setup)
'''
signatures = ["static int send_setup(", "static int bplist_port(", "static int complete_setup(", "static const char *rtsp_step_name(", "static int process_response(", "int airplay_client_start(", "int airplay_client_send_record(", "int airplay_client_setup_complete(", "int airplay_client_tick(", "int airplay_client_start_live_screen("]
funcs = "\n".join(function(source, s) for s in signatures)
main = r'''
#define CHECK(x) do { if(!(x)){fprintf(stderr,"FAIL line %d: %s\n",__LINE__,#x);return 1;} }while(0)
int main(int argc,char**argv){
 struct airplay_client c={0}; struct airplay_bplist_document doc;struct airplay_bplist_value root,v;uint64_t et=0;
 if(argc!=2)return 2; c.profile=AIRPLAY_PROFILE_LEGACY;c.event_fd=8;c.screen_data_fd=-1;c.next_control_cseq=100;
 if(!strcmp(argv[1],"legacy-start")){
  CHECK(airplay_client_start(&c)==0);CHECK(sent_count==1&&sent[0]==8&&!strcmp(sent_path[0],"/setup"));
  CHECK(airplay_bplist_init(&doc,last_body,last_body_len)==0&&airplay_bplist_root(&doc,&root)==0);
  CHECK(airplay_bplist_dict_get(&doc,&root,"et",&v)==1&&airplay_bplist_get_uint64(&doc,&v,&et)==0&&et==1);
 } else if(!strcmp(argv[1],"legacy-sequence")){
  c.setup_sent=1; CHECK(process_response(&c,200,8,legacy_setup,sizeof(legacy_setup))==0);
  CHECK(c.setup_complete&&sent_count==1&&sent[0]==1);CHECK(!airplay_client_setup_complete(&c));
  CHECK(process_response(&c,200,1,info,sizeof(info))==0);CHECK(airplay_client_setup_complete(&c));CHECK(audio_prepared==1);
  CHECK(!c.pair_setup_requested&&sent_count==1);CHECK(airplay_client_send_record(&c)==0&&sent[1]==10);
 } else if(!strcmp(argv[1],"hap-unchanged")){
  c.profile=AIRPLAY_PROFILE_HAP;CHECK(airplay_client_start(&c)==0&&sent[0]==1);
  CHECK(process_response(&c,200,1,info,sizeof(info))==0&&sent[1]==2&&c.pair_setup_requested);
  CHECK(audio_prepared==0);
  c.setup_sent=1;CHECK(process_response(&c,200,8,hap_setup,sizeof(hap_setup))==0&&c.setup_complete);
  CHECK(airplay_client_send_record(&c)<0);
 } else if(!strcmp(argv[1],"legacy-pair-required")){
  c.setup_sent=1;CHECK(process_response(&c,470,8,NULL,0)<0&&c.failed&&c.wants_hap_fallback);
 } else if(!strcmp(argv[1],"no-unsafe-fallback")){
  c.setup_sent=1;CHECK(process_response(&c,500,8,NULL,0)<0&&c.failed&&!c.wants_hap_fallback);
  memset(&c,0,sizeof(c));c.profile=AIRPLAY_PROFILE_LEGACY;c.screen_setup_sent=1;
  CHECK(process_response(&c,470,9,NULL,0)<0&&!c.wants_hap_fallback);
  memset(&c,0,sizeof(c));c.profile=AIRPLAY_PROFILE_HAP;c.setup_sent=1;
  CHECK(process_response(&c,470,8,NULL,0)<0&&!c.wants_hap_fallback);
 } else if(!strcmp(argv[1],"initial-modes-ack")){
  c.record_sent=1;CHECK(process_response(&c,200,10,NULL,0)==0);
  CHECK(c.pending_initial_modes_cseq==100&&!c.initial_modes_accepted&&c.initial_modes_deadline_ms==6000);
  CHECK(process_response(&c,200,100,modes_ok,sizeof(modes_ok))==0&&c.initial_modes_accepted&&!c.pending_initial_modes_cseq);
 } else if(!strcmp(argv[1],"initial-modes-refused")){
  c.record_sent=1;CHECK(process_response(&c,200,10,NULL,0)==0);
  CHECK(process_response(&c,200,100,modes_refused,sizeof(modes_refused))<0&&c.failed&&!c.initial_modes_accepted);
 } else if(!strcmp(argv[1],"initial-modes-timeout")){
  c.record_sent=1;CHECK(process_response(&c,200,10,NULL,0)==0);
  now_ms=6000;CHECK(airplay_client_tick(&c)<0&&c.failed&&!c.initial_modes_accepted);
 } else if(!strcmp(argv[1],"deferred-audio-response")){
  c.profile=AIRPLAY_PROFILE_HAP;c.audio_setup_pending=1;c.audio_setup_deadline_ms=6000;strcpy(c.audio_host,"127.0.0.1");
  CHECK(process_response(&c,200,11,modes_ok,sizeof(modes_ok))==0);
  CHECK(audio_completed==1&&audio_started==1&&!c.audio_setup_pending&&!c.failed);
  c.audio_setup_pending=1;audio_failure=1;
  CHECK(process_response(&c,200,11,modes_ok,sizeof(modes_ok))==0&&!c.failed);
  c.audio_setup_pending=1;audio_failure=2;
  CHECK(process_response(&c,200,11,modes_ok,sizeof(modes_ok))<0&&c.failed);
 } else if(!strcmp(argv[1],"deferred-audio-refused")){
  c.profile=AIRPLAY_PROFILE_HAP;c.audio_setup_pending=1;c.audio_setup_deadline_ms=6000;
  CHECK(process_response(&c,500,11,NULL,0)==0&&c.audio_setup_failed&&!c.failed&&!c.audio_setup_pending);
 } else if(!strcmp(argv[1],"deferred-audio-timeout")){
  c.profile=AIRPLAY_PROFILE_HAP;c.audio_setup_pending=1;c.audio_setup_deadline_ms=6000;now_ms=6000;
  CHECK(airplay_client_tick(&c)==0&&c.audio_setup_failed&&!c.failed&&!c.audio_setup_pending);
  CHECK(process_response(&c,200,11,modes_ok,sizeof(modes_ok))==0&&audio_completed==0&&audio_started==0);
 } else if(!strcmp(argv[1],"expected-post-handshake-acks")){
  c.pending_feedback_cseq=100;c.feedback_deadline_ms=6000;c.next_control_cseq=103;
  CHECK(process_response(&c,200,100,NULL,0)==0&&!c.pending_feedback_cseq&&!c.feedback_deadline_ms);
  CHECK(process_response(&c,200,101,modes_ok,sizeof(modes_ok))==0&&unexpected_logs==0);
 } else if(!strcmp(argv[1],"legacy-audio-response-dispatch")){
  c.legacy_audio_cseq=150;c.next_control_cseq=160;legacy_result=-1;
  CHECK(process_response(&c,470,150,modes_refused,sizeof(modes_refused))==-1);
  CHECK(legacy_responses==1&&legacy_status==470&&legacy_response_cseq==150&&legacy_body==modes_refused&&legacy_body_len==sizeof(modes_refused));
  CHECK(process_response(&c,200,151,NULL,0)==0&&legacy_responses==1);
  c.profile=AIRPLAY_PROFILE_HAP;
  CHECK(process_response(&c,200,150,NULL,0)==0&&legacy_responses==1);
 } else if(!strcmp(argv[1],"legacy-audio-drive-dispatch")){
  CHECK(airplay_client_tick(&c)==0&&legacy_drives==1);
  legacy_result=-1;CHECK(airplay_client_tick(&c)==-1&&legacy_drives==2);
  c.profile=AIRPLAY_PROFILE_HAP;CHECK(airplay_client_tick(&c)==0&&legacy_drives==2);
 } else if(!strcmp(argv[1],"video-thread-create-failure")){
  c.setup_sent=1;
  CHECK(process_response(&c,200,8,legacy_setup,sizeof(legacy_setup))==0&&c.setup_complete);
  CHECK(process_response(&c,200,1,info,sizeof(info))==0&&airplay_client_setup_complete(&c));
  c.screen_data_fd=9; /* Screen socket established by the connector. */
  CHECK(airplay_client_send_record(&c)==0&&sent[sent_count-1]==10);
  CHECK(process_response(&c,200,10,NULL,0)==0&&c.record_accepted);
  CHECK(airplay_client_start_live_screen(&c)==-1&&encoder_thread_creates==1);
  CHECK(!c.live_screen_started&&!c.failed&&atomic_load(&c.video_failure));
  CHECK(airplay_client_tick(&c)<0&&c.failed&&strstr(c.failure,"video recovery"));
 } else if(!strcmp(argv[1],"video-recovery-failure")){
  atomic_store(&c.video_failure,1);
  CHECK(!c.failed);
  CHECK(airplay_client_tick(&c)<0&&c.failed&&strstr(c.failure,"video recovery"));
  CHECK(legacy_drives==0);
 } else if(!strcmp(argv[1],"busy-video-control")){
  c.record_accepted=1;c.screen_data_fd=3;
  CHECK(pthread_mutex_init(&c.video_send_mutex,NULL)==0);
  CHECK(pthread_mutex_lock(&c.video_send_mutex)==0);
  alarm(2);
  CHECK(airplay_client_tick(&c)==0&&!c.failed&&c.pending_feedback_cseq);
  now_ms=c.feedback_deadline_ms;
  CHECK(airplay_client_tick(&c)<0&&c.failed);
  alarm(0);
  pthread_mutex_unlock(&c.video_send_mutex);pthread_mutex_destroy(&c.video_send_mutex);
 } else return 2;
 return 0;
}
'''
target = OUT / "profile_test.c"
target.write_text(prelude+arrays+funcs+main)
compiler = os.environ.get("CC") or shutil.which("gcc") or shutil.which("clang")
binary = OUT / ("profile_test.exe" if os.name == "nt" else "profile_test")
subprocess.run([compiler, "-std=c11", "-D__USE_MINGW_ANSI_STDIO=1", "-Wall", "-Wextra", "-Werror", "-Wno-unused-function", "-Wno-misleading-indentation", "-I"+str(ROOT/"src"), str(target), str(ROOT/"src/airplay_bplist.c"), str(ROOT/"src/carplay_capabilities.c"), "-pthread", "-o", str(binary)], check=True)
failures = 0
for case in ("legacy-start","legacy-sequence","hap-unchanged","legacy-pair-required","no-unsafe-fallback","initial-modes-ack","initial-modes-refused","initial-modes-timeout","deferred-audio-response","deferred-audio-refused","deferred-audio-timeout","expected-post-handshake-acks","legacy-audio-response-dispatch","legacy-audio-drive-dispatch","video-thread-create-failure","video-recovery-failure","busy-video-control"):
    result = subprocess.run([str(binary),case], capture_output=True, text=True, timeout=5)
    print(("PASS: " if result.returncode==0 else "FAIL: ")+case)
    if result.stderr: print(result.stderr.strip())
    failures += result.returncode!=0
raise SystemExit(bool(failures))
