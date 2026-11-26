#include "carplay_now_playing.h"
#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#include <string.h>
#include "carplay_route.h"
int main(int argc,char **argv) {
    if(argc==2 && !strcmp(argv[1],"route")){
        struct carplay_route_snapshot route;
        if(carplay_route_read(0,&route)<0){puts("route IPC unavailable");return 1;}
        printf("route state=%u maneuver=%u index=%u remaining=%u next=%s\n",route.state,route.maneuver,route.index,route.remaining,route.next);return 0;
    }
    if(argc==2) {
        struct carplay_communications c;
        if(carplay_comm_read(0,&c)<0){puts("communications unavailable");return 1;}
        printf("cellular=%u telephony=%u can_call=%u airplane=%u muted=%u\n",c.cellular,c.telephony,c.can_call,c.airplane,c.muted);return 0;
    }
    if(argc==3) {
        int rc=carplay_app_command((uint32_t)strtoul(argv[1],NULL,10),(uint32_t)strtoul(argv[2],NULL,10));
        printf("command=%s argument=%s result=%d\n",argv[1],argv[2],rc);return rc?1:0;
    }
    struct carplay_now_playing state;struct timespec t;clock_gettime(CLOCK_MONOTONIC,&t);
    if(carplay_now_read((uint64_t)t.tv_sec*1000+t.tv_nsec/1000000,&state)<0){puts("snapshot unavailable");return 1;}
    printf("status=%u position=%u duration=%u speed=%u seek=%u\npackage=%s\ntitle=%s\nalbum=%s\nartist=%s\n",
        state.status,state.position,state.duration,state.speed,state.can_seek,state.package,state.title,state.album,state.artist);
    printf("generation=%u artwork_bytes=%u queue_index=%u index_known=%d queue_count=%u count_known=%d\n",
        state.media_generation,state.art_size,state.queue_index,state.queue_index_known,state.queue_count,state.queue_count_known);
    if(state.art_size){
        uint8_t image[32768];size_t offset=0;
        while(offset<state.art_size){size_t n=0;
            if(carplay_art_read(state.media_generation,state.art_size,(uint32_t)offset,image+offset,sizeof(image)-offset,&n)<0 || !n){
                puts("artwork unavailable or generation changed");return 1;
            }offset+=n;
        }
        if(offset<4 || image[0]!=0xff || image[1]!=0xd8 || image[offset-2]!=0xff || image[offset-1]!=0xd9){puts("invalid JPEG");return 1;}
        puts("complete JPEG read through authenticated IPC");
    }
    return 0;
}
