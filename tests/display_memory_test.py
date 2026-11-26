"""Exercise production persistence retries with real nonblocking result pipes."""
from pathlib import Path
import subprocess
import tempfile
from ncm_compat_test import function
root=Path(__file__).resolve().parents[1]
s=(root/'src/carplay_display.c').read_text()
if 'void carplay_display_remember_step(' not in s:
    raise SystemExit('FAIL: no completion-aware persistence/retry state exists')
prelude=r'''
#define _GNU_SOURCE
#include "carplay_display.h"
#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
static int launches,fail_launch,writer=-1;
static void iap2_log(const char *fmt,...) {(void)fmt;}
static int start_remember_mode(unsigned w,unsigned h,unsigned d,int *fd) {
 assert(w==1280 && h==720 && d==240);launches++;
 if(fail_launch)return -1;
 int p[2];assert(!pipe2(p,O_NONBLOCK|O_CLOEXEC));*fd=p[0];writer=p[1];return 0;
}
'''
cases=r'''
static void result(unsigned char value) {assert(write(writer,&value,1)==1);close(writer);writer=-1;}
int main(void) {
 struct carplay_display_memory m={0};fail_launch=1;
 carplay_display_remember_step(&m,1280,720,240,100);
 assert(launches==1 && !m.complete && !m.pending);
 carplay_display_remember_step(&m,1280,720,240,101);assert(launches==1);
 fail_launch=0;carplay_display_remember_step(&m,1280,720,240,1100);
 assert(launches==2 && m.pending && !m.complete);
 carplay_display_remember_step(&m,1280,720,240,2100);assert(m.pending && !m.complete);
 result(2);carplay_display_remember_step(&m,1280,720,240,3100);
 assert(!m.pending && !m.complete);
 carplay_display_remember_step(&m,1280,720,240,4100);assert(launches==3);
 result(1);carplay_display_remember_step(&m,1280,720,240,5100);
 assert(m.complete && !m.pending);carplay_display_remember_step(&m,1280,720,240,99999);
 assert(launches==3);
 memset(&m,0,sizeof(m));launches=0;fail_launch=1;
 for(unsigned i=0;i<10;i++)carplay_display_remember_step(&m,1280,720,240,i*1000);
 assert(launches==3 && !m.complete);
 memset(&m,0,sizeof(m));fail_launch=0;
 carplay_display_remember_step(&m,1280,720,240,0);int old=m.fd;
 close(writer);writer=-1;carplay_display_remember_step(&m,1280,720,240,1000);
 assert(!m.pending && !m.complete && fcntl(old,F_GETFD)==-1);
 carplay_display_remember_step(&m,1280,720,240,2000);old=m.fd;
 carplay_display_remember_step(&m,1280,720,240,48000);
 assert(!m.pending && !m.complete && fcntl(old,F_GETFD)==-1);close(writer);writer=-1;
 carplay_display_remember_step(&m,1280,720,240,49000);old=m.fd;
 carplay_display_remember_close(&m);assert(!m.pending && fcntl(old,F_GETFD)==-1);
 close(writer);
 puts("display memory: dispatch failure, helper failure, success, timeout, retry budget and teardown PASS");
}
'''
with tempfile.TemporaryDirectory() as tmp:
    c=Path(tmp)/'test.c';exe=Path(tmp)/'test'
    c.write_text(prelude+function(s,'void carplay_display_remember_close(')+function(s,'void carplay_display_remember_step(')+cases)
    subprocess.run(['cc','-std=c11','-Wall','-Wextra','-Werror','-I',str(root/'src'),str(c),'-o',str(exe)],check=True)
    subprocess.run([str(exe)],check=True,timeout=8)
