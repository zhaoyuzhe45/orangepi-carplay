"""Execute production NCM connection, retirement and advertisement paths."""
from pathlib import Path
import os
import subprocess
import tempfile
import unittest
from ncm_compat_test import ROOT, function


class VideoReconnectTest(unittest.TestCase):
    def test_established_session_recovery(self):
        source = (ROOT / "src/ncm_bridge.c").read_text()
        structs = ""
        for name in ("ncm_connect_owner", "ncm_connect_request"):
            start = source.index(f"struct {name} {{")
            structs += source[start:source.index("\n};", start) + 3] + "\n"
        bodies = "\n".join(function(source, s) for s in (
            "static int connect_cancelled(", "static void free_connect_request(",
            "static int transport_ready_locked(", "static int connect_ready_locked(",
            "int ncm_bridge_host_authentication(", "static int connect_owned_locked(",
            "static int connect_owned(", "static int wait_connect_retry(", "static int wait_display_preparation(",
            "static void *connect_endpoint_main(", "static int launch_connect_request_locked(",
            "static int same_connect_endpoint(", "static int queue_connect_endpoint("))
        code = r'''
#define _GNU_SOURCE
#include <assert.h>
#include <pthread.h>
#include <stdatomic.h>
#include <arpa/inet.h>
#include <net/if.h>
#include <sys/socket.h>
#include <fcntl.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>
#include "airplay_client.h"
#include "ncm_bridge.h"
#include "carplay_display.h"
#include "carplay_log.h"
#define AIRPLAY_CONNECT_RETRY_ATTEMPTS 2u
#define AIRPLAY_CONNECT_RETRY_DELAY_US 250000u
#define AIRPLAY_RECOVERY_POLL_MS 100u
#define AIRPLAY_RECOVERY_STABLE_MS 60000u
#define DISPLAY_PREPARATION_TIMEOUT_MS 250000u
static void iap2_log(const char *s, ...) { (void)s; }
''' + structs + r'''
struct airplay_client { int step, record, setup, screen; enum airplay_profile profile; };
static pthread_mutex_t g_mu=PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t g_connect_cv=PTHREAD_COND_INITIALIZER;
static atomic_int g_connect_stop, g_connect_retiring;
static int g_connect_thread_running, g_connect_sock=-1;
static unsigned long g_usb_generation, g_transport_restart_epoch;
static int g_tun_index, g_running, g_kernel_ncm, g_host_netdev_ready;
static int g_airplay_transport_restart_pending, g_transport_restarting;
static int g_host_authenticated, g_host_auth_required, g_mdns_enabled=1;
static int g_airplay_media_enabled=1, g_airplay_microphone_enabled;
static int g_airplay_display_width, g_airplay_display_height, g_airplay_physical_width;
static int g_airplay_physical_height, g_airplay_display_density, g_airplay_video_fps, g_airplay_video_bitrate;
static unsigned long g_screen_data_rx_chunks;
static uint64_t g_screen_data_rx_bytes, g_vehicle_generation;
static struct carplay_vehicle_state *g_vehicle_state;
static struct airplay_client *g_active_airplay;
static struct ncm_connect_request *g_pending_connect_request, *g_connect_request;
static int g_connect_retrying;
static int launch_connect_request_locked(struct ncm_connect_request *r);
static int queue_connect_endpoint(const char *const a[],size_t n,uint16_t p,enum airplay_profile pr,int d);
int carplay_log_should_sample(unsigned long n) { (void)n;return 0; }
static const char *a="169.254.0.2", *b="169.254.0.3", *third="169.254.0.4";
enum step { STREAM_FAIL, TCP_FAIL, REFUSE, STREAM_STOP, STREAM_STABLE_FAIL, EVENT_OWNER_LOSS,
    CONTROL_OWNER_LOSS, SCREEN_OWNER_LOSS, STREAM_PEER_CLOSE, RECORD_AND_PEER_CLOSE, LEGACY_REFUSE, LEGACY_SCREEN_TCP_FAIL, LEGACY_SCREEN_ATTACH_FAIL };
enum action { NO_ACTION, STOP, AUTH_LOSS, USB_CHANGE, IFACE_CHANGE, RESTART, DUPLICATE, NEW_MDNS, NEW_EXPLICIT, IFACE_REANNOUNCE, CHANGE_BEFORE_WORKER, ALT_FIRST_ADDRESS, OVERLAPPING_REPLACEMENT, POLL_OWNER_CHANGE };
static enum action action;
static const enum step *steps;
static size_t step_count;
static int attempts, clients, frees, starts, attaches, publications, sleeps, launches, session_step;
static int control_fd, screen_peer, screen_data_fd;
static int end_requested, live_starts;
static uint64_t now_ms, attempts_at[32];
static const char *attempted[32];
static int peers[512];
static void *(*worker)(void *);
static void *worker_arg;
static uint64_t ncm_now_ms(void) { return now_ms; }
static void clean(void) {
    assert(!g_active_airplay && g_connect_sock==-1 && frees==clients);
    for (int i=100;i<512;i++) assert(!peers[i]);
}
static int test_pthread_create(pthread_t *t,const pthread_attr_t *pa,void *(*run)(void *),void *arg) {
    (void)t;(void)pa;assert(!worker);worker=run;worker_arg=arg;launches++;return 0;
}
static int test_usleep(useconds_t us) {
    assert(us>0 && us<=100000u);clean();now_ms+=us/1000u;sleeps++;
    if(sleeps!=1) return 0;
    switch(action) {
    case STOP: g_connect_stop=1;break;
    case AUTH_LOSS: assert(ncm_bridge_host_authentication(0)==1);break;
    case USB_CHANGE: g_usb_generation++;break;
    case IFACE_CHANGE: g_tun_index++;break;
    case RESTART: g_transport_restart_epoch++;break; /* restart has already completed */
    case IFACE_REANNOUNCE:
        g_tun_index++;
        assert(!queue_connect_endpoint(&a,1,7000,AIRPLAY_PROFILE_HAP,0));
        assert(launches==1);break;
    case OVERLAPPING_REPLACEMENT: {
        const char *addresses[]={third,a};
        assert(!queue_connect_endpoint(addresses,2,7000,AIRPLAY_PROFILE_HAP,0));
        assert(launches==1);break;
    }
    case DUPLICATE: assert(!queue_connect_endpoint(&a,1,7000,AIRPLAY_PROFILE_LEGACY,1));break;
    case NEW_MDNS:
    case NEW_EXPLICIT:
        assert(!queue_connect_endpoint(&b,1,7000,AIRPLAY_PROFILE_HAP,action==NEW_MDNS));
        assert(!queue_connect_endpoint(&b,1,7000,AIRPLAY_PROFILE_HAP,action==NEW_MDNS));
        assert(!queue_connect_endpoint(&a,1,7000,AIRPLAY_PROFILE_LEGACY,1));
        assert(launches==1);break;
    default: break;
    }
    return 0;
}
static void publish_airplay_status(struct airplay_client *c, const struct ncm_connect_request *r) {
    assert(r==g_connect_request);
    if(c){ assert(g_active_airplay==c);publications++; }
}
static int ensure_airplay_policy_routes(const char *s) { (void)s;return 0; }
static int address_is_ipv4(const char *s) { return strchr(s,':')==NULL; }
static int copy_learned_peer_ipv6(char *s,size_t n) { (void)s;(void)n;return 0; }
static int new_socket(int minimum) {
    int pair[2];assert(!socketpair(AF_UNIX,SOCK_STREAM,0,pair));
    int fd=fcntl(pair[0],F_DUPFD_CLOEXEC,minimum);close(pair[0]);
    assert(fd>=100 && fd<512 && pair[1]>0);peers[fd]=pair[1];return fd;
}
static int connect_one(const char *s,uint16_t p,int v4,char *r,size_t n,int v,int d) {
    (void)d;(void)v;assert(p==7000 && v4);clean();
    assert(attempts<(int)step_count && attempts<32);
    session_step=steps[attempts]; attempts_at[attempts]=now_ms;
    attempted[attempts++]=!strcmp(s,a)?a:!strcmp(s,b)?b:!strcmp(s,third)?third:NULL;
    if(session_step==TCP_FAIL)return -1;
    if(session_step==CONTROL_OWNER_LOSS)g_usb_generation++;
    snprintf(r,n,"%s",s);control_fd=new_socket(100);return control_fd;
}
static int connect_advertised_port(const struct ncm_connect_request *r,uint16_t p,char *s,size_t n) {
    assert(r->count==1 && p>=7001 && p<=7002);
    assert(!strcmp(r->addresses[0],attempted[attempts-1]));
    if(p==7002 && session_step==LEGACY_SCREEN_TCP_FAIL)return -1;
    snprintf(s,n,"%s",r->addresses[0]);int fd=new_socket(p==7001?200:300);
    if(p==7001 && session_step==EVENT_OWNER_LOSS)g_tun_index++;
    if(p==7002 && session_step==SCREEN_OWNER_LOSS)g_transport_restart_epoch++;
    if(p==7002) {screen_peer=peers[fd];screen_data_fd=fd;}
    return fd;
}
static int test_close(int fd) {
    assert(fd>=100 && fd<512 && peers[fd]);
    if(peers[fd]>0)close(peers[fd]);
    peers[fd]=0;assert(!close(fd));assert(fcntl(fd,F_GETFD)<0 && errno==EBADF);return 0;
}
static int test_shutdown(int fd,int how) { assert(fd>=100 && peers[fd]);return shutdown(fd,how); }
static int test_poll(struct pollfd *p,nfds_t n,int ms) {
    if(action==POLL_OWNER_CHANGE && attempts==2) {
        if(ms>100)fprintf(stderr,"replacement RTSP poll requested %d ms\n",ms);
        assert(ms<=100);now_ms+=ms;g_tun_index++;
    }
    if(end_requested && (session_step==STREAM_PEER_CLOSE || session_step==RECORD_AND_PEER_CLOSE)) {
        assert(n==3 && p[0].fd==control_fd && p[2].fd==screen_data_fd);
        p[2].revents=POLLHUP;
        if(session_step==RECORD_AND_PEER_CLOSE) {p[0].revents=POLLIN;return 2;}
        return 1;
    }
    return 0;
}
struct airplay_client *airplay_client_new_with_audio(int fd,int media,int mic) {
    assert(fd==control_fd && media==1 && mic==0);clients++;end_requested=0;
    struct airplay_client *c=calloc(1,sizeof(*c));c->step=session_step;return c;
}
void airplay_client_set_profile(struct airplay_client *c,enum airplay_profile p) {
    c->profile=p;
    int legacy=steps[0]==LEGACY_SCREEN_TCP_FAIL || steps[0]==LEGACY_SCREEN_ATTACH_FAIL;
    assert(p==((legacy || c->step==LEGACY_REFUSE)?AIRPLAY_PROFILE_LEGACY:AIRPLAY_PROFILE_HAP));
}
void airplay_client_set_video_options(struct airplay_client*c,int w,int h,int pw,int ph,int den,int fps,int br) {
    (void)c;(void)w;(void)h;(void)pw;(void)ph;(void)den;(void)fps;(void)br;
}
void airplay_client_set_vehicle_state(struct airplay_client*c,struct carplay_vehicle_state*s,uint64_t n) {(void)c;(void)s;(void)n;}
int airplay_client_start(struct airplay_client *c) { assert(c==g_active_airplay);starts++;return 0; }
int airplay_client_tick(struct airplay_client *c) {
    if(end_requested && c->step==STREAM_STABLE_FAIL)now_ms+=60000;
    return end_requested && c->step!=STREAM_PEER_CLOSE && c->step!=RECORD_AND_PEER_CLOSE?-1:0;
}
int airplay_client_failed(const struct airplay_client *c) { return c->step==REFUSE || c->step==LEGACY_REFUSE; }
int airplay_client_wants_hap_fallback(const struct airplay_client *c) { return c->step==LEGACY_REFUSE; }
int airplay_client_get_display_preparation(const struct airplay_client*c,struct carplay_display_plan*p) {(void)c;(void)p;return 0;}
int carplay_display_start_preparation(const struct carplay_display_plan*p,char*i,size_t n) {(void)p;(void)i;(void)n;assert(0);return -1;}
int carplay_display_preparation_result(const char*i,unsigned w,unsigned h,unsigned fps) {(void)i;(void)w;(void)h;(void)fps;assert(0);return 0;}
const char *airplay_client_failure(const struct airplay_client *c) { (void)c;return "test refusal"; }
int airplay_client_setup_complete(const struct airplay_client *c) { (void)c;return 1; }
uint16_t airplay_client_event_port(const struct airplay_client *c) { (void)c;return 7001; }
int airplay_client_attach_event(struct airplay_client *c,int fd) { (void)c;assert(peers[fd]);attaches++;return 0; }
int airplay_client_send_screen_setup(struct airplay_client *c) { c->setup=1;return 0; }
int airplay_client_screen_setup_complete(const struct airplay_client *c) { return c->setup; }
uint16_t airplay_client_screen_data_port(const struct airplay_client *c) { (void)c;return 7002; }
int airplay_client_attach_screen_data(struct airplay_client *c,int fd) {
    assert(peers[fd]);attaches++;
    if(c->step==LEGACY_SCREEN_ATTACH_FAIL)return -1;
    c->screen=1;return 0;
}
int airplay_client_start_audio(struct airplay_client *c,const char *s) { (void)c;(void)s;return 0; }
static void close_screen_peer(void) {assert(!close(screen_peer));peers[screen_data_fd]=-1;}
int airplay_client_send_record(struct airplay_client *c) {
    assert((c->profile==AIRPLAY_PROFILE_LEGACY || c->screen) && !c->record);
    if(c->step==RECORD_AND_PEER_CLOSE) {
        assert(write(peers[control_fd],"R",1)==1);close_screen_peer();end_requested=1;
    } else c->record=1;
    return 0;
}
int airplay_client_record_accepted(const struct airplay_client *c) { return c->record; }
int airplay_client_send_test_screen(struct airplay_client *c) { assert(c->record && c->screen);return 0; }
int airplay_client_start_live_screen(struct airplay_client *c) {
    live_starts++;end_requested=1;
    if(c->step==STREAM_STOP)g_connect_stop=1;
    if(c->step==STREAM_PEER_CLOSE)close_screen_peer();
    return 0;
}
void airplay_client_feed(struct airplay_client *c,const uint8_t *p,size_t n) {
    assert(c->step==RECORD_AND_PEER_CLOSE && n==1 && p[0]=='R');c->record=1;
}
void airplay_client_feed_event(struct airplay_client *c,const uint8_t *p,size_t n) { (void)c;(void)p;(void)n; }
void airplay_client_free(struct airplay_client *c) { if(c){assert(!g_active_airplay && g_connect_sock==-1);frees++;free(c);} }
#define close test_close
#define shutdown test_shutdown
#define poll test_poll
#define usleep test_usleep
#define pthread_create test_pthread_create
''' + bodies + r'''
#define CHECK(e) do { if(!(e)){fprintf(stderr,"%s:%d: %s; attempts=%d launches=%d sleeps=%d now=%llu\n",name,__LINE__,#e,attempts,launches,sleeps,(unsigned long long)now_ms);return 1;} } while(0)
static int run(const char *name,const enum step *seq,size_t count,enum action act) {
    steps=seq;step_count=count;action=act;
    attempts=clients=frees=starts=attaches=publications=sleeps=launches=live_starts=0;now_ms=101;
    g_connect_stop=0;g_host_netdev_ready=1;g_host_auth_required=1;g_host_authenticated=1;g_tun_index=7;
    const char *addresses[]={a,b};
    int legacy=seq[0]==LEGACY_REFUSE || seq[0]==LEGACY_SCREEN_TCP_FAIL || seq[0]==LEGACY_SCREEN_ATTACH_FAIL;
    int count_addresses=(act==ALT_FIRST_ADDRESS || act==OVERLAPPING_REPLACEMENT)?2:1;
    CHECK(!queue_connect_endpoint(addresses,count_addresses,7000,
        legacy?AIRPLAY_PROFILE_LEGACY:AIRPLAY_PROFILE_HAP,0));
    if(act==CHANGE_BEFORE_WORKER)g_usb_generation++;
    while(worker) { void *(*run)(void *)=worker;worker=NULL;run(worker_arg); }
    clean();CHECK(!g_connect_thread_running && !g_pending_connect_request);
    return 0;
}
#define RUN(label, act, ...) do { const enum step seq[]={__VA_ARGS__}; if(run(label,seq,sizeof(seq)/sizeof(*seq),act))return 1; } while(0)
int main(int argc,char **argv) {
    const char *name="owned recovery";
    if(argc==2 && !strcmp(argv[1],"poll")) {
        name="replacement RTSP poll checks owner within 100ms";
        RUN(name,POLL_OWNER_CHANGE,STREAM_FAIL,STREAM_STOP);
        CHECK(attempts==2 && starts==2 && live_starts==1 && now_ms==1201);
        return 0;
    }
    if(argc==2 && !strcmp(argv[1],"overlap")) {
        name="unused alternate does not mask endpoint replacement";
        RUN(name,OVERLAPPING_REPLACEMENT,TCP_FAIL,STREAM_FAIL,STREAM_STOP);
        CHECK(attempts==3 && launches==2 && attempted[1]==b && attempted[2]==third && attempts_at[2]==201);
        return 0;
    }
    if(argc==2 && !strcmp(argv[1],"legacy")) {
        name="legacy SETUP and RECORD accepted but screen transport unavailable";
        for(int i=LEGACY_SCREEN_TCP_FAIL;i<=LEGACY_SCREEN_ATTACH_FAIL;i++) {
            RUN(name,NO_ACTION,i,STREAM_STOP);
            CHECK(attempts==2 && launches==1 && starts==2 && live_starts==1 && attempts_at[1]==1101);
        }
        return 0;
    }
    RUN(name,NO_ACTION, STREAM_PEER_CLOSE,TCP_FAIL,REFUSE,STREAM_FAIL,STREAM_FAIL,STREAM_FAIL,
        STREAM_FAIL,STREAM_FAIL,STREAM_STABLE_FAIL,STREAM_STOP);
    const uint64_t offset[]={0,1000,3000,7000,15000,30000,45000,60000,75000,136000};
    CHECK(attempts==10 && launches==1 && starts==9);
    for(int i=0;i<attempts;i++)CHECK(attempts_at[i]==101+offset[i] && attempted[i]==a);
    name="initial refusal";
    RUN(name,NO_ACTION,REFUSE);CHECK(attempts==1 && starts==1 && !sleeps);
    name="cancel backoff";
    for(int i=STOP;i<=RESTART;i++) {RUN(name,i,STREAM_FAIL);CHECK(attempts==1 && sleeps==1 && launches==1);}
    name="deduplicate";
    RUN(name,DUPLICATE,STREAM_FAIL,STREAM_STOP);
    CHECK(attempts==2 && launches==1 && attempts_at[1]==1101 && attempted[1]==a);
    name="supersede";
    for(int i=NEW_MDNS;i<=NEW_EXPLICIT;i++) {
        RUN(name,i,STREAM_FAIL,STREAM_STOP);
        CHECK(attempts==2 && sleeps==1 && launches==2 && attempted[1]==b && attempts_at[1]==201);
    }
    name="rediscover same endpoint on new interface";
    RUN(name,IFACE_REANNOUNCE,STREAM_FAIL,STREAM_STOP);
    CHECK(attempts==2 && sleeps==1 && launches==2 && attempted[1]==a && attempts_at[1]==201);
    name="change before worker starts";
    RUN(name,CHANGE_BEFORE_WORKER,REFUSE);CHECK(!attempts && launches==1 && !clients);
    name="RECORD followed by peer close before encoder starts";
    RUN(name,NO_ACTION,RECORD_AND_PEER_CLOSE,STREAM_STOP);
    CHECK(attempts==2 && launches==1 && starts==2 && live_starts==1 && attempts_at[1]==1101);
    name="recover on the selected address";
    RUN(name,ALT_FIRST_ADDRESS,TCP_FAIL,STREAM_FAIL,STREAM_STOP);
    CHECK(attempts==3 && launches==1 && starts==2 && attempted[0]==a && attempted[1]==b &&
          attempted[2]==b && attempts_at[1]==101 && attempts_at[2]==1101);
    name="recover with the selected HAP profile";
    RUN(name,NO_ACTION,LEGACY_REFUSE,STREAM_FAIL,STREAM_STOP);
    CHECK(attempts==3 && launches==1 && starts==3 && attempts_at[1]==101 && attempts_at[2]==1101);
    name="stale control completion";
    RUN(name,NO_ACTION,CONTROL_OWNER_LOSS);CHECK(attempts==1 && !clients && !starts && !publications);
    name="stale event completion";
    RUN(name,NO_ACTION,EVENT_OWNER_LOSS);CHECK(attempts==1 && starts==1 && !attaches);
    name="stale screen completion";
    RUN(name,NO_ACTION,SCREEN_OWNER_LOSS);CHECK(attempts==1 && starts==1 && attaches==1);
    puts("NCM video recovery: bounded retry, stable reset, teardown, cancellation and endpoints PASS");return 0;
}
'''
        with tempfile.TemporaryDirectory() as tmp:
            cfile = Path(tmp)/"test.c"
            cfile.write_text(code)
            binary = Path(tmp)/"test"
            compiler = ["cc", "-std=c11", "-Wall", "-Wextra", "-Werror"]
            if os.environ.get("NCM_RECONNECT_SANITIZE"):
                compiler += ["-O1", "-g", "-fsanitize=address,undefined", "-fno-omit-frame-pointer"]
            subprocess.run(compiler+["-I",str(ROOT/"src"),str(cfile),"-pthread","-o",str(binary)],check=True)
            for args in ([], ["poll"], ["overlap"], ["legacy"]):
                with self.subTest(case=args):
                    result = subprocess.run([str(binary), *args],capture_output=True,text=True,timeout=8)
                    self.assertEqual(result.returncode,0,result.stdout+result.stderr)
                    print(result.stdout,end="")


if __name__ == "__main__":
    unittest.main()
