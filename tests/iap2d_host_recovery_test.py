"""Execute host round/reacquisition control flow with usbfs/time boundaries fake."""
from pathlib import Path
import os
import subprocess
import tempfile
import unittest
from iap2d_host_runtime_test import function

ROOT = Path(__file__).resolve().parents[1]


class HostRecoveryTest(unittest.TestCase):
    def test_role_handoff_cannot_clear_an_external_stop(self):
        source = Path(os.environ.get('IAP2D_SOURCE', ROOT / 'src/iap2d.c')).read_text()
        if 'static int host_handoff_resume(' in source:
            resume = function(source, 'static int host_handoff_resume(')
        else:
            main = function(source, 'int main(')
            start = main.index('g_stop = 0;')
            resume = 'static int host_handoff_resume(void) {' + main[start:start+len('g_stop = 0;')] + 'return 1;}'
        code = '#include <signal.h>\n#include <stdio.h>\nstatic volatile sig_atomic_t g_stop,g_external_stop,g_reload_requested;\n' + resume + r'''
int main(void) {
    g_stop=1;g_external_stop=1;
    if(host_handoff_resume() || !g_stop) {fprintf(stderr,"FAIL: handoff cleared an external stop\n");return 1;}
    g_external_stop=0;g_reload_requested=1;
    if(host_handoff_resume() || !g_stop)return 2;
    g_reload_requested=0;
    if(!host_handoff_resume() || g_stop)return 3;
    return 0;
}
'''
        with tempfile.TemporaryDirectory() as tmp:
            c=Path(tmp)/'test.c'; c.write_text(code)
            binary=Path(tmp)/'test'
            subprocess.run(['cc','-std=c11',str(c),'-o',str(binary)],check=True)
            result=subprocess.run([str(binary)],capture_output=True,text=True)
            self.assertEqual(result.returncode,0,result.stderr)

    def test_reacquisition_stop_and_exhaustion(self):
        source = Path(os.environ.get('IAP2D_SOURCE', ROOT / 'src/iap2d.c')).read_text()
        helpers = '\n'.join(function(source, signature) for signature in (
            'static int host_link_timed_out(', 'static void host_round_stop(',
            'static int host_round_start(', 'static int host_disconnect_error(')
            if signature in source)
        runtime = function(source, 'static int run_host_mode(')
        macros = '\n'.join(line for line in source.splitlines() if
                           line.startswith('#define HOST_'))
        macros += '\n' + next(line for line in (ROOT / 'src/iap2session.h').read_text().splitlines()
                              if line.startswith('#define IAP2_SESSION_WINDOW_POLL_MS'))
        harness = r'''
#define _GNU_SOURCE
#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>
#include <stdatomic.h>
#include <signal.h>
#include <errno.h>
#include "usbhost.h"
#define ROLE_REQUEST_PATH "/unused"
#define BOX_ROLE_DEVICE 0
#define IAP2_NORMAL 1
#define IAP2_DEAD 2
struct carplay_config { int unused; };
struct link { int state; unsigned tx_frames,rx_frames; };
static struct link link, *g_link=&link;
static struct session { int authenticated,authentication_reported,authentication_completed_once; } session,*g_session=&session;
static int mux, lockdown, *g_mux=&mux,*g_lockdown=&lockdown;
static int g_link_mu,g_link_cv,g_native_active,g_out_fd,g_mux_selected,g_native_prefix_pending;
static volatile sig_atomic_t g_stop,g_reader_exited,g_role_handoff;
static atomic_int g_host_failed,g_host_round_stop,g_host_active;
static int g_writer_started,g_reader_started,g_tx_initialized;
static unsigned long g_writer,g_reader;
static void *g_tls;
static uint64_t g_role_t0,clock_ms;
static int g_vehicle_state;
static uint64_t g_session_generation;
static void carplay_vehicle_state_end_session(int*s,uint64_t n) {(void)s;(void)n;}
static struct usbhost g_host;
static struct {int mu,cv,running,fd;size_t head,tail;struct usbhost *host;} g_tx;
static int mode,claims,opens,closes,live,requests,full_stops,broken,bridge_closes,failed_once;
static uint64_t down_at;
static int fault(void) { return mode==6 ? EIO : mode==7 || mode==14 ? EPIPE : mode==8 ? ENXIO : mode==9 ? ESHUTDOWN : mode==13 ? EPROTO : ENODEV; }
static uint64_t now_ms(void) {return clock_ms;}
static void advance(unsigned ms) {
    clock_ms+=ms;
    if (g_host_active && !atomic_load(&g_host_failed)) {
        if (mode==1 && claims==2) g_stop=1;
        else if (mode==4) g_stop=1;
        else {atomic_store(&g_host_failed,fault());down_at=clock_ms;}
    }
    if(mode==3 && claims==1 && !g_host_active) g_stop=1;
    if(clock_ms>30000) {broken=1;g_stop=1;}
}
static void usleep(unsigned us) {advance(us/1000);}
static void device_wait_ms(unsigned ms) {advance(ms);}
static void logmsg(const char*s,...) {(void)s;}
static void iap2_log(const char*s,...) {(void)s;}
static void pthread_mutex_lock(int*p) {(void)p;}
static void pthread_mutex_unlock(int*p) {(void)p;}
static void pthread_cond_broadcast(int*p) {(void)p;}
static int pthread_create(unsigned long*t,void*a,void*(*fn)(void*),void*b) {
    (void)a;(void)fn;(void)b;
    if(mode==10 && live==1 && !failed_once++) return EAGAIN;
    *t=(unsigned long)++live; return 0;
}
static int pthread_join(unsigned long t,void*p) {(void)t;(void)p;live--;return 0;}
static void *writer_main(void*p) {return p;}
static void *host_reader_main(void*p) {return p;}
static void txq_init(void) {g_tx_initialized=1;g_tx.running=1;g_tx.host=NULL;g_tx.fd=-1;}
static void txq_set_host(struct usbhost*h) {g_tx.host=h;}
static void *lockdown_tls_create(void) {return &g_host;}
static void lockdown_tls_destroy(void*p) {(void)p;}
static void stop_runtime(int ep,int final) {(void)ep;(void)final;g_stop=1;live=0;g_writer_started=g_reader_started=0;full_stops++;}
static void ncm_bridge_close(void) {bridge_closes++;}
static void ncm_bridge_stop(void) {bridge_closes++;}
static int ncm_bridge_host_authentication(int ready) {(void)ready;return 0;}
static int ncm_bridge_car_netdev_up(const char*s,unsigned ms) {(void)s;(void)ms;return 0;}
static int box_role_request_write(const char*p,int r) {(void)p;(void)r;requests++;if(g_stop)broken=1;return 0;}
int usbhost_open(struct usbhost*h,uint16_t v,uint16_t p) {
    (void)v;(void)p; opens++;
    if(claims && (live || h->fd>=0)) broken=1;
    if(mode==2 && claims==1) return -1;
    if(mode==3 && claims==1) {g_stop=1;return -1;}
    if(mode==11) {g_stop=1;return -1;}
    if(mode==12 && claims==1) clock_ms+=2100;
    if(mode==1 && claims==1 && clock_ms-down_at<150) return -1;
    claims++;h->fd=100+claims;strcpy(h->sysfs_name,"7-1");return 0;
}
void usbhost_close(struct usbhost*h) {if(live)broken=1;if(h->fd>=0)closes++;h->fd=-1;}
static void host_bus_report(const char*s) {(void)s;}
static void role_ts(const char*s) {(void)s;}
static int role_swap_stamp_take(uint64_t*p) {(void)p;return -1;}
static void iap2_link_init(struct link*l,uint64_t t) {(void)t;l->state=IAP2_NORMAL;}
static void iap2_session_init(struct session*p) {memset(p,0,sizeof(*p));}
static void begin_vehicle_session(void) {}
static void usbmux_init(int*p) {(void)p;}
static void lockdown_init(int*p) {(void)p;}
static void publish_control_status(const char*s,int n) {(void)s;(void)n;}
static void iap2_link_start(struct link*l,uint64_t t) {(void)l;(void)t;}
static void iap2_link_tick(struct link*l,uint64_t t) {(void)l;(void)t;}
static void pump_locked(void) {if(mode>=13)session.authenticated=session.authentication_reported=session.authentication_completed_once=1;}
''' + macros + '\n' + helpers + '\n' + runtime + r'''
int main(void) {
    struct carplay_config config={0};
    for(mode=1;mode<=14;mode++) {
        claims=opens=closes=live=requests=full_stops=broken=bridge_closes=failed_once=0;
        g_stop=g_host_active=g_role_handoff=g_writer_started=g_reader_started=0;
        g_host.fd=-1;clock_ms=1;g_tls=NULL;
        atomic_store(&g_host_failed,0);atomic_store(&g_host_round_stop,0);
        run_host_mode(&config,0);
        int expected_claims=(mode==1 || mode==12 ? 2 : mode==5 || mode==8 || mode==9 || mode>=13 ? 4 : mode==11 ? 0 : 1);
        int expected_requests=(mode==1 || mode==3 || mode==4 || mode==11) ? 0 : 1;
        if(claims!=expected_claims || requests!=expected_requests || live || broken || full_stops || closes!=claims) {
            fprintf(stderr,"FAIL mode %d: claims=%d expected=%d requests=%d expected=%d live=%d broken=%d full_stop=%d closes=%d\n",
                    mode,claims,expected_claims,requests,expected_requests,live,broken,full_stops,closes);return 1;
        }
        if(mode==2 && (clock_ms-down_at<1900 || clock_ms-down_at>2500)) {
            fprintf(stderr,"FAIL: reacquisition must have a short monotonic grace (elapsed=%llu)\n",(unsigned long long)(clock_ms-down_at));return 2;
        }
        if((mode==1 || mode==3 || mode==4 || mode==11) && !g_stop) {
            fprintf(stderr,"FAIL: external stop was lost in mode %d\n",mode);return 3;
        }
    }
    puts("host recovery: reconnect, deadline, 3-round ceiling, SIGTERM, fatal error, worker failure all passed");
    return 0;
}
'''
        with tempfile.TemporaryDirectory() as tmp:
            c = Path(tmp) / 'test.c'; c.write_text(harness)
            binary = Path(tmp) / 'test'
            subprocess.run(['cc', '-std=c11', '-I', str(ROOT / 'src'), str(c), '-o', str(binary)], check=True)
            result = subprocess.run([str(binary)], capture_output=True, text=True, timeout=5)
            self.assertEqual(result.returncode, 0, result.stderr)
            print(result.stdout, end='')


if __name__ == '__main__': unittest.main()
