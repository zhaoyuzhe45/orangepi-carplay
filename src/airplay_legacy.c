/* Traditional CarPlay video: continuous AES-128-CTR using the system crypto.
 * Control and event channels remain plaintext; avcC does not consume CTR bytes. */
#include "airplay_legacy.h"
#include <dlfcn.h>
#include <limits.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

struct airplay_legacy_cipher { void *ctx; };
static pthread_once_t once = PTHREAD_ONCE_INIT;
static void *library;
static void *(*ctx_new)(void);
static void (*ctx_free)(void *);
static const void *(*aes_ctr)(void);
static int (*encrypt_init)(void *,const void *,void *,const uint8_t *,const uint8_t *);
static int (*encrypt_update)(void *,uint8_t *,int *,const uint8_t *,int);
static uint8_t *(*sha512_hash)(const uint8_t *,size_t,uint8_t *);
static int ready;

static void load(void) {
    library=dlopen("libcrypto.so",RTLD_NOW|RTLD_LOCAL);
#ifndef __ANDROID__
    if(!library) library=dlopen("libcrypto.so.3",RTLD_NOW|RTLD_LOCAL);
    if(!library) library=dlopen("libcrypto.so.1.1",RTLD_NOW|RTLD_LOCAL);
#endif
    if(!library) return;
    ctx_new=dlsym(library,"EVP_CIPHER_CTX_new");
    ctx_free=dlsym(library,"EVP_CIPHER_CTX_free");
    aes_ctr=dlsym(library,"EVP_aes_128_ctr");
    encrypt_init=dlsym(library,"EVP_EncryptInit_ex");
    encrypt_update=dlsym(library,"EVP_EncryptUpdate");
    sha512_hash=dlsym(library,"SHA512");
    ready=ctx_new && ctx_free && aes_ctr && encrypt_init && encrypt_update && sha512_hash;
}
int airplay_legacy_keys(const uint8_t session_key[16],uint64_t id,uint8_t key[16],uint8_t iv[16]) {
    uint8_t input[80],digest[64];
    const char *labels[]={"AirPlayStreamKey","AirPlayStreamIV"};
    if(!session_key || !key || !iv) return -1;
    pthread_once(&once,load); if(!ready) return -1;
    for(unsigned i=0;i<2;i++) {
        int n=snprintf((char *)input,sizeof(input),"%s%llu",labels[i],(unsigned long long)id);
        if(n<0 || (size_t)n>sizeof(input)-16) return -1;
        memcpy(input+n,session_key,16);
        if(!sha512_hash(input,(size_t)n+16,digest)) return -1;
        memcpy(i?iv:key,digest,16);
    }
    memset(input,0,sizeof(input)); memset(digest,0,sizeof(digest));
    return 0;
}
struct airplay_legacy_cipher *airplay_legacy_new(const uint8_t key[16],const uint8_t iv[16]) {
    pthread_once(&once,load);
    if(!ready || !key || !iv) return NULL;
    struct airplay_legacy_cipher *c=calloc(1,sizeof(*c));
    if(!c) return NULL;
    c->ctx=ctx_new();
    if(!c->ctx || encrypt_init(c->ctx,aes_ctr(),NULL,key,iv)!=1) {
        airplay_legacy_free(c); return NULL;
    }
    return c;
}
int airplay_legacy_crypt(struct airplay_legacy_cipher *c,const uint8_t *in,uint8_t *out,size_t n) {
    int written=0;
    if(!c || !c->ctx || n>INT_MAX || (!in && n) || (!out && n)) return -1;
    if(!n) return 0;
    return encrypt_update(c->ctx,out,&written,in,(int)n)==1 && written==(int)n ? 0:-1;
}
void airplay_legacy_free(struct airplay_legacy_cipher *c) {
    if(c) { if(c->ctx) ctx_free(c->ctx); free(c); }
}
