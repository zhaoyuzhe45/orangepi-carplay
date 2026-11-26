#include "carplay_file.h"
#include <string.h>
int carplay_file_begin(struct carplay_file *f,uint8_t id,const uint8_t *data,size_t n,uint16_t type,uint64_t now){
    if(!f || (!data && n) || n>CARPLAY_ART_MAX || id<128 || id==255 || f->phase)return -1;
    f->id=id;f->type=type;f->size=n;f->offset=0;if(n)memmove(f->data,data,n);f->phase=1;f->deadline=now+10000;return 0;
}
int carplay_file_next(struct carplay_file *f,uint8_t *out,size_t cap,size_t *n,uint64_t now){
    if(!f || !out || !n)return -1;
    *n=0;if(!f->phase)return 0;
    if(now>=f->deadline){f->phase=0;return -1;}
    if(f->phase==1){
        /* The link advertises FileTransfer v1. Its Setup is ID/op/size only;
         * the CSM announcement identifies the artwork, not a v2 file-type tail. */
        if(cap<10)return -1;out[0]=f->id;out[1]=4;
        for(unsigned i=0;i<8;i++)out[9-i]=(uint8_t)((uint64_t)f->size>>(i*8));
        *n=10;return 1;
    }
    if(f->phase!=3)return 0;
    if(cap<3)return -1;
    size_t count=f->size-f->offset;if(count>cap-2)count=cap-2;
    out[0]=f->id;out[1]=(uint8_t)((f->offset?0:128)|((f->offset+count==f->size)?64:0));
    memcpy(out+2,f->data+f->offset,count);*n=count+2;return 1;
}
void carplay_file_commit(struct carplay_file *f,size_t n,uint64_t now){
    if(!f)return;
    if(f->phase==1 && n==10){f->phase=2;f->deadline=now+10000;}
    else if(f->phase==3 && n>=2 && n-2<=f->size-f->offset){
        f->offset+=n-2;f->deadline=now+10000;if(f->offset==f->size)f->phase=5;
    }
}
void carplay_file_control(struct carplay_file *f,const uint8_t *p,size_t n,uint64_t now){
    if(!f || !p || n!=2 || !f->phase || p[0]!=f->id)return;
    if(p[1]==1 && (f->phase==2 || f->phase==4)){f->phase=3;f->deadline=now+10000;}
    else if(p[1]==3 && f->phase==3){f->phase=4;f->deadline=now+10000;}
    else if(p[1]==2 || p[1]==6 || (p[1]==5 && f->phase==5))f->phase=0;
}
