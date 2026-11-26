#!/usr/bin/env python3
"""Exercise production host-netdev state transitions without devices or routes."""
import os
from pathlib import Path
import shutil
import subprocess


def function(source, signature):
    start = source.index(signature)
    return source[start:source.index("\n}", start) + 2]


def main():
    root = Path(__file__).resolve().parents[1]
    source = (root / "src/ncm_bridge.c").read_text()
    definitions = r'''
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <unistd.h>
#define IFNAMSIZ 16
#define NCM_NETDEV_WAIT_MS 1000u
#define SHUT_RDWR 2
#define iap2_log(...) ((void)0)
#define pthread_mutex_lock(x) ((void)(x))
#define pthread_mutex_unlock(x) ((void)(x))
#define pthread_cond_wait(x,y) ((void)(x),(void)(y))
#define usleep(x) ((void)(x))
#define shutdown(fd,how) ((void)(fd),(void)(how))
struct ncm_connect_request { int marker; };
static int g_mu, g_connect_cv, g_running, g_connect_stop, g_kernel_ncm;
static int g_host_netdev_ready, g_tun_index, g_tun_fd=-1;
static int g_host_auth_required, g_host_authenticated;
static int g_connect_sock=-1, g_connect_thread_running;
static int g_airplay_transport_restart_pending;
static int g_transport_restarting;
static unsigned long g_transport_restart_epoch;
static int g_connect_retiring;
static int g_mdns_enabled,g_peer_mac_valid,g_peer_ipv6_valid;
struct carplay_mdns;
static struct carplay_mdns *g_mdns;
static void host_mdns_start_locked(const char *name) {(void)name;}
static void carplay_mdns_stop(struct carplay_mdns *mdns) {(void)mdns;}
static struct ncm_connect_request *g_pending_connect_request;
static int lookup_result, configure_result, launch_result, launch_calls, freed;
static int car_netdev_name(const char *device,char *name,size_t len) {
    (void)device; snprintf(name,len,"usb7"); return lookup_result;
}
static int netdev_configure(const char *name) {
    (void)name;
    if (configure_result==0) g_tun_index=7;
    return configure_result;
}
static int netdev_apply(unsigned wait_ms) {
    (void)wait_ms;
    return g_kernel_ncm ? netdev_configure("usb7") : -1;
}
static int launch_connect_request_locked(struct ncm_connect_request *r) {
    (void)r; launch_calls++;
    if(!launch_result) g_connect_thread_running=1;
    return launch_result;
}
static void free_connect_request(struct ncm_connect_request *r) { if(r) freed++; }
static void usb_workers_stop(void) {}
static int fd_is_open(int fd) { return fd>=0; }
'''
    functions = "\n".join(function(source, signature) for signature in (
        "static int transport_ready_locked(",
        "static int connect_ready_locked(void)\n{",
        "int ncm_bridge_host_authentication(",
        "int ncm_bridge_car_netdev_up(",
        "int ncm_bridge_netdev_up(",
        "int ncm_bridge_netdev_prepare(",
        "void ncm_bridge_stop(",
        "void ncm_bridge_close(",
    ))
    main_code = r'''
static int check(int condition,const char *reason) {
    if(!condition) fprintf(stderr,"FAIL: %s\n",reason);
    return !condition;
}
int main(int argc,char **argv) {
    struct ncm_connect_request pending={123};
    (void)g_host_netdev_ready;
    (void)launch_connect_request_locked;
    if(argc!=2) return 2;
    if(strncmp(argv[1],"kernel-",7)==0) {
        g_kernel_ncm=1; g_tun_index=7;
        ncm_bridge_stop();
        if(check(!connect_ready_locked(),"stopped gadget cannot connect")) return 1;
        if(strcmp(argv[1],"kernel-prepare")==0)
            return check(ncm_bridge_netdev_prepare()==0 && g_connect_stop && !connect_ready_locked(),
                         "early preparation does not authorize a stopped session");
        g_pending_connect_request=&pending;
        if(strcmp(argv[1],"kernel-configure-failure")==0) {
            configure_result=-1;
            return check(ncm_bridge_netdev_up()<0 && g_connect_stop && !launch_calls &&
                         g_pending_connect_request==&pending,"failed kernel setup stays stopped");
        }
        if(strcmp(argv[1],"kernel-worker-retiring")==0) g_connect_thread_running=1;
        if(strcmp(argv[1],"kernel-auth-gated")==0) g_host_auth_required=1;
        if(strcmp(argv[1],"kernel-launch-failure")==0) launch_result=-1;
        if(strcmp(argv[1],"kernel-rebind")==0) g_pending_connect_request=NULL;
        if(ncm_bridge_netdev_up()!=0) return 1;
        if(strcmp(argv[1],"kernel-worker-retiring")==0)
            return check(g_connect_stop && !launch_calls && g_pending_connect_request==&pending,
                         "retiring worker observes stop until joined");
        if(strcmp(argv[1],"kernel-auth-gated")==0)
            return check(!connect_ready_locked() && !launch_calls && g_pending_connect_request==&pending,
                         "kernel rebind cannot bypass Host authentication");
        if(strcmp(argv[1],"kernel-launch-failure")==0)
            return check(launch_calls==1 && !g_connect_stop && g_pending_connect_request==&pending,
                         "failed launch preserves the endpoint for a later retry");
        if(strcmp(argv[1],"kernel-rebind")==0)
            return check(connect_ready_locked() && !g_running && !launch_calls,
                         "next kernel USB round permits its new endpoint request");
        if(strcmp(argv[1],"kernel-pending")==0) {
            ncm_bridge_netdev_up();
            return check(connect_ready_locked() && launch_calls==1 && !g_pending_connect_request,
                         "successful rebind resumes pending endpoint exactly once");
        }
        return 2;
    }
    if(strcmp(argv[1],"auth-first")==0 || strcmp(argv[1],"netdev-first")==0 ||
       strcmp(argv[1],"auth-launch-failure")==0) {
        ncm_bridge_host_authentication(0);
        g_kernel_ncm=1; g_tun_index=7; /* stale gadget index cannot bypass gate */
        g_pending_connect_request=&pending;
        if(check(!connect_ready_locked(),"begin Host must gate cached gadget readiness")) return 1;
        if(strcmp(argv[1],"auth-first")==0) {
            ncm_bridge_host_authentication(1);
            if(check(!launch_calls,"authentication alone is not a Host netdev")) return 1;
            ncm_bridge_car_netdev_up("2-1",0);
        } else {
            ncm_bridge_car_netdev_up("2-1",0);
            ncm_bridge_host_authentication(0); /* repeated ticks preserve early result */
            if(check(!launch_calls && g_pending_connect_request==&pending,
                     "discovery must remain pending before auth")) return 1;
            if(strcmp(argv[1],"auth-launch-failure")==0) launch_result=-1;
            ncm_bridge_host_authentication(1);
        }
        if(launch_result<0) {
            if(check(launch_calls==1 && g_pending_connect_request==&pending,
                     "failed launch must retain pending endpoint")) return 1;
            launch_result=0;
            ncm_bridge_host_authentication(1);
            return check(launch_calls==2 && !g_pending_connect_request,"retry launch on next tick");
        }
        ncm_bridge_host_authentication(1);
        return check(launch_calls==1 && !g_pending_connect_request && connect_ready_locked(),
                     "auth and netdev in either order launch exactly once");
    }
    if(strcmp(argv[1],"auth-revoked")==0) {
        ncm_bridge_host_authentication(1); ncm_bridge_car_netdev_up("2-1",0);
        g_pending_connect_request=&pending;
        if(check(ncm_bridge_host_authentication(0)==1 && g_connect_stop &&
                 !connect_ready_locked() && !g_pending_connect_request && freed==1,
                 "revocation must invalidate old pending request and stop connector")) return 1;
        ncm_bridge_stop();
        ncm_bridge_car_netdev_up("2-1",0);
        return check(!g_host_authenticated && !connect_ready_locked(),"new round requires new auth");
    }
    if(strcmp(argv[1],"fresh-host")==0) {
        if(ncm_bridge_car_netdev_up("2-1",0)!=0) return 1;
        return check(transport_ready_locked() && !g_running && !g_kernel_ncm,
            "fresh host netdev must be ready without gadget/USB worker flags");
    }
    if(strcmp(argv[1],"after-stop")==0) {
        ncm_bridge_stop();
        if(ncm_bridge_car_netdev_up("2-1",0)!=0) return 1;
        return check(transport_ready_locked() && !g_connect_stop,
            "host netdev must resume transport after device shutdown");
    }
    if(strcmp(argv[1],"pending")==0 || strcmp(argv[1],"launch-failure")==0) {
        g_connect_stop=1; g_pending_connect_request=&pending;
        launch_result=strcmp(argv[1],"launch-failure")==0 ? -1 : 0;
        if(ncm_bridge_car_netdev_up("2-1",0)!=0) return 1;
        return check(launch_calls==0 && g_pending_connect_request==&pending,
            "host netdev readiness alone must not launch unauthenticated AirPlay");
    }
    if(strcmp(argv[1],"stop-clear")==0 || strcmp(argv[1],"close-clear")==0) {
        if(ncm_bridge_car_netdev_up("2-1",0)!=0) return 1;
        if(strcmp(argv[1],"stop-clear")==0) ncm_bridge_stop();
        else ncm_bridge_close();
        return check(!g_host_netdev_ready && !g_host_authenticated && !g_host_auth_required && !transport_ready_locked(),
            "stop/close must invalidate host readiness");
    }
    if(strcmp(argv[1],"configure-failure")==0 || strcmp(argv[1],"missing-netdev")==0) {
        g_connect_stop=1; g_pending_connect_request=&pending;
        if(strcmp(argv[1],"configure-failure")==0) configure_result=-1;
        else lookup_result=-1;
        return check(ncm_bridge_car_netdev_up("2-1",0)<0 && !transport_ready_locked() &&
            g_connect_stop && !launch_calls && g_pending_connect_request==&pending,
            "failed netdev setup must not enable transport or dispatch pending work");
    }
    if(strcmp(argv[1],"active-connect")==0) {
        g_connect_thread_running=1; g_pending_connect_request=&pending;
        return check(ncm_bridge_car_netdev_up("2-1",0)==0 && launch_calls==0 &&
            g_pending_connect_request==&pending,"ready host must not start duplicate worker");
    }
    if(strcmp(argv[1],"gadget-unchanged")==0) {
        g_kernel_ncm=1; g_tun_index=3;
        if(check(transport_ready_locked(),"kernel gadget remains ready")) return 1;
        g_kernel_ncm=0;
        if(check(!transport_ready_locked(),"index alone does not enable transport")) return 1;
        g_running=1;
        return check(transport_ready_locked(),"userspace gadget remains ready");
    }
    return 2;
}
'''
    output = root / "build/review-20261110/ncm_host_transport.c"
    output.parent.mkdir(parents=True,exist_ok=True)
    output.write_text(definitions+functions+main_code)
    compiler=os.environ.get("CC") or shutil.which("gcc") or shutil.which("clang")
    binary=output.with_suffix(".exe" if os.name=="nt" else "")
    subprocess.run([compiler,"-std=c11","-O0","-Wall","-Wextra","-Werror",str(output),"-o",str(binary)],check=True)
    failures=0
    for case in ("fresh-host","after-stop","pending","launch-failure","stop-clear","close-clear","configure-failure","missing-netdev","active-connect","gadget-unchanged","auth-first","netdev-first","auth-launch-failure","auth-revoked",
                 "kernel-rebind","kernel-pending","kernel-configure-failure","kernel-worker-retiring","kernel-auth-gated","kernel-launch-failure","kernel-prepare"):
        result=subprocess.run([str(binary),case],capture_output=True,text=True,timeout=10)
        print(("PASS" if result.returncode==0 else "FAIL")+": "+case)
        if result.stderr: print(result.stderr.strip())
        failures+=result.returncode!=0
    return bool(failures)


if __name__=="__main__":
    raise SystemExit(main())
