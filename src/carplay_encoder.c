#define _POSIX_C_SOURCE 200809L
#include "carplay_encoder.h"
#include "carplay_config.h"
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/prctl.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#define ENCODER_JAR "/data/carplay/bin/carplay-encoder.jar"
#define ENCODER_LOG "/data/carplay/logs/encoder.err"

static uint32_t get32(const uint8_t *p)
{ return (uint32_t)p[0]<<24 | (uint32_t)p[1]<<16 | (uint32_t)p[2]<<8 | p[3]; }

void carplay_encoder_reader_destroy(struct carplay_encoder_reader *r)
{
    if (!r) return;
    free(r->payload);
    memset(r, 0, sizeof(*r));
}

int carplay_encoder_reader_read(struct carplay_encoder_reader *r, int fd,
                                struct carplay_encoder_packet *p, uint64_t now)
{
    if (!r || !p || fd < 0) { errno=EINVAL; return -1; }
    if (r->delivered) {
        r->header_used=0; r->payload_used=0; r->payload_size=0;
        r->partial_since_ms=0; r->delivered=0;
    }
    if (r->header_used && now-r->partial_since_ms >= 5000u) {
        errno=ETIMEDOUT; return -1;
    }
    for (;;) {
        uint8_t *target;
        size_t remaining;
        if (r->header_used<sizeof(r->header)) {
            target=r->header+r->header_used;
            remaining=sizeof(r->header)-r->header_used;
        } else {
            target=r->payload ? r->payload+r->payload_used : NULL;
            remaining=r->payload_size-r->payload_used;
        }
        if (remaining) {
            ssize_t n=recv(fd,target,remaining,MSG_DONTWAIT);
            if (n<0) {
                if (errno==EINTR) continue;
                if (errno==EAGAIN || errno==EWOULDBLOCK) return 0;
                return -1;
            }
            if (!n) { errno=ECONNRESET; return -1; }
            if (r->header_used<sizeof(r->header)) {
                if (!r->header_used) r->partial_since_ms=now;
                r->header_used+=(size_t)n;
                if (r->header_used<sizeof(r->header)) continue;
                r->flags=get32(r->header+4);
                r->payload_size=get32(r->header+8);
                r->pts_us=(uint64_t)get32(r->header+16)<<32 | get32(r->header+20);
                int heartbeat=r->flags==CARPLAY_ENCODER_HEARTBEAT;
                if (get32(r->header)!=0x43504531u || (r->flags & ~15u) ||
                    get32(r->header+12) || r->payload_size>CARPLAY_ENCODER_MAX_PACKET ||
                    ((r->flags & CARPLAY_ENCODER_HEARTBEAT) && (!heartbeat || r->payload_size)) ||
                    (!r->payload_size && !(r->flags & CARPLAY_ENCODER_EOS) && !heartbeat)) {
                    errno=EPROTO; return -1;
                }
                if (r->payload_size>r->capacity) {
                    uint8_t *next=realloc(r->payload,r->payload_size);
                    if (!next) return -1;
                    r->payload=next; r->capacity=r->payload_size;
                }
            } else r->payload_used+=(size_t)n;
        }
        if (r->header_used==sizeof(r->header) && r->payload_used==r->payload_size) {
            p->data=r->payload; p->size=r->payload_size;
            p->flags=r->flags; p->pts_us=r->pts_us;
            r->delivered=1;
            return 1;
        }
    }
}

void carplay_encoder_init(struct carplay_encoder *e)
{
    memset(e,0,sizeof(*e)); e->fd=-1; e->pid=-1;
}

int carplay_encoder_start(struct carplay_encoder *e, unsigned width,
                          unsigned height, unsigned bitrate, unsigned fps)
{
    return carplay_encoder_start_scaled(e, width, height, bitrate, fps, width, height);
}

int carplay_encoder_start_scaled(struct carplay_encoder *e, unsigned width,
    unsigned height, unsigned bitrate, unsigned fps,
    unsigned source_width, unsigned source_height)
{
    int pair[2], logfd;
    char width_arg[16],height_arg[16],bitrate_arg[16],fps_arg[16];
    char source_width_arg[16],source_height_arg[16];
    char *const env[] = { "CLASSPATH=" ENCODER_JAR, "ANDROID_ROOT=/system",
        "ANDROID_DATA=/data", "PATH=/system/bin:/system/xbin:/vendor/bin", NULL };
    pid_t parent=getpid(), child;
    if (bitrate > CARPLAY_VIDEO_BITRATE_MAX)
        bitrate = CARPLAY_VIDEO_BITRATE_MAX;
    if (!e || e->fd>=0 || e->pid>0 || !width || !height ||
        width>CARPLAY_ENCODER_MAX_WIDTH || height>CARPLAY_ENCODER_MAX_HEIGHT || width%2 || height%2 ||
        bitrate<100000 || !fps || fps>60) {
        errno=EINVAL; return -1;
    }
    if (access(ENCODER_JAR,R_OK)<0) return -1;
    snprintf(width_arg,sizeof(width_arg),"%u",width);
    snprintf(height_arg,sizeof(height_arg),"%u",height);
    snprintf(bitrate_arg,sizeof(bitrate_arg),"%u",bitrate);
    snprintf(fps_arg,sizeof(fps_arg),"%u",fps);
    if (!source_width || !source_height || (source_width & 1u) || (source_height & 1u)) {
        errno = EINVAL;
        return -1;
    }
    snprintf(source_width_arg,sizeof(source_width_arg),"%u",source_width);
    snprintf(source_height_arg,sizeof(source_height_arg),"%u",source_height);
    if (socketpair(AF_UNIX,SOCK_STREAM|SOCK_CLOEXEC,0,pair)<0) return -1;
    logfd=open(ENCODER_LOG,O_WRONLY|O_CREAT|O_APPEND|O_CLOEXEC,0644);
    if (logfd<0) { close(pair[0]); close(pair[1]); return -1; }
    child=fork();
    if (!child) {
        if (prctl(PR_SET_PDEATHSIG,SIGTERM)<0 || getppid()!=parent) _exit(126);
        if (dup2(pair[1],STDIN_FILENO)<0 || dup2(pair[1],STDOUT_FILENO)<0 ||
            dup2(logfd,STDERR_FILENO)<0) _exit(126);
        for (int fd=3; fd<4096; ++fd) close(fd);
        char *const args[] = { "/system/bin/app_process", "/system/bin",
            "--nice-name=carplay-encoder", "com.carplaybox.encoder.ScreenEncoder",
            width_arg,height_arg,bitrate_arg,fps_arg,source_width_arg,source_height_arg,NULL };
        execve(args[0],args,env);
        _exit(127);
    }
    int saved=errno;
    close(pair[1]); close(logfd);
    if (child<0) { close(pair[0]); errno=saved; return -1; }
    e->fd=pair[0]; e->pid=child;
    return 0;
}

static int encoder_command(struct carplay_encoder *e, char command)
{
    if (!e || e->fd<0) { errno=EINVAL; return -1; }
    ssize_t n;
    do { n=send(e->fd,&command,1,MSG_NOSIGNAL|MSG_DONTWAIT); } while(n<0 && errno==EINTR);
    return n==1 ? 0 : -1;
}

int carplay_encoder_request_keyframe(struct carplay_encoder *e)
{ return encoder_command(e,'K'); }

int carplay_encoder_request_refresh(struct carplay_encoder *e)
{ return encoder_command(e,'R'); }

static int wait_child(pid_t child, unsigned iterations)
{
    for (unsigned i=0;i<iterations;i++) {
        pid_t result=waitpid(child,NULL,WNOHANG);
        if (result==child || (result<0 && errno==ECHILD)) return 1;
        if (result<0 && errno!=EINTR) return -1;
        struct timespec delay={0,10000000};
        nanosleep(&delay,NULL);
    }
    return 0;
}

int carplay_encoder_stop(struct carplay_encoder *e)
{
    int result=0;
    if (!e) return 0;
    if (e->fd>=0) {
        (void)send(e->fd,"Q",1,MSG_NOSIGNAL|MSG_DONTWAIT);
        (void)shutdown(e->fd,SHUT_RDWR);
        close(e->fd); e->fd=-1;
    }
    if (e->pid>0) {
        int done=wait_child(e->pid,50);
        if (!done) { kill(e->pid,SIGTERM); done=wait_child(e->pid,50); }
        if (!done) {
            kill(e->pid,SIGKILL);
            while (waitpid(e->pid,NULL,0)<0) {
                if (errno!=EINTR) { if (errno!=ECHILD) result=-1; break; }
            }
        } else if (done<0) result=-1;
        e->pid=-1;
    }
    carplay_encoder_reader_destroy(&e->reader);
    return result;
}
