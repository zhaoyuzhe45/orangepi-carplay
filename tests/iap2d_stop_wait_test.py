"""Real poll/eventfd/thread checks for bounded device-loop shutdown latency."""
from pathlib import Path
import os
import subprocess
import tempfile
import unittest
from iap2d_host_runtime_test import function

ROOT = Path(__file__).resolve().parents[1]


class StopWaitTest(unittest.TestCase):
    def test_stop_wakes_wait_without_changing_normal_interval(self):
        source=Path(os.environ.get('IAP2D_SOURCE', ROOT/'src/iap2d.c')).read_text()
        # Before the change, the production waits are fixed usleep calls.
        if 'static void device_wait_ms(' in source:
            wait=function(source,'static void device_wait_ms(')
            wake=function(source,'static void wake_device_loop(')
        else:
            wait='static void device_wait_ms(unsigned ms) { usleep(ms*1000); }'
            wake='static void wake_device_loop(void) {}'
        code=r'''
#define _GNU_SOURCE
#include <sys/eventfd.h>
#include <poll.h>
#include <pthread.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <time.h>
#include <unistd.h>
#include <errno.h>
#include <stdatomic.h>
static volatile sig_atomic_t g_stop;
static int g_device_stop_fd=-1;
static uint64_t now_ms(void) {
    struct timespec t; clock_gettime(CLOCK_MONOTONIC,&t);
    return (uint64_t)t.tv_sec*1000 + t.tv_nsec/1000000;
}
''' + wake + wait + r'''
static uint64_t elapsed;
static atomic_int waiting;
static void *run_wait(void *p) {
    uint64_t t=now_ms();
    atomic_store(&waiting,1);
    device_wait_ms((unsigned)(uintptr_t)p);
    elapsed=now_ms()-t;
    return NULL;
}
static int exercise_stop(int fd) {
    pthread_t t;
    g_device_stop_fd=fd; g_stop=0; atomic_store(&waiting,0);
    if(pthread_create(&t,NULL,run_wait,(void *)(uintptr_t)1000)) return 10;
    while(!atomic_load(&waiting)) usleep(1000);
    usleep(20000);
    g_stop=1;
    errno=EDOM; wake_device_loop();
    if(errno!=EDOM) return 11;
    wake_device_loop(); /* repeated shutdown must never block */
    pthread_join(t,NULL);
    if(elapsed>250) {
        fprintf(stderr,"stop waited %llu ms (must wake well before 1000 ms)\n",
            (unsigned long long)elapsed);return 12;
    }
    printf("stop wait: %llu ms (%s)\n",(unsigned long long)elapsed,
        fd<0 ? "fallback" : "eventfd");
    uint64_t start=now_ms(); device_wait_ms(1000);
    if(now_ms()-start>100) return 13;
    return 0;
}
int main(void) {
    int fd=eventfd(0,EFD_CLOEXEC|EFD_NONBLOCK);
    if(fd<0) return 1;
    int rc=exercise_stop(fd); close(fd); if(rc) return rc;
    rc=exercise_stop(-1); if(rc) return rc;
    g_stop=0; g_device_stop_fd=eventfd(0,EFD_CLOEXEC|EFD_NONBLOCK);
    uint64_t start=now_ms(); device_wait_ms(100);
    elapsed=now_ms()-start;
    if(elapsed<90 || elapsed>500) return 3;
    printf("normal wait: %llu ms\n",(unsigned long long)elapsed);
    close(g_device_stop_fd);
    return 0;
}
'''
        if os.environ.get('STOP_WAIT_C_OUT'):
            Path(os.environ['STOP_WAIT_C_OUT']).write_text(code)
        with tempfile.TemporaryDirectory() as tmp:
            c=Path(tmp)/'test.c'; c.write_text(code)
            binary=Path(tmp)/'test'
            subprocess.run(['cc','-std=c11','-Wall','-Wextra',str(c),'-pthread','-o',str(binary)],check=True)
            r=subprocess.run([str(binary)],capture_output=True,text=True,timeout=8)
            self.assertEqual(r.returncode,0,r.stderr)
            print(r.stdout,end='')

if __name__=='__main__': unittest.main()
