"""Execute the production connector loop and its socket/client cleanup path."""
from pathlib import Path
import unittest
from ncm_compat_test import ROOT, function, run_c


class ProfileTest(unittest.TestCase):
    def test_legacy_order_and_one_fresh_hap_fallback(self):
        source=(ROOT/'src/ncm_bridge.c').read_text()
        start=source.index('struct ncm_connect_owner {')
        request=source[start:source.index('\n};',source.index('struct ncm_connect_request {'))+3]
        bodies='\n'.join(function(source,s) for s in (
            'static void free_connect_request(', 'static int connect_owned_locked(',
            'static int connect_owned(', 'static int wait_connect_retry(',
            'static void *connect_endpoint_main('))
        code=r'''
#define _GNU_SOURCE
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
#include "carplay_display.h"
#define AIRPLAY_CONNECT_RETRY_ATTEMPTS 1
#define AIRPLAY_CONNECT_RETRY_DELAY_US 1
#define AIRPLAY_RECOVERY_POLL_MS 100u
#define AIRPLAY_RECOVERY_STABLE_MS 60000u
static void iap2_log(const char *format, ...) { (void)format; }
''' + request + r'''
struct airplay_client {enum airplay_profile profile;int record,event,screen,attached,failed,fallback;};
static pthread_mutex_t g_mu=PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t g_connect_cv=PTHREAD_COND_INITIALIZER;
static atomic_int g_connect_stop;
static int g_connect_retiring;
static int connect_cancelled(void) {return g_connect_stop || g_connect_retiring;}
static int g_connect_thread_running,g_connect_sock=-1;
static unsigned long g_usb_generation, g_transport_restart_epoch;
static int g_tun_index,g_kernel_ncm,g_host_netdev_ready,g_airplay_transport_restart_pending;
static struct carplay_vehicle_state *g_vehicle_state;
static uint64_t g_vehicle_generation;
static struct airplay_client *g_active_airplay;
static struct ncm_connect_request *g_pending_connect_request,*g_connect_request;
static int g_connect_retrying;
static unsigned long g_screen_data_rx_chunks;
static uint64_t g_screen_data_rx_bytes;
int carplay_log_should_sample(unsigned long n) { return n<16; }
static uint64_t ncm_now_ms(void) {return 1;}
static int mode,bad,new_clients,freed,connects,records,screens,screen_requests,lives,ticks,polls,open_fds[512],peer_fds[512];
static int connect_ready_locked(void) {return !g_connect_stop;}
static int wait_display_preparation(const struct ncm_connect_request*r,const char*i,const struct carplay_display_plan*p) {(void)r;(void)i;(void)p;return -1;}
int carplay_display_start_preparation(const struct carplay_display_plan*p,char*i,size_t n) {(void)p;(void)i;(void)n;return -1;}
int airplay_client_get_display_preparation(const struct airplay_client*c,struct carplay_display_plan*p) {(void)c;(void)p;return 0;}
static int launch_connect_request_locked(struct ncm_connect_request*r) {(void)r;return -1;}
static void publish_airplay_status(struct airplay_client*c,const struct ncm_connect_request*r) {(void)c;(void)r;}
static int ensure_airplay_policy_routes(const char*s) {(void)s;return 0;}
static int address_is_ipv4(const char*s) {return strchr(s,':')==NULL;}
static int copy_learned_peer_ipv6(char*s,size_t n) {(void)s;(void)n;return 0;}
static int new_socket(int minimum) {
    int pair[2];if(socketpair(AF_UNIX,SOCK_STREAM,0,pair))return -1;
    int fd=fcntl(pair[0],F_DUPFD_CLOEXEC,minimum);close(pair[0]);
    if(fd<0 || fd>=512)exit(20);
    open_fds[fd]=1;peer_fds[fd]=pair[1];return fd;
}
static int connect_one(const char*s,uint16_t p,int v4,char*r,size_t n,int verbose,int only_ncm) {
    (void)p;(void)v4;(void)verbose;(void)only_ncm;
    if(connects && strcmp(s,"169.254.0.2"))bad=1;
    if(connects && (freed!=new_clients || g_active_airplay))bad=1;
    for(int i=100;i<512;i++)if(open_fds[i])bad=1;
    snprintf(r,n,"%s",s);connects++;return new_socket(100+connects);
}
static int connect_advertised_port(const struct ncm_connect_request*r,uint16_t p,char*s,size_t n) {
    (void)r;snprintf(s,n,"169.254.0.2");return new_socket(200+new_clients*10+(p==7001?1:2));
}
static int test_close(int fd) {
    if(fd>=100 && fd<512) {
        if(!open_fds[fd])bad=1;open_fds[fd]=0;
        close(peer_fds[fd]);close(fd);
        if(fcntl(fd,F_GETFD)!=-1 || errno!=EBADF)bad=1;
    }
    return 0;
}
static int test_shutdown(int fd,int how) {return shutdown(fd,how);}
static int test_poll(struct pollfd*p,nfds_t n,int ms) {(void)p;(void)n;(void)ms;if(++polls>10) {bad=1;g_connect_stop=1;}return 0;}
struct airplay_client *airplay_client_new_with_audio(int fd,int media,int mic) {
    (void)fd;if(media!=1 || mic!=0)bad=1;new_clients++;return calloc(1,sizeof(struct airplay_client));
}
void airplay_client_set_profile(struct airplay_client*c,enum airplay_profile p) {c->profile=p;}
enum airplay_profile airplay_client_profile(const struct airplay_client*c) {return c->profile;}
void airplay_client_set_video_options(struct airplay_client*c,int w,int h,int pw,int ph,int den,int fps,int br) {
    (void)c;(void)h;(void)pw;(void)ph;(void)den;(void)fps;(void)br;if(w!=1024)bad=1;
}
void airplay_client_set_vehicle_state(struct airplay_client*c,struct carplay_vehicle_state*s,uint64_t n) {(void)c;(void)s;(void)n;}
int airplay_client_start(struct airplay_client*c) {
    if(new_clients==1 && c->profile!=((mode==2 || mode==7)?AIRPLAY_PROFILE_HAP:AIRPLAY_PROFILE_LEGACY))bad=1;
    if(new_clients==2 && c->profile!=AIRPLAY_PROFILE_HAP)bad=1;
    if((mode==1 || mode==3) && new_clients==1) {c->failed=1;c->fallback=mode==1;return -1;}
    return 0;
}
int airplay_client_tick(struct airplay_client*c) {
    ticks++;
    if(mode==4) {c->failed=c->fallback=1;return -1;}
    if(mode==5) {g_connect_stop=1;c->fallback=1;return -1;}
    return 0;
}
int airplay_client_failed(const struct airplay_client*c) {return c->failed;}
int airplay_client_wants_hap_fallback(const struct airplay_client*c) {return c->fallback;}
const char *airplay_client_failure(const struct airplay_client*c) {(void)c;return "test";}
int airplay_client_setup_complete(const struct airplay_client*c) {return !c->failed;}
uint16_t airplay_client_event_port(const struct airplay_client*c) {(void)c;return 7001;}
int airplay_client_attach_event(struct airplay_client*c,int fd) {(void)fd;c->event=1;return 0;}
int airplay_client_send_screen_setup(struct airplay_client*c) {
    if((mode==6 || mode==7) && !screen_requests++)return 1;
    if(!c->event || (c->profile==AIRPLAY_PROFILE_LEGACY && !c->record))bad=1;
    c->screen=1;screens++;return 0;
}
int airplay_client_screen_setup_complete(const struct airplay_client*c) {return c->screen;}
uint16_t airplay_client_screen_data_port(const struct airplay_client*c) {(void)c;return 7002;}
int airplay_client_attach_screen_data(struct airplay_client*c,int fd) {(void)fd;if(!c->screen)bad=1;c->attached=1;return 0;}
int airplay_client_start_audio(struct airplay_client*c,const char*h) {(void)c;(void)h;return -1;}
int airplay_client_send_record(struct airplay_client*c) {
    if(!c->event || c->record || (c->profile==AIRPLAY_PROFILE_HAP && !c->attached) ||
       (c->profile==AIRPLAY_PROFILE_LEGACY && c->screen))bad=1;
    c->record=1;records++;return 0;
}
int airplay_client_record_accepted(const struct airplay_client*c) {return c->record;}
int airplay_client_send_test_screen(struct airplay_client*c) {if(!c->record || !c->attached)bad=1;return 0;}
int airplay_client_start_live_screen(struct airplay_client*c) {(void)c;lives++;g_connect_stop=1;return 0;}
void airplay_client_feed(struct airplay_client*c,const uint8_t*p,size_t n) {(void)c;(void)p;(void)n;}
void airplay_client_feed_event(struct airplay_client*c,const uint8_t*p,size_t n) {(void)c;(void)p;(void)n;}
void airplay_client_free(struct airplay_client*c) {if(c) {if(g_active_airplay==c)bad=1;freed++;free(c);}}
#define close test_close
#define shutdown test_shutdown
#define poll test_poll
''' + bodies + r'''
int main(void) {
    for(mode=0;mode<8;mode++) {
        bad=new_clients=freed=connects=records=screens=screen_requests=lives=ticks=polls=0;
        g_connect_stop=0;g_connect_thread_running=1;g_active_airplay=NULL;
        struct ncm_connect_request*r=calloc(1,sizeof(*r));
        r->count=2;r->addresses=calloc(2,sizeof(char*));
        r->addresses[0]=strdup("169.254.0.2");r->addresses[1]=strdup("169.254.0.3");
        r->port=7000;r->profile=(mode==2 || mode==7)?AIRPLAY_PROFILE_HAP:AIRPLAY_PROFILE_LEGACY;
        r->media_audio=1;r->display_width=1024;
        g_connect_request=r;
        connect_endpoint_main(r);
        int expected=(mode==1 || mode==4)?2:1;
        int success=mode<=2 || mode==6 || mode==7;
        for(int i=100;i<512;i++)if(open_fds[i])bad=1;
        if(bad || new_clients!=expected || connects!=expected || freed!=expected ||
           (success && (records!=1 || screens!=1 || lives!=1)) || (!success && lives) ||
           (mode!=3 && !ticks) || g_connect_thread_running || g_active_airplay) {
            fprintf(stderr,"FAIL mode=%d bad=%d clients=%d connect=%d freed=%d records=%d screens=%d live=%d ticks=%d\n",
                    mode,bad,new_clients,connects,freed,records,screens,lives,ticks);return 1;
        }
    }
    puts("NCM profiles: legacy order, HAP order, one fresh fallback, refusal and stop cleanup passed");return 0;
}
'''
        result=run_c(code);self.assertEqual(result.returncode,0,result.stderr)


if __name__=='__main__': unittest.main()
