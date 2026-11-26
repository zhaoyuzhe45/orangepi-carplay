#define _POSIX_C_SOURCE 200809L
#include "carplay_keys.h"
#include <assert.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <linux/input.h>
#include <poll.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

static long long now_ms(void) {
    struct timespec t;assert(clock_gettime(CLOCK_MONOTONIC,&t)==0);
    return (long long)t.tv_sec*1000+t.tv_nsec/1000000;
}
static void pause_ms(unsigned ms) {
    struct timespec t={(time_t)(ms/1000),(long)(ms%1000)*1000000};nanosleep(&t,NULL);
}
static void submit(struct carplay_keys *w,const struct carplay_key_batch *b) {
    long long end=now_ms()+500;int r;
    do {r=carplay_keys_submit(w,b);if(r==-2)pause_ms(1);} while(r==-2 && now_ms()<end);
    assert(r==0);
}
static void rejected(struct carplay_keys *w,const struct carplay_key_batch *b) {
    long long end=now_ms()+500;int r;
    do {r=carplay_keys_submit(w,b);if(r==-2)pause_ms(1);} while(r==-2 && now_ms()<end);
    assert(r==-1);
}
static struct carplay_keys_stats emitted(struct carplay_keys *w,unsigned count) {
    long long end=now_ms()+500;struct carplay_keys_stats stats;
    do {carplay_keys_get_stats(w,&stats);if(stats.emitted<count)pause_ms(1);}while(stats.emitted<count && now_ms()<end);
    assert(stats.emitted==count);return stats;
}
static void read_exact(int fd,void *data,size_t n) {
    uint8_t *p=data;long long end=now_ms()+1000;
    while(n && now_ms()<end){struct pollfd q={fd,POLLIN,0};assert(poll(&q,1,100)>=0);
        if(!(q.revents&POLLIN))continue;
        ssize_t got=read(fd,p,n);assert(got>0);p+=got;n-=(size_t)got;}
    assert(n==0);
}
static void expect_tap(int fd,int key) {
    struct input_event e[4];read_exact(fd,e,sizeof(e));
    assert(e[0].type==EV_KEY && e[0].code==key && e[0].value==1);
    assert(e[1].type==EV_SYN && e[1].code==SYN_REPORT && !e[1].value);
    assert(e[2].type==EV_KEY && e[2].code==key && !e[2].value);
    assert(e[3].type==EV_SYN && e[3].code==SYN_REPORT && !e[3].value);
}
static void ordered_and_mapped(void) {
    int p[2];assert(pipe(p)==0);struct carplay_keys *w=NULL;
    assert(carplay_keys_create_fd(&w,p[1])==0);close(p[1]);
    struct carplay_key_batch b={0};int android[]={19,20,21,22,66,3,4};
    int linux_codes[]={KEY_UP,KEY_DOWN,KEY_LEFT,KEY_RIGHT,KEY_ENTER,KEY_HOMEPAGE,KEY_BACK};
    b.count=7;for(unsigned i=0;i<7;i++){b.entries[i].keycode=android[i];b.entries[i].count=i==1?5:1;}
    submit(w,&b);
    struct carplay_key_batch second={0};second.count=1;second.entries[0].keycode=19;second.entries[0].count=2;submit(w,&second);
    for(unsigned i=0;i<7;i++)for(unsigned j=0;j<(i==1?5u:1u);j++)expect_tap(p[0],linux_codes[i]);
    expect_tap(p[0],KEY_UP);expect_tap(p[0],KEY_UP);
    struct carplay_keys_stats stats=emitted(w,13);
    assert(stats.accepted==13 && stats.emitted==13 && !stats.failures);
    uint64_t previous_rejected=stats.rejected;
    b.entries[0].keycode=999;assert(carplay_keys_submit(w,&b)==-1);
    carplay_keys_get_stats(w,&stats);assert(stats.rejected==previous_rejected+1);
    carplay_keys_suspend(w,1);rejected(w,&second);
    carplay_keys_suspend(w,0);submit(w,&second);expect_tap(p[0],KEY_UP);expect_tap(p[0],KEY_UP);
    carplay_keys_destroy(w);close(p[0]);
}
static void fill_pipe(int fd) {
    assert(fcntl(fd,F_SETFL,fcntl(fd,F_GETFL)|O_NONBLOCK)==0);
    uint8_t data[4096];memset(data,0x55,sizeof(data));
    while(write(fd,data,sizeof(data))>0){}assert(errno==EAGAIN || errno==EWOULDBLOCK);
}
static void drain_pipe(int fd) {
    assert(fcntl(fd,F_SETFL,fcntl(fd,F_GETFL)|O_NONBLOCK)==0);
    uint8_t data[4096];while(read(fd,data,sizeof(data))>0){}assert(errno==EAGAIN || errno==EWOULDBLOCK);
}
static void pressure_cancel_and_shutdown(void) {
    int p[2];assert(pipe(p)==0);fill_pipe(p[1]);struct carplay_keys *w=NULL;
    assert(carplay_keys_create_fd(&w,p[1])==0);
    struct carplay_key_batch big={0};big.count=1;big.entries[0].keycode=20;big.entries[0].count=CARPLAY_KEYS_QUEUE_ACTIONS;
    submit(w,&big);pause_ms(5);
    struct carplay_key_batch one={0};one.count=1;one.entries[0].keycode=4;one.entries[0].count=1;
    long long start=now_ms();assert(carplay_keys_submit(w,&one)==-2);assert(now_ms()-start<50);
    start=now_ms();carplay_keys_suspend(w,1);assert(now_ms()-start<500);
    rejected(w,&one);drain_pipe(p[0]);
    struct pollfd q={p[0],POLLIN,0};assert(poll(&q,1,20)==0);
    carplay_keys_suspend(w,0);submit(w,&one);expect_tap(p[0],KEY_BACK);
    fill_pipe(p[1]);submit(w,&big);pause_ms(5);
    start=now_ms();carplay_keys_clear(w);assert(now_ms()-start<500);drain_pipe(p[0]);
    assert(poll(&q,1,20)==0);submit(w,&one);expect_tap(p[0],KEY_BACK);
    struct carplay_keys_stats stats;carplay_keys_get_stats(w,&stats);assert(stats.cancelled==2*CARPLAY_KEYS_QUEUE_ACTIONS);
    fill_pipe(p[1]);submit(w,&big);pause_ms(5);start=now_ms();carplay_keys_destroy(w);assert(now_ms()-start<500);
    close(p[0]);close(p[1]);
}
static void broken_sink_and_bad_start(void) {
    struct carplay_keys *w=NULL;assert(carplay_keys_create(&w,"/does/not/exist/uinput")<0 && !w);
    assert(carplay_keys_create_fd(&w,-1)<0 && !w);
    int p[2];assert(pipe(p)==0);assert(carplay_keys_create_fd(&w,p[1])==0);close(p[0]);close(p[1]);
    struct carplay_key_batch b={0};b.count=1;b.entries[0].keycode=23;b.entries[0].count=1;submit(w,&b);
    long long end=now_ms()+500;struct carplay_keys_stats stats;
    do {carplay_keys_get_stats(w,&stats);if(!stats.failures)pause_ms(1);}while(!stats.failures && now_ms()<end);
    assert(stats.failures==1);rejected(w,&b);carplay_keys_destroy(w);
}
static unsigned descriptor_count(void) {
    DIR *d=opendir("/proc/self/fd");assert(d);unsigned n=0;struct dirent *e;
    while((e=readdir(d)))if(strcmp(e->d_name,".") && strcmp(e->d_name,".."))n++;
    closedir(d);return n;
}
static void sockets_entry_bound_and_cleanup(void) {
    unsigned before=descriptor_count();
    for(unsigned i=0;i<16;i++) {
        int s[2];assert(socketpair(AF_UNIX,SOCK_STREAM,0,s)==0);struct carplay_keys *w=NULL;
        assert(carplay_keys_create_fd(&w,s[1])==0);
        struct carplay_key_batch b={0};b.count=1;b.entries[0].keycode=23;b.entries[0].count=1;
        submit(w,&b);expect_tap(s[0],KEY_SELECT);carplay_keys_destroy(w);close(s[0]);close(s[1]);
    }
    assert(descriptor_count()==before);
    int p[2];assert(pipe(p)==0);fill_pipe(p[1]);struct carplay_keys *w=NULL;assert(carplay_keys_create_fd(&w,p[1])==0);
    struct carplay_key_batch b={0};b.count=32;
    for(unsigned i=0;i<32;i++){b.entries[i].keycode=19;b.entries[i].count=1;}
    for(unsigned i=0;i<4;i++)submit(w,&b);
    pause_ms(5);long long start=now_ms();assert(carplay_keys_submit(w,&b)==-2);assert(now_ms()-start<50);
    struct carplay_key_batch invalid=b;invalid.entries[31].keycode=999;
    assert(carplay_keys_submit(w,&invalid)==-1);
    struct carplay_keys_stats stats;carplay_keys_get_stats(w,&stats);assert(stats.accepted==128 && !stats.emitted);
    carplay_keys_destroy(w);close(p[0]);close(p[1]);assert(descriptor_count()==before);
}
int main(void){ordered_and_mapped();pressure_cancel_and_shutdown();broken_sink_and_bad_start();sockets_entry_bound_and_cleanup();puts("keys persistent FIFO/mapping/pressure/cancel/shutdown/socket/FD-cleanup tests: PASS");return 0;}
