#include "carplay_file.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>
int main(void){
    struct carplay_file f={0};uint8_t b[32];size_t n;
    uint8_t data[]={1,2,3,4,5};
    assert(carplay_file_begin(&f,128,data,5,2,100)==0);
    assert(carplay_file_next(&f,b,9,&n,101)<0);
    assert(carplay_file_next(&f,b,10,&n,101)==1 && n==10 && b[1]==4 && b[9]==5);
    /* Merely building a datagram must not advance after a failed send. */
    assert(carplay_file_next(&f,b,sizeof(b),&n,102)==1 && b[1]==4);
    carplay_file_commit(&f,n,102);
    assert(carplay_file_next(&f,b,sizeof(b),&n,103)==0);
    uint8_t start[]={128,1},wrong[]={129,1},success[]={128,5},pause[]={128,3};
    carplay_file_control(&f,wrong,2,104);assert(f.phase==2);
    carplay_file_control(&f,success,2,104);assert(f.phase==2); /* Premature success ignored. */
    carplay_file_control(&f,start,2,105);assert(f.phase==3);
    assert(carplay_file_next(&f,b,5,&n,106)==1 && n==5 && b[1]==0x80);
    carplay_file_commit(&f,n,106);assert(f.offset==3);
    carplay_file_control(&f,pause,2,107);assert(f.phase==4);
    assert(carplay_file_next(&f,b,sizeof(b),&n,108)==0);
    carplay_file_control(&f,start,2,109);
    assert(carplay_file_next(&f,b,sizeof(b),&n,110)==1 && n==4 && b[1]==0x40);
    carplay_file_commit(&f,n,110);assert(f.phase==5);
    carplay_file_control(&f,success,2,111);assert(f.phase==0);
    assert(carplay_file_begin(&f,129,data,5,2,200)==0);
    assert(carplay_file_next(&f,b,sizeof(b),&n,10201)<0 && !f.phase);
    assert(carplay_file_begin(&f,128,NULL,0,2,11000)==0);
    assert(carplay_file_next(&f,b,sizeof(b),&n,11001)==1 && n==10 && b[9]==0);
    carplay_file_commit(&f,n,11001);carplay_file_control(&f,start,2,11002);
    assert(carplay_file_next(&f,b,sizeof(b),&n,11003)==1 && n==2 && b[1]==0xc0);
    carplay_file_commit(&f,n,11003);assert(f.phase==5);
    carplay_file_control(&f,success,2,11004);assert(!f.phase);
    puts("file transfer state/backpressure/timeout tests: PASS");return 0;
}
