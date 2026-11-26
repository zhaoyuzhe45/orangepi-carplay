"""Replay fast car handoff and device recovery with real shell functions."""
from pathlib import Path
import os
import re
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
SUPERVISOR = ROOT / 'deploy/opi4/scripts/carplay_supervisor.sh'
ROLE_SWITCH = ROOT / 'deploy/opi4/scripts/usb_role_switch.sh'


def shell_function(name):
    return re.search(r'^' + name + r'\(\) \{\n.*?^}',
                     SUPERVISOR.read_text(), re.M | re.S).group()


class SupervisorHandoffTest(unittest.TestCase):
    def test_fast_host_handoff_is_not_an_attachment_failure(self):
        harness = r'''
set -eu
G=$TMP/g
STOCK_CONFIG=$G/configs/b.1
CARPLAY_CONFIG=$G/configs/b.2
STRINGS=$G/strings/0x409
OS_DESC=$G/os_desc
STOCK_OS_DESC_CONFIG_LINK=$OS_DESC/b.1
OS_DESC_CONFIG_LINK=$OS_DESC/b.2
LINK=$CARPLAY_CONFIG/f1
PTP_FUNC=$G/functions/ptp.usb0
PTP_LINK=$STOCK_CONFIG/f0
NCM_FUNC=$G/functions/ncm.usb0
NCM_LINK=$CARPLAY_CONFIG/f2
ROLE_REQUEST=$TMP/role_request
UDC=fe800000.dwc3
CHILD=$$
ATTACHED=0
APPLE_USB_MAX_POWER=250
APPLE_BM_ATTRIBUTES=0xc0
APPLE_USB_MANUFACTURER=Apple
APPLE_USB_PRODUCT=iPhone
APPLE_USB_SERIAL=123456
ORIGINAL_OS_DESC_VENDOR_CODE=
ORIGINAL_OS_DESC_SIGNATURE=
mkdir -p "$STOCK_CONFIG" "$CARPLAY_CONFIG" "$STRINGS" "$OS_DESC" "$G/functions/ffs.iap2"
: > "$G/UDC"
ensure_carplay_config() { :; }
setup_kernel_ptp() { [ "$LAYOUT" = multi ] && mkdir -p "$PTP_FUNC" || true; }
release_kernel_ptp() { :; }
log_event() { echo "$*"; }
sleep() {
    case "$SCENARIO" in
        handoff) echo host > "$ROLE_REQUEST"; : > "$G/UDC" ;;
        unexpected) : > "$G/UDC" ;;
        stale) CHILD=99999999; echo host > "$ROLE_REQUEST"; : > "$G/UDC" ;;
    esac
}
''' + shell_function('attach_gadget') + r'''
rc=0
attach_gadget || rc=$?
case "$SCENARIO" in
    normal|handoff) [ "$rc" -eq 0 ] || { echo 'FAIL: valid attach/handoff rejected'; exit 1; } ;;
    unexpected|stale) [ "$rc" -ne 0 ] || { echo 'FAIL: unexpected detach accepted'; exit 1; } ;;
esac
[ "$ATTACHED" -eq 1 ] || { echo 'FAIL: successful bind must retain cleanup ownership'; exit 1; }
'''
        for layout, scenario in ((layout, scenario) for layout in ('single', 'multi')
                                 for scenario in ('handoff', 'unexpected', 'stale', 'normal')):
            with self.subTest(layout=layout, scenario=scenario), tempfile.TemporaryDirectory() as tmp:
                result = subprocess.run(['sh', '-c', harness],
                                        env=dict(os.environ, TMP=tmp, SCENARIO=scenario, LAYOUT=layout),
                                        capture_output=True, text=True)
                self.assertEqual(result.returncode, 0, result.stdout + result.stderr)

    def test_supervisor_recovery_does_not_rebind_closed_functionfs(self):
        source = SUPERVISOR.read_text()
        start = source.index('            device)\n', source.index('case "$_requested" in'))
        recovery = source[start + len('            device)\n'):source.index('                ;;', start)]
        for standalone in (False, True):
            with self.subTest(standalone=standalone), tempfile.TemporaryDirectory() as tmp:
                path = Path(tmp)
                helper = path / 'switch'
                helper.write_text('#!/bin/sh\nexec sh "' + str(ROLE_SWITCH) + '" "$@"\n')
                helper.chmod(0o755)
                harness = r'''
set -eu
ROOT=$TMP
mkdir -p "$ROOT/logs" "$ROOT/g/configs/b.1" "$ROOT/udc/fe800000.dwc3"
ln -s /closed-ffs-endpoints "$ROOT/g/configs/b.1/f1"
echo host > "$ROOT/mode"
: > "$ROOT/g/UDC"
echo host > "$ROOT/state"
export MODE_PATH=$ROOT/mode GADGET=$ROOT/g UDC_DIR=$ROOT/udc
ROLE_SWITCH=$ROOT/switch
host_role_active() { [ "$(cat "$ROOT/state")" = host ]; }
record_role_switch_failure() { :; }
set_role_state() { echo "$1" > "$ROOT/state"; }
log_event() { echo "$*"; }
'''
                harness += ('GADGET_REBIND=0 "$ROLE_SWITCH" --device\nset_role_state device\n'
                            if standalone else recovery)
                harness += r'''
[ "$(cat "$ROOT/mode")" = peripheral ] || exit 1
[ "$(cat "$ROOT/state")" = device ] || exit 2
[ -z "$(cat "$ROOT/g/UDC")" ] || { echo 'FAIL: rebound stale FunctionFS before new descriptors'; exit 3; }
'''
                result = subprocess.run(['sh', '-c', harness], env=dict(os.environ, TMP=tmp),
                                        capture_output=True, text=True)
                self.assertEqual(result.returncode, 0, result.stdout + result.stderr)


if __name__ == '__main__':
    unittest.main()
