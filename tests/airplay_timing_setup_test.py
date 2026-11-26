#!/usr/bin/env python3
"""Compile the production SETUP sender against the actual timing/plist modules."""
from pathlib import Path
import os
import shutil
import subprocess


def function(source, signature):
    start = source.index(signature)
    # Skip the forward declaration, if present.
    while source.index(";", start) < source.index("{", start):
        start = source.index(signature, source.index(";", start) + 1)
    return source[start:source.index("\n}", start)+2]


def main():
    root = Path(__file__).resolve().parents[1]
    client = (root / "src/airplay_client.c").read_text()
    prelude = r'''
#define _POSIX_C_SOURCE 200809L
#include "airplay_timing.h"
#include "airplay_bplist.h"
#include "airplay_client.h"
#include <arpa/inet.h>
#include <errno.h>
#include <poll.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>
#define iap2_log(...) ((void)0)
#define CHECK(x) do { if(!(x)) {fprintf(stderr,"FAIL line %d: %s\n",__LINE__,#x);exit(1);} } while(0)
struct airplay_client { int fd, setup_sent, failed; enum airplay_profile profile; char failure[256]; struct airplay_timing *timing; };
static int sent;
static uint64_t advertised_port;
static atomic_int force_poll_failure;
int __real_poll(struct pollfd *fds, nfds_t count, int timeout);
int __wrap_poll(struct pollfd *fds, nfds_t count, int timeout) {
    if (atomic_load(&force_poll_failure)) { errno=EIO; return -1; }
    return __real_poll(fds,count,timeout);
}
static int send_request(struct airplay_client *client, const char *method,
    const char *path, const char *content_type, unsigned cseq,
    const uint8_t *body, size_t len) {
    struct airplay_bplist_document doc;
    struct airplay_bplist_value root, value;
    (void)client; (void)method; (void)path; (void)content_type; (void)cseq;
    CHECK(airplay_bplist_init(&doc,body,len)==0);
    CHECK(airplay_bplist_root(&doc,&root)==0);
    CHECK(airplay_bplist_dict_get(&doc,&root,"timingPort",&value)==1);
    CHECK(airplay_bplist_get_uint64(&doc,&value,&advertised_port)==0);
    CHECK(airplay_bplist_dict_get(&doc,&root,"et",&value)==0);
    sent++; return 0;
}
'''
    extracted = "\n".join(function(client, s) for s in (
        "static void fail_session(", "static int send_setup(",
        "static uint64_t screen_sender_ntp(", "int airplay_client_failed(",
        "const char *airplay_client_failure("))
    main_code = r'''
int main(int argc,char **argv) {
    struct airplay_client client={.fd=-1};
    (void)screen_sender_ntp; (void)fail_session;
    if(argc==2 && strcmp(argv[1],"failure")==0) {
        CHECK(send_setup(&client)<0);
        CHECK(airplay_client_failed(&client) && airplay_client_failure(&client)[0]);
        CHECK(sent==0 && !client.setup_sent && client.timing==NULL);
        puts("PASS: timing start failure marks client failed before SETUP"); return 0;
    }
    struct sockaddr_in addr={.sin_family=AF_INET,.sin_addr.s_addr=htonl(INADDR_LOOPBACK)};
    socklen_t len=sizeof(addr);
    int server=socket(AF_INET,SOCK_STREAM,0), peer;
    CHECK(server>=0 && bind(server,(struct sockaddr*)&addr,len)==0 && listen(server,1)==0);
    CHECK(getsockname(server,(struct sockaddr*)&addr,&len)==0);
    peer=socket(AF_INET,SOCK_STREAM,0); CHECK(peer>=0);
    CHECK(connect(peer,(struct sockaddr*)&addr,len)==0);
    client.fd=accept(server,NULL,NULL); CHECK(client.fd>=0);
    CHECK(send_setup(&client)==0 && sent==1 && client.setup_sent);
    CHECK(advertised_port>0 && advertised_port==airplay_timing_port(client.timing));
    uint64_t a,b,c;
    CHECK(airplay_timing_now(&a)==0); b=screen_sender_ntp(); CHECK(airplay_timing_now(&c)==0);
    CHECK(a<=b && b<=c && !airplay_client_failed(&client));
    atomic_store(&force_poll_failure,1);
    for(int i=0;i<1000 && !airplay_client_failed(&client);i++) {
        struct timespec pause={.tv_nsec=1000000}; nanosleep(&pause,NULL);
    }
    CHECK(airplay_client_failed(&client) && airplay_client_failure(&client)[0]);
    airplay_timing_stop(&client.timing);
    close(client.fd); close(peer); close(server);
    puts("PASS: SETUP live timing port, omitted et, shared clock, worker failure propagation");
    return 0;
}
'''
    output = root / "build/review-20261110/airplay_timing_setup.c"
    output.write_text(prelude + extracted + main_code)
    compiler = os.environ.get("CC") or shutil.which("gcc")
    binary=output.with_suffix("")
    subprocess.run([compiler,"-std=c11","-O2","-Wall","-Wextra","-Werror","-I"+str(root/"src"),str(output),str(root/"src/airplay_timing.c"),str(root/"src/airplay_bplist.c"),"-pthread","-Wl,--wrap=poll","-o",str(binary)],check=True)
    failed=False
    for case in ("success","failure"):
        result=subprocess.run([str(binary),case])
        failed |= result.returncode!=0
    return failed


if __name__=="__main__":
    raise SystemExit(main())
