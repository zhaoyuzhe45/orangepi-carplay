#!/usr/bin/env python3
"""Run the actual RTSP receive functions and address loop on a desktop C compiler.

Android UI/crypto and network calls are replaced at their boundaries; the parser,
buffering, error propagation and address-selection code are extracted unchanged.
No device, route changes, or network access is needed.
"""
import argparse
import os
from pathlib import Path
import shutil
import subprocess


def function(source, signature):
    start = source.index(signature)
    while source.index(";", start) < source.index("{", start):
        start = source.index(signature, source.index(";", start) + 1)
    return source[start:source.index("\n}", start) + 2]


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--source-dir", type=Path, default=Path(__file__).resolve().parents[1] / "src")
    parser.add_argument("--output-dir", type=Path, default=Path(__file__).resolve().parents[1] / "build/review-20261110")
    args = parser.parse_args()
    client = (args.source_dir / "airplay_client.c").read_text()
    bridge = (args.source_dir / "ncm_bridge.c").read_text()
    funcs = [function(client, signature) for signature in (
        "static const uint8_t *find_header_end(",
        "static int header_uint(",
        "static int append_response_plaintext(",
    )]
    if "static void fail_session(" in client:
        funcs.append(function(client, "static void fail_session("))
    funcs.append(function(client, "void airplay_client_feed("))
    loop_start = bridge.index("\tfor (attempt = 0;", bridge.index("static void *connect_endpoint_main("))
    loop_end = bridge.index("\n\tif (fd < 0)", loop_start)
    loop = bridge[loop_start:loop_end]
    prelude = r'''
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <limits.h>
#ifdef _WIN32
#define strncasecmp _strnicmp
#else
#include <strings.h>
#endif
#define iap2_log(...) ((void)0)
#define AIRPLAY_RESPONSE_BUFFER 1024
struct airplay_client {
    int control_channel_ready, failed;
    char failure[256];
    uint8_t response[AIRPLAY_RESPONSE_BUFFER];
    size_t response_len, wire_response_len;
};
static int calls, process_result, decrypt_result;
static unsigned last_cseq, last_body;
static int process_response(struct airplay_client *c, unsigned status,
    unsigned seq, const uint8_t *body, size_t len) {
    (void)c; (void)status; (void)body;
    calls++; last_cseq=seq; last_body=(unsigned)len; return process_result;
}
static int decrypt_control_input(struct airplay_client *c,
    const uint8_t *data, size_t len) {
    (void)c; (void)data; (void)len; return decrypt_result;
}
'''
    network = r'''
#define AIRPLAY_CONNECT_RETRY_ATTEMPTS 1
#define AIRPLAY_CONNECT_RETRY_DELAY_US 1
#define usleep(n) ((void)(n))
struct ncm_connect_request { const char **addresses; size_t count; uint16_t port; int discovered; };
static int connect_calls, first_succeeds=1;
static int connect_owned(const struct ncm_connect_request *r) { (void)r; return 1; }
static int wait_connect_retry(const struct ncm_connect_request *r,unsigned us) { (void)r;(void)us;return 1; }
static int carplay_log_should_sample(unsigned long n) { (void)n;return 0; }
static int ensure_airplay_policy_routes(const char *s) { (void)s; return 0; }
static int address_is_ipv4(const char *s) { return s[0]!=':'; }
static int connect_one(const char *s, uint16_t port, int ipv4,
    char *resolved, size_t capacity, int verbose, int restrict_to_ncm) {
    (void)port; (void)ipv4; (void)verbose; (void)restrict_to_ncm;
    connect_calls++; snprintf(resolved,capacity,"%s",s);
    return strcmp(s, first_succeeds ? "one" : "two")==0 ? 42 : -1;
}
static int run_connect(void) {
    const char *addresses[]={"one","two","::three"};
    struct ncm_connect_request req={addresses,3,7000,0}, *request=&req;
    int fallback_used=0; const char *selected_address=NULL;
    int preparation_retry_used=0,recovering=0;
    unsigned long recovery_waits=0;
    unsigned attempt_limit=AIRPLAY_CONNECT_RETRY_ATTEMPTS;
    char resolved[64]; unsigned attempt; int fd=-1;
'''+loop+r'''
out:
    (void)selected_address;return fd;
}
static int check(int condition, const char *message) {
    if (!condition) { fprintf(stderr,"FAIL: %s\n",message); return 1; }
    return 0;
}
int main(int argc, char **argv) {
    struct airplay_client c; const char *wire; size_t len;
    if(argc!=2) return 2;
    memset(&c,0,sizeof(c));
    if(strcmp(argv[1],"first-address")==0)
        return check(run_connect()==42 && connect_calls==1,"first successful endpoint must stop address search");
    if(strcmp(argv[1],"fallback-address")==0) {
        first_succeeds=0;
        return check(run_connect()==42 && connect_calls==2,"failed first endpoint must permit next address");
    }
    if(strcmp(argv[1],"missing-length")==0) {
        wire="RTSP/1.0 200 OK\r\nCSeq: 1\r\n\r\n";
        /* Poison unread buffer bytes: they must not be parsed as headers. */
        memcpy(c.response+strlen(wire),"Content-Length: 1\r\n",19);
        airplay_client_feed(&c,(const uint8_t*)wire,strlen(wire));
        return check(calls==1 && last_body==0,"absent Content-Length must not scan past header boundary");
    }
    if(strcmp(argv[1],"bad-body")==0) {
        wire="RTSP/1.0 200 OK\r\nCSeq: 2\r\nContent-Length: 3\r\n\r\nbad";
        process_result=-1;
        airplay_client_feed(&c,(const uint8_t*)wire,strlen(wire));
        return check(c.failed && c.failure[0],"200 response rejected by handshake parser must mark session failed");
    }
    if(strcmp(argv[1],"oversized-body")==0) {
        wire="RTSP/1.0 200 OK\r\nCSeq: 2\r\nContent-Length: 2048\r\n\r\n";
        airplay_client_feed(&c,(const uint8_t*)wire,strlen(wire));
        return check(c.failed,"body larger than receive buffer must fail instead of waiting forever");
    }
    if(strcmp(argv[1],"overflow-length")==0) {
        wire="RTSP/1.0 200 OK\r\nCSeq: 2\r\nContent-Length: 4294967296\r\n\r\n";
        airplay_client_feed(&c,(const uint8_t*)wire,strlen(wire));
        return check(c.failed && calls==0,"overflowing numeric header must not wrap to zero");
    }
    if(strcmp(argv[1],"missing-cseq")==0) {
        wire="RTSP/1.0 200 OK\r\nContent-Length: 0\r\n\r\n";
        memcpy(c.response+strlen(wire),"CSeq: 2\r\n",9);
        airplay_client_feed(&c,(const uint8_t*)wire,strlen(wire));
        return check(c.failed && calls==0,"missing CSeq must not scan beyond header");
    }
    if(strcmp(argv[1],"bad-encryption")==0) {
        c.control_channel_ready=1; decrypt_result=-1;
        airplay_client_feed(&c,(const uint8_t*)"bad",3);
        return check(c.failed,"unrecoverable encrypted frame failure must mark session failed");
    }
    if(strcmp(argv[1],"fragmented-coalesced")==0) {
        wire="RTSP/1.0 200 OK\r\nCSeq: 2\r\nContent-Length: 3\r\n\r\nabc";
        len=strlen(wire);
        airplay_client_feed(&c,(const uint8_t*)wire,len-1);
        if(check(calls==0 && !c.failed,"partial response must wait")) return 1;
        airplay_client_feed(&c,(const uint8_t*)wire+len-1,1);
        if(check(calls==1 && last_cseq==2 && last_body==3 && !c.failed,"complete response must dispatch")) return 1;
        char pair[256]; snprintf(pair,sizeof(pair),"%s%s",wire,wire);
        airplay_client_feed(&c,(const uint8_t*)pair,strlen(pair));
        return check(calls==3 && !c.failed && c.response_len==0,"coalesced responses must dispatch separately");
    }
    return 2;
}
'''
    args.output_dir.mkdir(parents=True, exist_ok=True)
    source = args.output_dir / "airplay_connection_regression.c"
    source.write_text(prelude + "\n".join(funcs) + network)
    compiler = os.environ.get("CC") or shutil.which("gcc") or shutil.which("clang")
    if not compiler:
        raise SystemExit("A desktop C compiler (CC, gcc, or clang) is required")
    binary = source.with_suffix(".exe" if os.name == "nt" else "")
    subprocess.run([compiler,"-std=c11","-O0","-Wall","-Wextra","-Werror",str(source),"-o",str(binary)],check=True)
    failures = 0
    for case in ("missing-length","bad-body","oversized-body","overflow-length","missing-cseq","bad-encryption","first-address","fallback-address","fragmented-coalesced"):
        result = subprocess.run([str(binary.resolve()),case],capture_output=True,text=True,timeout=10)
        print(("PASS" if result.returncode==0 else "FAIL")+": "+case)
        if result.stderr: print(result.stderr.strip())
        failures += result.returncode != 0
    return bool(failures)


if __name__ == "__main__":
    raise SystemExit(main())
