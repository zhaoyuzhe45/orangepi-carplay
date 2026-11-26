#include "carplay_calls.h"
#include <string.h>
static uint16_t u16(const uint8_t *p) { return (uint16_t)((p[0]<<8)|p[1]); }
static int uuid_valid(const char *id) {
    for(unsigned i=0;i<36;i++) {
        if(i==8 || i==13 || i==18 || i==23) { if(id[i]!='-')return 0; }
        else if(!((id[i]>='0' && id[i]<='9') || (id[i]>='a' && id[i]<='f')))return 0;
    }
    return id[36]==0;
}
int carplay_calls_decode(const uint8_t *wire,size_t len,uint64_t now,struct carplay_call_list *out) {
    struct carplay_call_list list;
    if(!wire || !out || len<13 || len>2048 || memcmp(wire,"CPB1",4))return -1;
    memset(&list,0,sizeof(list));
    for(unsigned i=4;i<12;i++)list.sampled_ms=(list.sampled_ms<<8)|wire[i];
    if(list.sampled_ms>now || now-list.sampled_ms>5000 || wire[12]>CARPLAY_CALLS_MAX)return -1;
    list.count=wire[12];size_t off=13;
    for(size_t i=0;i<list.count;i++) {
        struct carplay_call *call=&list.calls[i];
        if(len-off<39)return -1;
        memcpy(call->uuid,wire+off,36);off+=36;
        call->status=wire[off++];call->direction=wire[off++];size_t n=wire[off++];
        if(!uuid_valid(call->uuid) || call->status>6 || call->direction<1 || call->direction>2 || n>64 || n>len-off)return -1;
        for(size_t j=0;j<i;j++)if(!strcmp(list.calls[j].uuid,call->uuid))return -1;
        for(size_t j=0;j<n;j++)if(wire[off+j]<32 || wire[off+j]>126)return -1;
        memcpy(call->number,wire+off,n);off+=n;
    }
    if(off!=len)return -1;
    *out=list;return 0;
}
int carplay_calls_subscribe(uint32_t *fields,const uint8_t *body,size_t len) {
    uint32_t next=0;
    if(!fields || (len && !body) || len>2048)return -1;
    for(size_t i=0;i<len;i+=4) {
        if(len-i<4 || u16(body+i)!=4)return -1;
        unsigned id=u16(body+i+2);
        if(id<32) { if(next&(1u<<id))return -1;next|=1u<<id; }
    }
    *fields=next;return 0;
}
static int field(uint8_t *out,size_t cap,size_t *used,uint8_t id,const void *data,size_t n) {
    if(*used>cap || n+4>cap-*used)return -1;
    out[*used]=0;out[*used+1]=(uint8_t)(n+4);out[*used+2]=0;out[*used+3]=id;
    memcpy(out+*used+4,data,n);*used+=n+4;return 0;
}
int carplay_call_encode(uint32_t fields,const struct carplay_call *call,uint8_t *out,size_t cap,size_t *len) {
    size_t used=0;const uint8_t service=1;
    if(!call || !out || !len || !uuid_valid(call->uuid) || call->status>6 || call->direction<1 || call->direction>2)return -1;
    if(!memchr(call->number,0,sizeof(call->number)))return -1;
    size_t required=5+((fields&(1u<<3))?5:0)+((fields&(1u<<4))?41:0)+((fields&(1u<<8))?5:0);
    if(cap<required)return -1;
    /* A truncated telephone number identifies a different party. Omit this
     * optional field when the peer's packet budget cannot carry it intact. */
    if((fields&1u) && call->number[0] && strlen(call->number)+5<=cap-required &&
       field(out,cap,&used,0,call->number,strlen(call->number)+1)<0)return -1;
    /* Status is required in each CallStateUpdate even if other fields were requested. */
    if(field(out,cap,&used,2,&call->status,1)<0)return -1;
    if((fields&(1u<<3)) && field(out,cap,&used,3,&call->direction,1)<0)return -1;
    if((fields&(1u<<4)) && field(out,cap,&used,4,call->uuid,37)<0)return -1;
    if((fields&(1u<<8)) && field(out,cap,&used,8,&service,1)<0)return -1;
    *len=used;return 0;
}
