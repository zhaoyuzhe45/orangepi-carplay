"""Production NCM connector behavior with deterministic OS/AirPlay boundaries."""
from pathlib import Path
import subprocess
import tempfile
import unittest
from iap2d_host_runtime_test import function as _function

ROOT = Path(__file__).resolve().parents[1]


def function(source, signature):
    start=source.index(signature)
    while source.index(';',start)<source.index('{',start):
        start=source.index(signature,source.index(';',start)+1)
    return _function(source[start:],signature)


def run_c(code):
    with tempfile.TemporaryDirectory() as tmp:
        source=Path(tmp)/'test.c';source.write_text(code)
        binary=Path(tmp)/'test'
        subprocess.run(['cc','-std=c11','-I',str(ROOT/'src'),str(source),'-pthread','-o',str(binary)],check=True)
        return subprocess.run([str(binary)],capture_output=True,text=True,timeout=8)


class NcmCompatTest(unittest.TestCase):
    def test_scoped_ipv6_retains_selected_ncm_interface(self):
        source=(ROOT/'src/ncm_bridge.c').read_text()
        helpers='\n'.join(function(source,s) for s in (
            'static int numeric_address(', 'static int address_is_link_local(', 'static int connect_one(')
            if s in source)
        restricted=', 1' if 'int restrict_to_ncm)' in helpers else ''
        code=r'''
#define _GNU_SOURCE
#include <arpa/inet.h>
#include <stdint.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <net/if.h>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>
#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#define iap2_log(...) ((void)0)
static int g_tun_index=7,binds,device_binds,bad;
static int ensure_airplay_policy_routes(const char*s) {(void)s;return 0;}
static int ipv6_source_state(int index) {return index==7;}
static int connect_wait(int fd,int ms) {(void)fd;(void)ms;return 0;}
static int test_socket(int af,int type,int protocol) {(void)af;(void)type;(void)protocol;return 30;}
static int test_fcntl(int fd,int op,...) {(void)fd;(void)op;return 0;}
static unsigned test_if_nametoindex(const char*s) {return strcmp(s,"usb7")==0?7:strcmp(s,"usb8")==0?8:0;}
static char *test_if_indextoname(unsigned n,char*s) {if(n!=7)return NULL;strcpy(s,"usb7");return s;}
static int test_setsockopt(int fd,int level,int option,const void*value,socklen_t size) {
    (void)fd;(void)level;(void)size;
    if(option==SO_BINDTODEVICE) {device_binds++;if(strcmp(value,"usb7"))bad=1;}
    return 0;
}
static int test_bind(int fd,const struct sockaddr*s,socklen_t len) {
    (void)fd;(void)len;binds++;
    if(s->sa_family==AF_INET6 && ((const struct sockaddr_in6*)s)->sin6_scope_id!=7)bad=1;
    return 0;
}
static int test_connect(int fd,const struct sockaddr*s,socklen_t len) {
    (void)fd;(void)len;
    if(s->sa_family==AF_INET6 && ((const struct sockaddr_in6*)s)->sin6_scope_id!=7)bad=1;
    return 0;
}
static int test_close(int fd) {(void)fd;return 0;}
#define socket test_socket
#define fcntl test_fcntl
#define if_nametoindex test_if_nametoindex
#define if_indextoname test_if_indextoname
#define setsockopt test_setsockopt
#define bind test_bind
#define connect test_connect
#define close test_close
''' + helpers + r'''
int main(void) {
    char resolved[96];
    int fd=connect_one("fe80::1234%usb7",7000,0,resolved,sizeof(resolved),1''' + restricted + r''');
    if(fd!=30 || bad || binds!=1 || device_binds!=1 || strcmp(resolved,"[fe80::1234%usb7]")) {
        fprintf(stderr,"FAIL scoped IPv6: fd=%d bad=%d binds=%d devbinds=%d resolved=%s\n",fd,bad,binds,device_binds,resolved);return 1;
    }
    binds=device_binds=bad=0;
    if(connect_one("fe80::1234%usb8",7000,0,resolved,sizeof(resolved),1''' + restricted + r''')>=0) {
        fprintf(stderr,"FAIL: another interface's zone must not be used\n");return 2;
    }
    if(connect_one("192.0.2.2",7000,1,resolved,sizeof(resolved),1''' + restricted + r''')!=30 ||
       binds || device_binds!=1) {
        fprintf(stderr,"FAIL: discovered routable addresses must still bind the NCM interface\n");return 3;
    }
    puts("NCM scoped IPv6 preserves interface and rejects other zones");return 0;
}
'''
        result=run_c(code);self.assertEqual(result.returncode,0,result.stderr)

    def test_bench_profile_override_does_not_change_iap2(self):
        source=(ROOT/'src/ncm_bridge.c').read_text()
        code=r'''
#define _GNU_SOURCE
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "airplay_client.h"
#define iap2_log(...) ((void)0)
static pthread_mutex_t g_mu=PTHREAD_MUTEX_INITIALIZER;
static int g_connect_thread_running,g_running,g_connect_stop,g_connect_sock;
static int queued,chosen;
static int queue_connect_endpoint(const char *const *a,size_t n,uint16_t p,enum airplay_profile profile,int discovered) {
    (void)a;(void)n;(void)p;if(discovered)return -1;queued++;chosen=profile;return 0;
}
''' + function(source,'int ncm_bridge_connect_endpoint(') + function(source,'int ncm_bridge_bench_start(') + r'''
int main(void) {
    const char *address="127.0.0.1";
    setenv("CARPLAY_AIRPLAY_PROFILE","legacy",1);
    if(ncm_bridge_bench_start(address,7000) || chosen!=AIRPLAY_PROFILE_LEGACY)return 1;
    if(ncm_bridge_connect_endpoint(&address,1,7000) || chosen!=AIRPLAY_PROFILE_HAP)return 2;
    unsetenv("CARPLAY_AIRPLAY_PROFILE");
    if(ncm_bridge_bench_start(address,7000) || chosen!=AIRPLAY_PROFILE_HAP)return 3;
    g_connect_thread_running=1;
    if(ncm_bridge_bench_start(address,7000)!=-1 || queued!=3)return 4;
    return 0;
}
'''
        result=run_c(code);self.assertEqual(result.returncode,0,result.stderr)

    def test_discovery_shares_connector_and_stops_before_join(self):
        source=(ROOT/'src/ncm_bridge.c').read_text()
        request=source[source.index('struct ncm_connect_owner {'):source.index('\n};',source.index('struct ncm_connect_request {'))+3]
        bodies='\n'.join(function(source,s) for s in (
            'static int numeric_address(', 'static int transport_ready_locked(',
            'static int connect_ready_locked(', 'static int connect_owned_locked(',
            'static void free_connect_request(', 'static int same_connect_endpoint(', 'static int queue_connect_endpoint(',
            'int ncm_bridge_host_authentication(',
            'int ncm_bridge_connect_endpoint(', 'static void host_mdns_found(',
            'static void host_mdns_start_locked(', 'int ncm_bridge_car_netdev_up(',
            'void ncm_bridge_stop(', 'void ncm_bridge_close(') if s in source)
        code=r'''
#define _GNU_SOURCE
#include <pthread.h>
#include <stdatomic.h>
#include <arpa/inet.h>
#include <stdint.h>
#include <net/if.h>
#include <sys/socket.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include "airplay_client.h"
#include "carplay_mdns.h"
#define iap2_log(...) ((void)0)
''' + request + r'''
static pthread_mutex_t g_mu=PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t g_connect_cv=PTHREAD_COND_INITIALIZER;
static int g_running,g_kernel_ncm,g_host_netdev_ready,g_tun_index,g_tun_fd=-1;
static int g_host_auth_required,g_host_authenticated;
static int g_peer_mac_valid,g_peer_ipv6_valid;
static atomic_int g_connect_stop;
static int g_connect_thread_running,g_connect_sock=-1,g_airplay_transport_restart_pending;
static int g_transport_restarting;
static unsigned long g_transport_restart_epoch, g_usb_generation;
static int g_connect_retiring;
static int g_airplay_media_enabled=1,g_airplay_microphone_enabled=0;
static int g_airplay_display_width=1024,g_airplay_display_height=600,g_airplay_physical_width=240;
static int g_airplay_physical_height=120,g_airplay_display_density=160,g_airplay_video_fps=30,g_airplay_video_bitrate=8000000;
static struct carplay_vehicle_state *g_vehicle_state;
static uint64_t g_vehicle_generation;
static struct ncm_connect_request *g_pending_connect_request, *g_connect_request;
static int g_connect_retrying;
static struct carplay_mdns *g_mdns;
static int g_mdns_enabled;
static void (*found_callback)(void*,const char*,uint16_t);
static void *found_ctx;
static int starts,stops,launches,bad,last_profile;
static struct ncm_connect_request *active;
static int car_netdev_name(const char*d,char*n,size_t z) {(void)d;snprintf(n,z,"usb7");return 0;}
static int netdev_configure(const char*n) {(void)n;g_tun_index=7;return 0;}
static int launch_connect_request_locked(struct ncm_connect_request*r) {
    launches++;active=g_connect_request=r;last_profile=r->profile;g_connect_thread_running=1;return 0;
}
static void usb_workers_stop(void) {}
static int fd_is_open(int fd) {return fd>=0;}
int carplay_mdns_start(struct carplay_mdns **out,const char*n,void(*cb)(void*,const char*,uint16_t),void*ctx) {
    if(strcmp(n,"usb7"))bad=1;
    starts++;*out=(struct carplay_mdns*)&starts;found_callback=cb;found_ctx=ctx;return 0;
}
void carplay_mdns_stop(struct carplay_mdns*m) {
    if(!m)return;
    stops++;
    if(pthread_mutex_trylock(&g_mu)!=0) {bad=1;return;}
    if(g_mdns_enabled)bad=1;
    pthread_mutex_unlock(&g_mu);
    int before=launches;
    found_callback(found_ctx,"169.254.0.2",7000);
    if(launches!=before || g_pending_connect_request)bad=1;
    g_connect_thread_running=0;
}
''' + bodies + r'''
int main(void) {
    const char *address="169.254.0.2";
    if(ncm_bridge_connect_endpoint(&address,1,7000) || !g_pending_connect_request)return 1;
    if(ncm_bridge_car_netdev_up("7-1",0) || starts!=1 || launches || !g_pending_connect_request) {
        fprintf(stderr,"FAIL: Host readiness must discover but defer pending 4301 until auth\n");return 2;
    }
    found_callback(found_ctx,address,7000);
    if(g_pending_connect_request->profile!=AIRPLAY_PROFILE_HAP || launches)return 7;
    ncm_bridge_host_authentication(1);
    if(launches!=1 || last_profile!=AIRPLAY_PROFILE_HAP)return 8;
    found_callback(found_ctx,address,7000);
    if(launches!=1 || g_pending_connect_request)return 3;
    if(ncm_bridge_car_netdev_up("7-1",0) || starts!=1)return 4;
    ncm_bridge_close();free_connect_request(active);active=NULL;
    if(stops!=1 || bad || g_tun_index || g_mdns_enabled || g_mdns ||
       g_airplay_display_width!=1024 || g_airplay_media_enabled!=1 || g_airplay_microphone_enabled!=0) {
        fprintf(stderr,"FAIL: discovery must stop without held lock before connector join and preserve options\n");return 5;
    }
    ncm_bridge_host_authentication(0);
    if(ncm_bridge_car_netdev_up("7-1",0))return 9;
    found_callback(found_ctx,address,7000);
    if(!g_pending_connect_request || launches!=1 || g_pending_connect_request->profile!=AIRPLAY_PROFILE_LEGACY)return 10;
    ncm_bridge_host_authentication(0);
    if(!g_pending_connect_request)return 11;
    ncm_bridge_host_authentication(1);
    if(launches!=2 || last_profile!=AIRPLAY_PROFILE_LEGACY || g_pending_connect_request)return 12;
    if(!ncm_bridge_host_authentication(0))return 13;
    found_callback(found_ctx,address,7000);
    if(launches!=2 || g_pending_connect_request)return 14;
    ncm_bridge_close();free_connect_request(active);active=NULL;
    g_connect_stop=0;g_mdns_enabled=1;
    ncm_bridge_connect_endpoint(&address,1,7000);
    struct ncm_connect_request *pending=g_pending_connect_request;
    found_callback(found_ctx,address,7000);
    if(g_pending_connect_request!=pending || pending->profile!=AIRPLAY_PROFILE_HAP)return 6;
    free_connect_request(pending);g_pending_connect_request=NULL;
    puts("NCM discovery shares pending/active connector; stop disables callback and joins without mutex");return 0;
}
'''
        result=run_c(code);self.assertEqual(result.returncode,0,result.stderr)


if __name__=='__main__': unittest.main()
