#include "carplay_knob.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>
/* Exact 70-byte descriptor from airplay_info_audi_no_touch fixture. */
static const uint8_t audi[]={5,1,9,8,0xa1,1,5,9,9,1,0x15,0,0x25,1,
    0x75,1,0x95,1,0x81,2,5,12,0x0a,0x23,2,0x0a,0x24,2,0x95,2,0x81,2,
    0x95,5,0x81,1,5,1,9,1,0xa1,0,9,0x30,9,0x31,0x15,0x81,0x25,0x7f,
    0x75,8,0x95,2,0x81,2,0xc0,9,0x38,0x15,0x81,0x25,0x7f,0x75,8,
    0x95,1,0x81,6,0xc0};
static unsigned actions(const struct carplay_key_batch *b,int key) {
    unsigned n=0; for(unsigned i=0;i<b->count;i++)if(b->entries[i].keycode==key)n+=b->entries[i].count;return n;
}
static void relative_regressions(void) {
    struct carplay_knob_layout p;struct carplay_knob_state s={0};struct carplay_key_batch b;
    assert(carplay_knob_parse(audi,sizeof(audi),&p)==0);
    uint8_t r[]={0,0,0,1}; unsigned total=0;
    for(unsigned i=0;i<5;i++){assert(carplay_knob_decode(&p,&s,r,sizeof(r),&b)==0);total+=actions(&b,22);}
    assert(total==5); r[3]=5;assert(carplay_knob_decode(&p,&s,r,4,&b)==0);assert(actions(&b,22)==5);
    r[3]=(uint8_t)-5;assert(carplay_knob_decode(&p,&s,r,4,&b)==0);assert(actions(&b,21)==5);
    puts("five relative +1 => 5 right actions; relative +5/-5 => 5 right/left actions each");
}
static void audi_edges_and_isolation(void) {
    struct carplay_knob_layout p;struct carplay_knob_state s={0},other={0},before;struct carplay_key_batch b;
    assert(carplay_knob_parse(audi,sizeof(audi),&p)==0);
    assert(p.field_count==6 && p.reports[0].bits==32);
    uint8_t r[]={7,127,(uint8_t)-127,2};
    assert(carplay_knob_decode(&p,&s,r,4,&b)==0);
    int order[]={66,3,4,22,19,22};assert(b.count==6);
    for(unsigned i=0;i<6;i++)assert(b.entries[i].keycode==order[i] && b.entries[i].count==(i==5?2u:1u));
    assert(carplay_knob_decode(&p,&s,r,4,&b)==0 && b.count==1 && actions(&b,22)==2);
    assert(carplay_knob_decode(&p,&other,r,4,&b)==0 && b.count==6);
    r[1]=1;r[3]=0;assert(carplay_knob_decode(&p,&s,r,4,&b)==0 && b.count==0);
    before=s;assert(carplay_knob_decode(&p,&s,r,3,&b)<0 && b.count==0 && memcmp(&s,&before,sizeof(s))==0);
    r[0]=0;r[1]=255;r[3]=128;
    assert(carplay_knob_decode(&p,&s,r,4,&b)<0 && memcmp(&s,&before,sizeof(s))==0);
    memset(r,0,4);assert(carplay_knob_decode(&p,&s,r,4,&b)==0 && b.count==0);
    r[0]=7;r[1]=255;r[2]=1;assert(carplay_knob_decode(&p,&s,r,4,&b)==0 && b.count==5);
}
static void ids_bit_offsets_and_bounds(void) {
    uint8_t d[]={5,1,9,8,0xa1,1,0x85,4,0x75,3,0x95,1,0x81,3,
        0x16,0,0xf8,0x26,0xff,7,0x75,12,9,0x38,0x81,6,
        0x85,9,5,9,9,1,0x15,0,0x25,1,0x75,2,0x81,2,0xc0};
    struct carplay_knob_layout p;struct carplay_knob_state s={0},before;struct carplay_key_batch b;
    assert(carplay_knob_parse(d,sizeof(d),&p)==0);
    uint8_t a[]={4,0xd8,0x7f},button[]={9,1},unknown[]={8,1};
    assert(carplay_knob_owns_report(&p,a,1) && !carplay_knob_owns_report(&p,unknown,2));
    assert(carplay_knob_decode(&p,&s,a,3,&b)==0 && actions(&b,21)==5);
    a[1]=40;a[2]=0;assert(carplay_knob_decode(&p,&s,a,3,&b)==0 && actions(&b,22)==5);
    assert(carplay_knob_decode(&p,&s,button,2,&b)==0 && actions(&b,66)==1);
    before=s;button[1]=2;assert(carplay_knob_decode(&p,&s,button,2,&b)<0 && memcmp(&s,&before,sizeof(s))==0);
    button[1]=1;assert(carplay_knob_decode(&p,&s,button,2,&b)==0 && b.count==0);
    assert(carplay_knob_decode(&p,&s,unknown,2,&b)<0);
    assert(carplay_knob_parse(d,sizeof(d)-1,&p)<0 && !p.valid);
    uint8_t overflow[]={0x75,32,0x96,0xff,0xff,0x81,3};
    assert(carplay_knob_parse(overflow,sizeof(overflow),&p)<0 && !p.valid);
    uint8_t badid[]={0x85,0};assert(carplay_knob_parse(badid,sizeof(badid),&p)<0);
    uint8_t underflow[]={0xb4};assert(carplay_knob_parse(underflow,1,&p)<0);
    uint8_t mixed[]={5,1,0x15,0x81,0x25,0x7f,0x75,8,0x95,1,9,0x38,0x81,6,
        0x85,1,9,0x38,0x81,6};assert(carplay_knob_parse(mixed,sizeof(mixed),&p)<0);
    uint8_t array[]={5,1,0x15,0x81,0x25,0x7f,0x75,8,0x95,1,9,0x38,0x81,0};
    assert(carplay_knob_parse(array,sizeof(array),&p)<0);
    uint8_t stack[]={5,1,0x15,0x81,0x25,0x7f,0x75,8,0x95,1,0xa4,5,9,
        0x15,0,0x25,1,9,1,0x81,2,0xb4,9,0x38,0x81,6};
    assert(carplay_knob_parse(stack,sizeof(stack),&p)==0);
    uint8_t pair[]={1,5};memset(&s,0,sizeof(s));
    assert(carplay_knob_decode(&p,&s,pair,2,&b)==0 && actions(&b,66)==1 && actions(&b,22)==5);
    uint8_t ranged[]={5,9,0x15,0,0x25,1,0x75,1,0x95,3,0x19,1,0x29,3,0x81,2};
    assert(carplay_knob_parse(ranged,sizeof(ranged),&p)==0 && p.field_count==1);
}
static void wide_relative_axes_and_composite_reports(void) {
    uint8_t d[]={5,1,0x85,3,0x17,0,0,0,0x80,0x27,0xff,0xff,0xff,0x7f,
        0x75,32,0x95,2,9,0x30,9,0x31,0x81,6,
        0x85,7,5,12,0x15,0,0x25,1,0x75,8,0x95,1,0x0a,0xcd,0,0x81,2};
    struct carplay_knob_layout p;struct carplay_knob_state s={0};struct carplay_key_batch b;
    assert(carplay_knob_parse(d,sizeof(d),&p)==0);
    uint8_t r[]={3,0,0,0,0x80,5,0,0,0},other[]={7,1};
    assert(carplay_knob_decode(&p,&s,r,sizeof(r),&b)==0 && b.count==2);
    assert(b.entries[0].keycode==21 && b.entries[0].count==UINT32_C(2147483648));
    assert(b.entries[1].keycode==20 && b.entries[1].count==5);
    assert(carplay_knob_decode(&p,&s,r,sizeof(r),&b)==0 && b.count==2);
    assert(!carplay_knob_owns_report(&p,other,sizeof(other)));
    uint8_t extended_usage[]={0x17,0,0x80,0xff,0xff,0x27,0xff,0x7f,0,0,
        0x75,16,0x95,1,0x0b,0x38,0,1,0,0x81,6};
    assert(carplay_knob_parse(extended_usage,sizeof(extended_usage),&p)==0);
    uint8_t negative[]={0xfb,0xff};memset(&s,0,sizeof(s));
    assert(carplay_knob_decode(&p,&s,negative,2,&b)==0 && actions(&b,21)==5);
}
static void malformed_item_shapes(void) {
    struct carplay_knob_layout p;
    uint8_t no_collection_argument[]={5,9,9,1,0x15,0,0x25,1,0x75,1,0x95,1,0xa0,0x81,2,0xc0};
    assert(carplay_knob_parse(no_collection_argument,sizeof(no_collection_argument),&p)<0 && !p.valid);
    uint8_t bad_push[]={5,9,0xa5,1,9,1,0x15,0,0x25,1,0x75,1,0x95,1,0x81,2,0xb4};
    assert(carplay_knob_parse(bad_push,sizeof(bad_push),&p)<0 && !p.valid);
    uint8_t reserved[]={5,9,9,1,0x15,0,0x25,1,0x75,1,0x95,1,0x81,2,0x0c};
    assert(carplay_knob_parse(reserved,sizeof(reserved),&p)<0 && !p.valid);
}
int main(void){relative_regressions();audi_edges_and_isolation();ids_bit_offsets_and_bounds();wide_relative_axes_and_composite_reports();malformed_item_shapes();puts("knob descriptor/edges/isolation/bounds tests: PASS");return 0;}
