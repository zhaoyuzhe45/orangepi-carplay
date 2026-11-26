"""Exercise the production send boundary: only a sent real IDR remembers mode."""
import os
from pathlib import Path
import subprocess
import tempfile
from airplay_connection_regression_test import function
root = Path(__file__).resolve().parents[1]
source = (root/'src/airplay_client.c').read_text(encoding='utf-8')
prelude = r'''
#include <stdint.h>
#include <stdlib.h>
#include <stdio.h>
#include <stdatomic.h>
#include <assert.h>
#define AIRPLAY_SCREEN_HEADER_BYTES 128
#define AIRPLAY_CHACHA_TAG_BYTES 16
static void iap2_log(const char *fmt,...){(void)fmt;}
#define pthread_mutex_lock(p) ((void)(p))
#define pthread_mutex_unlock(p) ((void)(p))
struct airplay_client {int screen_data_fd,video_send_mutex,screen_suspended,
 screen_needs_keyframe,screen_config_pending,display_profile_eligible,actual_density;
 unsigned physical_width,physical_height;unsigned long live_frame_count,video_batch_packets;};
static int sent,send_result,config_result;
static int send_cached_screen_config_locked(struct airplay_client *c){(void)c;return config_result;}
static uint64_t screen_sender_ntp(void){return 0;}
static int encode_video_frame(struct airplay_client *c,uint8_t *m,size_t n,size_t *len,
 const uint8_t *p,size_t pn,uint64_t t,int key){(void)c;(void)m;(void)n;(void)p;(void)t;(void)key;*len=pn;return 0;}
static int send_all(int fd,const void *m,size_t n){(void)fd;(void)m;(void)n;sent++;return send_result;}
'''
cases = r'''
int main(void){
 struct airplay_client c={0};uint8_t p[]={1,2,3};c.screen_data_fd=5;
 c.physical_width=1280;c.physical_height=720;c.actual_density=240;
 c.screen_suspended=1;assert(!send_screen_payload(&c,p,sizeof(p),1,1));assert(!sent&&!c.display_profile_eligible);
 c.screen_suspended=0;c.screen_needs_keyframe=1;
 assert(!send_screen_payload(&c,p,sizeof(p),1,0));assert(!sent&&!c.display_profile_eligible);
 config_result=-1;assert(send_screen_payload(&c,p,sizeof(p),1,1)<0);assert(!c.display_profile_eligible);
 config_result=0;send_result=-1;assert(send_screen_payload(&c,p,sizeof(p),1,1)<0);assert(!c.display_profile_eligible);
 send_result=0;assert(!send_screen_payload(&c,p,sizeof(p),1,1));assert(c.display_profile_eligible&&!c.screen_needs_keyframe);
 assert(!send_screen_payload(&c,p,sizeof(p),1,1));assert(c.display_profile_eligible);
 puts("display profile commit: PASS");return 0;
}
'''
with tempfile.TemporaryDirectory() as tmp:
    path=Path(tmp)/'commit.c'
    path.write_text(prelude+function(source,'static int send_screen_payload(')+cases,encoding='utf-8')
    exe=Path(tmp)/('commit.exe' if os.name=='nt' else 'commit')
    subprocess.run(['gcc','-std=c11','-Wall','-Wextra','-Werror','-o',str(exe),str(path)],check=True)
    subprocess.run([str(exe)],check=True)
