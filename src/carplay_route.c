#include "carplay_route.h"
#include <string.h>
#include <stdio.h>
static unsigned u16(const uint8_t *p){return ((unsigned)p[0]<<8)|p[1];}
static uint32_t u32(const uint8_t *p){return ((uint32_t)u16(p)<<16)|u16(p+2);}
static void w16(uint8_t *p,unsigned v){p[0]=(uint8_t)(v>>8);p[1]=(uint8_t)v;}
static int tlv(const uint8_t *p,size_t len,size_t off,unsigned *id,size_t *size){
    if(len-off<4)return -1;
    *size=u16(p+off);*id=u16(p+off+2);
    return *size>=4 && *size<=len-off?0:-1;
}
static int text_valid(const uint8_t *p,size_t n){
    for(size_t i=0;i<n;) {
        uint32_t c=p[i++],min=0;unsigned more;
        if(c<32 || c==127)return 0;
        if(c<128)continue;
        if(c>=0xc2 && c<=0xdf){more=1;min=128;c&=31;}
        else if(c>=0xe0 && c<=0xef){more=2;min=2048;c&=15;}
        else if(c>=0xf0 && c<=0xf4){more=3;min=65536;c&=7;}
        else return 0;
        if(more>n-i)return 0;
        while(more--){if((p[i]&192)!=128)return 0;c=c*64+(p[i++]&63);}
        if(c<min || c>0x10ffff || (c>=0xd800 && c<=0xdfff))return 0;
    }
    return 1;
}
int carplay_route_identify(const uint8_t *p,size_t len,struct carplay_route_components *out){
    struct carplay_route_components c={0};
    if(!out || (!p && len))return -1;
    for(size_t off=0;off<len;){
        unsigned id;size_t size;if(tlv(p,len,off,&id,&size)<0)return -1;
        if(id==30){
            if(c.count==CARPLAY_ROUTE_COMPONENTS)return -1;
            struct carplay_route_component *v=&c.item[c.count];unsigned seen=0;
            for(size_t at=off+4;at<off+size;){
                unsigned field;size_t n;if(tlv(p,off+size,at,&field,&n)<0)return -1;
                if(field<=8){if(seen&(1u<<field))return -1;seen|=1u<<field;}
                if(field==1){if(n<6 || p[at+n-1] || !text_valid(p+at+4,n-5))return -1;}
                else if(field<=8){
                    if(n!=6)return -1;unsigned value=u16(p+at+4);
                    if(field==0)v->id=(uint16_t)value;
                    if(field==2)v->current_chars=(uint16_t)value;
                    if(field==3)v->destination_chars=(uint16_t)value;
                    if(field==4)v->next_chars=(uint16_t)value;
                }
                at+=n;
            }
            if((seen&3)!=3)return -1;
            for(unsigned i=0;i<c.count;i++)if(c.item[i].id==v->id)return -1;
            c.count++;
        }
        off+=size;
    }
    *out=c;return 0;
}
int carplay_route_subscribe(const struct carplay_route_components *c,const uint8_t *p,size_t len,struct carplay_route_subscription *out){
    struct carplay_route_subscription s={0};
    if(!c || !out || (!p && len) || len>1024 || !c->count)return -1;
    for(size_t off=0;off<len;){
        unsigned id;size_t size;if(tlv(p,len,off,&id,&size)<0)return -1;
        if(id==0){
            if(size!=6 || s.count==CARPLAY_ROUTE_COMPONENTS)return -1;
            unsigned value=u16(p+off+4),i;
            for(i=0;i<c->count;i++)if(c->item[i].id==value)break;
            if(i==c->count)return -1;
            for(i=0;i<s.count;i++)if(s.ids[i]==value)return -1;
            s.ids[s.count++]=(uint16_t)value;
        } else if(id<=5){
            if(size!=4 || (s.flags&(1u<<id)))return -1;s.flags|=1u<<id;
        }
        off+=size;
    }
    if(!s.count)for(unsigned i=0;i<c->count;i++)s.ids[s.count++]=c->item[i].id;
    *out=s;return 0;
}
int carplay_route_decode(const uint8_t *p,size_t len,uint64_t now,struct carplay_route_snapshot *out){
    struct carplay_route_snapshot s={0};
    if(!p || !out || len<36 || len>CARPLAY_ROUTE_WIRE_MAX || memcmp(p,"CPR1",4))return -1;
    s.sampled=((uint64_t)u32(p+4)<<32)|u32(p+8);
    if(s.sampled>now || now-s.sampled>10000)return -1;
    s.state=p[12];s.maneuver=p[13];s.index=(uint16_t)u16(p+14);
    if((s.state>1 && s.state!=255) || (s.maneuver>54 && s.maneuver!=255))return -1;
    s.remaining=u32(p+16);s.seconds=u32(p+20);s.next_distance=u32(p+24);
    if(s.remaining>100000000 || s.seconds>604800 || s.next_distance>100000000)return -1;
    char *fields[]={s.current,s.next,s.destination,s.source};size_t off=28;
    for(unsigned i=0;i<4;i++){
        if(len-off<2)return -1;size_t n=u16(p+off);off+=2;
        if(n>255 || n>len-off || !text_valid(p+off,n))return -1;
        memcpy(fields[i],p+off,n);off+=n;
    }
    if(off!=len)return -1;*out=s;return 0;
}
static int put(uint8_t *out,size_t cap,size_t *n,unsigned id,const void *p,size_t size){
    if(*n>cap || cap-*n<size+4)return -1;
    w16(out+*n,(unsigned)size+4);w16(out+*n+2,id);if(size)memcpy(out+*n+4,p,size);*n+=size+4;return 0;
}
static int number(uint8_t *out,size_t cap,size_t *n,unsigned id,uint64_t v,unsigned bytes){
    uint8_t b[8];for(unsigned i=0;i<bytes;i++)b[bytes-1-i]=(uint8_t)(v>>(i*8));return put(out,cap,n,id,b,bytes);
}
static int string(uint8_t *out,size_t cap,size_t *n,unsigned id,const char *s,unsigned chars){
    char b[256];size_t z=0;unsigned count=0;
    if(*n>cap || cap-*n<5)return 0; /* Optional text never blocks core state. */
    size_t budget=cap-*n-5;if(budget>255)budget=255;
    while(z<255 && s[z] && count<chars){
        unsigned char c=(unsigned char)s[z];
        size_t width=c<128?1:c<224?2:c<240?3:4;
        if(width>budget-z || width>255-z)break;
        memcpy(b+z,s+z,width);z+=width;count++;
    }
    b[z]=0;return put(out,cap,n,id,b,z+1);
}
#define NUM(id,v,bytes) do{if(number(out,cap,&n,id,v,bytes)<0)return -1;}while(0)
#define STR(id,v,chars) do{if(string(out,cap,&n,id,v,chars)<0)return -1;}while(0)
int carplay_route_update(const struct carplay_route_subscription *sub,const struct carplay_route_component *c,
    const struct carplay_route_snapshot *s,uint8_t *out,size_t cap,size_t *len){
    if(!sub || !c || !s || !out || !len)return -1;
    size_t n=0;NUM(0,c->id,2);
    if(s->state!=255)NUM(1,s->state,1);
    if(s->state==1 && s->maneuver!=255 && s->index!=65535)NUM(13,s->index,2);
    else if(put(out,cap,&n,13,NULL,0)<0)return -1;
    if((sub->flags&4) && cap-n>=5)NUM(20,s->state!=255,1);
    /* Reserve empty road fields so stale labels can be cleared on small peers. */
    size_t reserve=(c->current_chars?5:0)+(c->destination_chars?5:0);
    if(s->state==1 && cap-n>=reserve+28){
        NUM(6,s->seconds,8);NUM(7,s->remaining,4);NUM(10,s->next_distance,4);
    }
    if(s->state==1 && cap-n>=reserve+66){
        char text[24];snprintf(text,sizeof(text),"%u",s->remaining);STR(8,text,23);NUM(9,2,1);
        snprintf(text,sizeof(text),"%u",s->next_distance);STR(11,text,23);NUM(12,2,1);
    }
    if(c->current_chars){
        size_t text_cap=cap;if(c->destination_chars && cap-n>=10)text_cap-=5;
        if(string(out,text_cap,&n,3,s->state==1?s->current:"",c->current_chars)<0)return -1;
    }
    if(c->destination_chars)STR(4,s->state==1?s->destination:"",c->destination_chars);
    if(sub->flags&2)STR(19,s->source,255);
    *len=n;return 0;
}
int carplay_route_maneuver(const struct carplay_route_component *c,const struct carplay_route_snapshot *s,uint8_t *out,size_t cap,size_t *len){
    if(!c || !s || !out || !len || s->state!=1 || s->maneuver==255 || s->index==65535)return -1;
    size_t n=0;NUM(0,c->id,2);NUM(1,s->index,2);NUM(3,s->maneuver,1);
    if(c->next_chars)STR(4,s->next,c->next_chars);
    *len=n;return 0;
}
