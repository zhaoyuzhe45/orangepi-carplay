#define _POSIX_C_SOURCE 200809L
#include "carplay_encoder.h"
#include "airplay_h264.h"
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>
static uint64_t now_ms(void) {
    struct timespec t; clock_gettime(CLOCK_MONOTONIC,&t);
    return (uint64_t)t.tv_sec*1000u+(unsigned)t.tv_nsec/1000000u;
}
static int write_nal(FILE *out,const uint8_t *nal,size_t len) {
    static const uint8_t prefix[]={0,0,0,1};
    return fwrite(prefix,1,4,out)==4 && fwrite(nal,1,len,out)==len?0:-1;
}
static int config_out(void *ctx,const uint8_t *data,size_t len) {
    FILE *out=ctx;size_t offset=6;
    if(len<7)return -1;
    unsigned count=data[5]&31u;
    for(unsigned group=0;group<2;group++) {
        for(unsigned i=0;i<count;i++) {
            if(offset+2>len)return -1;
            size_t n=(size_t)data[offset]*256+data[offset+1];offset+=2;
            if(n>len-offset || write_nal(out,data+offset,n)<0)return -1;
            offset+=n;
        }
        if(!group) {if(offset>=len)return -1;count=data[offset++];}
    }
    return offset==len?0:-1;
}
static int frame_out(void *ctx,const uint8_t *nal,size_t len,int key) {
    (void)key;return write_nal(ctx,nal,len);
}
int main(int argc,char **argv) {
    if(argc!=6)return 64;
    unsigned w=(unsigned)atoi(argv[1]),h=(unsigned)atoi(argv[2]);
    unsigned sw=(unsigned)atoi(argv[3]),sh=(unsigned)atoi(argv[4]);
    struct carplay_encoder encoder;carplay_encoder_init(&encoder);
    FILE *out=fopen(argv[5],"wb");if(!out)return 1;
    uint8_t *buffer=malloc(CARPLAY_ENCODER_MAX_PACKET);
    if(!buffer){fclose(out);return 1;}
    struct airplay_h264_parser parser;
    airplay_h264_parser_init(&parser,buffer,CARPLAY_ENCODER_MAX_PACKET,config_out,frame_out,out);
    /* Exercise the same SPS padding removal as the production sender. */
    airplay_h264_parser_set_stream_crop(&parser,w,h);
    unsigned config=0,frames=0,keys=0;int result=1;
    if(carplay_encoder_start_scaled(&encoder,w,h,8000000,30,sw,sh)<0)goto done;
    uint64_t deadline=now_ms()+15000;
    while(now_ms()<deadline && frames<3) {
        struct carplay_encoder_packet p;
        int got=carplay_encoder_reader_read(&encoder.reader,encoder.fd,&p,now_ms());
        if(got<0)break;
        if(got) {
            if(p.flags==CARPLAY_ENCODER_HEARTBEAT)continue;
            if(airplay_h264_feed(&parser,p.data,p.size)<0 || airplay_h264_flush(&parser)<0)break;
            if(p.flags&CARPLAY_ENCODER_CONFIG)config++;
            else if(p.size)frames++;
            if(p.flags&CARPLAY_ENCODER_KEYFRAME)keys++;
        } else {struct pollfd fd={encoder.fd,POLLIN,0};poll(&fd,1,50);}
    }
    result=!(config && frames>=3 && keys);
done:
    carplay_encoder_stop(&encoder);free(buffer);fclose(out);
    printf("scaled encoder source=%ux%u output=%ux%u config=%u frames=%u keys=%u %s\n",sw,sh,w,h,config,frames,keys,result?"FAIL":"PASS");
    return result;
}
