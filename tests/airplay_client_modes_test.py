#!/usr/bin/env python3
"""Actual client mode/video/audio functions; mock only transport/codecs/platform."""
import os
from pathlib import Path
import plistlib
import shutil
import subprocess
from airplay_connection_regression_test import function

ROOT = Path(__file__).resolve().parents[1]
OUT = ROOT / 'build/compat-20261111/client-modes-test'
OUT.mkdir(parents=True, exist_ok=True)
source = Path(os.environ.get('CARPLAY_CLIENT_SOURCE', ROOT / 'src/airplay_client.c')).read_text()
start = source.index('struct airplay_client {')
state = source[start:source.index('\n};', start)+3]
prelude = r'''
#define _GNU_SOURCE
#include <stdint.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <stdatomic.h>
#include <pthread.h>
#include <unistd.h>
#include <poll.h>
#include <errno.h>
#include <limits.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <net/if.h>
#include <time.h>
#include "airplay_client.h"
#include "carplay_knob.h"
#include "carplay_display.h"
#include "carplay_keys.h"
#include "airplay_protocol.h"
#include "airplay_bplist.h"
#include "airplay_video.h"
#include "airplay_legacy.h"
#include "carplay_modes.h"
#include "carplay_audio_bridge.h"
#include "carplay_encoder.h"
#include "carplay_capabilities.h"
#include "carplay_identity.h"
#define AIRPLAY_RESPONSE_BUFFER 131072u
static void iap2_log(const char *fmt,...){(void)fmt;}
'''
stubs = r'''
static uint64_t now_ms = 1000;
static uint64_t video_monotonic_ms(void) { return now_ms; }
static uint64_t screen_sender_ntp(void) { return 1; }
static void fail_session(struct airplay_client *c,const char*s) {c->failed=1;snprintf(c->failure,sizeof(c->failure),"%s",s);}
static unsigned last_seq, starts, request_count, control_writes, event_writes, mode_announcements;static int request_failure, setup_failure;static uint8_t request_body[8192];static size_t request_len;
static struct carplay_modes announced[8], replied;
static unsigned writes_at_announcement[8], starts_at_announcement[8];
static int event_failure, announcement_failure, audio_paused, stream_active;
static uint8_t event_body[2048];static size_t event_body_len;
static int send_request(struct airplay_client*c,const char*m,const char*p,const char*t,unsigned seq,const uint8_t*b,size_t n){
 (void)c;(void)m;(void)p;(void)t;last_seq=seq;request_count++;request_len=n;if(n)memcpy(request_body,b,n);return request_failure?-1:0;
}
static int send_all(int fd,const void*b,size_t n){return send(fd,b,n,MSG_NOSIGNAL)==(ssize_t)n?0:-1;}
static int send_control_frames(struct airplay_client*c,const void*b,size_t n,uint64_t started){
 (void)started;if(!c->control_channel_ready)return send_all(c->fd,b,n);control_writes++;return 0;
}
static uint64_t event_header_started;
static int send_event_frames(struct airplay_client*c,const void*b,size_t n,uint64_t started){
 event_writes++;if(n>=8&&!memcmp(b,"bplist00",8)){
  if(started!=event_header_started)abort();
  replied=c->modes;if(n>sizeof(event_body))return -1;memcpy(event_body,b,n);event_body_len=n;
 }else{event_header_started=started;now_ms+=2500;}
 return event_failure?-1:0;
}
static void handle_control_command(struct airplay_client*c,const uint8_t*b,size_t n){(void)c;(void)b;(void)n;}
static int encode_video_config(struct airplay_client*c,uint8_t*out,size_t cap,size_t*n,const uint8_t*b,size_t l,uint64_t ts,unsigned cw,unsigned ch,unsigned dw,unsigned dh){
 (void)c;(void)ts;(void)cw;(void)ch;(void)dw;(void)dh;if(cap<l+1)return -1;out[0]='C';memcpy(out+1,b,l);*n=l+1;return 0;
}
static int encode_video_frame(struct airplay_client*c,uint8_t*out,size_t cap,size_t*n,const uint8_t*b,size_t l,uint64_t ts,int keyframe){
 (void)ts;(void)keyframe;if(cap<l+1)return -1;c->video_write_counter++;out[0]='F';memcpy(out+1,b,l);*n=l+1;return 0;
}
void carplay_keys_suspend(struct carplay_keys *keys,int suspended){(void)keys;(void)suspended;}
size_t carplay_audio_bridge_setup_count(const struct carplay_audio_bridge*b){return b?1:0;}
int carplay_audio_bridge_fill_setups(const struct carplay_audio_bridge*b,struct airplay_stream_setup*s,size_t n){
 if(!b||!n||setup_failure)return -1;memset(s,0,sizeof(*s));s->type=100;s->stream_id=2;s->connection_id=123;s->audio_type="media";s->audio_format=0x8000;return 0;
}
int carplay_audio_bridge_start_stream(struct carplay_audio_bridge*b,int t,const char*a,uint16_t d,uint16_t p,const char*h){
 (void)b;(void)t;(void)a;(void)d;(void)p;(void)h;if(!stream_active){starts++;stream_active=1;}return 0;
}
void carplay_audio_bridge_set_paused(struct carplay_audio_bridge*b,int p){(void)b;audio_paused=p;}
const char*carplay_audio_bridge_last_error(const struct carplay_audio_bridge*b){(void)b;return "test";}
int carplay_audio_bridge_resolve_response(const struct carplay_audio_bridge*b,int t,uint64_t id,char*out,size_t n,size_t*l){
 (void)b;(void)t;(void)id;if(n<6)return -1;memcpy(out,"media",6);*l=5;return 0;
}
int airplay_legacy_keys(const uint8_t secret[16],uint64_t id,uint8_t key[16],uint8_t iv[16]){(void)secret;(void)id;(void)key;(void)iv;return -1;}
struct airplay_legacy_cipher *airplay_legacy_new(const uint8_t key[16],const uint8_t iv[16]){(void)key;(void)iv;return NULL;}
void airplay_legacy_free(struct airplay_legacy_cipher*c){(void)c;}
int airplay_hkdf_sha512(const uint8_t*i,size_t il,const uint8_t*s,size_t sl,const uint8_t*f,size_t fl,uint8_t*o,size_t ol){
 (void)i;(void)il;(void)s;(void)sl;(void)f;(void)fl;(void)o;(void)ol;return -1;
}
int airplay_uinput_touch_up(struct airplay_uinput*b,unsigned x,unsigned y,unsigned w,unsigned h){(void)b;(void)x;(void)y;(void)w;(void)h;return 0;}
static int send_modes_changed(struct airplay_client*c){
 unsigned i=mode_announcements++;if(i<8){announced[i]=c->modes;writes_at_announcement[i]=event_writes;starts_at_announcement[i]=starts;}
 return announcement_failure?-1:0;
}
static int send_deferred_audio_setup(struct airplay_client *client);
static int legacy_audio_drive(struct airplay_client *client){(void)client;return 0;}
static int send_cached_screen_config_locked(struct airplay_client *client);
const uint8_t airplay_screen_test_config[]={9};const size_t airplay_screen_test_config_len=1;
const uint8_t airplay_screen_test_frame[]={7};const size_t airplay_screen_test_frame_len=1;
'''
fixtures = {}
for name, rid, transfer in [('borrow_audio',2,3),('unborrow_audio',2,4),('borrow_screen',1,3),('unborrow_screen',1,4)]:
    fixtures[name]={'type':'changeModes','params':{'resources':[{'resourceID':rid,'transferType':transfer}]}}
fixtures['audio_reply']={'streams':[{'type':110,'dataPort':7777},{'type':100,'streamConnectionID':0,'dataPort':9999}]}
for name,transfer in [('take_both',1),('untake_both',2)]:
    resources=[dict(resourceID=i,transferType=transfer) for i in [1,2]]
    if transfer==1:
        for resource in resources:resource.update(transferPriority=500,takeConstraint=100,borrowConstraint=100)
    fixtures[name]={'type':'changeModes','params':{'resources':resources}}
arrays='\n'.join('static const uint8_t %s[]={%s};'%(name,','.join(map(str,plistlib.dumps(value,fmt=plistlib.FMT_BINARY)))) for name,value in fixtures.items())
signatures=['static const uint8_t *find_header_end(', 'static int header_uint(',
            'int airplay_client_send_screen_setup(', 'int airplay_client_start_audio(',
            'static int send_screen_config(', 'static int send_screen_payload(',
            'static int send_placeholder_frame(', 'int airplay_client_send_test_screen(',
            'static int apply_modes_command(', 'static int process_event_plaintext(']
optional=['static void screen_output_size(', 'static void log_event_buffer_preview(', 'static int send_deferred_audio_setup(', 'static int send_cached_screen_config_locked(', 'static int complete_stream_setup(', 'static int apply_mode_state(']
funcs='\n'.join(function(source,s) for s in optional if s in source)+'\n'+'\n'.join(function(source,s) for s in signatures)
funcs += '\n' + function(source, 'static int send_request(').replace('static int send_request(', 'static int production_send_request(', 1)
main=r'''
#define CHECK(x) do{if(!(x)){fprintf(stderr,"FAIL line%d: %s\n",__LINE__,#x);return 1;}}while(0)
static int change(struct airplay_client*c,const uint8_t*b,size_t n){uint8_t reply[2048];size_t l=0;int accepted=0;return apply_modes_command(c,b,n,reply,sizeof(reply),&l,&accepted)==1&&accepted?0:-1;}
static int event(struct airplay_client*c,const uint8_t*b,size_t n){
 int h=snprintf((char*)c->event_response,sizeof(c->event_response),"POST /command RTSP/1.0\r\nCSeq: 20\r\nContent-Length: %zu\r\n\r\n",n);
 if(h<0||(size_t)h+n>sizeof(c->event_response))return -1;
 memcpy(c->event_response+h,b,n);c->event_response_len=(size_t)h+n;return process_event_plaintext(c,NULL,0);
}
int main(int argc,char**argv){
 struct airplay_client c={0};int pair[2];uint8_t got[100];struct pollfd pfd;
 CHECK(argc==2);CHECK(socketpair(AF_UNIX,SOCK_STREAM,0,pair)==0);
 c.screen_data_fd=pair[0];c.event_fd=8;c.setup_complete=c.screen_setup_complete=c.record_accepted=1;c.audio_bridge=(void*)1;
 c.next_control_cseq=100;carplay_modes_init(&c.modes,1);pthread_mutex_init(&c.video_send_mutex,NULL);pthread_mutex_init(&c.event_send_mutex,NULL);
 pfd.fd=pair[1];pfd.events=POLLIN;
 if(!strcmp(argv[1],"config-resume")||!strcmp(argv[1],"config-resume-legacy")){
  if(!strcmp(argv[1],"config-resume-legacy"))c.profile=AIRPLAY_PROFILE_LEGACY;
  uint8_t first=1,latest=2,frame=3;
  CHECK(change(&c,borrow_screen,sizeof(borrow_screen))==0);
  CHECK(send_screen_config(&c,&first,1)==0);CHECK(send_screen_config(&c,&latest,1)==0);
  CHECK(poll(&pfd,1,0)==0&&c.video_write_counter==0);
  CHECK(change(&c,unborrow_screen,sizeof(unborrow_screen))==0);
  CHECK(send_screen_payload(&c,&frame,1,1,0)==0&&poll(&pfd,1,0)==0);
  CHECK(send_screen_payload(&c,&frame,1,1,1)==0);CHECK(recv(pair[1],got,sizeof(got),MSG_DONTWAIT)==4);
  CHECK(got[0]=='C'&&got[1]==latest&&got[2]=='F'&&got[3]==frame&&c.video_write_counter==1);
 }else if(!strcmp(argv[1],"fixture-borrowed")){
  CHECK(change(&c,borrow_screen,sizeof(borrow_screen))==0);
  CHECK(airplay_client_send_test_screen(&c)>=0);CHECK(poll(&pfd,1,0)==0&&c.video_write_counter==0);
 }else if(!strcmp(argv[1],"audio-prestart")){
  c.audio_port_count=1;c.audio_ports[0].type=100;c.audio_ports[0].data_port=999;strcpy(c.audio_ports[0].audio_type,"media");
  CHECK(change(&c,borrow_audio,sizeof(borrow_audio))==0);
  CHECK(airplay_client_start_audio(&c,"127.0.0.1")==0&&starts==0);
  CHECK(change(&c,unborrow_audio,sizeof(unborrow_audio))==0);CHECK(starts==1);
 }else if(!strcmp(argv[1],"audio-setup-borrowed")){
  struct airplay_bplist_document d;struct airplay_bplist_value r,a;uint64_t n;
  c.screen_setup_complete=0;CHECK(change(&c,borrow_audio,sizeof(borrow_audio))==0);
  CHECK(airplay_client_send_screen_setup(&c)==0);
  CHECK(airplay_bplist_init(&d,request_body,request_len)==0&&airplay_bplist_root(&d,&r)==0);
  CHECK(airplay_bplist_dict_get(&d,&r,"streams",&a)==1&&airplay_bplist_array_count(&d,&a,&n)==0&&n==1);
  c.screen_setup_complete=1;CHECK(airplay_client_start_audio(&c,"127.0.0.1")==0);
  CHECK(change(&c,unborrow_audio,sizeof(unborrow_audio))==0);
  CHECK(airplay_client_start_audio(&c,c.audio_host)==0&&last_seq==11&&request_count==2);
  CHECK(airplay_client_start_audio(&c,c.audio_host)==0&&request_count==2);
  CHECK(airplay_bplist_init(&d,request_body,request_len)==0&&airplay_bplist_root(&d,&r)==0);
  CHECK(airplay_bplist_dict_get(&d,&r,"streams",&a)==1&&airplay_bplist_array_count(&d,&a,&n)==0&&n==1);
 }else if(!strcmp(argv[1],"audio-partial-control-failure")){
  c.audio_setup_deferred=1;setup_failure=1;
  CHECK(airplay_client_start_audio(&c,"127.0.0.1")==0&&c.audio_setup_failed&&!c.failed&&request_count==0);
  c.audio_setup_failed=0;setup_failure=0;request_failure=1;
  CHECK(airplay_client_start_audio(&c,"127.0.0.1")<0);
  CHECK(c.failed&&request_count==1&&request_len>0&&!c.audio_setup_pending);
  CHECK(airplay_client_start_audio(&c,"127.0.0.1")<0&&request_count==1);
  CHECK(send_deferred_audio_setup(&c)<0&&request_count==1);
  c.control_channel_ready=1;
  CHECK(production_send_request(&c,"POST","/feedback",NULL,100,NULL,0)<0&&control_writes==0);
 }else if(!strcmp(argv[1],"audio-event-control-failure")){
  c.audio_setup_deferred=1;request_failure=1;strcpy(c.audio_host,"127.0.0.1");
  CHECK(change(&c,borrow_audio,sizeof(borrow_audio))==0);
  int h=snprintf((char*)c.event_response,sizeof(c.event_response),"POST /command RTSP/1.0\r\nCSeq: 20\r\nContent-Length: %zu\r\n\r\n",sizeof(unborrow_audio));
  CHECK(h>0);memcpy(c.event_response+h,unborrow_audio,sizeof(unborrow_audio));c.event_response_len=(size_t)h+sizeof(unborrow_audio);
  memcpy(c.event_response+c.event_response_len,c.event_response,c.event_response_len);c.event_response_len*=2;
  CHECK(process_event_plaintext(&c,NULL,0)<0&&c.failed&&request_count==1);
  CHECK(event_writes==2&&mode_announcements==1);
 }else if(!strcmp(argv[1],"audio-response-preserves-video")){
  c.profile=AIRPLAY_PROFILE_LEGACY;c.legacy_video=(void*)123;c.screen_data_port=4567;c.video_write_counter=99;
  CHECK(complete_stream_setup(&c,audio_reply,sizeof(audio_reply),1)==0);
  CHECK(c.audio_port_count==1&&c.audio_ports[0].data_port==9999);
  CHECK(c.legacy_video==(void*)123&&c.screen_data_port==4567&&c.video_write_counter==99&&c.screen_setup_complete);
 }else if(!strcmp(argv[1],"hap-normal-setup")){
  struct airplay_bplist_document d;struct airplay_bplist_value r,a;uint64_t n;
  c.screen_setup_complete=0;CHECK(airplay_client_send_screen_setup(&c)==0);
  CHECK(airplay_bplist_init(&d,request_body,request_len)==0&&airplay_bplist_root(&d,&r)==0);
  CHECK(airplay_bplist_dict_get(&d,&r,"streams",&a)==1&&airplay_bplist_array_count(&d,&a,&n)==0&&n==2);
 }else if(!strcmp(argv[1],"initial-modes-gate")){
  c.profile=AIRPLAY_PROFILE_LEGACY;c.screen_setup_complete=0;
  CHECK(airplay_client_send_screen_setup(&c)==1&&request_count==0);
 }else if(!strncmp(argv[1],"permanent-",10)){
  int disabled=!strcmp(argv[1],"permanent-disabled-audio");
  int fail_response=!strcmp(argv[1],"permanent-failed-response");
  int fail_notify=!strcmp(argv[1],"permanent-failed-notification");
  if(!strcmp(argv[1],"permanent-legacy"))c.profile=AIRPLAY_PROFILE_LEGACY;
  carplay_modes_init(&c.modes,!disabled);
  c.audio_port_count=1;c.audio_ports[0].type=100;c.audio_ports[0].data_port=999;strcpy(c.audio_ports[0].audio_type,"media");
  strcpy(c.audio_host,"127.0.0.1");
  CHECK(event(&c,take_both,sizeof(take_both))==0);
  CHECK(atomic_load(&c.screen_suspended)&&!atomic_load(&c.screen_resume_generation)&&audio_paused&&starts==0);
  CHECK(mode_announcements==1&&announced[0].screen_entity==2&&announced[0].audio_entity==2);
  uint8_t config=2,frame=3;CHECK(send_screen_config(&c,&config,1)==0);
  event_failure=fail_response;announcement_failure=fail_notify;
  int rc=event(&c,untake_both,sizeof(untake_both));
  if(fail_response||fail_notify){
   CHECK(rc<0&&c.modes.screen_entity==0&&c.modes.audio_entity==0);
   CHECK(atomic_load(&c.screen_suspended)&&audio_paused&&starts==0);
   CHECK(mode_announcements==(fail_response?1u:2u));
  }else{
   CHECK(rc==0&&mode_announcements==3&&event_writes==4);
   CHECK(replied.screen_entity==0&&replied.audio_entity==0);
   struct airplay_bplist_document doc;struct airplay_bplist_value root,params,resources,resource,value;uint64_t number;
   CHECK(airplay_bplist_init(&doc,event_body,event_body_len)==0&&airplay_bplist_root(&doc,&root)==0);
   CHECK(airplay_bplist_dict_get(&doc,&root,"status",&value)==1&&airplay_bplist_get_uint64(&doc,&value,&number)==0&&number==0);
   CHECK(airplay_bplist_dict_get(&doc,&root,"params",&params)==1&&airplay_bplist_dict_get(&doc,&params,"resources",&resources)==1);
   for(unsigned i=0;i<2;i++){
    CHECK(airplay_bplist_array_get(&doc,&resources,i,&resource)==0);
    CHECK(airplay_bplist_dict_get(&doc,&resource,"entity",&value)==1&&airplay_bplist_get_uint64(&doc,&value,&number)==0&&number==0);
    CHECK(airplay_bplist_dict_get(&doc,&resource,"permanentEntity",&value)==1&&airplay_bplist_get_uint64(&doc,&value,&number)==0&&number==0);
   }
   CHECK(announced[1].screen_entity==0&&announced[1].audio_entity==0&&starts_at_announcement[1]==0);
   CHECK(writes_at_announcement[1]==4&&writes_at_announcement[2]==4);
   CHECK(announced[2].screen_entity==1&&announced[2].audio_entity==(disabled?0:1));
   CHECK(!atomic_load(&c.screen_suspended)&&atomic_load(&c.screen_needs_keyframe)&&atomic_load(&c.force_key_frame));
   CHECK(atomic_load(&c.screen_resume_generation)==1);
   CHECK(starts==((disabled||c.profile==AIRPLAY_PROFILE_LEGACY)?0u:1u)&&audio_paused==disabled);
   CHECK(send_screen_payload(&c,&frame,1,1,0)==0&&poll(&pfd,1,0)==0);
   CHECK(send_screen_payload(&c,&frame,1,1,1)==0&&recv(pair[1],got,sizeof(got),MSG_DONTWAIT)==4);
   CHECK(got[0]=='C'&&got[1]==config&&got[2]=='F'&&got[3]==frame);
   CHECK(event(&c,untake_both,sizeof(untake_both))==0&&mode_announcements==4);
   CHECK(c.modes.screen_entity==1&&c.modes.audio_entity==(disabled?0:1));
  }
 }else return 2;
 close(pair[0]);close(pair[1]);pthread_mutex_destroy(&c.video_send_mutex);pthread_mutex_destroy(&c.event_send_mutex);return 0;
}
'''
target=OUT/'client_modes_test.c';target.write_text(prelude+state+stubs+arrays+funcs+main)
cc=os.environ.get('CC') or shutil.which('gcc') or shutil.which('clang')
binary=OUT/'client_modes_test'
subprocess.run([cc,'-std=c11','-Wall','-Wextra','-Werror','-Wno-unused-function','-Wno-misleading-indentation','-I'+str(ROOT/'src'),str(target),str(ROOT/'src/airplay_bplist.c'),str(ROOT/'src/carplay_modes.c'),str(ROOT/'src/carplay_capabilities.c'),str(ROOT/'src/airplay_protocol.c'),'-pthread','-o',str(binary)],check=True)
failures=0
for case in ['config-resume','config-resume-legacy','fixture-borrowed','audio-prestart','audio-setup-borrowed','audio-partial-control-failure','audio-event-control-failure','audio-response-preserves-video','hap-normal-setup','initial-modes-gate','permanent-resume','permanent-legacy','permanent-disabled-audio','permanent-failed-response','permanent-failed-notification']:
    r=subprocess.run([str(binary),case],capture_output=True,text=True,timeout=5)
    print(('PASS: ' if r.returncode==0 else 'FAIL: ')+case)
    if r.stderr:print(r.stderr.strip())
    failures+=r.returncode!=0
raise SystemExit(bool(failures))
