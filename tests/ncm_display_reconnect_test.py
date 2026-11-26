"""Run the production connector with real socket teardown and a virtual clock."""
from pathlib import Path
import os
import subprocess
import tempfile
import unittest
from ncm_compat_test import ROOT, function


class DisplayReconnectTest(unittest.TestCase):
    def test_owned_bounded_display_reconnect(self):
        source = (ROOT / "src/ncm_bridge.c").read_text()
        start = source.index("struct ncm_connect_owner {")
        request = source[start:source.index("\n};", source.index("struct ncm_connect_request {")) + 3]
        bodies = "\n".join(function(source, s) for s in (
            "static void free_connect_request(", "static int connect_owned_locked(",
            "static int connect_owned(", "static int wait_connect_retry(",
            "static int wait_display_preparation(", "static void *connect_endpoint_main("))
        code = r'''
#define _GNU_SOURCE
#include <assert.h>
#include <pthread.h>
#include <stdatomic.h>
#include <arpa/inet.h>
#include <net/if.h>
#include <stdint.h>
#include <sys/socket.h>
#include <fcntl.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>
#include "airplay_client.h"
#include "carplay_log.h"
#include "carplay_capabilities.h"
#define AIRPLAY_CONNECT_RETRY_ATTEMPTS 1
#define AIRPLAY_CONNECT_RETRY_DELAY_US 1
#define AIRPLAY_RECOVERY_POLL_MS 100u
#define AIRPLAY_RECOVERY_STABLE_MS 60000u
#define DISPLAY_PREPARATION_TIMEOUT_MS 250000u
static void iap2_log(const char *format,...) { (void)format; }
''' + request + r'''
struct airplay_client { enum airplay_profile profile; int serial,ticks,screen,event,record; };
static pthread_mutex_t g_mu=PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t g_connect_cv=PTHREAD_COND_INITIALIZER;
static atomic_int g_connect_stop;
static int g_connect_thread_running,g_connect_sock=-1;
static unsigned long g_usb_generation, g_transport_restart_epoch;
static int g_connect_retiring;
static int g_tun_index,g_kernel_ncm,g_host_netdev_ready,g_airplay_transport_restart_pending;
static int g_host_authenticated;
static struct carplay_vehicle_state *g_vehicle_state;
static uint64_t g_vehicle_generation;
static struct airplay_client *g_active_airplay;
static struct ncm_connect_request *g_pending_connect_request, *g_connect_request;
static int g_connect_retrying;
static unsigned long g_screen_data_rx_chunks;
static uint64_t g_screen_data_rx_bytes;
int carplay_log_should_sample(unsigned long n) { return n<16; }
static struct ncm_connect_request replacement;
static int scenario,new_clients,freed,connects,starts,results,sleeps,launches;
static int open_fds[512],peer_fds[512];
static int connect_cancelled(void) { return g_connect_stop || g_connect_retiring; }
static uint64_t fake_ms;
static uint64_t ncm_now_ms(void) { return fake_ms; }
static int connect_ready_locked(void) { return !g_connect_stop && g_host_authenticated && g_tun_index>0; }
static int launch_connect_request_locked(struct ncm_connect_request *r) { assert(r==&replacement);launches++;return -1; }
static void publish_airplay_status(struct airplay_client *c, const struct ncm_connect_request *r) { (void)c;(void)r; }
static int ensure_airplay_policy_routes(const char *s) { (void)s;return 0; }
static int address_is_ipv4(const char *s) { return strchr(s,':')==NULL; }
static int copy_learned_peer_ipv6(char *s,size_t n) { (void)s;(void)n;return 0; }
static int new_socket(int minimum) {
    int pair[2];assert(socketpair(AF_UNIX,SOCK_STREAM,0,pair)==0);
    int fd=fcntl(pair[0],F_DUPFD_CLOEXEC,minimum);close(pair[0]);assert(fd>=100 && fd<512);
    open_fds[fd]=1;peer_fds[fd]=pair[1];return fd;
}
static void all_closed(void) {
    for(int i=100;i<512;i++)assert(!open_fds[i]);
}
static int connect_one(const char *s,uint16_t p,int v4,char *r,size_t n,int verbose,int discovered) {
    (void)v4;(void)verbose;(void)discovered;assert(p==7000);
    if(scenario==18 && !new_clients && !strcmp(s,"169.254.0.2"))return -1;
    assert(!strcmp(s,scenario==18?"169.254.0.3":"169.254.0.2"));
    assert(freed==new_clients && !g_active_airplay);all_closed();
    snprintf(r,n,"%s",s);connects++;return new_socket(100+connects);
}
static int connect_advertised_port(const struct ncm_connect_request *r,uint16_t p,char *s,size_t n) {
    assert(r->count==1);snprintf(s,n,"169.254.0.2");return new_socket(200+new_clients*10+(p==7001?1:2));
}
static int test_close(int fd) {
    assert(fd>=100 && fd<512 && open_fds[fd]);open_fds[fd]=0;
    close(peer_fds[fd]);assert(close(fd)==0);assert(fcntl(fd,F_GETFD)<0 && errno==EBADF);return 0;
}
static int test_shutdown(int fd,int how) { assert(fd>=100 && open_fds[fd]);return shutdown(fd,how); }
static int test_poll(struct pollfd *p,nfds_t n,int ms) { (void)p;assert(n>=1 && ms<=200);return 0; }
static int test_usleep(useconds_t us) {
    if(us>100000u)fprintf(stderr,"display preparation requested %u us in scenario %d\n",us,scenario);
    assert(us>0 && us<=100000u);fake_ms+=us/1000;sleeps++;
    if(scenario==3)g_connect_stop=1;
    if(scenario==4)g_usb_generation++;
    if(scenario==5)g_pending_connect_request=&replacement;
    if(scenario==6)g_vehicle_generation++;
    if(scenario==13)g_host_authenticated=0;
    if(scenario==14)g_tun_index++;
    if(scenario==15)g_airplay_transport_restart_pending=1;
    return 0;
}
struct airplay_client *airplay_client_new_with_audio(int fd,int media,int mic) {
    assert(open_fds[fd] && media==1 && mic==0);struct airplay_client *c=calloc(1,sizeof(*c));
    c->serial=++new_clients;return c;
}
void airplay_client_set_profile(struct airplay_client *c,enum airplay_profile p) {
    enum airplay_profile expected=scenario==19?AIRPLAY_PROFILE_HAP:AIRPLAY_PROFILE_LEGACY;
    if((scenario==9 && c->serial==3) || ((scenario==10 || scenario==11) && c->serial>=2))expected=AIRPLAY_PROFILE_HAP;
    assert(p==expected);c->profile=p;
}
void airplay_client_set_video_options(struct airplay_client *c,int w,int h,int pw,int ph,int den,int fps,int br) {
    (void)c;(void)h;(void)pw;(void)ph;(void)den;(void)fps;(void)br;assert(w==800);
}
void airplay_client_set_vehicle_state(struct airplay_client *c,struct carplay_vehicle_state *s,uint64_t n) {(void)c;(void)s;(void)n;}
int airplay_client_start(struct airplay_client *c) { (void)c;return 0; }
int airplay_client_tick(struct airplay_client *c) {
    if(scenario==12)g_connect_stop=1;
    c->ticks++;
    if(c->ticks>1) {
        if(scenario==20)g_pending_connect_request=&replacement;
        if(scenario==21)g_usb_generation++;
        if(scenario==22)g_host_netdev_ready=0;
        if(scenario==23)g_vehicle_state=(struct carplay_vehicle_state *)(void *)&replacement;
        if(scenario==24)g_kernel_ncm=1;
    }
    /* Stop after the last expected client. This fixture can establish video
     * before asking for preparation/fallback, so failure alone would start
     * the separate general recovery path once the special path has run. */
    if((c->serial==2 && scenario!=8 && scenario!=9 && scenario!=10) || c->serial==3)
        g_connect_stop=1;
    return c->ticks>1 || c->serial>1?-1:0;
}
int airplay_client_failed(const struct airplay_client *c) { (void)c;return 0; }
int airplay_client_wants_hap_fallback(const struct airplay_client *c) {
    return (scenario==9 && c->serial==2) || ((scenario==10 || scenario==11) && c->serial==1);
}
int airplay_client_get_display_preparation(const struct airplay_client *c,struct carplay_display_plan *p) {
    int wanted=scenario==11?0:scenario==10?c->serial==2:c->serial==1 || (scenario==8 && c->serial==2);
    memset(p,0,sizeof(*p));p->width=p->physical_width=800;p->height=p->physical_height=480;p->physical_fps=60;p->density=160;
    return wanted;
}
int carplay_display_start_preparation(const struct carplay_display_plan *p,char *id,size_t n) {
    assert(p->physical_width==800 && p->physical_height==480 && p->physical_fps==60);
    assert(freed==new_clients && !g_active_airplay && g_connect_sock==-1);all_closed();
    assert(++starts==1);snprintf(id,n,"prep-1");return scenario==7?-1:0;
}
int carplay_display_preparation_result(const char *id,unsigned w,unsigned h,unsigned fps) {
    assert(!strcmp(id,"prep-1") && w==800 && h==480 && fps==60);results++;
    if(scenario==1)return -1;
    if(scenario==2)return 0;
    if(scenario==16){g_usb_generation++;return 1;}
    return fake_ms>=500?1:0;
}
const char *airplay_client_failure(const struct airplay_client *c) { (void)c;return "test"; }
int airplay_client_setup_complete(const struct airplay_client *c) { (void)c;return 1; }
uint16_t airplay_client_event_port(const struct airplay_client *c) { (void)c;return 7001; }
int airplay_client_attach_event(struct airplay_client *c,int fd) { assert(open_fds[fd]);c->event=1;return 0; }
int airplay_client_send_screen_setup(struct airplay_client *c) { c->screen=1;return 0; }
int airplay_client_screen_setup_complete(const struct airplay_client *c) { return c->screen; }
uint16_t airplay_client_screen_data_port(const struct airplay_client *c) { (void)c;return 7002; }
int airplay_client_attach_screen_data(struct airplay_client *c,int fd) { (void)c;assert(open_fds[fd]);return 0; }
int airplay_client_start_audio(struct airplay_client *c,const char *h) { (void)c;(void)h;return 0; }
int airplay_client_send_record(struct airplay_client *c) { c->record=1;return 0; }
int airplay_client_record_accepted(const struct airplay_client *c) { return c->record; }
int airplay_client_send_test_screen(struct airplay_client *c) { (void)c;return 0; }
int airplay_client_start_live_screen(struct airplay_client *c) { (void)c;return 0; }
void airplay_client_feed(struct airplay_client *c,const uint8_t *p,size_t n) { (void)c;(void)p;(void)n; }
void airplay_client_feed_event(struct airplay_client *c,const uint8_t *p,size_t n) { (void)c;(void)p;(void)n; }
void airplay_client_free(struct airplay_client *c) { if(c){assert(g_active_airplay!=c && g_connect_sock==-1);freed++;free(c);} }
#define close test_close
#define shutdown test_shutdown
#define poll test_poll
#define usleep test_usleep
''' + bodies + r'''
int main(void) {
    for(scenario=0;scenario<25;scenario++) {
        new_clients=freed=connects=starts=results=sleeps=launches=0;fake_ms=100;
        g_connect_stop=0;g_connect_thread_running=1;g_connect_sock=-1;g_usb_generation=20;
        g_tun_index=7;g_kernel_ncm=0;g_host_netdev_ready=1;g_host_authenticated=1;
        g_airplay_transport_restart_pending=0;g_vehicle_state=NULL;g_vehicle_generation=0;g_pending_connect_request=NULL;
        struct ncm_connect_request *r=calloc(1,sizeof(*r));r->count=2;r->addresses=calloc(2,sizeof(char*));
        r->addresses[0]=strdup("169.254.0.2");r->addresses[1]=strdup("169.254.0.3");r->port=7000;
        r->profile=scenario==19?AIRPLAY_PROFILE_HAP:AIRPLAY_PROFILE_LEGACY;r->media_audio=1;r->display_width=800;
        r->owner=(struct ncm_connect_owner){.usb_generation=g_usb_generation,.tun_index=g_tun_index,
            .host_netdev_ready=g_host_netdev_ready};
        g_connect_request=r;
        connect_endpoint_main(r);
        int expected_clients=(scenario==0 || scenario==8 || scenario==11 || scenario==17 || scenario==18 || scenario==19)?2:
            (scenario==9 || scenario==10)?3:1;
        int expected_starts=scenario==11 || scenario==12 || scenario>=20?0:1;
        if(new_clients!=expected_clients || starts!=expected_starts) {
            fprintf(stderr,"scenario=%d clients=%d expected=%d starts=%d expected=%d\n",scenario,new_clients,expected_clients,starts,expected_starts);return 1;
        }
        assert(connects==expected_clients && freed==expected_clients && !g_active_airplay && !g_connect_thread_running);all_closed();
        if(scenario==2)assert(sleeps==2500 && fake_ms==250100 && results<=2500);
        if(scenario==3 || scenario==4 || scenario==5 || scenario==6 || scenario==13 || scenario==14 || scenario==15)assert(sleeps==1);
        if(scenario==1 || scenario==7 || scenario==12 || scenario==16)assert(!sleeps);
        if(scenario==5)assert(launches==1 && g_pending_connect_request==&replacement);
        if(scenario==20)assert(launches==1 && g_pending_connect_request==&replacement);
    }
    puts("NCM display reconnect: 25 ownership/success/failure/timeout/stop/teardown/profile scenarios PASS");return 0;
}
'''
        with tempfile.TemporaryDirectory() as tmp:
            cfile=Path(tmp)/"test.c";cfile.write_text(code)
            binary=Path(tmp)/"test"
            compiler=["cc","-std=c11","-Wall","-Wextra","-Werror"]
            if os.environ.get("NCM_RECONNECT_SANITIZE"):
                compiler += ["-O1","-g","-fsanitize=address,undefined","-fno-omit-frame-pointer"]
            subprocess.run(compiler+["-I",str(ROOT/"src"),str(cfile),"-pthread","-o",str(binary)],check=True)
            result=subprocess.run([str(binary)],capture_output=True,text=True,timeout=8)
            self.assertEqual(result.returncode,0,result.stdout+result.stderr)
            print(result.stdout,end="")


if __name__ == "__main__":
    unittest.main()
