"""Exercise actual input parser and dispatch with platform boundaries stubbed."""
from pathlib import Path
import os, plistlib, subprocess
from airplay_connection_regression_test import function
ROOT=Path(__file__).resolve().parents[1]
OUT=ROOT/'build/app-integration-20261111/input';OUT.mkdir(parents=True,exist_ok=True)
fixtures={f'voice{i}':{'type':'requestSiri','params':{'siriAction':i}} for i in (0,1,2,3,4)}
fixtures.update({'bad':{'type':'requestSiri','params':{'siriAction':True}},
                 'media':{'type':'hidSendReport','uuid':'2a2a2a2c','hidReport':b'\x04'},
                 'release':{'type':'hidSendReport','uuid':'2a2a2a2c','hidReport':b'\0'},
                 'touch':{'type':'hidSendReport','uuid':'2a2a2a2a','hidReport':b'\x01\x01\x01\0\x02\0'}})
# Captured /info descriptors from the reference Audi and DiPlay tests.
audi = bytes.fromhex(
    '05 0c 09 01 a1 01 15 00 25 08 05 0c 0a 00 00 00 0a b0 00 0a b1 00 '
    '0a b3 00 0a b4 00 0a b5 00 0a b6 00 0a b9 00 0a bc 00 75 08 95 01 81 00 c0')
diplay = bytes.fromhex(
    '05 0c 09 01 a1 01 15 00 25 06 05 0c 0a 00 00 00 0a b0 00 0a b1 00 '
    '0a cd 00 0a b5 00 0a b6 00 0a 9e 02 75 08 95 01 81 00 c0')
# Same wire values with the opposite Usage order, matching the reported symptom.
reversed_tracks = diplay.replace(bytes.fromhex('0a b5 00 0a b6 00'),
                                bytes.fromhex('0a b6 00 0a b5 00'))
fixtures['media_info'] = {'hidDevices': [
    {'uuid': uuid, 'hidDescriptor': descriptor}
    for uuid, descriptor in [('0', audi), ('2a2a2a2c', diplay), ('remote', reversed_tracks)]]}
for name, uuid, values in [('audi', '0', (4, 5, 6, 7, 0)),
                           ('diplay', '2a2a2a2c', (3, 4, 5, 0)),
                           ('remote', 'remote', (1, 2, 4, 5, 0))]:
    for value in values:
        fixtures[f'{name}{value}'] = {'type': 'hidSendReport', 'uuid': uuid,
                                     'hidReport': bytes([value])}
fixtures['malformed_media'] = {'type': 'hidSendReport', 'uuid': 'remote', 'hidReport': b'\x05\x00'}
arrays='\n'.join('static const uint8_t %s[]={%s};'%(k,','.join(map(str,plistlib.dumps(v,fmt=plistlib.FMT_BINARY)))) for k,v in fixtures.items())
source=(ROOT/'src/airplay_client.c').read_text()
prelude=r'''
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include <stdatomic.h>
#include "airplay_input.h"
#include "carplay_capabilities.h"
#include "carplay_keys.h"
struct airplay_client {struct carplay_session_capabilities capabilities;atomic_int screen_suspended;
 struct carplay_phone_hid_state phone_reports[CARPLAY_MAX_HID_DEVICES];
 struct carplay_knob_layout knob_layouts[CARPLAY_MAX_HID_DEVICES];
 struct carplay_knob_state knob_states[CARPLAY_MAX_HID_DEVICES];
 struct carplay_keys *key_input; unsigned long knob_reports,knob_rejected;
 unsigned knob_buttons; signed char knob_x,knob_y,knob_wheel;int force_key_frame;uint8_t media_reports[CARPLAY_MAX_HID_DEVICES],media_report;};
int carplay_keys_submit(struct carplay_keys*k,const struct carplay_key_batch*b){(void)k;(void)b;return 0;}
static unsigned touches,keys,commands,voices;static int bridge_result, last_media_key, last_fallback_key;
static void iap2_log(const char *s,...){(void)s;}
int carplay_app_command(uint32_t op,uint32_t arg){if(op==1){last_media_key=(int)arg;commands++;}else{assert(op==2);voices++;}return bridge_result;}
static int run_input_keycode(int k){if(k){last_fallback_key=k;keys++;}return 0;}
static void handle_touch_report(struct airplay_client*c,const struct carplay_hid_device_capabilities*d,const uint8_t*r,size_t n){(void)c;(void)d;(void)r;(void)n;touches++;}
static void handle_keyboard_report(struct airplay_client*c,const uint8_t*r,size_t n){(void)c;(void)r;(void)n;keys++;}
static void apply_car_night_mode(struct airplay_client*c,int n){(void)c;(void)n;}
'''
main=r'''
static void expect_media(struct airplay_client *c, const uint8_t *body, size_t n,
                         int key, const char *label)
{
    unsigned before=commands;
    handle_control_command(c,body,n);
    if (commands != before+(key!=0) || (key && last_media_key!=key)) {
        fprintf(stderr, "%s: expected key=%d, got key=%d (dispatched=%u)\n",
                label,key,last_media_key,commands-before);
        assert(0);
    }
}
#define MEDIA(fixture, key) expect_media(&c, fixture, sizeof(fixture), key, #fixture)
int main(void){struct airplay_client c={0};struct airplay_input_command p;
 assert(!airplay_input_parse(voice0,sizeof(voice0),&p));
 assert(!airplay_input_parse(voice4,sizeof(voice4),&p));
 assert(!airplay_input_parse(bad,sizeof(bad),&p));
 assert(airplay_input_parse(voice2,sizeof(voice2),&p));
 atomic_store(&c.screen_suspended,1);
 handle_control_command(&c,media,sizeof(media));assert(commands==1&&keys==0&&last_media_key==87);
 handle_control_command(&c,media,sizeof(media));assert(commands==1);
 handle_control_command(&c,release,sizeof(release));
 bridge_result=-1;handle_control_command(&c,media,sizeof(media));assert(keys==1);
 handle_control_command(&c,release,sizeof(release));bridge_result=-2;
 handle_control_command(&c,media,sizeof(media));assert(keys==1); /* uncertain must not double-skip */
 handle_control_command(&c,touch,sizeof(touch));assert(touches==0);
 atomic_store(&c.screen_suspended,0);handle_control_command(&c,touch,sizeof(touch));assert(touches==1);
 handle_control_command(&c,voice1,sizeof(voice1));handle_control_command(&c,voice2,sizeof(voice2));handle_control_command(&c,voice3,sizeof(voice3));assert(voices==3);
 memset(&c,0,sizeof(c));bridge_result=0;
 assert(carplay_capabilities_parse(media_info,sizeof(media_info),&c.capabilities)==0);
 assert(c.capabilities.has_media&&c.capabilities.hid_count==3);
 MEDIA(remote4,88); MEDIA(remote4,0); /* same held button */
 MEDIA(diplay4,87); /* same value, different device: not a duplicate */
 MEDIA(remote4,0); MEDIA(remote0,0); MEDIA(remote4,88);
 MEDIA(malformed_media,0); MEDIA(remote5,87); MEDIA(remote0,0);
 MEDIA(remote1,126); MEDIA(remote2,127);
 MEDIA(audi5,87); MEDIA(audi6,88); MEDIA(audi4,0); MEDIA(audi7,0); MEDIA(audi0,0);
 MEDIA(diplay0,0); MEDIA(diplay3,85); MEDIA(diplay4,87); MEDIA(diplay5,88);
 puts("app input dispatch: PASS");return 0;}
'''
test=OUT/'test.c';test.write_text(prelude+arrays+'\nstatic void '+function(source,'handle_hid_report')+'\nstatic void '+function(source,'handle_control_command')+main)
exe=OUT/('test.exe' if os.name == 'nt' else 'test')
subprocess.run(['gcc','-std=c11','-Wall','-Wextra','-Werror','-I'+str(ROOT/'src'),str(test),str(ROOT/'src/carplay_knob.c'),str(ROOT/'src/airplay_input.c'),str(ROOT/'src/airplay_bplist.c'),str(ROOT/'src/carplay_capabilities.c'),'-lm','-o',str(exe)],check=True)
subprocess.run([str(exe)],check=True)
