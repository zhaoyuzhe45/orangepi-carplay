#!/bin/sh
# The RTSP/display C code may queue a worker, never control Android graphics.
set -eu
src=${1:-src/carplay_display.c}
script=deploy/opi4/scripts/apply_display_mode.sh
[ -f "$src" ] && [ -f "$script" ] || { echo 'FAIL: missing display source/helper' >&2; exit 1; }
scratch=$(mktemp "${TMPDIR:-/tmp}/carplay-hwc.XXXXXX")
trap 'rm -f "$scratch"' EXIT HUP INT TERM
sed -e '/\/\*/,/\*\//d' -e 's:[[:space:]]*//.*$::' "$src" > "$scratch"
if grep -Eq 'ctl\.(restart|stop|start)|hwcomposer-2-1|persist\.sys\.framebuffer\.main' "$scratch"; then
 echo 'FAIL: C display code directly controls Android graphics' >&2; exit 1
fi
if grep -Eq 'usleep\(10000000\)|sleep\(10\)' "$scratch"; then
 echo 'FAIL: C display code blocks the handshake during preparation' >&2; exit 1
fi
sed '/^[[:space:]]*#/d' "$script" > "$scratch"
if grep -q 'ctl\.restart hwcomposer' "$scratch"; then
 echo 'FAIL: helper triggers the vendor onrestart cascade' >&2; exit 1
fi
body=$(sed -n '/^restart_hwc()/,/^}/p' "$scratch")
stop_sf=$(printf '%s\n' "$body" | grep -n 'ctl.stop surfaceflinger' | cut -d: -f1)
stop_hwc=$(printf '%s\n' "$body" | grep -n 'ctl.stop hwcomposer-2-1' | cut -d: -f1)
start_hwc=$(printf '%s\n' "$body" | grep -n 'ctl.start hwcomposer-2-1' | cut -d: -f1)
start_sf=$(printf '%s\n' "$body" | grep -n 'ctl.start surfaceflinger' | cut -d: -f1)
[ "$stop_sf" -lt "$stop_hwc" ] && [ "$start_hwc" -lt "$start_sf" ] || {
 echo 'FAIL: helper graphics stop/start ordering is unsafe' >&2; exit 1;
}
boot=$(sed -n '/^cmd_boot()/,/^}/p' "$scratch")
if printf '%s\n' "$boot" | grep -Eq 'setprop|wm |sleep|restart_|wait_|dumpsys'; then
 echo 'FAIL: compatibility boot path performs graphics work or waits' >&2; exit 1
fi
printf 'PASS: graphics transitions stay in guarded script; boot path has no waits/restarts\n'
