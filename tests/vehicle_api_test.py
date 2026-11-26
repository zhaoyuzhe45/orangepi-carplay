"""Exercise the real management formatter, including expiry and JSON escaping."""
from pathlib import Path
import json, os, shutil, subprocess
from iap2d_host_runtime_test import function
ROOT = Path(__file__).resolve().parents[1]
OUT = ROOT / "build/legacy-services-20261111"
source = (ROOT / "src/carplay_control.c").read_text()
code = r'''
#define _GNU_SOURCE
#include "carplay_control.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
static struct carplay_control_status state;
uint64_t carplay_vehicle_monotonic_ms(void){return 10000;}
void carplay_control_get_status(struct carplay_control_status*out){*out=state;}
static int send_response(int fd,int status,const char*reason,const char*type,const char*body,size_t len,const char*extra){
 (void)fd;(void)reason;(void)type;(void)extra;if(status!=200)exit(1);fwrite(body,1,len,stdout);return 0;}
static void send_json_error(int fd,int status,const char*reason,const char*message){(void)fd;(void)status;(void)reason;fprintf(stderr,"%s",message);exit(2);}
''' + function(source,"static int append_json_escaped(") + function(source,"static void handle_status(") + r'''
int main(int argc,char**argv){
 state.gps=1;state.vehicle.active=1;state.vehicle.received=1;state.vehicle.sampled_ms=9900;
 state.vehicle.rmc_age_ms=200;state.vehicle.gga_age_ms=100;
 strcpy(state.vehicle.rmc,"$GPRMC,\"quoted\"\\value\r\n");strcpy(state.vehicle.gga,"$GPGGA,current");
 strcpy(state.vehicle.night_error,"permission \"denied\"");
 if(argc>1 && !strcmp(argv[1],"expired"))state.vehicle.sampled_ms=4000;
 if(argc>1 && !strcmp(argv[1],"disconnected"))state.vehicle.active=0;
 handle_status(1);return 0;}
'''
path = OUT / "vehicle_api_test.c"; path.write_text(code)
binary = OUT / ("vehicle_api_test.exe" if os.name == "nt" else "vehicle_api_test")
cc = os.environ.get("CC") or shutil.which("gcc")
subprocess.run([cc,"-D__USE_MINGW_ANSI_STDIO=1","-std=c11","-Wall","-Wextra","-Werror","-I"+str(ROOT/"src"),str(path),"-o",str(binary)],check=True)
for case in ["fresh", "expired", "disconnected"]:
    root = json.loads(subprocess.check_output([str(binary),case],text=True))
    vehicle = root["vehicle"]; location=vehicle["location"]
    assert vehicle["gps"] and not vehicle["appearanceApplied"]
    assert vehicle["appearanceError"] == 'permission "denied"'
    if case in ("fresh", "expired"):
        assert location["received"]
        assert location["rmc"] == '$GPRMC,"quoted"\\value\r\n'
        assert location["gga"] == "$GPGGA,current"
        if case == "fresh":
            assert location["rmcAgeMs"]==300 and location["ggaAgeMs"]==200
    else:
        assert not location["received"] and not location["rmc"] and not location["gga"]
    print("PASS vehicle API " + case)
