#!/bin/sh
# Init/installer contract: early physical default and no display boot worker.
set -eu
here=$(cd "$(dirname "$0")" && pwd)
root=$(cd "$here/.." && pwd)
rc=$root/deploy/opi4/scripts/init.carplay.rc
installer=$root/deploy/opi4/scripts/install_carplay.sh
code=$(sed '/^[[:space:]]*#/d' "$rc")
fail() { printf 'FAIL: %s\n' "$*" >&2; exit 1; }
printf '%s\n' "$code" | grep -q 'setprop persist.sys.framebuffer.main 800x480@60' || fail 'init lacks early native default'
printf '%s\n' "$code" | grep -q 'setprop persist.sys.sf.lcd_density 160' || fail 'init lacks early density160'
printf '%s\n' "$code" | grep -Eq 'carplay_display_mode|apply_display_mode.sh boot' && fail 'init still launches a boot display worker'
printf '%s\n' "$code" | grep -Eq 'ctl\.(restart|stop)|sleep |sys.boot_completed.*display' && fail 'init default contains restart or wait'
printf '%s\n' "$code" | grep -q 'on early-boot' || fail 'cache must be applied after persist replay and before boot graphics startup'
printf '%s\n' "$code" | grep -q 'on property:sys.usb.configfs=1' || fail 'USB start rule lost'
printf '%s\n' "$code" | grep -q 'setprop sys.usb.configfs carplay' || fail 'USB ownership gate lost'
grep -q 'graphics-init.original' "$installer" || fail 'installer lacks recoverable graphics init backup'
printf '%s\n' "$code" | grep -q 'ro.persistent_properties.ready=true' || fail 'late encrypted-data property replay lacks active profile guard'
printf '%s\n' "$code" | grep -q 'property:sys.carplay.display.active_mode=\*' || fail 'replay must preserve selected boot/runtime target'
printf 'PASS: cache precedes graphics, replay preserves active target, USB gates preserved\n'
