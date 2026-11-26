#define _POSIX_C_SOURCE 200809L
#include "carplay_encoder.h"
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <unistd.h>

#define CHECK(x) do { if (!(x)) { fprintf(stderr, "FAIL %d: %s errno=%d\n", __LINE__, #x, errno); exit(1); } } while (0)

static void put32(unsigned char *p, unsigned v)
{ p[0] = v >> 24; p[1] = v >> 16; p[2] = v >> 8; p[3] = v; }

static void header(unsigned char *p, unsigned flags, unsigned size)
{
    memset(p, 0, 24);
    put32(p, 0x43504531); put32(p + 4, flags); put32(p + 8, size);
    put32(p + 16, 1); put32(p + 20, 234);
}

static void framing(void)
{
    int s[2]; unsigned char bytes[56];
    struct carplay_encoder_reader r = {0}; struct carplay_encoder_packet p;
    CHECK(socketpair(AF_UNIX, SOCK_STREAM, 0, s) == 0);
    header(bytes, CARPLAY_ENCODER_CONFIG, 4); memcpy(bytes+24,"\0\0\1\x67",4);
    header(bytes+28, CARPLAY_ENCODER_KEYFRAME, 4); memcpy(bytes+52,"\0\0\1\x65",4);
    CHECK(write(s[0],bytes,7)==7);
    CHECK(carplay_encoder_reader_read(&r,s[1],&p,100)==0);
    CHECK(write(s[0],bytes+7,18)==18);
    CHECK(carplay_encoder_reader_read(&r,s[1],&p,110)==0);
    CHECK(write(s[0],bytes+25,31)==31);
    CHECK(carplay_encoder_reader_read(&r,s[1],&p,120)==1);
    CHECK(p.flags==CARPLAY_ENCODER_CONFIG && p.size==4 && p.data[3]==0x67);
    CHECK(p.pts_us==(((uint64_t)1<<32)|234));
    CHECK(carplay_encoder_reader_read(&r,s[1],&p,121)==1);
    CHECK(p.flags==CARPLAY_ENCODER_KEYFRAME && p.data[3]==0x65);
    CHECK(carplay_encoder_reader_read(&r,s[1],&p,10000)==0); /* idle is healthy */
    close(s[0]); CHECK(carplay_encoder_reader_read(&r,s[1],&p,10001)<0);
    carplay_encoder_reader_destroy(&r); close(s[1]);
}

static void malformed(unsigned flags, unsigned size, unsigned reserved, unsigned magic)
{
    int s[2]; unsigned char bytes[24];
    struct carplay_encoder_reader r = {0}; struct carplay_encoder_packet p;
    CHECK(socketpair(AF_UNIX,SOCK_STREAM,0,s)==0);
    header(bytes, flags, size); put32(bytes,magic); put32(bytes+12,reserved);
    CHECK(write(s[0],bytes,24)==24);
    CHECK(carplay_encoder_reader_read(&r,s[1],&p,100)<0);
    CHECK(r.capacity <= CARPLAY_ENCODER_MAX_PACKET);
    carplay_encoder_reader_destroy(&r); close(s[0]); close(s[1]);
}

static void incomplete(void)
{
    int s[2]; unsigned char bytes[24];
    struct carplay_encoder_reader r = {0}; struct carplay_encoder_packet p;
    CHECK(socketpair(AF_UNIX,SOCK_STREAM,0,s)==0); header(bytes,0,32);
    CHECK(write(s[0],bytes,24)==24);
    CHECK(carplay_encoder_reader_read(&r,s[1],&p,100)==0);
    CHECK(carplay_encoder_reader_read(&r,s[1],&p,5101)<0 && errno==ETIMEDOUT);
    carplay_encoder_reader_destroy(&r); close(s[0]); close(s[1]);
    CHECK(socketpair(AF_UNIX,SOCK_STREAM,0,s)==0);
    CHECK(write(s[0],bytes,12)==12); close(s[0]);
    CHECK(carplay_encoder_reader_read(&r,s[1],&p,100)<0);
    carplay_encoder_reader_destroy(&r); close(s[1]);
}

static void heartbeat(void)
{
    int s[2]; unsigned char bytes[24];
    struct carplay_encoder_reader r={0}; struct carplay_encoder_packet p;
    CHECK(socketpair(AF_UNIX,SOCK_STREAM,0,s)==0);
    header(bytes,8,0);
    CHECK(write(s[0],bytes,24)==24);
    CHECK(carplay_encoder_reader_read(&r,s[1],&p,100)==1);
    CHECK(p.flags==8 && p.size==0);
    carplay_encoder_reader_destroy(&r);close(s[0]);close(s[1]);
    malformed(9,0,0,0x43504531);
    malformed(8,1,0,0x43504531);
}

static void commands_and_cleanup(void)
{
    int s[2], status; unsigned char command; struct carplay_encoder e;
    carplay_encoder_init(&e);
    CHECK(socketpair(AF_UNIX,SOCK_STREAM,0,s)==0); e.fd=s[1];
    CHECK(carplay_encoder_request_keyframe(&e)==0);
    CHECK(read(s[0],&command,1)==1 && command=='K');
    CHECK(carplay_encoder_request_refresh(&e)==0);
    CHECK(read(s[0],&command,1)==1 && command=='R');
    CHECK(carplay_encoder_stop(&e)==0 && e.fd==-1);
    CHECK(read(s[0],&command,1)==1 && command=='Q');
    close(s[0]);
    CHECK(socketpair(AF_UNIX,SOCK_STREAM,0,s)==0);
    e.pid=fork(); CHECK(e.pid>=0);
    if(e.pid==0) { close(s[1]); while(read(s[0],&command,1)>0) { if(command=='Q') _exit(0); } _exit(0); }
    pid_t child=e.pid; close(s[0]); e.fd=s[1];
    CHECK(carplay_encoder_stop(&e)==0 && e.pid==-1);
    CHECK(waitpid(child,&status,WNOHANG)==-1 && errno==ECHILD);
    CHECK(carplay_encoder_stop(&e)==0);
}

int main(void)
{
    framing(); incomplete(); commands_and_cleanup(); heartbeat();
    malformed(8,4,0,0x43504531);
    malformed(0,CARPLAY_ENCODER_MAX_PACKET+1u,0,0x43504531);
    malformed(0,4,1,0x43504531);
    malformed(0,4,0,0x42414421);
    malformed(0,0,0,0x43504531);
    puts("PASS: encoder split/coalesced packets, bounds, partial EOF/deadline, commands and child cleanup");
    return 0;
}
