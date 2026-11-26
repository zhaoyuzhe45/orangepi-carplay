"""Focused host regressions; output is retained for the November 14 repair."""
from pathlib import Path
import os
import subprocess
import sys
root=Path(__file__).resolve().parents[1]
out=root/'build/fix-review-20261114'
out.mkdir(parents=True,exist_ok=True)
log=[]
environment=dict(os.environ,PATH='/usr/local/sbin:/usr/local/bin:/usr/sbin:/usr/bin:/sbin:/bin')
def run(args,expected=0):
    result=subprocess.run(args,cwd=root,env=environment,capture_output=True,text=True,timeout=90)
    text='$ '+' '.join(map(str,args))+'\n'+result.stdout+result.stderr
    log.append(text);print(text,flush=True)
    log_name='baseline-input.log' if '--baseline' in sys.argv else 'shell-regression.log' if '--shell-only' in sys.argv else 'host-regression.log'
    (out/log_name).write_text('\n'.join(log))
    if expected==0 and result.returncode!=0:raise SystemExit(result.returncode)
    if expected!=0 and result.returncode==0:raise SystemExit('Baseline unexpectedly passed')

common=['cc','-std=c11','-Wall','-Wextra','-Werror','-Wno-format-truncation','-Wno-unused-result',
        '-ffunction-sections','-fdata-sections','-Isrc']
if '--baseline' in sys.argv:
    test=(root/'tests/input_lifecycle_test.c').read_text()
    for name in ['carplay_keys','airplay_client']:
        test=test.replace('../src/'+name+'.c',str(out/'before/src'/f'{name}.c'))
    baseline=out/'input-baseline.c';baseline.write_text(test)
    run(common+[str(baseline),'src/carplay_display.c','-pthread','-Wl,--gc-sections','-o',str(out/'input-baseline')])
    for case in ['contention','suspend','touch']:run([str(out/'input-baseline'),case],expected=1)
    raise SystemExit(0)

if '--shell-only' in sys.argv:
    for name in ['display_mode_apply_test','display_boot_default_test','display_init_order_test',
                 'display_hwc_guard_test','display_modes_inventory_test','screen_packetization_test','packaged_scripts_test']:
        run(['sh',f'tests/{name}.sh'])
    raise SystemExit(0)

for name,sources,extra,cases in [
    ('input-lifecycle',['tests/input_lifecycle_test.c','src/carplay_display.c'],[],['contention','suspend','touch']),
    ('connection-queries',['tests/connection_display_queries_test.c'],['-O2'],['']),
    ('display-helper',['tests/display_helper_completion_test.c'],[],['']),
    ('display-policy',['tests/display_policy_test.c','src/carplay_display.c'],[],['']),
    ('display-preparation',['tests/display_preparation_test.c','src/carplay_display.c'],[],['']),
    ('display-presets',['tests/display_preset_test.c','src/carplay_capabilities.c','src/airplay_bplist.c'],[],[''])
]:
    exe=out/name
    run(common+extra+sources+['-pthread','-Wl,--gc-sections','-o',str(exe)])
    for case in cases:run([str(exe)]+([case] if case else []))
for name in ['ncm_restart_test','ncm_compat_test','ncm_host_transport_test','ncm_display_reconnect_test',
             'ncm_profile_test','display_memory_test','display_profile_commit_test','mediacodec_pipeline_test']:
    run(['python3',f'tests/{name}.py'])
run(['python3','tests/airplay_connection_regression_test.py','--output-dir',str(out/'connection')])
for name in ['run_carplay_knob_keys_test','display_mode_apply_test','display_boot_default_test',
             'display_init_order_test','display_hwc_guard_test','display_modes_inventory_test',
             'screen_packetization_test','packaged_scripts_test']:
    run(['sh',f'tests/{name}.sh'])
print('Focused host regressions: PASS')
