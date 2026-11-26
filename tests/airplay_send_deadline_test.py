#!/usr/bin/env python3
"""Exercise production send_all with adversarial clocks and real socket backpressure."""
from pathlib import Path
import os
import subprocess
import tempfile
from airplay_connection_regression_test import function

ROOT = Path(__file__).resolve().parents[1]
SOURCE = Path(os.environ.get("CARPLAY_CLIENT_SOURCE", ROOT / "src/airplay_client.c"))
production = SOURCE.read_text()
send_body = "\n".join(function(production, signature) for signature in (
    "static int send_all_since(", "static int send_all(",
) if signature in production)
message_bodies = "\n".join(function(production, signature) for signature in (
    "static void nonce64(", "static int send_encrypted_frames(",
    "static int send_control_frames(", "static int send_event_frames(",
    "static int send_request(", "int airplay_client_request_car_ui(",
))
common = r'''
#define _GNU_SOURCE
#include <assert.h>
#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <pthread.h>
#include <poll.h>
#include <signal.h>
#include <sys/socket.h>
#include <time.h>
#define AIRPLAY_SEND_TIMEOUT_MS 3000u
static void iap2_log(const char *fmt,...) {(void)fmt;errno=EDOM;}
'''
fake = r'''
static uint64_t ms;
static unsigned calls,closed,writes_before_stall,writes;
static int scenario;
static uint64_t video_monotonic_ms(void) { return ms; }
static ssize_t fake_send(int fd,const void *p,size_t n,int flags) {
    (void)fd;(void)p;assert(flags==(MSG_NOSIGNAL|MSG_DONTWAIT));calls++;
    if(scenario>=6) {
        if(writes<writes_before_stall) {
            if(!writes)ms+=2500;
            writes++;return (ssize_t)n;
        }
        errno=EAGAIN;return -1;
    }
    if(scenario==0) {ms+=10;return n>7?7:(ssize_t)n;}
    if(scenario==1) {ms+=600;return 1;}
    if(scenario==2) {ms+=400;errno=calls<50?EINTR:EPIPE;return -1;}
    if(scenario==3) {errno=EAGAIN;return -1;}
    if(scenario==4) {errno=EPIPE;return -1;}
    errno=EAGAIN;return -1;
}
static int fake_poll(struct pollfd *p,nfds_t n,int timeout) {
    (void)p;(void)n;
    if(scenario==3 || scenario>=6) {ms+=(unsigned)timeout;return 0;}
    ms+=400;errno=calls<50?EINTR:EPIPE;return -1;
}
static int fake_shutdown(int fd,int how) {(void)fd;assert(how==SHUT_RDWR);closed++;errno=EBADF;return 0;}
#define send fake_send
#define poll fake_poll
#define shutdown fake_shutdown
'''
message_prelude = r'''
#include "airplay_crypto.h"
#include "airplay_client.h"
#include "carplay_identity.h"
#define AIRPLAY_CONTROL_MAX_PAYLOAD 0x4000u
struct airplay_client {
    int fd,event_fd,profile,failed,control_channel_ready;
    unsigned event_request_cseq,pending_handshake_cseq;
    uint64_t handshake_deadline_ms,control_write_counter,event_write_counter;
    uint8_t control_write_key[AIRPLAY_CHACHA_KEY_BYTES],event_write_key[AIRPLAY_CHACHA_KEY_BYTES];
    struct {int request_car_ui_supported;} capabilities;
    pthread_mutex_t event_send_mutex;
};
/* The board's crypto backend exports BoringSSL callbacks. Keep the actual
 * record framing and transport here; the crypto self-test covers sealing. */
int airplay_chacha20_poly1305_seal(const uint8_t key[AIRPLAY_CHACHA_KEY_BYTES],
    const uint8_t nonce[AIRPLAY_CHACHA_NONCE_BYTES],
    const uint8_t *plaintext,size_t len,const uint8_t *aad,size_t aad_len,
    uint8_t *sealed,size_t capacity,size_t *sealed_len) {
    (void)key;(void)nonce;
    assert(aad_len==2 && len==((size_t)aad[0]|((size_t)aad[1]<<8)));
    assert(capacity>=len+AIRPLAY_CHACHA_TAG_BYTES);
    memcpy(sealed,plaintext,len);
    memset(sealed+len,42,AIRPLAY_CHACHA_TAG_BYTES);
    *sealed_len=len+AIRPLAY_CHACHA_TAG_BYTES;
    return 0;
}
static int airplay_bplist_encode_car_ui_request(uint8_t *data,size_t cap,size_t *len) {
    assert(cap>=10);memset(data,42,10);*len=10;return 0;
}
'''
fake_main = r'''
int main(void) {
    char data[100]={0};
    for(scenario=0;scenario<6;scenario++) {
        ms=100;calls=closed=0;errno=0;
        int result=send_all(9,data,sizeof(data));
        if(!scenario) {assert(result==0 && !closed);continue;}
        if(scenario!=4 && (result!=-1 || errno!=ETIMEDOUT || ms>3500)) {
            fprintf(stderr,"FAIL total deadline scenario=%d rc=%d errno=%d elapsed=%llu\n",
                    scenario,result,errno,(unsigned long long)(ms-100));return 1;
        }
        if(scenario==4) assert(result==-1 && errno==EPIPE);
        assert(closed==1);
    }
    puts("PASS send deadline: progress, EINTR, poll interruptions, timeout and poisoned socket");

    int failed=0;
    for(scenario=6;scenario<=11;scenario++) {
        struct airplay_client c={.fd=9,.event_fd=10,.event_send_mutex=PTHREAD_MUTEX_INITIALIZER};
        const char *name;
        int result;
        ms=100;calls=closed=writes=0;errno=0;
        if(scenario<=8) {
            c.control_channel_ready=scenario!=6;
            name=scenario==6?"plain RTSP header/body":scenario==7?"HAP header/sealed payload":"encrypted RTSP header/body";
            writes_before_stall=scenario==8?2:1;
            result=send_request(&c,"SETUP","/stream",NULL,1,(const uint8_t *)data,sizeof(data));
            assert(!c.pending_handshake_cseq && !c.handshake_deadline_ms);
            assert(c.control_write_counter==(scenario==8?1u:0u));
        } else if(scenario==9) {
            uint8_t event[AIRPLAY_CONTROL_MAX_PAYLOAD+20]={0};
            name="encrypted multi-record event";
            writes_before_stall=2;
#ifdef EXPLICIT_EVENT_START
            result=send_event_frames(&c,event,sizeof(event),video_monotonic_ms());
#else
            result=send_event_frames(&c,event,sizeof(event));
#endif
            assert(c.event_write_counter==1);
        } else {
            name=scenario==10?"plain event request header/body":"encrypted event request header/body";
            c.profile=scenario==10?AIRPLAY_PROFILE_LEGACY:AIRPLAY_PROFILE_HAP;
            c.capabilities.request_car_ui_supported=1;
            writes_before_stall=scenario==10?1:2;
            result=airplay_client_request_car_ui(&c);
            assert(c.event_write_counter==(scenario==11?1u:0u));
        }
        assert(writes==writes_before_stall && !pthread_mutex_destroy(&c.event_send_mutex));
        if(result!=-1 || errno!=ETIMEDOUT || closed!=1 || ms-100!=3000) {
            fprintf(stderr,"FAIL %s shared deadline: rc=%d errno=%d shutdown=%u elapsed=%llu\n",
                    name,result,errno,closed,(unsigned long long)(ms-100));
            failed=1;
        }
    }
    if(failed)return 1;
    struct airplay_client next={.fd=9};
    ms=100;writes_before_stall=1;
    for(unsigned cseq=1;cseq<=2;cseq++) {
        writes=0;
        assert(send_request(&next,"POST","/setup",NULL,cseq,NULL,0)==0);
        assert(next.pending_handshake_cseq==cseq && next.handshake_deadline_ms==ms+15000);
    }
    assert(ms-100==5000); /* Each outbound request owns its own budget. */
    puts("PASS logical send deadline: plain and encrypted splits, multi-record event, independent requests");
}
'''
real = r'''
static uint64_t video_monotonic_ms(void) {
    struct timespec t;assert(clock_gettime(CLOCK_MONOTONIC,&t)==0);
    return (uint64_t)t.tv_sec*1000+t.tv_nsec/1000000;
}
static int notify_poll=-1;
static int notify_backpressure(struct pollfd *fds,nfds_t n,int timeout) {
    if(notify_poll>=0) {
        assert(write(notify_poll,"P",1)==1);
        notify_poll=-1;
    }
    return poll(fds,n,timeout);
}
#define poll notify_backpressure
static void *drain(void *arg) {
    int fd=*(int*)arg;char buffer[4096];ssize_t n;size_t received=0;
    while((n=recv(fd,buffer,sizeof(buffer),0))>0) {
        for(ssize_t i=0;i<n;i++)assert(buffer[i]==42);
        received+=(size_t)n;
    }
    assert(n==0 && received==131072);return NULL;
}
'''
real_main = r'''
struct blocked_send {int fd;const void *data;size_t len;int result,error;};
static void *send_while_stopping(void *opaque) {
    struct blocked_send *args=opaque;
    args->result=send_all(args->fd,args->data,args->len);
    args->error=errno;
    return NULL;
}
int main(void) {
    alarm(12);
    int s[2],small=1024;char data[131072];memset(data,42,sizeof(data));
    assert(socketpair(AF_UNIX,SOCK_STREAM,0,s)==0);
    assert(setsockopt(s[0],SOL_SOCKET,SO_SNDBUF,&small,sizeof(small))==0);
    uint64_t start=video_monotonic_ms();
    assert(send_all(s[0],data,sizeof(data))==-1 && errno==ETIMEDOUT);
    uint64_t elapsed=video_monotonic_ms()-start;assert(elapsed>=2900 && elapsed<4500);
    char buffer[4096];ssize_t n;while((n=recv(s[1],buffer,sizeof(buffer),0))>0){}
    assert(n==0);close(s[0]);close(s[1]);
    assert(socketpair(AF_UNIX,SOCK_STREAM,0,s)==0);
    pthread_t reader;assert(pthread_create(&reader,NULL,drain,&s[1])==0);
    assert(send_all(s[0],data,sizeof(data))==0);shutdown(s[0],SHUT_WR);
    assert(pthread_join(reader,NULL)==0);close(s[0]);close(s[1]);
    assert(socketpair(AF_UNIX,SOCK_STREAM,0,s)==0);close(s[1]);
    assert(send_all(s[0],data,sizeof(data))==-1);close(s[0]);
    assert(socketpair(AF_UNIX,SOCK_STREAM,0,s)==0);
    assert(setsockopt(s[0],SOL_SOCKET,SO_SNDBUF,&small,sizeof(small))==0);
    int notification[2];assert(pipe(notification)==0);notify_poll=notification[1];
    struct blocked_send blocked={.fd=s[0],.data=data,.len=sizeof(data)};
    pthread_t sender;assert(pthread_create(&sender,NULL,send_while_stopping,&blocked)==0);
    /* The sender has reached its real POLLOUT wait after getting EAGAIN. */
    char mark;assert(read(notification[0],&mark,1)==1 && mark=='P');
    start=video_monotonic_ms();assert(shutdown(s[0],SHUT_RDWR)==0);
    assert(pthread_join(sender,NULL)==0);
    elapsed=video_monotonic_ms()-start;
    assert(elapsed<1000 && blocked.result==-1 && blocked.error==EPIPE);
    close(notification[0]);close(notification[1]);close(s[0]);close(s[1]);
    printf("PASS real send: timeout, healthy stream, closed peer, and shutdown wake in %llums\n",
           (unsigned long long)elapsed);
}
'''
with tempfile.TemporaryDirectory() as temp:
    for name, code in (("fake", common + fake + message_prelude + send_body + message_bodies + fake_main),
                       ("real", common + real + send_body + real_main)):
        source = Path(temp) / (name + ".c")
        binary = Path(temp) / name
        source.write_text(code)
        flags = ["-DEXPLICIT_EVENT_START"] if "static int send_all_since(" in production else []
        subprocess.run(["cc", "-std=c11", "-Wall", "-Wextra", "-Werror",
                        "-Wno-unused-function", "-I" + str(ROOT / "src"), *flags,
                        str(source), "-pthread", "-o", str(binary)], check=True)
        result = subprocess.run([str(binary)], timeout=15)
        if name == "fake":
            fake_result = result
    fake_result.check_returncode()
    result.check_returncode()
