#define _POSIX_C_SOURCE 200809L
#include "carplay_keys.h"
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <linux/input.h>
#include <poll.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

static uint64_t now_ns(void)
{
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC,&t);
    return (uint64_t)t.tv_sec*UINT64_C(1000000000)+(uint64_t)t.tv_nsec;
}

static void delay_ms(unsigned ms)
{
    struct timespec t={(time_t)(ms/1000),(long)(ms%1000)*1000000};
    while(nanosleep(&t,&t)<0 && errno==EINTR) {}
}

static int keyboard_path(char *out,size_t capacity)
{
    DIR *dir=opendir("/sys/class/input");
    if(!dir)return -1;
    struct dirent *entry;
    int found=-1;
    while((entry=readdir(dir))) {
        if(strncmp(entry->d_name,"event",5))continue;
        char path[512],name[128];
        int length=snprintf(path,sizeof(path),"/sys/class/input/%s/device/name",entry->d_name);
        if(length<0 || (size_t)length>=sizeof(path))continue;
        FILE *f=fopen(path,"r");
        if(!f)continue;
        int read_ok=fgets(name,sizeof(name),f)!=NULL;
        fclose(f);
        if(!read_ok)continue;
        name[strcspn(name,"\r\n")]=0;
        if(strcmp(name,"CarPlay rotary keyboard"))continue;
        length=snprintf(out,capacity,"/dev/input/%s",entry->d_name);
        if(length>0 && (size_t)length<capacity)found=0;
        break;
    }
    closedir(dir);
    return found;
}

int main(void)
{
    struct carplay_keys *worker=NULL;
    if(carplay_keys_create(&worker,NULL)<0) {
        printf("FAIL carplay_keys_create errno=%d %s\n",errno,strerror(errno));
        return 2;
    }
    char path[128]={0};
    int fd=-1;
    /* ueventd creates /dev/input after the kernel sysfs event appears. */
    for(unsigned i=0;i<100;i++) {
        if(keyboard_path(path,sizeof(path))==0)
            fd=open(path,O_RDONLY|O_NONBLOCK|O_CLOEXEC);
        if(fd>=0)break;
        delay_ms(50);
    }
    if(fd<0) {
        printf("FAIL opening keyboard event path=%s errno=%d\n",path,errno);
        carplay_keys_destroy(worker);
        return 3;
    }
    printf("CREATED name=CarPlay rotary keyboard event=%s enumerate_wait_ms=6000\n",path);
    fflush(stdout);
    /* Probe only: give InputReader discovery time while the host captures dump. */
    delay_ms(6000);
    struct carplay_key_batch batch={0};
    batch.count=2;
    batch.entries[0].keycode=22;batch.entries[0].count=5;
    batch.entries[1].keycode=21;batch.entries[1].count=5;
    uint64_t began=now_ns();
    int result=carplay_keys_submit(worker,&batch);
    uint64_t submitted=now_ns();
    printf("SUBMIT result=%d duration_ns=%llu RIGHT=5 LEFT=5\n",result,(unsigned long long)(submitted-began));
    fflush(stdout);
    unsigned keys=0,syncs=0,bad=0;
    uint64_t end=now_ns()+UINT64_C(3000000000);
    while((keys<20 || syncs<20) && now_ns()<end) {
        struct pollfd p={fd,POLLIN,0};
        if(poll(&p,1,100)<=0)continue;
        struct input_event events[64];
        ssize_t n=read(fd,events,sizeof(events));
        if(n<0 && (errno==EAGAIN || errno==EINTR))continue;
        if(n<=0 || (size_t)n%sizeof(events[0])){bad=1;break;}
        for(size_t i=0;i<(size_t)n/sizeof(events[0]);i++) {
            struct input_event *event=&events[i];
            if(event->type==EV_KEY) {
                unsigned expected_code=keys<10?KEY_RIGHT:KEY_LEFT;
                int expected_value=keys%2?0:1;
                printf("RAW key=%u value=%d\n",event->code,event->value);
                if(event->code!=expected_code || event->value!=expected_value)bad=1;
                keys++;
            } else if(event->type==EV_SYN) {
                if(event->code!=SYN_REPORT || event->value)bad=1;
                syncs++;
            }
        }
    }
    struct carplay_keys_stats stats={0};
    for(unsigned i=0;i<100;i++) {
        carplay_keys_get_stats(worker,&stats);
        if(stats.emitted==10 || stats.failures)break;
        delay_ms(1);
    }
    printf("OBSERVED keys=%u syn=%u bad=%u elapsed_ns=%llu accepted=%llu emitted=%llu rejected=%llu cancelled=%llu failures=%llu\n",
        keys,syncs,bad,(unsigned long long)(now_ns()-began),
        (unsigned long long)stats.accepted,(unsigned long long)stats.emitted,
        (unsigned long long)stats.rejected,(unsigned long long)stats.cancelled,(unsigned long long)stats.failures);
    printf("POST_HOLD ms=5000 for Android InputReader dump\n");
    fflush(stdout);
    delay_ms(5000);
    close(fd);
    uint64_t close_began=now_ns();
    carplay_keys_destroy(worker);
    printf("DESTROYED duration_ns=%llu result=%s\n",(unsigned long long)(now_ns()-close_began),
        result==0 && keys==20 && syncs==20 && !bad && stats.accepted==10 && stats.emitted==10 && !stats.failures?"PASS":"FAIL");
    return result==0 && keys==20 && syncs==20 && !bad && stats.accepted==10 && stats.emitted==10 && !stats.failures?0:1;
}
