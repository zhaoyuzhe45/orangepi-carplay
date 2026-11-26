#define _DEFAULT_SOURCE
#define _POSIX_C_SOURCE 200809L
/* Include the production internals to deterministically schedule mutex
 * contention; there are no test-only hooks in the worker. */
#include "../src/carplay_keys.c"
#include "../src/airplay_client.c"
#include <assert.h>
#include <stdio.h>

void iap2_log(const char *format, ...) { (void)format; }
int airplay_input_touch_point(const uint8_t *p, size_t n, struct airplay_input_touch *t)
{ (void)p; (void)n; memset(t,0,sizeof(*t)); t->down=1; t->x=100; t->y=100; return 1; }
int airplay_uinput_touch_down(struct airplay_uinput *p,unsigned x,unsigned y,unsigned w,unsigned h)
{ (void)p;(void)x;(void)y;(void)w;(void)h;return -1; }
int airplay_uinput_touch_move(struct airplay_uinput *p,unsigned x,unsigned y,unsigned w,unsigned h)
{ (void)p;(void)x;(void)y;(void)w;(void)h;return 0; }
int airplay_uinput_touch_up(struct airplay_uinput *p,unsigned x,unsigned y,unsigned w,unsigned h)
{ (void)p;(void)x;(void)y;(void)w;(void)h;return 0; }
static int touch_closed;
void airplay_uinput_close(struct airplay_uinput *p) { assert(p); touch_closed++; }
static struct carplay_key_batch batch(unsigned count)
{ struct carplay_key_batch b={0};b.count=1;b.entries[0].keycode=20;b.entries[0].count=count;return b; }
struct submission { struct carplay_keys *keys; atomic_int entered,done; int result; };
static void *submit_once(void *arg)
{ struct submission *s=arg;struct carplay_key_batch b=batch(1);atomic_store(&s->entered,1);
  s->result=carplay_keys_submit(s->keys,&b);atomic_store(&s->done,1);return NULL; }
static void contention(void)
{
    int fd=open("/dev/null",O_WRONLY);assert(fd>=0);struct carplay_keys *w=NULL;
    assert(carplay_keys_create_fd(&w,fd)==0);close(fd);
    pthread_mutex_lock(&w->mutex);
    struct submission s={.keys=w};atomic_init(&s.entered,0);atomic_init(&s.done,0);
    pthread_t t;assert(!pthread_create(&t,NULL,submit_once,&s));
    while(!atomic_load(&s.entered))usleep(100);
    long long deadline=monotonic_ms()+30;
    while(!atomic_load(&s.done) && monotonic_ms()<deadline)usleep(100);
    pthread_mutex_unlock(&w->mutex);pthread_join(t,NULL);
    assert(s.result==0);struct carplay_keys_stats stats;carplay_keys_get_stats(w,&stats);
    assert(stats.accepted==1 && stats.rejected==0);carplay_keys_destroy(w);
}
static void unchanged_suspend(void)
{
    int p[2];assert(pipe(p)==0);assert(!nonblock_cloexec(p[1]));
    char fill[4096]={0};size_t filled=0;ssize_t n;
    while((n=write(p[1],fill,sizeof(fill)))>0)filled+=(size_t)n;
    assert(errno==EAGAIN);struct carplay_keys *w=NULL;assert(!carplay_keys_create_fd(&w,p[1]));
    struct carplay_key_batch b=batch(5);assert(!carplay_keys_submit(w,&b));
    carplay_keys_suspend(w,0);carplay_keys_suspend(w,0);
    struct carplay_keys_stats stats;carplay_keys_get_stats(w,&stats);assert(stats.cancelled==0);
    while(filled){n=read(p[0],fill,filled<sizeof(fill)?filled:sizeof(fill));assert(n>0);filled-=(size_t)n;}
    for(unsigned i=0;i<5;i++){
        struct input_event events[4];struct pollfd q={p[0],POLLIN,0};assert(poll(&q,1,1000)==1);
        assert(read(p[0],events,sizeof(events))==(ssize_t)sizeof(events));
        assert(events[0].code==KEY_DOWN && events[0].value==1 && events[2].value==0);
    }
    carplay_keys_suspend(w,1);assert(carplay_keys_submit(w,&b)==-1);
    carplay_keys_suspend(w,1);carplay_keys_suspend(w,0);
    carplay_keys_destroy(w);close(p[0]);close(p[1]);
}
static void touch_failure(void)
{
    int p[2];assert(pipe(p)==0);struct airplay_client c={0};
    assert(!carplay_keys_create_fd(&c.key_input,p[1]));struct carplay_keys *expected=c.key_input;
    c.touch_input=(struct airplay_uinput *)&c;c.input_width=800;c.input_height=480;
    uint8_t report[12]={0};handle_touch_report(&c,NULL,report,sizeof(report));
    assert(c.key_input==expected && c.touch_input==NULL && touch_closed==1);
    struct carplay_key_batch b=batch(1);assert(!carplay_keys_submit(c.key_input,&b));
    struct pollfd q={p[0],POLLIN,0};assert(poll(&q,1,1000)==1);
    carplay_keys_destroy(c.key_input);close(p[0]);close(p[1]);
}
int main(int argc,char **argv)
{
    assert(argc==2);
    if(!strcmp(argv[1],"contention"))contention();
    else if(!strcmp(argv[1],"suspend"))unchanged_suspend();
    else if(!strcmp(argv[1],"touch"))touch_failure();
    else return 2;
    printf("input lifecycle %s: PASS\n",argv[1]);return 0;
}
