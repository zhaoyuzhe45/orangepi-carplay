"""Run real legacy audio transaction functions with transport/platform seams."""
from pathlib import Path
import plistlib
import subprocess
from airplay_connection_regression_test import function
ROOT=Path(__file__).resolve().parents[1]
OUT=ROOT/'build/legacy-services-20261111/client-audio-test';OUT.mkdir(exist_ok=True)
s=(ROOT/'src/airplay_client.c').read_text()
begin=s.index('struct airplay_client {');state=s[begin:s.index('\n};',begin)+3]
preamble=r'''
#define _GNU_SOURCE
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdatomic.h>
#include <pthread.h>
#include <limits.h>
#include "airplay_client.h"
#include "airplay_protocol.h"
#include "airplay_bplist.h"
#include "airplay_video.h"
#include "carplay_modes.h"
#include "carplay_audio_bridge.h"
#include "carplay_encoder.h"
#include "carplay_capabilities.h"
#include "carplay_identity.h"
#include "carplay_knob.h"
#include "carplay_display.h"
#define AIRPLAY_RESPONSE_BUFFER 131072u
'''
stubs=r'''
static uint64_t clock_ms=1000;static int demand,selected,started,stopped,media_available=1,send_fail,nav_ready,nav_started;static unsigned sent;static char method[32],path[32];
static uint64_t video_monotonic_ms(void){return clock_ms;}
static void iap2_log(const char*s,...){(void)s;}
static void fail_session(struct airplay_client*c,const char*s){(void)s;c->failed=1;}
int carplay_audio_bridge_microphone_demand(struct carplay_audio_bridge*b){(void)b;return demand;}
void carplay_audio_bridge_get_status(const struct carplay_audio_bridge*b,struct carplay_audio_status*s){(void)b;memset(s,0,sizeof(*s));s->media_available=media_available;s->microphone_available=1;}
int carplay_audio_bridge_legacy_select(struct carplay_audio_bridge*b,int call){(void)b;selected=call;return 0;}
int carplay_audio_bridge_active_count(const struct carplay_audio_bridge*b){(void)b;return started;}
void carplay_audio_bridge_legacy_stop_stream(struct carplay_audio_bridge*b){(void)b;started=0;stopped++;}
int carplay_audio_bridge_fill_setups(const struct carplay_audio_bridge*b,struct airplay_stream_setup*v,size_t n){
 (void)b;(void)n;memset(v,0,sizeof(*v));v->type=100;v->legacy_audio=1;v->audio_type=selected?"telephony":"media";v->input=selected;v->audio_format=selected?16:32768;v->control_port=7777;v->data_port=selected?8888:0;return 0;}
int carplay_audio_bridge_legacy_nav_setup(struct carplay_audio_bridge*b,struct airplay_stream_setup*v){
 (void)b;if(!nav_ready)return 0;
 *v=(struct airplay_stream_setup){.type=101,.legacy_audio=1,.audio_type="default",.audio_format=16,.control_port=9876};return 1;}
int carplay_audio_bridge_start_stream(struct carplay_audio_bridge*b,int t,const char*a,uint16_t d,uint16_t c,const char*h){
 (void)b;(void)h;
 if(t==101){if(strcmp(a,"default")||d!=7878||c!=0)return -1;nav_started++;}
 else started=1;
 return 0;}
static int send_request(struct airplay_client*c,const char*m,const char*p,const char*t,unsigned seq,const uint8_t*b,size_t n){(void)c;(void)t;(void)b;(void)n;sent=seq;strcpy(method,m);strcpy(path,p);return send_fail?-1:0;}
static int send_modes_changed(struct airplay_client*c){return send_request(c,"POST","/command",NULL,c->next_control_cseq++,NULL,0);}
static int complete_stream_setup(struct airplay_client*c,const uint8_t*b,size_t n,int audio){(void)b;(void)n;(void)audio;c->audio_port_count=1;c->audio_ports[0].type=100;strcpy(c->audio_ports[0].audio_type,selected?"telephony":"media");c->audio_ports[0].data_port=9999;return 0;}
'''
funcs=''
for signature in ['static int bplist_port(', 'static int legacy_nav_ports(', 'static int legacy_nav_response(', 'static int legacy_audio_command_ok(', 'static void legacy_audio_backoff(', 'static int legacy_audio_setup(', 'static int legacy_audio_response(', 'static int legacy_nav_drive(', 'static int legacy_audio_drive(']:
    if signature in s: funcs+=function(s,signature)+'\n'
if 'static int legacy_audio_drive(' not in s:
    funcs+='static int legacy_audio_drive(struct airplay_client*c){(void)c;return 0;}\n'
def stream_fixture(name, **entry):
    raw=plistlib.dumps({'streams':[entry]}, fmt=plistlib.FMT_BINARY)
    return f'static const uint8_t {name}[]={{'+','.join(str(v) for v in raw)+'};\n'

main=stream_fixture('nav_ok',type=101,dataPort=7878,controlPort=0)+stream_fixture('nav_wrong',type=100,dataPort=7878)+r'''
#define CHECK(x) do{if(!(x)){fprintf(stderr,"FAIL %d: %s\n",__LINE__,#x);return 1;}}while(0)
static int nav_transaction(int reply){
 struct airplay_client c={0};c.profile=AIRPLAY_PROFILE_LEGACY;c.audio_bridge=(void*)1;c.next_control_cseq=1000;
 carplay_modes_init(&c.modes,1);strcpy(c.audio_host,"127.0.0.1");
 c.screen_setup_complete=c.record_accepted=c.initial_modes_accepted=1;
 c.legacy_audio_have_stream=1;c.audio_port_count=1;c.audio_ports[0].data_port=9999;
 clock_ms=1000;sent=0;media_available=1;demand=0;started=1;send_fail=0;nav_ready=1;nav_started=0;
 CHECK(legacy_audio_drive(&c)==0&&!strcmp(method,"SETUP")&&c.legacy_nav_cseq==sent);
 unsigned setup=sent;
 CHECK(legacy_nav_response(&c,200,setup-1,nav_ok,sizeof(nav_ok))==0&&!nav_started);
 if(reply==200) CHECK(legacy_nav_response(&c,200,setup,nav_ok,sizeof(nav_ok))==0&&nav_started==1);
 if(reply==400) CHECK(legacy_nav_response(&c,200,setup,nav_wrong,sizeof(nav_wrong))==0&&!nav_started);
 if(reply==470) CHECK(legacy_nav_response(&c,470,setup,NULL,0)==0&&!nav_started);
 if(reply==408){clock_ms=c.legacy_nav_deadline;CHECK(legacy_audio_drive(&c)==0&&!nav_started);}
 CHECK(!c.failed&&!c.legacy_nav_cseq&&!c.legacy_nav_deadline&&!c.audio_setup_failed);
 CHECK(started&&c.audio_port_count==1&&c.audio_ports[0].data_port==9999);
 CHECK(legacy_nav_response(&c,200,setup,nav_ok,sizeof(nav_ok))==0&&nav_started==(reply==200));
 CHECK(legacy_audio_drive(&c)==0&&sent==setup);
 nav_ready=0;return 0;
}
static int call_release_recovery(int teardown_timeout){
 struct airplay_client c={0};c.profile=AIRPLAY_PROFILE_LEGACY;c.audio_bridge=(void*)1;c.next_control_cseq=1000;
 carplay_modes_init(&c.modes,1);strcpy(c.audio_host,"127.0.0.1");
 c.screen_setup_complete=c.record_accepted=c.initial_modes_accepted=1;
 clock_ms=1000;sent=0;media_available=0;demand=1;started=0;send_fail=0;
 CHECK(legacy_audio_drive(&c)==0&&!strcmp(method,"POST")&&c.modes.phone_call);
 CHECK(legacy_audio_response(&c,200,sent,NULL,0)==0);
 CHECK(legacy_audio_drive(&c)==0&&!strcmp(method,"SETUP"));
 CHECK(legacy_audio_response(&c,200,sent,NULL,0)==0&&started);
 demand=0;CHECK(legacy_audio_drive(&c)==0&&!strcmp(method,"TEARDOWN")&&!started);
 if(teardown_timeout){clock_ms=c.legacy_audio_deadline;CHECK(legacy_audio_drive(&c)==0&&c.audio_setup_failed);}
 else {CHECK(legacy_audio_response(&c,200,sent,NULL,0)==0);CHECK(legacy_audio_drive(&c)==0);}
 CHECK(!strcmp(method,"POST")&&!c.modes.phone_call);
 for(unsigned attempt=0;attempt<6;attempt++){
  unsigned release=sent;
  if(attempt&1u){clock_ms=c.legacy_audio_deadline;CHECK(legacy_audio_drive(&c)==0);}
  else CHECK(legacy_audio_response(&c,470,release,NULL,0)==0);
  CHECK(!c.failed&&!started&&!c.legacy_audio_refused_call);
  CHECK(c.audio_setup_failed==teardown_timeout);
  uint64_t retry=c.legacy_audio_retry_ms;
  CHECK(retry>clock_ms&&retry-clock_ms>=1000&&retry-clock_ms<=4000);
  clock_ms=retry-1;CHECK(legacy_audio_drive(&c)==0&&sent==release);
  clock_ms=retry;CHECK(legacy_audio_drive(&c)==0&&sent>release&&!strcmp(method,"POST")&&!c.modes.phone_call);
  unsigned pending=sent;
  CHECK(legacy_audio_response(&c,200,release,NULL,0)==0); /* late ack cannot satisfy a newer clear */
  CHECK(c.legacy_audio_phase==2&&c.legacy_audio_cseq==pending);
 }
 CHECK(legacy_audio_response(&c,200,sent,NULL,0)==0);
 unsigned accepted=sent;CHECK(legacy_audio_drive(&c)==0);
 clock_ms+=20000;CHECK(legacy_audio_drive(&c)==0&&sent==accepted&&!started&&!c.modes.phone_call);
 if(!teardown_timeout){
  demand=1;CHECK(legacy_audio_drive(&c)==0&&!strcmp(method,"POST")&&c.modes.phone_call);
  CHECK(legacy_audio_response(&c,200,sent,NULL,0)==0);
  CHECK(legacy_audio_drive(&c)==0&&!strcmp(method,"SETUP"));
  CHECK(legacy_audio_response(&c,200,sent,NULL,0)==0&&started);
 }
 printf("phoneCall release retry/rejection/timeout/ack passed (teardown_timeout=%d)\n",teardown_timeout);
 return 0;
}
int main(void){
 struct airplay_client c={0};c.profile=AIRPLAY_PROFILE_LEGACY;c.audio_bridge=(void*)1;c.next_control_cseq=100;
 carplay_modes_init(&c.modes,1);strcpy(c.audio_host,"127.0.0.1");
 CHECK(legacy_audio_drive(&c)==0&&sent==0); /* never before screen */
 c.screen_setup_complete=1;c.record_accepted=1;c.initial_modes_accepted=1;
 CHECK(legacy_audio_drive(&c)==0&&sent==100&&!strcmp(method,"POST"));
'''
if 'static int legacy_audio_response(' in s:
    main+=r'''
 CHECK(legacy_audio_response(&c,200,100,NULL,0)==0);CHECK(legacy_audio_drive(&c)==0&&!strcmp(method,"SETUP")&&!strcmp(path,"/"));
 unsigned setup=sent;CHECK(legacy_audio_response(&c,200,setup-1,NULL,0)==0&&!started);
 CHECK(legacy_audio_response(&c,200,setup,NULL,0)==0&&started&&!c.modes.phone_call);
 demand=1;CHECK(legacy_audio_drive(&c)==0&&!strcmp(method,"TEARDOWN")&&!started);
 unsigned teardown=sent;CHECK(legacy_audio_response(&c,200,teardown,NULL,0)==0);
 CHECK(legacy_audio_drive(&c)==0&&!strcmp(method,"POST")&&c.modes.phone_call);
 unsigned modes=sent;CHECK(legacy_audio_response(&c,200,modes,NULL,0)==0);
 c.modes.audio_borrowed=1;c.modes.audio_entity=2;unsigned previous=sent;
 CHECK(legacy_audio_drive(&c)==0&&sent==previous);
 c.modes.audio_borrowed=0;c.modes.audio_entity=1;
 CHECK(legacy_audio_drive(&c)==0&&!strcmp(method,"SETUP"));setup=sent;
 CHECK(legacy_audio_response(&c,470,setup,NULL,0)==0&&!started);
 clock_ms+=4000;CHECK(legacy_audio_drive(&c)==0&&!strcmp(method,"POST")&&!c.modes.phone_call);
 CHECK(legacy_audio_response(&c,200,sent,NULL,0)==0);CHECK(legacy_audio_drive(&c)==0&&!strcmp(method,"SETUP"));setup=sent;
 CHECK(legacy_audio_response(&c,200,setup,NULL,0)==0&&started);
 previous=sent;CHECK(legacy_audio_drive(&c)==0&&sent==previous); /* no retry storm while demand held */
 demand=0;CHECK(legacy_audio_drive(&c)==0);demand=1;CHECK(legacy_audio_drive(&c)==0&&!strcmp(method,"TEARDOWN"));
 clock_ms+=20000;CHECK(legacy_audio_drive(&c)==0&&!c.failed&&!started);previous=sent;
 CHECK(legacy_audio_drive(&c)==0&&sent==previous); /* uncertain teardown disables optional audio */
 CHECK(legacy_audio_response(&c,200,setup,NULL,0)==0&&!started); /* late setup cannot revive */
 c.audio_setup_failed=0;c.legacy_audio_have_stream=0;c.legacy_audio_phase=0;c.modes.phone_call=0;c.legacy_audio_retry_ms=0;
 media_available=0;demand=0;previous=sent;CHECK(legacy_audio_drive(&c)==0&&sent==previous);
 demand=1;CHECK(legacy_audio_drive(&c)==0&&!strcmp(method,"POST")&&c.modes.phone_call);
 CHECK(legacy_audio_response(&c,200,sent,NULL,0)==0);CHECK(legacy_audio_drive(&c)==0&&!strcmp(method,"SETUP"));
 CHECK(legacy_audio_response(&c,200,sent,NULL,0)==0&&started);demand=0;
 CHECK(legacy_audio_drive(&c)==0&&!strcmp(method,"TEARDOWN"));CHECK(legacy_audio_response(&c,200,sent,NULL,0)==0);
 CHECK(legacy_audio_drive(&c)==0&&!strcmp(method,"POST")&&!c.modes.phone_call);CHECK(legacy_audio_response(&c,200,sent,NULL,0)==0);
 CHECK(legacy_audio_drive(&c)==0);previous=sent;CHECK(legacy_audio_drive(&c)==0&&sent==previous);
 c.audio_setup_failed=0;c.legacy_audio_phase=1;c.legacy_audio_cseq=sent+1;c.legacy_audio_deadline=clock_ms;
 c.legacy_audio_have_stream=1;c.modes.phone_call=1;send_fail=1;
 CHECK(legacy_audio_drive(&c)==-1&&c.failed); /* control-send failure during timeout is fatal */
 previous=sent;CHECK(legacy_audio_drive(&c)==-1&&sent==previous);
'''
main+='CHECK(call_release_recovery(0)==0);CHECK(call_release_recovery(1)==0);CHECK(nav_transaction(200)==0);CHECK(nav_transaction(400)==0);CHECK(nav_transaction(470)==0);CHECK(nav_transaction(408)==0);puts("legacy audio transactions and failure isolation passed");return 0;}\n'
f=OUT/'test.c';f.write_text(preamble+state+stubs+funcs+main)
subprocess.run(['gcc','-std=c11','-Wall','-Wextra','-Werror','-Wno-unused-function','-Wno-unused-variable','-I'+str(ROOT/'src'),str(f),str(ROOT/'src/carplay_modes.c'),str(ROOT/'src/airplay_bplist.c'),'-pthread','-o',str(OUT/'test')],check=True)
subprocess.run([str(OUT/'test')],check=True,timeout=5)
