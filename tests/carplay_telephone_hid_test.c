#include "carplay_capabilities.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>
int main(void) {
    struct carplay_phone_hid layout;
    struct carplay_phone_hid_state state={0};
    /* Two variable buttons following three constant padding bits. */
    uint8_t desc[]={5,11,9,7,0xa1,1,0x15,0,0x25,1,0x75,3,0x95,1,0x81,3,
        0x75,1,0x95,2,9,0x20,9,0x26,0x81,2,0x75,3,0x95,1,0x81,3,0xc0};
    assert(carplay_phone_hid_parse(desc,sizeof(desc),&layout)==0);
    uint8_t report=8;
    assert(carplay_phone_hid_press(&layout,&state,&report,1)==1);
    assert(carplay_phone_hid_press(&layout,&state,&report,1)==0);
    report=0;assert(carplay_phone_hid_press(&layout,&state,&report,1)==0);
    report=16;assert(carplay_phone_hid_press(&layout,&state,&report,1)==2);
    assert(carplay_phone_hid_press(&layout,&state,&report,0)<0);
    assert(carplay_phone_hid_press(&layout,&state,&report,1)==0);
    uint8_t ids[]={5,11,9,7,0xa1,1,0x15,0,0x25,1,0x75,8,0x95,1,
        0x85,4,9,0x20,0x81,2,0x85,9,9,0x26,0x81,2,0xc0};
    assert(carplay_phone_hid_parse(ids,sizeof(ids),&layout)==0);
    memset(&state,0,sizeof(state));
    uint8_t a[]={4,1},b[]={9,1},bad[]={7,1};
    assert(carplay_phone_hid_owns_report(&layout,a,2));
    assert(carplay_phone_hid_owns_report(&layout,a,1)); /* malformed phone payload stays owned */
    assert(!carplay_phone_hid_owns_report(&layout,bad,2));
    assert(carplay_phone_hid_press(&layout,&state,a,2)==1);
    assert(carplay_phone_hid_press(&layout,&state,b,2)==2);
    assert(carplay_phone_hid_press(&layout,&state,a,2)==0);
    assert(carplay_phone_hid_press(&layout,&state,bad,2)<0);
    a[1]=2;assert(carplay_phone_hid_press(&layout,&state,a,2)<0);
    a[1]=1;assert(carplay_phone_hid_press(&layout,&state,a,2)==0);
    ids[19]=0; /* Hook input becomes an array: do not guess selector encoding. */
    assert(carplay_phone_hid_parse(ids,sizeof(ids),&layout)<0);
    assert(!layout.valid);
    assert(carplay_phone_hid_parse(desc,sizeof(desc)-2,&layout)<0);
    uint8_t overflow[]={0x75,32,0x96,0xff,0xff,0x81,3};
    assert(carplay_phone_hid_parse(overflow,sizeof(overflow),&layout)<0);
    uint8_t mixed[]={5,11,0x15,0,0x25,1,0x75,1,0x95,1,9,0x20,0x81,2,
        0x85,1,9,0x26,0x81,2};
    assert(carplay_phone_hid_parse(mixed,sizeof(mixed),&layout)<0);
    uint8_t composite[]={5,11,0x15,0,0x25,1,0x75,8,0x95,1,0x85,4,9,0x20,0x81,2,
        5,12,0x85,7,9,0xcd,0x81,2};
    assert(carplay_phone_hid_parse(composite,sizeof(composite),&layout)==0);
    assert(carplay_phone_hid_owns_report(&layout,a,2) && !carplay_phone_hid_owns_report(&layout,bad,2));
    puts("telephone HID descriptor/edge/bounds tests: PASS");return 0;
}
