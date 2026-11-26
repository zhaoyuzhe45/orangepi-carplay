"""Execute the production initialization slices with only iAP2 init stubbed."""
from pathlib import Path
import os, shutil, subprocess
ROOT = Path(__file__).resolve().parents[1]
OUT = ROOT / "build/legacy-services-20261111"
source = (ROOT / "src/iap2d.c").read_text()
blocks = []
for anchor, end in [("if (g_link->state == IAP2_IDLE)", 'iap2_log("pump:'),
                    ("if (!g_native_active)", "iap2_link_start(g_link")]:
    start = source.index("iap2_session_init(g_session);", source.index(anchor))
    blocks.append(source[start:source.index(end, start)])
code = '''#include <string.h>
#include <stdio.h>
#include "iap2session.h"
static struct iap2_session session, *g_session=&session;
static int g_vehicle_state;
static uint64_t g_session_generation=42;
static struct {int car_gps;} g_config;
void iap2_session_init(struct iap2_session*s){memset(s,0,sizeof(*s));}
void iap2_session_set_vehicle_state(struct iap2_session*s,struct carplay_vehicle_state*p,uint64_t g){s->vehicle_state=p;s->generation=g;}
'''.replace('static int g_vehicle_state;', 'static struct carplay_vehicle_state {int unused;} g_vehicle_state;')
for i, block in enumerate(blocks): code += f"static void init_{i}(void){{{block}}}\n"
code += '''int main(void){
 for(int enabled=0;enabled<2;enabled++){
  g_config.car_gps=enabled;
  init_0(); if(!!session.vehicle_state!=enabled || (enabled && session.generation!=42))return 1;
  init_1(); if(!!session.vehicle_state!=enabled || (enabled && session.generation!=42))return 2;
 } puts("PASS native-device and usbmux idle vehicle binding");return 0;}
'''
path = OUT / "vehicle_binding_test.c"; path.write_text(code)
binary = OUT / ("vehicle_binding_test.exe" if os.name == "nt" else "vehicle_binding_test")
cc = os.environ.get("CC") or shutil.which("gcc")
subprocess.run([cc,"-std=c11","-Wall","-Wextra","-Werror","-I"+str(ROOT/"src"),str(path),"-o",str(binary)], check=True)
raise SystemExit(subprocess.run([str(binary)], timeout=5).returncode)
