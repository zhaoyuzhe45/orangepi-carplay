#define _GNU_SOURCE
#include "carplay_now_playing.h"
#include "carplay_calls.h"
#include "carplay_route.h"
#include <string.h>
#ifndef CARPLAY_NOW_TEST_PROVIDER
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <time.h>
#include <unistd.h>
#endif

static uint16_t get16(const uint8_t *p) { return (uint16_t)((p[0]<<8)|p[1]); }
static uint32_t get32(const uint8_t *p) {
    return ((uint32_t)get16(p)<<16)|get16(p+2);
}
static void put16(uint8_t *p, uint16_t v) { p[0]=(uint8_t)(v>>8);p[1]=(uint8_t)v; }
static void put32(uint8_t *p, uint32_t v) { put16(p,(uint16_t)(v>>16));put16(p+2,(uint16_t)v); }

static int utf8_valid(const uint8_t *p, size_t n) {
    for(size_t i=0;i<n;) {
        uint32_t c=p[i++], min=0; unsigned more=0;
        if(!c) return 0;
        if(c<0x80) continue;
        if(c>=0xc2 && c<=0xdf) { more=1;min=0x80;c&=31; }
        else if(c>=0xe0 && c<=0xef) { more=2;min=0x800;c&=15; }
        else if(c>=0xf0 && c<=0xf4) { more=3;min=0x10000;c&=7; }
        else return 0;
        if(more>n-i) return 0;
        while(more--) { if((p[i]&0xc0)!=0x80)return 0;c=(c<<6)|(p[i++]&63); }
        if(c<min || c>0x10ffff || (c>=0xd800 && c<=0xdfff))return 0;
    }
    return 1;
}

int carplay_now_decode(const uint8_t *wire,size_t len,uint64_t now_ms,
                       struct carplay_now_playing *out) {
    struct carplay_now_playing p;
    if(!wire || !out || len<36 || len>CARPLAY_NOW_MAX ||
        (memcmp(wire,"CPN1",4) && memcmp(wire,"CPN2",4) && memcmp(wire,"CPN3",4)))return -1;
    memset(&p,0,sizeof(p));
    p.sampled_ms=((uint64_t)get32(wire+4)<<32)|get32(wire+8);
    if(p.sampled_ms>now_ms || now_ms-p.sampled_ms>5000)return -1;
    p.status=wire[12];p.can_seek=wire[13];p.speed=get16(wire+14);
    p.duration=get32(wire+16);p.position=get32(wire+20);
    if((p.status>4 && p.status!=255) || p.can_seek>1)return -1;
    char *fields[]={p.title,p.album,p.artist,p.composer,p.app_name,p.package};
    size_t offset=24;
    for(size_t i=0;i<6;i++) {
        if(len-offset<2)return -1;
        size_t count=get16(wire+offset);offset+=2;
        if(count>255 || count>len-offset || !utf8_valid(wire+offset,count))return -1;
        memcpy(fields[i],wire+offset,count);offset+=count;
    }
    if(wire[3]=='2' || wire[3]=='3'){
        if(len-offset!=(wire[3]=='3'?25u:16u))return -1;
        p.media_generation=get32(wire+offset);p.art_size=get32(wire+offset+4);
        p.queue_index=get32(wire+offset+8);p.queue_count=get32(wire+offset+12);
        p.queue_count_known=p.queue_count!=UINT32_MAX;p.queue_index_known=p.queue_index!=UINT32_MAX;
        if(p.art_size>32768 || (p.art_size && !p.media_generation) ||
            (p.queue_count_known && (!p.queue_count || p.queue_count>10000)) ||
            (p.queue_index_known && (!p.queue_count_known || p.queue_index>=p.queue_count)))return -1;
        offset+=16;
        if(wire[3]=='3'){
            p.queue_version=((uint64_t)get32(wire+offset)<<32)|get32(wire+offset+4);
            p.queue_available=wire[offset+8];
            if(p.queue_available>1 || (p.queue_available && (!p.queue_version || !p.queue_count_known)))return -1;
            offset+=9;
        }
    }
    if(offset!=len)return -1;
    *out=p;return 0;
}

int carplay_now_subscribe(struct carplay_now_subscription *out,
                         const uint8_t *body,size_t len) {
    struct carplay_now_subscription next={0}; unsigned groups=0;
    if(!out || (len && !body) || len>CARPLAY_NOW_MAX)return -1;
    for(size_t off=0;off<len;) {
        if(len-off<4)return -1;
        size_t size=get16(body+off);unsigned group=get16(body+off+2);
        if(size<4 || size>len-off)return -1;
        if(group<=2) {
            if(groups&(1u<<group))return -1;
            groups|=1u<<group;
            uint32_t fields=0;
            for(size_t pos=off+4;pos<off+size;) {
                if(off+size-pos<4)return -1;
                size_t count=get16(body+pos);unsigned id=get16(body+pos+2);
                if(count<4 || count>off+size-pos)return -1;
                if(group==1 && id==17) { if(count!=8)return -1; }
                else if(count!=4)return -1;
                if(id<32) { if(fields&(1u<<id))return -1;fields|=1u<<id; }
                pos+=count;
            }
            if(group==0)next.media=fields;
            if(group==1)next.playback=fields;
        }
        off+=size;
    }
    *out=next;return 0;
}

static int param(uint8_t *out,size_t cap,size_t *used,unsigned id,const void *p,size_t n) {
    if(*used>cap || cap-*used<n+4)return -1;
    put16(out+*used,(uint16_t)(n+4));put16(out+*used+2,(uint16_t)id);
    if(n)memcpy(out+*used+4,p,n);
    *used+=n+4;return 0;
}

static int string_param(uint8_t *out,size_t cap,size_t *used,unsigned id,
                        const char *text,size_t budget) {
    char bounded[256];size_t n=strlen(text);
    if(n>budget) { n=budget;while(n && ((unsigned char)text[n]&0xc0)==0x80)n--; }
    if(n>=sizeof(bounded))return -1;
    memcpy(bounded,text,n);bounded[n]=0;
    return param(out,cap,used,id,bounded,n+1);
}

int carplay_now_encode(const struct carplay_now_subscription *sub,
                      const struct carplay_now_playing *p,
                      uint8_t *out,size_t cap,size_t *len) {
    uint8_t group[CARPLAY_NOW_MAX], number[4], zero=0;
    size_t used=0,n=0,fixed=0,strings=0;
    if(!sub || !p || !out || !len)return -1;
    struct carplay_now_subscription supported=*sub;
    if(p->status==255) supported.playback&=~((1u<<0)|(1u<<1)|(1u<<12)|(1u<<13));
    if(!p->queue_index_known)supported.playback&=~(1u<<2);
    if(!p->queue_count_known)supported.playback&=~(1u<<3);
    sub=&supported;
    const unsigned media_strings[]={1,6,12,18},playback_strings[]={7,16};
    for(size_t i=0;i<4;i++)if(sub->media&(1u<<media_strings[i]))strings++;
    for(size_t i=0;i<2;i++)if(sub->playback&(1u<<playback_strings[i]))strings++;
    if(sub->media&((1u<<1)|(1u<<4)|(1u<<6)|(1u<<12)|(1u<<18)))fixed+=4;
    if(sub->playback&((1u<<0)|(1u<<1)|(1u<<2)|(1u<<3)|(1u<<7)|(1u<<12)|(1u<<13)|(1u<<14)|(1u<<16)))fixed+=4;
    fixed+=strings*5;
    if(sub->media&(1u<<4))fixed+=8;
    if(sub->playback&1u)fixed+=5;
    if(sub->playback&(1u<<1))fixed+=8;
    if(sub->playback&(1u<<2))fixed+=8;
    if(sub->playback&(1u<<3))fixed+=8;
    if(sub->playback&(1u<<12))fixed+=6;
    if(sub->playback&(1u<<13))fixed+=5;
    if(sub->playback&(1u<<14))fixed+=5;
    if(cap<fixed)return -1;
    size_t budget=strings?(cap-fixed)/strings:0;
    if(budget>255)budget=255;
#define FIELD(mask,id,data,size) do { if((mask)&(1u<<(id))) { \
    if(param(group,sizeof(group),&n,id,data,size)<0)return -1; } } while(0)
#define STRING(mask,id,text) do { if((mask)&(1u<<(id))) { \
    if(string_param(group,sizeof(group),&n,id,text,budget)<0)return -1; } } while(0)
    STRING(sub->media,1,p->title);
    put32(number,p->duration);FIELD(sub->media,4,number,4);
    STRING(sub->media,6,p->album);STRING(sub->media,12,p->artist);
    STRING(sub->media,18,p->composer);
    if(n && param(out,cap,&used,0,group,n)<0)return -1;
    n=0;
    FIELD(sub->playback,0,&p->status,1);
    put32(number,p->position);FIELD(sub->playback,1,number,4);
    put32(number,p->queue_index);FIELD(sub->playback,2,number,4);
    put32(number,p->queue_count);FIELD(sub->playback,3,number,4);
    STRING(sub->playback,7,p->app_name);
    put16(number,p->speed);FIELD(sub->playback,12,number,2);
    FIELD(sub->playback,13,&p->can_seek,1);
    FIELD(sub->playback,14,&zero,1);
    STRING(sub->playback,16,p->package);
    if(n && param(out,cap,&used,1,group,n)<0)return -1;
    *len=used;return 0;
#undef STRING
#undef FIELD
}

int carplay_comm_decode(const uint8_t *wire,size_t len,uint64_t now_ms,
                       struct carplay_communications *out) {
    struct carplay_communications p;
    if(!wire || !out || len!=17 || memcmp(wire,"CPC1",4))return -1;
    memset(&p,0,sizeof(p));p.sampled_ms=((uint64_t)get32(wire+4)<<32)|get32(wire+8);
    if(p.sampled_ms>now_ms || now_ms-p.sampled_ms>5000)return -1;
    for(size_t i=12;i<17;i++)if(wire[i]>1)return -1;
    p.airplane=wire[12];p.cellular=wire[13];p.telephony=wire[14];p.can_call=wire[15];p.muted=wire[16];
    *out=p;return 0;
}
int carplay_comm_encode(uint32_t fields,const struct carplay_communications *p,
                       uint8_t *out,size_t cap,size_t *len) {
    size_t used=0;const unsigned ids[]={2,5,6,7,8,9,12};
    if(!p || !out || !len)return -1;
    const uint8_t values[]={p->airplane,p->cellular,p->telephony,0,0,p->muted,p->can_call};
    for(size_t i=0;i<sizeof(ids)/sizeof(ids[0]);i++)
        if(fields&(1u<<ids[i]))if(param(out,cap,&used,ids[i],values+i,1)<0)return -1;
    *len=used;return 0;
}

#ifndef CARPLAY_NOW_TEST_PROVIDER
static uint64_t monotonic_ms(void) {
    struct timespec t;clock_gettime(CLOCK_MONOTONIC,&t);
    return (uint64_t)t.tv_sec*1000+(uint64_t)t.tv_nsec/1000000;
}
static int transfer(int fd,void *data,size_t len,int writing,uint64_t deadline) {
    size_t offset=0;
    while(offset<len) {
        uint64_t now=monotonic_ms();if(now>=deadline)return -1;
        struct pollfd p={fd,writing?POLLOUT:POLLIN,0};
        int rc=poll(&p,1,(int)(deadline-now));
        if(rc<0 && errno==EINTR)continue;
        if(rc<=0)return -1;
        ssize_t n=writing?send(fd,(char*)data+offset,len-offset,MSG_NOSIGNAL):
                          recv(fd,(char*)data+offset,len-offset,0);
        if(n<0 && (errno==EINTR || errno==EAGAIN))continue;
        if(n<=0)return -1;
        offset+=(size_t)n;
    }
    return 0;
}
static int exchange(uint32_t op,uint32_t arg,uint8_t *out,size_t *len) {
    struct sockaddr_un address;struct stat st;struct ucred peer;
    socklen_t peer_len=sizeof(peer);uint8_t request[12],header[4];
    static const char name[]="carplay.app.integration.v1";
    int rc=-1,fd=-1;uint64_t deadline=monotonic_ms()+50;
    /* Directory owner is installed app UID, not a caller-supplied identity. */
    if(stat("/data/user/0/com.carplaybox.audio",&st)<0 || !S_ISDIR(st.st_mode))return -1;
    fd=socket(AF_UNIX,SOCK_STREAM|SOCK_CLOEXEC|SOCK_NONBLOCK,0);if(fd<0)return -1;
    memset(&address,0,sizeof(address));address.sun_family=AF_UNIX;
    memcpy(address.sun_path+1,name,sizeof(name)-1);
    if(connect(fd,(struct sockaddr*)&address,
            (socklen_t)(offsetof(struct sockaddr_un,sun_path)+sizeof(name)))<0)goto done;
    if(getsockopt(fd,SOL_SOCKET,SO_PEERCRED,&peer,&peer_len)<0 || peer.uid!=st.st_uid)goto done;
    memcpy(request,"CPQ1",4);put32(request+4,op);put32(request+8,arg);
    rc=-2; /* Once writing starts, delivery is ambiguous: do not replay keys. */
    if(transfer(fd,request,sizeof(request),1,deadline)<0 || transfer(fd,header,4,0,deadline)<0)goto done;
    size_t n=get32(header);if(!n || n>*len || n>CARPLAY_NOW_MAX)goto done;
    if(transfer(fd,out,n,0,deadline)<0)goto done;
    *len=n;rc=0;
done:
    close(fd);return rc;
}
int carplay_now_read(uint64_t now_ms,struct carplay_now_playing *out) {
    uint8_t wire[CARPLAY_NOW_MAX];size_t len=sizeof(wire);
    if(exchange(0,0,wire,&len)<0)return -1;
    /* Response might be freshly sampled after the caller's timestamp. */
    (void)now_ms;
    return carplay_now_decode(wire,len,monotonic_ms(),out);
}
int carplay_app_command(uint32_t op,uint32_t argument) {
    uint8_t response[4];size_t len=sizeof(response);
    if((op<1 || op>3) && !(op==6 && (argument==1 || argument==2)))return -1;
    int result=exchange(op,argument,response,&len);
    if(result<0)return result;
    return len==1 && response[0]==0 ? 0:len==1 && response[0]==1 ? -1:-2;
}
int carplay_comm_read(uint64_t now_ms,struct carplay_communications *out) {
    uint8_t wire[32];size_t len=sizeof(wire);(void)now_ms;
    if(exchange(4,0,wire,&len)<0)return -1;
    return carplay_comm_decode(wire,len,monotonic_ms(),out);
}
int carplay_calls_read(uint64_t now_ms,struct carplay_call_list *out) {
    uint8_t wire[CARPLAY_NOW_MAX];size_t len=sizeof(wire);(void)now_ms;
    if(exchange(5,0,wire,&len)<0)return -1;
    return carplay_calls_decode(wire,len,monotonic_ms(),out);
}
int carplay_route_read(uint64_t now_ms,struct carplay_route_snapshot *out) {
    uint8_t wire[CARPLAY_ROUTE_WIRE_MAX];size_t len=sizeof(wire);(void)now_ms;
    if(exchange(7,0,wire,&len)<0)return -1;
    return carplay_route_decode(wire,len,monotonic_ms(),out);
}
int carplay_art_read(uint32_t generation,uint32_t total,uint32_t offset,uint8_t *out,size_t cap,size_t *len){
    uint8_t wire[1040];size_t n=sizeof(wire);
    if(!out || !len || !generation || !total || total>32768 || offset>=total)return -1;
    if(exchange(8,offset,wire,&n)<0 || n<=16 || memcmp(wire,"CPA1",4) ||
        get32(wire+4)!=generation || get32(wire+8)!=total || get32(wire+12)!=offset ||
        n-16>total-offset || n-16>cap)return -1;
    memcpy(out,wire+16,n-16);*len=n-16;return 0;
}
#endif
