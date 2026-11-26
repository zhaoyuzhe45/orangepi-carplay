"""Execute production daemon functions with transport/system boundaries stubbed."""
from pathlib import Path
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]


def function(source, signature):
    start = source.index(signature)
    begin = source.index('{', start)
    depth = 1
    end = begin + 1
    while depth:
        if source[end] == '{': depth += 1
        if source[end] == '}': depth -= 1
        end += 1
    return source[start:end]


class HostRuntimeTest(unittest.TestCase):
    def test_bus_report_syncs_empty_and_error_evidence(self):
        source = (ROOT / 'src/iap2d.c').read_text()
        report = function(source, 'static void host_bus_report(')
        harness = r'''
#define _GNU_SOURCE
#include <stdio.h>
#include <string.h>
#include <stdarg.h>
static int buses, devices, syncs;
static char message[4096];
static int usbhost_role_bus_count(void) { return buses; }
static int usbhost_snapshot(char *buf, size_t len) {
    snprintf(buf, len, "%s", devices > 0 ? "7-1 05ac:12a8 480Mbps\n" : "");
    return devices;
}
static void carplay_log_sync(void) { syncs++; }
static void logmsg(const char *fmt, ...) {
    va_list ap;
    size_t used = strlen(message);
    va_start(ap, fmt);
    vsnprintf(message + used, sizeof(message) - used, fmt, ap);
    va_end(ap);
}
''' + report + r'''
int main(void) {
    const int counts[][2] = {{0,0}, {2,0}, {-1,0}, {2,-1}, {2,1}};
    const char *expected[] = {"no xhci root bus", "no downstream devices enumerated",
        "sysfs unreadable", "sysfs unreadable", "1 device(s) visible"};
    for (int i = 0; i < 5; i++) {
        buses = counts[i][0]; devices = counts[i][1]; syncs = 0; message[0] = 0;
        host_bus_report("give-up");
        if (syncs != 1 || !strstr(message, expected[i])) {
            fprintf(stderr, "FAIL: bus report case %d must classify and sync evidence (syncs=%d)\n", i, syncs);
            return 1;
        }
    }
    return 0;
}
'''
        with tempfile.TemporaryDirectory() as tmp:
            c = Path(tmp) / 'test.c'
            c.write_text(harness)
            binary = Path(tmp) / 'test'
            subprocess.run(['cc', '-std=c11', str(c), '-o', str(binary)], check=True)
            result = subprocess.run([str(binary)], capture_output=True, text=True)
            self.assertEqual(result.returncode, 0, result.stderr)

    def test_negotiation_deadline_does_not_expire_completed_link(self):
        source=(ROOT/'src/iap2d.c').read_text()
        helper=function(source,'static int host_link_timed_out(')
        csource='#include <stdint.h>\n'+helper+'''
int main(void) {
    return host_link_timed_out(15000,14999) ||
           !host_link_timed_out(15000,15000) ||
           host_link_timed_out(0,90000);
}
'''
        with tempfile.TemporaryDirectory() as tmp:
            c=Path(tmp)/'test.c'; c.write_text(csource)
            binary=Path(tmp)/'test'
            subprocess.run(['cc','-std=c11',str(c),'-o',str(binary)],check=True)
            self.assertEqual(subprocess.run([str(binary)]).returncode,0)

    def test_role_switch_is_not_attempted_after_failed_ack(self):
        source=(ROOT/'src/iap2d.c').read_text()
        body=function(source,'static void handle_role_swap(')
        harness=r'''
#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
#include <stdarg.h>
#define BOX_ROLE_HOST 1
#define ROLE_SWITCH_MAX_ATTEMPTS 3
#define ROLE_REQUEST_PATH "/unused"
struct usb_ctrlrequest { uint8_t bRequestType,bRequest; uint16_t wValue,wIndex,wLength; };
static int g_switch_pending=-1,g_stop;
static int ack_result=-1, request_result, intents, order_failed, wakeups;
static uint64_t clock_ms=1;
static uint64_t g_role_t0;
static int box_role_from_request(int a,int b,int c) {(void)a;(void)b;(void)c;return 1;}
static const char *box_role_name(int r) {(void)r;return "host";}
static int consume_control_out(int a,int b) {(void)a;(void)b;return ack_result;}
static int box_role_request_write(const char*p,int r) {(void)p;(void)r;intents++;return request_result;}
static void iap2_log(const char*f,...) {(void)f;}
static const char *udc_state(void) {return "configured";}
static void carplay_log_sync(void) {clock_ms+=40;}
static int role_switch_armed(int a,int b) {(void)a;(void)b;return 1;}
static int role_switch_failures(void) {return 0;}
static uint64_t now_ms(void) {return clock_ms;}
static void wake_device_loop(void) {wakeups++;if(!g_stop || !intents) order_failed=1;}
static void role_swap_stamp_write(uint64_t t) {(void)t;if(!intents) order_failed=1;}
static void role_ts(const char*s) {(void)s;}
'''+body+r'''
int main(void) {
    struct usb_ctrlrequest q={0x40,0x51,1,0,0};
    handle_role_swap(0,&q);
    if(g_switch_pending!=-1 || g_stop || intents) return 1;
    ack_result=0;request_result=-1;
    handle_role_swap(0,&q);
    if(g_switch_pending!=-1 || g_stop) {
        fprintf(stderr,"FAIL: cannot unbind before notifying supervisor of host intent\n");return 2;
    }
    intents=0;request_result=0;clock_ms=1;
    handle_role_swap(0,&q);
    if(g_switch_pending!=BOX_ROLE_HOST || !g_stop || !intents || order_failed ||
       wakeups!=1 || g_role_t0!=1) {
        fprintf(stderr,"FAIL: accepted handoff must announce intent before teardown\n");return 3;
    }
    return 0;
}
'''
        with tempfile.TemporaryDirectory() as tmp:
            c=Path(tmp)/'test.c';c.write_text(harness)
            binary=Path(tmp)/'test'
            subprocess.run(['cc','-std=c11',str(c),'-o',str(binary)],check=True)
            result=subprocess.run([str(binary)])
            self.assertEqual(result.returncode,0)

    def test_failed_host_read_ends_round_instead_of_spinning(self):
        source = (ROOT / 'src/iap2d.c').read_text()
        reader = function(source, 'static void *host_reader_main(')
        harness = r'''
#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
#include <errno.h>
#include <string.h>
#include <stdatomic.h>
static int g_stop, g_host_active=1, g_host, g_reader_exited, calls;
static atomic_int g_host_failed, g_host_round_stop;
static void usleep(int n) {(void)n; if (++calls>3) g_stop=1;}
static int usbhost_read(int *h,void *b,size_t n,int t) {(void)h;(void)b;(void)n;(void)t;errno=ENODEV;return -1;}
static void iap2_log(const char *s,...) {(void)s;}
static void dispatch_out_frame(const uint8_t*b,size_t n) {(void)b;(void)n;}
'''
        harness += reader + r'''
int main(void) {
    host_reader_main(NULL);
    if (!atomic_load(&g_host_failed) || !g_reader_exited || calls>1) {
        fprintf(stderr,"FAIL: disconnected device must end host round, not spin on old fd\n");return 1;
    }
    return 0;
}
'''
        with tempfile.TemporaryDirectory() as tmp:
            c=Path(tmp)/'test.c'; c.write_text(harness)
            binary=Path(tmp)/'test'
            subprocess.run(['cc','-std=c11',str(c),'-o',str(binary)],check=True)
            result=subprocess.run([str(binary)],capture_output=True,text=True)
            self.assertEqual(result.returncode,0,result.stderr)

    def test_dispatch_and_host_start(self):
        source = (ROOT / 'src/iap2d.c').read_text()
        classifier = function(source, 'static int frame_is_iap2(')
        dispatch = function(source, 'static void dispatch_out_frame(')
        host = function(source, 'static int run_host_mode(')
        start = host.index('iap2_link_init(g_link, now_ms());')
        end = host.index('pthread_mutex_unlock(&g_link_mu);', start) + len('pthread_mutex_unlock(&g_link_mu);')
        startup = host[start:end]
        # Preserve production magic constants; the original confused CSM and LSP.
        macros = '\n'.join(line for line in source.splitlines()
                           if line.startswith('#define IAP2_FRAME_MAGIC') or line.startswith('#define USBMUX_FRAME_MAGIC'))
        harness = r'''
#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>
#include <stdatomic.h>
static int g_native_active, g_native_in_fd = -1, g_host_active, g_stop;
static int g_mux_selected, g_native_prefix_pending;
static atomic_int g_host_failed;
static int g_link_mu, link, session, mux, g_host;
static struct {int car_gps;} g_config;
static int g_vehicle_state;
static uint64_t g_session_generation;
static void iap2_session_set_vehicle_state(int*s,int*v,uint64_t n) {(void)s;(void)v;(void)n;}
static int *g_link=&link, *g_session=&session, *g_mux=&mux;
static int selected_host, starts, native_feeds, mux_feeds, pumps;
static uint64_t now_ms(void) { return 1; }
static void pthread_mutex_lock(int *p) {(void)p;}
static void pthread_mutex_unlock(int *p) {(void)p;}
static void txq_set_fd(int fd) {(void)fd; selected_host=0;}
static void txq_set_host(int *h) {(void)h; selected_host=1;}
static void iap2_link_init(int *l,uint64_t t) {(void)l;(void)t;}
static void iap2_session_init(int *s) {(void)s;}
static void iap2_link_start(int *l,uint64_t t) {(void)l;(void)t;starts++;}
static void iap2_link_feed(int *l,const uint8_t *b,size_t n,uint64_t t) {(void)l;(void)b;(void)n;(void)t;native_feeds++;}
static void usbmux_feed(int *l,const uint8_t *b,size_t n) {(void)l;(void)b;(void)n;mux_feeds++;}
static void usbmux_init(int *p) {(void)p;}
static void lockdown_init(int *p) {(void)p;}
static int lockdown, *g_lockdown=&lockdown;
static void begin_vehicle_session(void) {}
static void publish_control_status(const char *s,int n) {(void)s;(void)n;}
static void pthread_cond_broadcast(int *p) {(void)p;}
static int g_link_cv;
static void pump_locked(void) {pumps++;}
static void iap2_log(const char *fmt,...) {(void)fmt;}
static void log_bytes(const char *s,const uint8_t*b,size_t n) {(void)s;(void)b;(void)n;}
'''
        harness += macros + '\n' + classifier + '\n' + dispatch
        harness += '\nstatic void startup(void) {\nuint64_t link_deadline;\n' + startup + '\n(void)link_deadline;\n}\n'
        harness += r'''
int main(void) {
    int failed=0;
    uint8_t marker[]={0xff,0x55,0x02,0x00,0xee,0x10};
    uint8_t frame[]={0xff,0x5a,0,9,0x40,100,99,10,0};
    uint8_t usbmux[]={0,0,0,16,0,0,0,1};
    g_native_active=0;
    if (!frame_is_iap2(marker,sizeof(marker)) || !frame_is_iap2(frame,sizeof(frame))) {
        fprintf(stderr,"FAIL: native marker/link frame misrouted as USBMUX\n");failed++;
    }
    if (frame_is_iap2(usbmux,sizeof(usbmux))) {failed++;}
    g_mux_selected=1;
    if(frame_is_iap2(frame,sizeof(frame))) {fprintf(stderr,"FAIL: mux continuation changed transport\n");failed++;}
    g_mux_selected=0;
    g_host_active=1;selected_host=1;g_native_active=0;
    dispatch_out_frame(frame,sizeof(frame));
    if (native_feeds!=1 || mux_feeds || !selected_host) {
        fprintf(stderr,"FAIL: native dispatch must preserve host bulk transport\n");failed++;
    }
    g_native_active=0;starts=0;selected_host=0;pumps=0;
    startup();
    if (!g_native_active || !selected_host || !starts || !pumps) {
        fprintf(stderr,"FAIL: host must initiate bare iAP2 after claim\n");failed++;
    }
    { uint8_t continuation[]={0xfe,0xed,0xff,0x5a};
      if (!frame_is_iap2(continuation,sizeof(continuation))) {
        fprintf(stderr,"FAIL: native continuation must stay on selected transport\n");failed++;
      }
    }
    g_stop=1; native_feeds=0; mux_feeds=0;
    dispatch_out_frame(frame,sizeof(frame));
    if (native_feeds || mux_feeds) {fprintf(stderr,"FAIL: stopped runtime processed a late packet\n");failed++;}
    g_stop=0;g_native_active=0;g_mux_selected=0;g_native_prefix_pending=0;native_feeds=0;mux_feeds=0;
    dispatch_out_frame(marker,1);
    dispatch_out_frame(marker+1,sizeof(marker)-1);
    if(!g_native_active || !native_feeds || mux_feeds) {fprintf(stderr,"FAIL: split marker lost protocol selection\n");failed++;}
    return failed?1:0;
}
'''
        with tempfile.TemporaryDirectory() as tmp:
            c = Path(tmp) / 'test.c'
            c.write_text(harness)
            binary = Path(tmp) / 'test'
            subprocess.run(['cc', '-std=c11', str(c), '-o', str(binary)], check=True)
            result = subprocess.run([str(binary)], capture_output=True, text=True)
            self.assertEqual(result.returncode, 0, result.stderr)


if __name__ == '__main__': unittest.main()
