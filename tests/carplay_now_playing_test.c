#include "carplay_now_playing.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

int main(void) {
    struct carplay_now_playing p, old;
    struct carplay_now_subscription sub = {0}, before;
    unsigned char wire[2048] = {'C','P','N','1',0,0,0,0,0,0,3,232,1,1,0,100,
        0,0,117,48,0,0,3,232};
    size_t n = 24, out_n;
    unsigned char out[2048];
    const char *strings[] = {"歌曲", "album", "artist", "", "Music", "app.music"};
    for (int i=0;i<6;i++) {
        size_t len=strlen(strings[i]); wire[n++]=0;wire[n++]=(unsigned char)len;
        memcpy(wire+n,strings[i],len);n+=len;
    }
    assert(carplay_now_decode(wire,n,1200,&p)==0);
    assert(p.status==1 && p.position==1000 && p.duration==30000);
    assert(!strcmp(p.title,"歌曲")); old=p;
    wire[3]='2';memset(wire+n,0,16);wire[n+3]=9;wire[n+7]=3;wire[n+11]=2;wire[n+15]=8;
    assert(carplay_now_decode(wire,n+16,1200,&p)==0);
    wire[3]='3';memset(wire+n+16,0,9);wire[n+23]=123;wire[n+24]=1;
    assert(carplay_now_decode(wire,n+25,1200,&p)==0 && p.queue_version==123 && p.queue_available);
    wire[n+24]=2;assert(carplay_now_decode(wire,n+25,1200,&p)<0);
    wire[3]='1';p=old;
    assert(carplay_now_decode(wire,n-1,1200,&p)<0);
    assert(!memcmp(&p,&old,sizeof(p))); /* malformed decode is atomic */
    assert(carplay_now_decode(wire,n,6001,&p)<0);
    assert(carplay_now_decode(wire,n,999,&p)<0);
    wire[26]=0xff; assert(carplay_now_decode(wire,n,1200,&p)<0); wire[26]=0xe6;
    unsigned char request[]={0,12,0,0,0,4,0,1,0,4,0,4,
        0,24,0,1,0,4,0,0,0,4,0,1,0,4,0,13,0,8,0,17,0,0,0,10};
    assert(carplay_now_subscribe(&sub,request,sizeof(request))==0);
    assert(sub.media==((1u<<1)|(1u<<4)));
    before=sub;
    assert(carplay_now_subscribe(&sub,request,sizeof(request)-1)<0);
    assert(!memcmp(&sub,&before,sizeof(sub)));
    unsigned char duplicate[]={0,12,0,0,0,4,0,1,0,4,0,1};
    assert(carplay_now_subscribe(&sub,duplicate,sizeof(duplicate))<0);
    assert(carplay_now_encode(&sub,&old,out,sizeof(out),&out_n)==0);
    assert(out_n==45); /* title+duration and state+position+seek */
    assert(out[3]==0 && out[7]==1 && out[8]==0xe6);
    assert(carplay_now_encode(&sub,&old,out,4,&out_n)<0);
    sub.media=(1u<<1)|(1u<<4)|(1u<<6)|(1u<<12)|(1u<<18);
    sub.playback=(1u<<0)|(1u<<1)|(1u<<7)|(1u<<12)|(1u<<13)|(1u<<14)|(1u<<16);
    memset(old.album,'x',255);old.album[255]=0;
    assert(carplay_now_encode(&sub,&old,out,128,&out_n)==0 && out_n<=128);
    old.status=255;sub.media=0;sub.playback=(1u<<0)|(1u<<1)|(1u<<12)|(1u<<13);
    assert(carplay_now_encode(&sub,&old,out,sizeof(out),&out_n)==0 && out_n==0);
    assert(carplay_now_subscribe(&sub,NULL,0)==0);
    assert(carplay_now_encode(&sub,&old,out,sizeof(out),&out_n)==0 && out_n==0);
    struct carplay_communications comm, saved;
    unsigned char caps[]={'C','P','C','1',0,0,0,0,0,0,3,232,1,0,0,0,1};
    assert(carplay_comm_decode(caps,sizeof(caps),1200,&comm)==0);
    assert(comm.airplane && !comm.cellular && comm.muted);saved=comm;
    caps[13]=2;assert(carplay_comm_decode(caps,sizeof(caps),1200,&comm)<0);
    assert(!memcmp(&comm,&saved,sizeof(comm)));caps[13]=0;
    assert(carplay_comm_decode(caps,sizeof(caps),6001,&comm)<0);
    assert(carplay_comm_encode((1u<<2)|(1u<<5)|(1u<<10),&comm,out,sizeof(out),&out_n)==0);
    assert(out_n==10 && out[3]==2 && out[4]==1 && out[8]==5 && out[9]==0);
    puts("now playing parser/serializer: PASS"); return 0;
}
