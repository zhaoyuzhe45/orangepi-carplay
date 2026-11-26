#include "carplay_calls.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>
int main(void) {
    uint8_t wire[256]={ 'C','P','B','1',0,0,0,0,0,0,3,232,1 };
    const char uuid[]="01234567-89ab-cdef-0123-456789abcdef";
    memcpy(wire+13,uuid,36);wire[49]=4;wire[50]=1;wire[51]=3;memcpy(wire+52,"123",3);
    struct carplay_call_list list,original;
    memset(&list,0x5a,sizeof(list));original=list;
    assert(carplay_calls_decode(wire,55,1000,&list)==0);
    assert(list.count==1 && list.calls[0].status==4 && !strcmp(list.calls[0].uuid,uuid));
    assert(!strcmp(list.calls[0].number,"123"));
    original=list;
    assert(carplay_calls_decode(wire,55,6001,&list)<0 && !memcmp(&list,&original,sizeof(list)));
    assert(carplay_calls_decode(wire,54,1000,&list)<0);
    wire[49]=9;assert(carplay_calls_decode(wire,55,1000,&list)<0);wire[49]=4;
    wire[13]='z';assert(carplay_calls_decode(wire,55,1000,&list)<0);wire[13]='0';
    wire[12]=2;memcpy(wire+55,wire+13,42);assert(carplay_calls_decode(wire,97,1000,&list)<0);wire[12]=1;
    uint8_t subscription[]={0,4,0,0,0,4,0,2,0,4,0,3,0,4,0,4,0,4,0,8};
    uint32_t fields=0;
    assert(carplay_calls_subscribe(&fields,subscription,sizeof(subscription))==0);
    assert(fields==0x11d);
    uint8_t duplicate[]={0,4,0,2,0,4,0,2};
    assert(carplay_calls_subscribe(&fields,duplicate,sizeof(duplicate))<0 && fields==0x11d);
    uint8_t out[256];size_t len=0;
    assert(carplay_call_encode(fields,&list.calls[0],out,sizeof(out),&len)==0);
    assert(len==64); /* remote8 + status5 + direction5 + uuid41 + service5 */
    assert(carplay_call_encode(fields,&list.calls[0],out,20,&len)<0);
    memset(list.calls[0].number,'1',64);list.calls[0].number[64]=0;
    assert(carplay_call_encode(fields,&list.calls[0],out,56,&len)==0 && len==56 && out[3]==2);
    puts("Call snapshot/fields/UUID/bounds: PASS");
    return 0;
}
