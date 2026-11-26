"""Execute production host I/O workers with real pthread teardown and usbfs fakes."""
from pathlib import Path
import os
import subprocess
import tempfile
import unittest
from iap2d_host_runtime_test import function

ROOT = Path(__file__).resolve().parents[1]


def run_c(code):
    if 'static void *writer_main(' in code:
        clock = function((ROOT / 'src/iap2d.c').read_text(), 'static uint64_t now_ms(')
        code = code.replace('static void *writer_main(', '#include <time.h>\n' + clock + '\nstatic void *writer_main(', 1)
    with tempfile.TemporaryDirectory() as tmp:
        c = Path(tmp) / 'test.c'; c.write_text(code)
        binary = Path(tmp) / 'test'
        subprocess.run(['cc', '-std=c11', '-Wall', '-Wextra', '-I', str(ROOT / 'src'),
                        str(c), '-pthread', '-o', str(binary)], check=True)
        return subprocess.run([str(binary)], capture_output=True, text=True, timeout=5)


class HostWorkersTest(unittest.TestCase):
    def test_native_host_waits_for_complete_frame_and_stops_on_short_write(self):
        source = (ROOT / 'src/iap2d.c').read_text()
        queue = source[source.index('struct txq {'):source.index('\n};', source.index('struct txq {'))+3]
        bodies = '\n'.join(function(source, signature) for signature in (
            'static void txq_push(', 'static void txq_set_host(', 'static void *writer_main('))
        code = r'''
#define _GNU_SOURCE
#include <pthread.h>
#include <stdatomic.h>
#include <stdint.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>
#include <stdio.h>
#include "usbhost.h"
''' + queue + r'''
static struct txq g_tx;
static int g_stop;
static atomic_int calls,ready,bad,g_host_failed,g_endpoint_round_stop;
static void iap2_log(const char*s,...) {(void)s;errno=EDOM;}
int usbhost_write(struct usbhost*h,const void*b,size_t n,int t) {
    (void)h;(void)b;(void)t;
    if(!atomic_load(&ready) || n!=6)atomic_store(&bad,1);
    atomic_fetch_add(&calls,1);
    return 2; /* Do not send the suffix as a separate transfer. */
}
''' + bodies + r'''
int main(void) {
    struct usbhost h={.fd=22};pthread_t writer;
    uint8_t marker[]={0xff,0x55,2,0,0xee,0x10};
    pthread_mutex_init(&g_tx.mu,NULL);pthread_cond_init(&g_tx.cv,NULL);
    g_tx.running=1;txq_set_host(&h);
    txq_push(marker,3);
    if(pthread_create(&writer,NULL,writer_main,NULL))return 1;
    usleep(30000);
    if(atomic_load(&calls))atomic_store(&bad,1);
    atomic_store(&ready,1);txq_push(marker+3,3);
    pthread_join(writer,NULL);
    if(atomic_load(&bad) || atomic_load(&calls)!=1 || atomic_load(&g_host_failed)!=EIO)return 2;
    /* A malformed internal header must fail without any USB submission. */
    atomic_store(&calls,0);atomic_store(&g_host_failed,0);
    txq_set_host(&h);marker[0]=0;
    txq_push(marker,6);writer_main(NULL);
    if(atomic_load(&calls) || atomic_load(&g_host_failed)!=EPROTO)return 3;
    return 0;
}
'''
        result = run_c(code)
        self.assertEqual(result.returncode, 0, result.stderr)

    def test_native_host_preserves_link_packet_boundaries(self):
        source = (ROOT / 'src/iap2d.c').read_text()
        queue = source[source.index('struct txq {'):source.index('\n};', source.index('struct txq {'))+3]
        bodies = '\n'.join(function(source, signature) for signature in (
            'static void txq_push(', 'static void txq_set_host(', 'static void *writer_main('))
        code = r'''
#define _GNU_SOURCE
#include <pthread.h>
#include <stdatomic.h>
#include <stdint.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>
#include <stdio.h>
#include "usbhost.h"
#include "iap2link.c"
''' + queue + r'''
static struct txq g_tx;
static int g_stop,calls,bad;
static atomic_int g_host_failed,g_endpoint_round_stop;
void iap2_log(const char*s,...) {(void)s;}
int usbhost_write(struct usbhost*h,const void*b,size_t n,int t) {
    (void)h;(void)t;
    const uint8_t *p=b;
    const size_t sizes[]={6,9,16,3010};
    if(calls>=4 || n!=sizes[calls])bad=1;
    if(calls && (p[0]!=0xff || p[1]!=0x5a || n!=((size_t)p[2]<<8 | p[3])))bad=1;
    if(++calls==4 || bad)g_stop=1;
    return (int)n;
}
''' + bodies + r'''
int main(void) {
    struct usbhost h={.fd=22};
    struct iap2_link l;
    uint8_t marker[]={0xff,0x55,2,0,0xee,0x10};
    uint8_t rx[]={0xff,0x5a,0,16,0x40,100,99,10,0,0x40,0x40,0,6,0x1d,1,0};
    uint8_t csm[]={0x40,0x40,0,6,0x1d,2},big[3000]={0},part[7];
    for(size_t i=0;i<8;i++)rx[8]-=rx[i];
    for(size_t i=9;i<15;i++)rx[15]-=rx[i];
    pthread_mutex_init(&g_tx.mu,NULL);pthread_cond_init(&g_tx.cv,NULL);
    g_tx.running=1;txq_set_host(&h);
    txq_push(marker,sizeof(marker));
    iap2_link_init(&l,0);l.state=IAP2_NORMAL;l.last_recv=99;l.peer_max_length=4096;
    iap2_link_feed(&l,rx,sizeof(rx),1); /* Queues ACK before the response. */
    if(iap2_link_send_control(&l,csm,sizeof(csm),1)<0)return 2;
    if(iap2_link_send_control(&l,big,sizeof(big),1)<0)return 3;
    size_t n;
    while((n=iap2_link_take(&l,part,sizeof(part))))txq_push(part,n);
    writer_main(NULL);
    if(bad || calls!=4 || g_tx.head!=g_tx.tail) {
        fprintf(stderr,"FAIL native host merged ACK/data or split a link frame: calls=%d bad=%d\n",calls,bad);
        return 1;
    }
    puts("native host: detect, ACK, CSM and 3010-byte frame use separate whole transfers");
    return 0;
}
'''
        result = run_c(code)
        self.assertEqual(result.returncode, 0, result.stderr)

    def test_queue_wrap_during_inflight_write_preserves_bytes(self):
        source = (ROOT / 'src/iap2d.c').read_text()
        queue = source[source.index('struct txq {'):source.index('\n};', source.index('struct txq {'))+3]
        push = function(source, 'static void txq_push(')
        writer = function(source, 'static void *writer_main(')
        code = r'''
#define _GNU_SOURCE
#include <pthread.h>
#include <stdatomic.h>
#include <stdint.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>
#include <stdio.h>
#include "usbhost.h"
''' + queue + r'''
static struct txq g_tx;
static int g_stop,calls,bad;
static atomic_int g_host_failed,g_endpoint_round_stop;
static void iap2_log(const char*s,...) {(void)s;}
''' + push + r'''
int usbhost_write(struct usbhost*h,const void*b,size_t n,int t) {
    (void)h;(void)t; calls++;
    if(calls==1) {
        if(n!=6 || memcmp(b,"abcdef",6))bad=1;
        txq_push((const uint8_t*)"ghij",4); /* producer runs while USB is in flight */
    } else {
        if(n!=4 || memcmp(b,"ghij",4))bad=1;
        g_stop=1;
    }
    return (int)n;
}
''' + writer + r'''
int main(void) {
    struct usbhost h={.fd=22};
    pthread_mutex_init(&g_tx.mu,NULL);pthread_cond_init(&g_tx.cv,NULL);
    g_tx.running=1;g_tx.fd=-1;g_tx.host=&h;
    g_tx.tail=sizeof(g_tx.buf);g_tx.head=g_tx.tail-6;
    memcpy(g_tx.buf+g_tx.head,"abcdef",6);
    writer_main(NULL);
    if(bad || calls!=2 || g_tx.head!=g_tx.tail) {
        fprintf(stderr,"FAIL queue wrap corrupted in-flight accounting head=%zu tail=%zu calls=%d\n",
                g_tx.head,g_tx.tail,calls); return 1;
    }
    /* A genuinely full stream is terminated, never silently overwritten. */
    for(int host_mode=0;host_mode<2;host_mode++) {
        g_tx.running=1;g_tx.head=0;g_tx.tail=sizeof(g_tx.buf);
        g_tx.host=host_mode?&h:NULL;g_tx.fd=host_mode?-1:22;
        g_tx.buf[0]=42;
        atomic_store(&g_host_failed,0);atomic_store(&g_endpoint_round_stop,0);
        txq_push((const uint8_t*)"z",1);
        if(g_tx.running || g_tx.head || g_tx.tail!=sizeof(g_tx.buf) || g_tx.buf[0]!=42)
            return 2;
        if(host_mode ? atomic_load(&g_host_failed)!=EOVERFLOW : !atomic_load(&g_endpoint_round_stop))
            return 3;
    }
    return 0;
}
'''
        result = run_c(code)
        self.assertEqual(result.returncode, 0, result.stderr)

    def test_failed_partial_output_is_never_replayed(self):
        source = Path(os.environ.get('IAP2D_SOURCE', ROOT / 'src/iap2d.c')).read_text()
        queue = source[source.index('struct txq {'):source.index('\n};', source.index('struct txq {'))+3]
        writer = function(source, 'static void *writer_main(')
        code = r'''
#define _GNU_SOURCE
#include <pthread.h>
#include <stdatomic.h>
#include <stdint.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>
#include <stdio.h>
#include "usbhost.h"
''' + queue + r'''
static struct txq g_tx;
static int g_stop,calls,bad,error_code;
static atomic_int g_host_failed;
static void iap2_log(const char*s,...) {(void)s;errno=EDOM;}
int usbhost_write(struct usbhost*h,const void*b,size_t n,int t) {
    (void)h;(void)t;calls++;
    if(calls==1) {if(n!=6 || memcmp(b,"abcdef",6))bad=1;return 2;}
    if(n!=4 || memcmp(b,"cdef",4))bad=1;
    if(calls>2)g_stop=1;
    errno=error_code;return -1;
}
''' + writer + r'''
int main(void) {
    const int errors[]={EPIPE,ETIMEDOUT,EINTR,EAGAIN,ENODEV,ESHUTDOWN};
    struct usbhost h={.fd=22};
    pthread_mutex_init(&g_tx.mu,NULL);pthread_cond_init(&g_tx.cv,NULL);
    for(size_t i=0;i<sizeof(errors)/sizeof(errors[0]);i++) {
        error_code=errors[i];calls=bad=g_stop=0;atomic_store(&g_host_failed,0);
        g_tx.head=0;g_tx.tail=6;g_tx.running=1;g_tx.fd=-1;g_tx.host=&h;
        memcpy(g_tx.buf,"abcdef",6);
        writer_main(NULL);
        if(calls!=2 || bad || atomic_load(&g_host_failed)!=error_code) {
            fprintf(stderr,"FAIL output errno=%d calls=%d failed=%d bad=%d\n",
                    error_code,calls,atomic_load(&g_host_failed),bad);return 1;
        }
    }
    pthread_cond_destroy(&g_tx.cv);pthread_mutex_destroy(&g_tx.mu);
    puts("host writer: short positive prefix advances; all uncertain errors end without replay");
    return 0;
}
'''
        try:
            result = run_c(code)
        except subprocess.TimeoutExpired:
            self.fail('host writer did not terminate after an uncertain failed output')
        self.assertEqual(result.returncode, 0, result.stderr)
        print(result.stdout, end='')

    def test_round_stop_joins_real_io_threads_and_preserves_process_stop(self):
        source = (ROOT / 'src/iap2d.c').read_text()
        queue = source[source.index('struct txq {'):source.index('\n};', source.index('struct txq {'))+3]
        bodies = '\n'.join(function(source, signature) for signature in (
            'static void txq_init(', 'static void txq_push(', 'static void txq_set_host(',
            'static void *writer_main(', 'static void *host_reader_main(',
            'static void host_round_stop(', 'static int host_round_start('))
        code = r'''
#define _GNU_SOURCE
#include <pthread.h>
#include <stdatomic.h>
#include <stdint.h>
#include <signal.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>
#include <stdio.h>
#include "usbhost.h"
''' + queue + r'''
static struct txq g_tx;
static volatile sig_atomic_t g_stop,g_reader_exited;
static atomic_int g_host_failed,g_host_round_stop,g_host_active,g_endpoint_round_stop;
static struct usbhost g_host;
static int g_tx_initialized,g_writer_started,g_reader_started,g_native_active,g_out_fd;
static pthread_t g_writer,g_reader;
static pthread_mutex_t g_link_mu=PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t g_link_cv=PTHREAD_COND_INITIALIZER;
static atomic_int reading,writing,read_started,write_started,bad;
static int bridges_closed;
static int g_vehicle_state;
static uint64_t g_session_generation;
static void carplay_vehicle_state_end_session(int*s,uint64_t n) {(void)s;(void)n;}
static void publish_control_status(const char*s,int n) {(void)s;(void)n;}
static void iap2_log(const char*s,...) {(void)s;}
static void dispatch_out_frame(const uint8_t*b,size_t n) {(void)b;(void)n;}
static void ncm_bridge_close(void) {
    if(atomic_load(&reading) || atomic_load(&writing))atomic_store(&bad,1);
    bridges_closed++;
}
int usbhost_read(struct usbhost*h,void*b,size_t n,int t) {
    (void)b;(void)n;(void)t;int fd=h->fd;
    atomic_store(&reading,1);atomic_store(&read_started,1);
    usleep(80000);
    if(h->fd!=fd)atomic_store(&bad,1);
    atomic_store(&reading,0);errno=ETIMEDOUT;return -1;
}
int usbhost_write(struct usbhost*h,const void*b,size_t n,int t) {
    (void)b;(void)t;int fd=h->fd;
    atomic_store(&writing,1);atomic_store(&write_started,1);
    usleep(100000);
    if(h->fd!=fd)atomic_store(&bad,1);
    atomic_store(&writing,0);return (int)n;
}
''' + bodies + r'''
int main(void) {
    uint8_t packet[]={0xff,0x55,2,0,0xee,0x10};
    for(int round=0;round<2;round++) {
        g_host.fd=100+round;g_stop=0;
        atomic_store(&read_started,0);atomic_store(&write_started,0);
        if(host_round_start())return 1;
        txq_set_host(&g_host);g_host_active=1;txq_push(packet,sizeof(packet));
        for(int waited=0;waited<1000;waited++) {
            if(atomic_load(&read_started) && atomic_load(&write_started))break;
            usleep(1000);
        }
        if(!atomic_load(&read_started) || !atomic_load(&write_started))return 2;
        if(round==1)g_stop=1;
        host_round_stop();
        if(g_stop!=round || g_writer_started || g_reader_started || !g_reader_exited ||
           atomic_load(&reading) || atomic_load(&writing) || atomic_load(&bad) ||
           g_host.fd!=100+round || bridges_closed!=round+1) {
            fprintf(stderr,"FAIL: round teardown reused a live handle or lost stop\n");return 3;
        }
        g_host.fd=-1;
    }
    puts("host workers: real reader/writer joined before handle reuse; external stop preserved");
    return 0;
}
'''
        result = run_c(code)
        self.assertEqual(result.returncode, 0, result.stderr)
        print(result.stdout, end='')


if __name__ == '__main__': unittest.main()
