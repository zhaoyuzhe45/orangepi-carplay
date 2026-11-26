#define _DEFAULT_SOURCE
#define _POSIX_C_SOURCE 200809L
#include "../src/airplay_client.c"
#include <stdarg.h>
#define CHECK(x) do { if(!(x)) { fprintf(stderr,"FAIL %d: %s errno=%d\n",__LINE__,#x,errno);exit(1); } } while(0)
void iap2_log(const char *format,...) {
    va_list args;va_start(args,format);vfprintf(stderr,format,args);va_end(args);fputc('\n',stderr);
}
int carplay_display_framework_ready(void) { return 1; }
void carplay_audio_bridge_destroy(struct carplay_audio_bridge **bridge) { CHECK(!*bridge); }
void airplay_uinput_close(struct airplay_uinput *input) { CHECK(!input); }

static struct airplay_client *capture(int fd) {
    struct airplay_client *c=calloc(1,sizeof(*c));CHECK(c);
    c->screen_data_fd=fd;c->record_accepted=1;c->test_screen_sent=1;
    c->capture_width=1920;c->capture_height=1080;
    c->input_width=1800;c->input_height=1080;c->actual_fps=30;c->bitrate=8000000;
    c->capabilities.display.present=1;
    c->capabilities.display.width_pixels=800;
    c->capabilities.display.height_pixels=480;
    carplay_encoder_init(&c->encoder);
    atomic_init(&c->force_key_frame,0);atomic_init(&c->live_screen_stop,0);
    pthread_mutex_init(&c->video_send_mutex,NULL);pthread_mutex_init(&c->event_send_mutex,NULL);
    CHECK(airplay_client_start_live_screen(c)==0);return c;
}
static void receive_exact(int fd,uint8_t *p,size_t n) {
    while(n) {
        struct pollfd f={fd,POLLIN,0};CHECK(poll(&f,1,5000)>0);
        ssize_t got=recv(fd,p,n,0);CHECK(got>0);p+=got;n-=(size_t)got;
    }
}
static uint32_t le32(const uint8_t *p) {return p[0]|(uint32_t)p[1]<<8|(uint32_t)p[2]<<16|(uint32_t)p[3]<<24;}
static uint32_t be32(const uint8_t *p) {return (uint32_t)p[0]<<24|(uint32_t)p[1]<<16|(uint32_t)p[2]<<8|p[3];}
int main(void) {
    int s[2];CHECK(socketpair(AF_UNIX,SOCK_STREAM,0,s)==0);
    struct airplay_client *c=capture(s[0]);
    unsigned encoded_width=0,encoded_height=0;screen_output_size(c,&encoded_width,&encoded_height);
    CHECK(encoded_width==800 && encoded_height==480);
    c->capabilities.display.present=0;
    screen_output_size(c,&encoded_width,&encoded_height);
    CHECK(encoded_width==1800 && encoded_height==1080);
    c->capabilities.display.present=1;
    uint8_t header[128],key[32]={0},*body=malloc(5*1024*1024),*plain=malloc(5*1024*1024);
    CHECK(body && plain);unsigned configs=0,frames=0,keys=0,key_headers=0;uint64_t counter=0,start=video_monotonic_ms();
    FILE *out=fopen("/data/local/tmp/mediacodec-airplay-decrypted.h264","wb");CHECK(out);
    while(video_monotonic_ms()-start<6000 || frames<4) {
        CHECK(video_monotonic_ms()-start<15000);
        receive_exact(s[1],header,sizeof(header));size_t length=le32(header);CHECK(length>0 && length<5*1024*1024);
        receive_exact(s[1],body,length);
        if(header[4]==1) {CHECK(body[0]==1);configs++;}
        else {
            CHECK(header[4]==0);uint8_t nonce[12]={0};size_t plain_len;
            for(unsigned i=0;i<8;i++) nonce[i+4]=(uint8_t)(counter>>(8*i));counter++;
            CHECK(airplay_chacha20_poly1305_open(key,nonce,body,length,header,sizeof(header),plain,5*1024*1024,&plain_len)==0);
            size_t offset=0;unsigned slices=0;int has_idr=0,has_embedded_config=0;
            while(offset<plain_len) {
                CHECK(plain_len-offset>=4);uint32_t n=be32(plain+offset);offset+=4;
                CHECK(n>0 && n<=plain_len-offset);unsigned type=plain[offset]&31;
                if(type==1 || type==5) slices++;
                if(type==5) { has_idr=1;keys++; }
                if(type==7 || type==8) has_embedded_config=1;
                if(configs) { fwrite("\0\0\0\1",1,4,out);fwrite(plain+offset,1,n,out); }
                offset+=n;
            }
            CHECK(slices>0);
            CHECK(!has_embedded_config);
            CHECK((header[5] & AIRPLAY_SCREEN_FLAG_KEYFRAME) ==
                  (has_idr ? AIRPLAY_SCREEN_FLAG_KEYFRAME : 0));
            if(has_idr) key_headers++;
            frames++;
            if(frames==3) atomic_store(&c->force_key_frame,1);
        }
    }
    fclose(out);CHECK(configs>0 && keys>0 && key_headers==keys);
    airplay_client_free(c);close(s[0]);close(s[1]);free(body);free(plain);
    printf("PASS: production logical-canvas encode->complete picture->AirPlay encryption->decrypt config=%u frames=%u keys=%u key_headers=%u\n",configs,frames,keys,key_headers);
    /* Deliberately never drain the next receiver. Shutdown-before-join must
     * wake a sender blocked behind this tiny socket buffer. */
    CHECK(socketpair(AF_UNIX,SOCK_STREAM,0,s)==0);int small=1024;
    CHECK(setsockopt(s[0],SOL_SOCKET,SO_SNDBUF,&small,sizeof(small))==0);
    c=capture(s[0]);sleep(2);start=video_monotonic_ms();airplay_client_free(c);
    CHECK(video_monotonic_ms()-start<3000);close(s[0]);close(s[1]);
    puts("PASS: production teardown releases blocked receiver and child");return 0;
}
