#define _DEFAULT_SOURCE
#define _POSIX_C_SOURCE 200809L
#define pthread_create test_pthread_create
#include "../src/airplay_client.c"
#undef pthread_create
#include <assert.h>
static int probes, readiness, threads;
static unsigned encoded_w,encoded_h,source_w,source_h;
void iap2_log(const char *format, ...) { (void)format; }
struct airplay_uinput *airplay_uinput_open(void) { return NULL; }
void carplay_encoder_init(struct carplay_encoder *c) { memset(c,0,sizeof(*c));c->fd=-1; }
int carplay_encoder_start_scaled(struct carplay_encoder *c,unsigned w,unsigned h,
    unsigned bitrate,unsigned fps,unsigned sw,unsigned sh)
{ (void)c;(void)bitrate;(void)fps;encoded_w=w;encoded_h=h;source_w=sw;source_h=sh;return 0; }
void carplay_modes_init(struct carplay_modes *m,int audio) { (void)audio;memset(m,0,sizeof(*m)); }
int carplay_display_probe(struct carplay_display_state *s)
{ probes++;memset(s,0,sizeof(*s));s->width=1920;s->height=1080;return 0; }
int carplay_display_framework_ready(void) { readiness++;return 1; }
int test_pthread_create(pthread_t *t,const pthread_attr_t *a,void *(*fn)(void *),void *arg)
{ (void)t;(void)a;(void)fn;(void)arg;threads++;return 0; }
int main(void)
{
    for(int profile=0;profile<2;profile++) {
        struct airplay_client *c=airplay_client_new_with_audio(-1,0,0);assert(c);
        airplay_client_set_profile(c,profile?AIRPLAY_PROFILE_HAP:AIRPLAY_PROFILE_LEGACY);
        assert(probes==0); /* /info must not wait for a discarded WM probe. */
        assert(c->input_width==800 && c->input_height==480);
        c->input_width=1024;c->input_height=480;c->record_accepted=1;c->screen_data_fd=12;
        assert(!airplay_client_start_live_screen(c));assert(readiness==0);
        assert(c->input_width==1024 && c->input_height==480 && c->live_screen_started);
        c->capture_width=1280;c->capture_height=720;c->output_width=1366;c->output_height=768;
        assert(!start_encoder(c));
        assert(encoded_w==1366 && encoded_h==768 && source_w==1280 && source_h==720);
        pthread_mutex_destroy(&c->video_send_mutex);pthread_mutex_destroy(&c->event_send_mutex);free(c);
    }
    assert(threads==2);puts("connection queries: both profiles skip redundant probes and preserve negotiated geometry PASS");
}
