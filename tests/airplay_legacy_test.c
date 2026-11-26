#include "airplay_legacy.h"
#include "airplay_video.h"
#include "airplay_bplist.h"
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <stdarg.h>

void iap2_log(const char *fmt, ...) { (void)fmt; }

static void hex(const char *s, uint8_t *p, size_t n) {
    for (size_t i=0;i<n;i++) { unsigned v; assert(sscanf(s+i*2,"%2x",&v)==1); p[i]=(uint8_t)v; }
}
int main(void) {
    uint8_t key[16],iv[16],expected[64],plain[64],out[64],zero[16]={0};
    assert(airplay_legacy_keys(zero,0,key,iv)==0);
    hex("51376cf5cf85231963cec9ba9f4806b6",expected,16);
    assert(!memcmp(key,expected,16));
    hex("025641101bed4a53c861a33a234074e2",expected,16);
    assert(!memcmp(iv,expected,16));
    /* NIST SP800-38A F.5.1: split across non-block-aligned video frames. */
    hex("2b7e151628aed2a6abf7158809cf4f3c",key,16);
    hex("f0f1f2f3f4f5f6f7f8f9fafbfcfdfeff",iv,16);
    hex("6bc1bee22e409f96e93d7e117393172aae2d8a571e03ac9c9eb76fac45af8e5130c81c46a35ce411e5fbc1191a0a52eff69f2445df4f9b17ad2b417be66c3710",plain,64);
    hex("874d6191b620e3261bef6864990db6ce9806f66b7970fdff8617187bb9fffdff5ae4df3edbd5d35e5b4f09020db03eab1e031dda2fbe03d1792170a0f3009cee",expected,64);
    struct airplay_legacy_cipher *c=airplay_legacy_new(key,iv);
    assert(c);
    const size_t parts[]={1,2,13,7,3,38}; size_t pos=0;
    for(size_t i=0;i<sizeof(parts)/sizeof(parts[0]);i++) {
        assert(airplay_legacy_crypt(c,plain+pos,out+pos,parts[i])==0);pos+=parts[i];
    }
    assert(pos==64 && !memcmp(out,expected,64));
    airplay_legacy_free(c);
    /* Real 128-byte framing must not consume AES on config or keepalive. */
    uint8_t wire[256], config[]={1,2,3}; size_t n;
    c=airplay_legacy_new(key,iv); assert(c);
    assert(airplay_screen_encode_legacy_frame(wire,sizeof(wire),&n,c,1,plain,1,123)==0);
    assert(n==129 && wire[0]==1 && wire[4]==0 && wire[5]==AIRPLAY_SCREEN_FLAG_KEYFRAME && wire[8]==123 && wire[128]==expected[0]);
    assert(airplay_screen_encode_legacy_config(wire,sizeof(wire),&n,config,3,456,
        1800,1080,800,480)==0);
    assert(n==131 && wire[4]==1 && wire[6]==0x14 && wire[7]==1 && !memcmp(wire+128,config,3));
    assert(wire[16]==0 && wire[17]==0 && wire[18]==0xe1 && wire[19]==0x44);
    assert(wire[20]==0 && wire[21]==0 && wire[22]==0x87 && wire[23]==0x44);
    assert(!memcmp(wire+40,wire+16,8));
    assert(wire[56]==0 && wire[57]==0 && wire[58]==0x48 && wire[59]==0x44);
    assert(wire[60]==0 && wire[61]==0 && wire[62]==0xf0 && wire[63]==0x43);
    assert(airplay_screen_encode_keepalive(wire,sizeof(wire),&n,1)==0 && n==170 && wire[4]==5);
    struct airplay_bplist_document doc; struct airplay_bplist_value root;
    assert(airplay_bplist_init(&doc,wire+128,n-128)==0 && airplay_bplist_root(&doc,&root)==0);
    assert(root.type==AIRPLAY_BPLIST_DICTIONARY && root.count==0);
    assert(airplay_screen_encode_keepalive(wire,sizeof(wire),&n,0)==0 && n==128 && wire[4]==2);
    assert(airplay_screen_encode_legacy_frame(wire,sizeof(wire),&n,c,0,plain+1,63,789)==0);
    assert(n==191 && wire[5]==0 && !memcmp(wire+128,expected+1,63));
    assert(airplay_screen_encode_legacy_frame(wire,128,&n,c,0,plain,1,0)<0);
    airplay_legacy_free(c);
    puts("legacy AES-CTR derivation and NIST split-frame vector passed");
    return 0;
}
