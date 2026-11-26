#define _POSIX_C_SOURCE 200809L
#include "carplay_keys.h"
#include <errno.h>
#include <fcntl.h>
#include <linux/input.h>
#include <linux/uinput.h>
#include <poll.h>
#include <pthread.h>
#include <signal.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <time.h>
#include <unistd.h>

struct carplay_keys {
    int fd, wake[2], uinput, suspended, resetting, stopping, failed, active;
    unsigned head, count, queued_actions;
    uint64_t generation;
    struct { int code; uint32_t count; } queue[CARPLAY_KEYS_QUEUE_ENTRIES];
    pthread_mutex_t mutex;
    pthread_cond_t idle;
    pthread_t thread;
    struct carplay_keys_stats stats;
    atomic_uint_fast64_t rejected;
};

/* Gaode's focus ring consumes DPAD_CENTER without activating the selected
 * control. Its keyboard path handles ENTER, so the physical knob select emits
 * Linux KEY_ENTER (Android 8.1 maps it to KEYCODE_ENTER). SELECT remains an
 * accepted alias for older queued reports. */
static int linux_key(int key)
{
    switch (key) {
    case 19: return KEY_UP;
    case 20: return KEY_DOWN;
    case 21: return KEY_LEFT;
    case 22: return KEY_RIGHT;
    case 66: return KEY_ENTER;
    case 23: return KEY_SELECT;
    case 3: return KEY_HOMEPAGE;
    case 4: return KEY_BACK;
    default: return 0;
    }
}

static int nonblock_cloexec(int fd)
{
    int flags=fcntl(fd,F_GETFL);
    if (flags<0 || fcntl(fd,F_SETFL,flags|O_NONBLOCK)<0) return -1;
    return fcntl(fd,F_SETFD,FD_CLOEXEC);
}

static void notify(struct carplay_keys *w)
{
    uint8_t byte=1;
    /* A full pipe already wakes the worker. Never block a report producer. */
    ssize_t result=write(w->wake[1],&byte,1);
    (void)result;
}

static void drain_wake(struct carplay_keys *w)
{
    uint8_t bytes[128];
    while (read(w->wake[0],bytes,sizeof(bytes))>0) {}
}

static int cancelled(struct carplay_keys *w,uint64_t generation)
{
    pthread_mutex_lock(&w->mutex);
    int value=w->stopping || w->generation!=generation;
    pthread_mutex_unlock(&w->mutex);
    return value;
}

static long long monotonic_ms(void)
{
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC,&t);
    return (long long)t.tv_sec*1000+t.tv_nsec/1000000;
}

/* One complete tap is submitted in a single write. Pipe sinks write this
 * atomically; a partial socket/device write is finished before cancellation.
 * Only that partial-write recovery has a 100ms deadline. Backpressure before
 * a key is down waits on both fd and cancellation pipe, without losing taps. */
static int emit_tap(struct carplay_keys *w,int key,uint64_t generation)
{
    struct input_event events[4];
    memset(events,0,sizeof(events));
    events[0].type=EV_KEY; events[0].code=(uint16_t)key; events[0].value=1;
    events[1].type=EV_SYN; events[1].code=SYN_REPORT;
    events[2].type=EV_KEY; events[2].code=(uint16_t)key;
    events[3].type=EV_SYN; events[3].code=SYN_REPORT;
    size_t offset=0;
    long long deadline=0;
    while (offset<sizeof(events)) {
        if (!offset && cancelled(w,generation)) return 1;
        ssize_t n=write(w->fd,(const uint8_t *)events+offset,sizeof(events)-offset);
        if (n>0) {
            offset+=(size_t)n;
            if (offset<sizeof(events) && !deadline) deadline=monotonic_ms()+100;
            continue;
        }
        if (n<0 && errno==EINTR) continue;
        if (n==0 || (errno!=EAGAIN && errno!=EWOULDBLOCK)) return -1;
        int timeout=-1;
        if (offset) {
            long long left=deadline-monotonic_ms();
            if (left<=0) return -1;
            timeout=(int)left;
        }
        struct pollfd fds[2]={{w->fd,POLLOUT,0},{w->wake[0],POLLIN,0}};
        int r=poll(fds,2,timeout);
        if (r<0 && errno==EINTR) continue;
        if (r<=0 || (fds[0].revents&(POLLERR|POLLHUP|POLLNVAL))) return -1;
        if (fds[1].revents&POLLIN) drain_wake(w);
    }
    return 0;
}

static void discard_queue(struct carplay_keys *w)
{
    w->stats.cancelled+=w->queued_actions;
    w->count=w->head=w->queued_actions=0;
}

static void *worker(void *arg)
{
    struct carplay_keys *w=arg;
    sigset_t signals;
    sigemptyset(&signals); sigaddset(&signals,SIGPIPE);
    pthread_sigmask(SIG_BLOCK,&signals,NULL);
    for (;;) {
        pthread_mutex_lock(&w->mutex);
        if (w->stopping || w->failed) {
            pthread_mutex_unlock(&w->mutex);
            break;
        }
        if (!w->count || w->suspended || w->resetting) {
            pthread_mutex_unlock(&w->mutex);
            struct pollfd wake={w->wake[0],POLLIN,0};
            int r=poll(&wake,1,-1);
            if (r<0 && errno==EINTR) continue;
            if (r<=0) break;
            drain_wake(w);
            continue;
        }
        int code=w->queue[w->head].code;
        if (!--w->queue[w->head].count) {
            w->head=(w->head+1)%CARPLAY_KEYS_QUEUE_ENTRIES;
            w->count--;
        }
        w->queued_actions--;
        w->active=1;
        uint64_t generation=w->generation;
        pthread_mutex_unlock(&w->mutex);

        int result=emit_tap(w,code,generation);
        /* Device removal releases every key even on an irrecoverable partial
         * uinput write. Mark it unavailable; a new session can create anew. */
        if (result<0 && w->uinput) {
            ioctl(w->fd,UI_DEV_DESTROY);
            close(w->fd); /* Also removes the device if destroy ioctl failed. */
            w->fd=-1;
            w->uinput=0;
        }
        pthread_mutex_lock(&w->mutex);
        if (!result) w->stats.emitted++;
        else if (result>0) w->stats.cancelled++;
        else {
            w->stats.failures++;
            w->failed=1;
            discard_queue(w);
        }
        w->active=0;
        pthread_cond_broadcast(&w->idle);
        pthread_mutex_unlock(&w->mutex);
    }
    return NULL;
}

static int start(struct carplay_keys **out,int fd,int uinput)
{
    struct carplay_keys *w=calloc(1,sizeof(*w));
    if (!w) goto failed_fd;
    w->fd=fd; w->uinput=uinput;
    atomic_init(&w->rejected,0);
    if (pthread_mutex_init(&w->mutex,NULL)) goto failed_alloc;
    if (pthread_cond_init(&w->idle,NULL)) goto failed_mutex;
    if (pipe(w->wake)<0) goto failed_cond;
    if (nonblock_cloexec(w->wake[0])<0 || nonblock_cloexec(w->wake[1])<0) goto failed_pipe;
    if (pthread_create(&w->thread,NULL,worker,w)) goto failed_pipe;
    *out=w;
    return 0;
failed_pipe:
    close(w->wake[0]); close(w->wake[1]);
failed_cond:
    pthread_cond_destroy(&w->idle);
failed_mutex:
    pthread_mutex_destroy(&w->mutex);
failed_alloc:
    free(w);
failed_fd:
    if (uinput) ioctl(fd,UI_DEV_DESTROY);
    close(fd);
    return -1;
}

int carplay_keys_create_fd(struct carplay_keys **out,int fd)
{
    if (!out) return -1;
    *out=NULL;
    int owned=fcntl(fd,F_DUPFD_CLOEXEC,3);
    if (owned<0) return -1;
    if (nonblock_cloexec(owned)<0) { close(owned); return -1; }
    return start(out,owned,0);
}

int carplay_keys_create(struct carplay_keys **out,const char *path)
{
    if (!out) return -1;
    *out=NULL;
    int fd=open(path?path:"/dev/uinput",O_WRONLY|O_NONBLOCK|O_CLOEXEC);
    if (fd<0 && !path) fd=open("/dev/input/uinput",O_WRONLY|O_NONBLOCK|O_CLOEXEC);
    if (fd<0) return -1;
    if (ioctl(fd,UI_SET_EVBIT,EV_KEY)<0 || ioctl(fd,UI_SET_EVBIT,EV_SYN)<0) goto failed;
    const int keys[]={KEY_UP,KEY_DOWN,KEY_LEFT,KEY_RIGHT,KEY_ENTER,KEY_SELECT,KEY_HOMEPAGE,KEY_BACK};
    for (unsigned i=0;i<sizeof(keys)/sizeof(keys[0]);i++)
        if (ioctl(fd,UI_SET_KEYBIT,keys[i])<0) goto failed;
    struct uinput_user_dev device;
    memset(&device,0,sizeof(device));
    memcpy(device.name,"CarPlay rotary keyboard",sizeof("CarPlay rotary keyboard"));
    device.id.bustype=BUS_VIRTUAL; device.id.version=1;
    /* Android 8.1 kernels support this legacy create protocol; no modern
     * UI_DEV_SETUP ioctl or per-event subprocess is required. */
    if (write(fd,&device,sizeof(device))!=(ssize_t)sizeof(device) || ioctl(fd,UI_DEV_CREATE)<0) goto failed;
    return start(out,fd,1);
failed:
    close(fd);
    return -1;
}

static int reject(struct carplay_keys *w,int result)
{
    if (w) atomic_fetch_add_explicit(&w->rejected,1,memory_order_relaxed);
    return result;
}

int carplay_keys_submit(struct carplay_keys *w,const struct carplay_key_batch *batch)
{
    if (!w || !batch || batch->count>CARPLAY_KEY_BATCH_ENTRIES) return reject(w,-1);
    uint64_t actions=0;
    for (unsigned i=0;i<batch->count;i++) {
        if (!linux_key(batch->entries[i].keycode) || !batch->entries[i].count) return reject(w,-1);
        actions+=batch->entries[i].count;
    }
    /* Every holder keeps this mutex only for bounded in-memory bookkeeping.
     * Device IO and lifecycle waits release it. Do not drop a valid report
     * just because the worker is briefly updating the queue. */
    pthread_mutex_lock(&w->mutex);
    int result=0;
    if (w->stopping || w->failed || w->suspended || w->resetting) result=-1;
    else if (batch->count>CARPLAY_KEYS_QUEUE_ENTRIES-w->count ||
        actions>CARPLAY_KEYS_QUEUE_ACTIONS-w->queued_actions-(unsigned)w->active) result=-2;
    if (!result) {
        for (unsigned i=0;i<batch->count;i++) {
            unsigned tail=(w->head+w->count++)%CARPLAY_KEYS_QUEUE_ENTRIES;
            w->queue[tail].code=linux_key(batch->entries[i].keycode);
            w->queue[tail].count=batch->entries[i].count;
        }
        w->queued_actions+=(unsigned)actions;
        w->stats.accepted+=actions;
    }
    pthread_mutex_unlock(&w->mutex);
    if (result) return reject(w,result);
    if (!result && actions) notify(w);
    return result;
}

static void reset(struct carplay_keys *w,int suspend)
{
    if (!w) return;
    pthread_mutex_lock(&w->mutex);
    if (suspend>=0 && w->suspended==(suspend!=0)) {
        pthread_mutex_unlock(&w->mutex);
        return;
    }
    w->resetting=1;
    if (suspend>=0) w->suspended=suspend!=0;
    w->generation++;
    discard_queue(w);
    notify(w);
    /* The worker can cancel a blocked atomic tap immediately. If a sink made
     * a partial write, finish its release within 100ms or remove the device. */
    struct timespec deadline;
    clock_gettime(CLOCK_REALTIME,&deadline);
    deadline.tv_nsec+=200000000;
    if (deadline.tv_nsec>=1000000000) { deadline.tv_sec++; deadline.tv_nsec-=1000000000; }
    while (w->active) {
        if (pthread_cond_timedwait(&w->idle,&w->mutex,&deadline)) break;
    }
    w->resetting=0;
    pthread_mutex_unlock(&w->mutex);
    notify(w);
}

void carplay_keys_clear(struct carplay_keys *w) { reset(w,-1); }
void carplay_keys_suspend(struct carplay_keys *w,int suspended) { reset(w,suspended!=0); }

void carplay_keys_get_stats(struct carplay_keys *w,struct carplay_keys_stats *stats)
{
    if (!stats) return;
    memset(stats,0,sizeof(*stats));
    if (!w) return;
    pthread_mutex_lock(&w->mutex);
    *stats=w->stats;
    stats->rejected=atomic_load_explicit(&w->rejected,memory_order_relaxed);
    pthread_mutex_unlock(&w->mutex);
}

void carplay_keys_destroy(struct carplay_keys *w)
{
    if (!w) return;
    pthread_mutex_lock(&w->mutex);
    w->stopping=1; w->generation++;
    discard_queue(w);
    pthread_mutex_unlock(&w->mutex);
    notify(w);
    pthread_join(w->thread,NULL);
    if (w->uinput) ioctl(w->fd,UI_DEV_DESTROY);
    close(w->fd); close(w->wake[0]); close(w->wake[1]);
    pthread_cond_destroy(&w->idle);
    pthread_mutex_destroy(&w->mutex);
    free(w);
}
