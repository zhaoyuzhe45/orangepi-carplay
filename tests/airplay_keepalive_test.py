#!/usr/bin/env python3
"""Real loopback UDP timing and worker teardown; no device/network changes."""
import os
from pathlib import Path
import plistlib
import shutil
import subprocess
from airplay_connection_regression_test import function
ROOT=Path(__file__).resolve().parents[1]
OUT=ROOT/'build/compat-20261111/keepalive-test';OUT.mkdir(parents=True,exist_ok=True)
source=Path(os.environ.get('CARPLAY_CLIENT_SOURCE',ROOT/'src/airplay_client.c')).read_text()
start=source.index('struct airplay_client {');state=source[start:source.index('\n};',start)+3]
prelude=r'''
#define _GNU_SOURCE
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdatomic.h>
#include <pthread.h>
#include <fcntl.h>
#include <errno.h>
#include <poll.h>
#include <unistd.h>
#include <time.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <net/if.h>
#include "airplay_client.h"
#include "airplay_protocol.h"
#include "carplay_modes.h"
#include "carplay_encoder.h"
#include "carplay_audio_bridge.h"
#include "carplay_capabilities.h"
#include "carplay_knob.h"
#include "carplay_display.h"
#include "airplay_bplist.h"
#define AIRPLAY_RESPONSE_BUFFER 131072u
static void iap2_log(const char *fmt,...){(void)fmt;}
'''
stubs=r'''
static int udp_fd, ping_before_info;
static int send_request(struct airplay_client*c,const char*m,const char*p,const char*t,unsigned seq,const uint8_t*b,size_t n){
 (void)c;(void)m;(void)p;(void)t;(void)b;(void)n;
 if(seq==1){uint8_t bytes[8];struct pollfd p={udp_fd,POLLIN,0};ping_before_info=poll(&p,1,100)==1&&recv(udp_fd,bytes,sizeof(bytes),MSG_DONTWAIT)==4;}
 return 0;
}
static int start_keepalive_worker(struct airplay_client*c);
static void stop_keepalive_worker(struct airplay_client*c);
static uint64_t millis(void){struct timespec t;clock_gettime(CLOCK_MONOTONIC,&t);return (uint64_t)t.tv_sec*1000+t.tv_nsec/1000000;}
'''
signatures=['static int open_keepalive_socket(', 'static void send_udp_keepalive(',
            'static void *keepalive_main(', 'static int start_keepalive_worker(',
            'static void stop_keepalive_worker(', 'static int bplist_port(', 'static int complete_setup(']
funcs='\n'.join(function(source,s) for s in signatures if s in source)
if 'static int start_keepalive_worker(' not in source:
    funcs+='\nstatic int start_keepalive_worker(struct airplay_client*c){(void)c;return 0;}\nstatic void stop_keepalive_worker(struct airplay_client*c){(void)c;}\n'
body=plistlib.dumps({'eventPort':7001,'timingPort':0,'keepAlivePort':65535},fmt=plistlib.FMT_BINARY)
offset=body.index(b'\x11\xff\xff')+1
fixture='static uint8_t setup[]={'+','.join(map(str,body))+'};\n#define PORT_OFFSET '+str(offset)+'\n'
main=r'''
#define CHECK(x) do{if(!(x)){fprintf(stderr,"FAIL line%d: %s\n",__LINE__,#x);return 1;}}while(0)
int main(void){
 struct airplay_client c={0};struct sockaddr_in tcp={0},udp={0};socklen_t n=sizeof(tcp);
 int listener=socket(AF_INET,SOCK_STREAM,0),remote;uint8_t packet[8];struct pollfd p;
 c.profile=AIRPLAY_PROFILE_LEGACY;c.keepalive_fd=-1;
 tcp.sin_family=AF_INET;tcp.sin_addr.s_addr=htonl(INADDR_LOOPBACK);
 CHECK(bind(listener,(struct sockaddr*)&tcp,sizeof(tcp))==0&&listen(listener,1)==0);
 CHECK(getsockname(listener,(struct sockaddr*)&tcp,&n)==0);
 c.fd=socket(AF_INET,SOCK_STREAM,0);CHECK(connect(c.fd,(struct sockaddr*)&tcp,sizeof(tcp))==0);
 remote=accept(listener,NULL,NULL);CHECK(remote>=0);
 udp_fd=socket(AF_INET,SOCK_DGRAM,0);udp.sin_family=AF_INET;udp.sin_addr.s_addr=htonl(INADDR_LOOPBACK);
 CHECK(bind(udp_fd,(struct sockaddr*)&udp,sizeof(udp))==0);n=sizeof(udp);CHECK(getsockname(udp_fd,(struct sockaddr*)&udp,&n)==0);
 unsigned port=ntohs(udp.sin_port);setup[PORT_OFFSET]=port>>8;setup[PORT_OFFSET+1]=port;
 int setup_result=complete_setup(&c,setup,sizeof(setup));
 if(setup_result||!ping_before_info)fprintf(stderr,"setup_result=%d keepalive_fd=%d port=%u errno=%d\n",setup_result,c.keepalive_fd,c.keep_alive_port,errno);
 CHECK(setup_result==0&&ping_before_info);
 CHECK(!c.record_accepted&&!c.info_complete);
 p.fd=udp_fd;p.events=POLLIN;
 for(int i=0;i<2;i++){uint64_t then=millis();CHECK(poll(&p,1,1600)==1);CHECK(recv(udp_fd,packet,sizeof(packet),0)==4);CHECK(millis()-then>=700);}
 uint64_t stop_at=millis();stop_keepalive_worker(&c);CHECK(millis()-stop_at<300);
 CHECK(c.keepalive_fd==-1);CHECK(poll(&p,1,1200)==0);stop_keepalive_worker(&c);
 close(c.fd);close(remote);close(listener);close(udp_fd);
 puts("UDP keepalive: before /info and RECORD, periodic during blocked control, bounded joined stop");return 0;
}
'''
target=OUT/'keepalive_test.c';target.write_text(prelude+state+stubs+fixture+funcs+main)
cc=os.environ.get('CC') or shutil.which('gcc') or shutil.which('clang');binary=OUT/'keepalive_test'
subprocess.run([cc,'-std=c11','-Wall','-Wextra','-Werror','-Wno-unused-function','-I'+str(ROOT/'src'),str(target),str(ROOT/'src/airplay_bplist.c'),'-pthread','-o',str(binary)],check=True)
raise SystemExit(subprocess.run([str(binary)],timeout=8).returncode)
