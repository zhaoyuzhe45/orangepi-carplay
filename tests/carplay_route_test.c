#include "carplay_route.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>
int main(void) {
    uint8_t identity[]={0,25,0,30,0,6,0,0,0,7,0,7,0,1,'H','U',0,0,6,0,2,0,16,0,2};
    /* A valid component contains identifier, NUL-terminated name and text limit. */
    identity[1]=23;
    struct carplay_route_components c;
    assert(carplay_route_identify(identity,23,&c)==0 && c.count==1 && c.item[0].id==7);
    struct carplay_route_subscription s;
    uint8_t request[]={0,6,0,0,0,7,0,4,0,1,0,4,0,2};
    assert(carplay_route_subscribe(&c,request,sizeof(request),&s)==0 && s.count==1 && s.flags==6);
    request[5]=8;assert(carplay_route_subscribe(&c,request,sizeof(request),&s)<0);request[5]=7;
    assert(carplay_route_subscribe(&c,request,13,&s)<0);
    assert(carplay_route_subscribe(&c,NULL,0,&s)==0 && s.count==1);
    uint8_t wire[128]={'C','P','R','1',0,0,0,0,0,0,3,232,1,1,0,3,0,0,3,232,0,0,0,60,0,0,0,50};
    size_t n=28;const char *strings[]={"road","next","dest","Amap"};
    for(unsigned i=0;i<4;i++){size_t z=strlen(strings[i]);wire[n++]=0;wire[n++]=(uint8_t)z;memcpy(wire+n,strings[i],z);n+=z;}
    struct carplay_route_snapshot p,old;
    assert(carplay_route_decode(wire,n,1200,&p)==0 && p.index==3 && p.remaining==1000);
    old=p;assert(carplay_route_decode(wire,n,12001,&p)<0 && !memcmp(&p,&old,sizeof(p)));
    assert(carplay_route_decode(wire,n-1,1200,&p)<0);
    uint8_t out[1024];size_t z;
    assert(carplay_route_update(&s,&c.item[0],&p,out,sizeof(out),&z)==0 && z>30);
    assert(carplay_route_maneuver(&c.item[0],&p,out,sizeof(out),&z)==0 && z>12);
    assert(carplay_route_update(&s,&c.item[0],&p,out,8,&z)<0);
    c.item[0].current_chars=c.item[0].destination_chars=c.item[0].next_chars=255;
    for(unsigned i=0;i<84;i++)memcpy(p.current+3*i,"\xe9\x81\x93",3);
    p.current[252]=0;strcpy(p.next,p.current);strcpy(p.destination,p.current);
    strcpy(p.source,p.current);s.flags=6;
    /* Small receivers must still get the selection; optional text is bounded. */
    assert(carplay_route_update(&s,&c.item[0],&p,out,48,&z)==0 && z<=48);
    assert(carplay_route_maneuver(&c.item[0],&p,out,27,&z)==0 && z==25);
    assert(out[z-1]==0 && out[z-2]==0x93); /* no split UTF-8 character */
    p.state=255;
    assert(carplay_route_update(&s,&c.item[0],&p,out,sizeof(out),&z)==0);
    /* Unavailable provider cannot leave a previously cached maneuver selected. */
    int empty=0;for(size_t off=0;off<z;off+=(size_t)(out[off]<<8|out[off+1]))
        if(out[off+3]==13 && out[off+1]==4)empty=1;
    assert(empty && carplay_route_maneuver(&c.item[0],&p,out,sizeof(out),&z)<0);
    assert(carplay_route_update(&s,&c.item[0],&p,out,32,&z)==0 && z<=32);
    puts("route component/subscription/freshness/serialization tests: PASS");return 0;
}
