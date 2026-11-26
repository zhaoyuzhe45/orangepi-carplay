"""Production restart/retirement blocks, deterministic USB/netdev boundaries."""
from pathlib import Path
from types import SimpleNamespace
import unittest
import ncm_compat_test as compat

ROOT=Path(__file__).resolve().parents[1]

class RestartTest(unittest.TestCase):
    def test_replacement_and_shutdown(self):
        captured=[]
        original=compat.run_c
        try:
            compat.run_c=lambda code: captured.append(code) or SimpleNamespace(returncode=0,stderr="")
            compat.NcmCompatTest("test_discovery_shares_connector_and_stops_before_join").test_discovery_shares_connector_and_stops_before_join()
        finally:
            compat.run_c=original
        scaffold=captured[0].split("int main(void) {")[0]
        source=(ROOT/"src/ncm_bridge.c").read_text()
        # OS mocks can retire the old connector or stop/revoke during restart.
        scaffold=scaffold.replace('static int launch_connect_request_locked(struct ncm_connect_request*r) {',
            'static int launch_connect_request_locked(struct ncm_connect_request*r) {\n g_airplay_transport_restart_pending=0;')
        connector=compat.function(source,'static void *connect_endpoint_main(')
        retirement=connector[connector.index('\n\tstruct ncm_connect_request *replacement = NULL;'):]
        code=scaffold+r'''
#include <assert.h>
#define NCM_NETDEV_WAIT_MS 5000u
static void *retire_request(struct ncm_connect_request *request);
static int boundary;
static void retire_old(void) { retire_request(calloc(1,sizeof(struct ncm_connect_request))); }
static int restart_boundary(void) {
    if(boundary==1)retire_old();
    if(boundary==2)return -1;
    if(boundary==3){retire_old();ncm_bridge_stop();}
    if(boundary==4)ncm_bridge_host_authentication(0);
    if(boundary==6 || boundary==7) {
        const char *address="169.254.0.2";
        assert(!queue_connect_endpoint(&address,1,7000,AIRPLAY_PROFILE_HAP,0));
        assert(launches==0);
        if(boundary==6)retire_old();
        assert(launches==0); /* OS re-enable is not complete yet. */
    }
    return 0;
}
static int netdev_apply(unsigned timeout) {(void)timeout;return restart_boundary();}
static int usb_workers_start(int out,int in) {(void)out;(void)in;return restart_boundary();}
'''
        code+=compat.function(source,'int ncm_bridge_netdev_up(')
        if 'static void complete_transport_restart(' in source:
            code+=compat.function(source,'static void complete_transport_restart(')
        code+=compat.function(source,'int ncm_bridge_restart_usb(')
        code+='\nstatic void *retire_request(struct ncm_connect_request *request) {'+retirement
        code+=r'''
int main(void) {
 const char *address="169.254.0.2";
 for(int kernel=0;kernel<=1;kernel++)for(int scenario=0;scenario<8;scenario++) {
    g_kernel_ncm=kernel;g_running=!kernel;g_tun_fd=123;g_tun_index=7;
    g_connect_stop=0;g_host_auth_required=scenario==4;g_host_authenticated=scenario==4;
    g_host_netdev_ready=scenario==4;g_connect_thread_running=1;g_connect_sock=-1;
    g_airplay_transport_restart_pending=0;g_pending_connect_request=NULL;
    launches=0;active=NULL;boundary=scenario;
    int result=ncm_bridge_restart_usb(10,11);
    if(kernel && scenario==0)assert(g_connect_retiring==0); /* Keep a usable socket until a new endpoint. */
    if(scenario==7)assert(g_connect_retiring==1);
    if(scenario==2)assert(result<0);
    else assert(result==0);
    if(scenario==5){g_connect_thread_running=0;ncm_bridge_stop();}
    if(scenario<6)assert(!queue_connect_endpoint(&address,1,7000,AIRPLAY_PROFILE_HAP,0));
    if(scenario!=1 && scenario!=3 && scenario!=5 && scenario!=6)retire_old();
    if(scenario<=1 || scenario>=6) {
        assert(launches==1 && !g_pending_connect_request && g_connect_thread_running);
        free_connect_request(active);active=NULL;
    } else {
        assert(launches==0 && !g_connect_thread_running);
        free_connect_request(g_pending_connect_request);g_pending_connect_request=NULL;
    }
    g_connect_thread_running=0;g_mdns=NULL;found_callback=NULL;
    ncm_bridge_stop();
 }
 puts("NCM re-enable: 16 TUN/kernel retirement/failure/stop/auth/early-endpoint cases PASS");return 0;
}
'''
        result=compat.run_c(code)
        self.assertEqual(result.returncode,0,result.stdout+result.stderr)
        print(result.stdout,end='')

if __name__=='__main__':unittest.main()
