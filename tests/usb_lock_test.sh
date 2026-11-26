#!/bin/sh
# Run with sh from the project root: sh tests/usb_lock_test.sh
#
# Exercises usb_lock.sh against a temporary configfs tree. The point is the two
# decisions that are easy to get wrong:
#   * ADB may only be evicted when a CarPlay function is already in that
#     configuration (otherwise b.1 ends up empty and the kernel rejects the
#     whole gadget with -EINVAL);
#   * a role node that does not exist must be reported, never faked.
#
# The sandbox uses a fixed path and never deletes or renames anything: this
# workspace wraps those operations in a confirmation shim that would stall an
# unattended run.
set -eu
# Windows/MSYS cannot create symlinks and wraps rm/mv in confirmation shims, so
# a run here measures the harness, not the guard. Run it on the device (toybox)
# or on a POSIX host -- that is the environment the code actually executes in.
case "$(uname -s 2>/dev/null || echo unknown)" in
    MSYS*|MINGW*|CYGWIN*)
        echo "SKIP: usb_lock_test needs a POSIX host (configfs symlinks); run on the device"
        exit 0
        ;;
esac
target=${1:-deploy/opi4/scripts/usb_lock.sh}
tmp=tests/build/usb-lock-sandbox
mkdir -p "$tmp"
: > "$tmp/functions.sh"

for fn in log prop_get prop_set udc_state gadget_vid gadget_pid udc_bound \
    is_link link_target role_nodes role_describe role_apply pin_props \
    evict_foreign_links status reassert apply reset; do
    body=$(sed -n "/^$fn() {/,/^}/p" "$target")
    [ -n "$body" ] || { echo "FAIL: function $fn not found in $target" >&2; exit 1; }
    printf '%s\n' "$body" >> "$tmp/functions.sh"
done

# In-memory property store: usb_lock.sh calls getprop/setprop, which are shell
# functions here instead of the device binaries.
PROPS_STORE=
getprop() {
    [ $# -ge 1 ] || return 0
    printf '%s\n' "$PROPS_STORE" | sed -n "s/^$1=//p" | tail -n 1
}
setprop() {
    [ $# -eq 2 ] || return 0
    PROPS_STORE=$(printf '%s\n' "$PROPS_STORE" | grep -v "^$1=" || true)
    PROPS_STORE="$PROPS_STORE
$1=$2"
}

. "$tmp/functions.sh"

ROOT=$tmp/runtime
LOG_DIR=$ROOT/logs
LOG=$LOG_DIR/usb-lock.log
mkdir -p "$ROOT/logs"
G=$tmp/g1
DWC3=$tmp/dwc3
UDC_CLASS=$tmp/udc
LOG_MAX_BYTES=262144
WATCH_INTERVAL=2
STATE_FILE=$ROOT/usb-stock.state
failures=0
fail() {
    echo "FAIL: $*" >&2
    failures=$((failures + 1))
}
pass() {
    echo "ok   $*"
}

# Windows cannot create unprivileged symlinks (MSYS `ln -s` silently copies),
# so the two link primitives are shadowed here. Production implementations are
# `[ -L ]` + readlink; only the storage of the target differs.
TGT=$tmp/targets
mkdir -p "$TGT"
is_link() {
    [ -e "$1" ] && [ -e "$TGT/$(basename "$1")" ]
}
link_target() {
    cat "$TGT/$(basename "$1")" 2>/dev/null || true
}
make_link() {
    : > "$2"
    printf '%s\n' "$1" > "$TGT/$(basename "$2")"
}

seed_props() {
    PROPS_STORE="sys.usb.configfs=1
sys.usb.config=adb
persist.sys.usb.config=adb"
}


# --- 1. property gate -------------------------------------------------------
seed_props
pin_props
[ "$(getprop sys.usb.configfs)" = carplay ] || fail "configfs not parked at carplay"
[ "$(getprop sys.usb.config)" = carplay ] || fail "sys.usb.config not moved off adb"
[ "$(getprop persist.sys.usb.config)" = none ] || fail "persist default still requests adb"
if [ "$(getprop persist.sys.usb.config)" = none ] && [ "$(getprop sys.usb.config)" = carplay ]; then
    pass "property gate parks configfs/sys.usb.config and drops the persisted adb default"
fi

# An unrelated value must be left alone: only adb-ish functions are ours to touch.
PROPS_STORE="sys.usb.configfs=carplay
sys.usb.config=carplay
persist.sys.usb.config=none"
if pin_props; then fail "pin_props reported a change when nothing needed changing"; else
    pass "pin_props is a no-op when already locked"
fi

# --- 2. ADB eviction -------------------------------------------------------
G=$tmp/g1
mkdir -p "$G/configs/b.1" "$G/functions/ffs.iap2" "$G/functions/ffs.adb"
make_link "$G/functions/ffs.iap2" "$G/configs/b.1/f2"
make_link "$G/functions/ffs.adb" "$G/configs/b.1/f1"
evict_foreign_links || fail "eviction did not fire on a mixed layout"
is_link "$G/configs/b.1/f1" && fail "ffs.adb link survived eviction"
is_link "$G/configs/b.1/f2" || fail "eviction removed the CarPlay function"
is_link "$G/configs/b.1/f2" && ! is_link "$G/configs/b.1/f1" && \
    pass "mixed layout: ADB link removed, ffs.iap2 kept"

# Idle pass: nothing to evict, nothing reported.
evict_foreign_links && fail "eviction reported a change when the layout was already clean"
pass "eviction is a no-op on a clean layout"

# PTP on interface 0 is part of the iPhone layout, not a foreign Android
# interface. The supervisor links functions/ptp.gs0 into slot f0 so a head unit
# finds the 06/01/01 interface it probes for before starting iAP2; evicting it
# silently dropped interface 0 one second after every attach (2026-10-27).
mkdir -p "$G/functions/ptp.gs0"
make_link "$G/functions/ptp.gs0" "$G/configs/b.1/f0"
make_link "$G/functions/ffs.adb" "$G/configs/b.1/f1"
evict_foreign_links || fail "eviction did not fire while f0=ptp was present"
is_link "$G/configs/b.1/f0" || fail "eviction removed the PTP interface 0 (iPhone layout)"
is_link "$G/configs/b.1/f2" || fail "eviction removed the CarPlay function"
is_link "$G/configs/b.1/f1" && fail "ffs.adb survived eviction next to PTP"
is_link "$G/configs/b.1/f0" && pass "PTP in slot f0 survives eviction, adb link still removed"

# A ptp/mtp link anywhere else is still foreign: only interface 0 is ours.
make_link "$G/functions/ptp.gs0" "$G/configs/b.1/f3"
evict_foreign_links || fail "eviction did not fire on a ptp link outside slot f0"
is_link "$G/configs/b.1/f3" && fail "a ptp link outside slot f0 survived eviction"
is_link "$G/configs/b.1/f0" || fail "evicting f3 also removed interface 0"
pass "ptp outside slot f0 is still evicted"

# Stock-only configuration (supervisor down): b.1 must keep its single function,
# otherwise the kernel rejects the only configuration and the gadget dies.
G=$tmp/g2
mkdir -p "$G/configs/b.1" "$G/functions/ffs.adb"
TGT=$tmp/targets2
mkdir -p "$TGT"
make_link "$G/functions/ffs.adb" "$G/configs/b.1/f1"
evict_foreign_links && fail "eviction touched a stock-only configuration"
is_link "$G/configs/b.1/f1" || fail "stock-only b.1 was emptied (kernel would reject the gadget)"
is_link "$G/configs/b.1/f1" && pass "stock-only layout left untouched"
TGT=$tmp/targets
G=$tmp/g1

# --- 3. role lock ----------------------------------------------------------
mkdir -p "$DWC3"
printf 'otg\n' > "$DWC3/mode"
role_apply || fail "role_apply failed on a writable mode node (rc=$?)"
[ "$(cat "$DWC3/mode")" = peripheral ] || fail "mode node did not become peripheral"
[ "$(cat "$DWC3/mode")" = peripheral ] && pass "role locked to peripheral via dwc3/mode"

# Already locked: no rewrite, no failure.
printf 'peripheral\n' > "$DWC3/mode"
role_apply || fail "role_apply failed when already peripheral (rc=$?)"
pass "role_apply is idempotent"

# No role node at all: must report, must not pretend.
DWC3=$tmp/dwc3-empty
mkdir -p "$DWC3"
if role_apply; then fail "role_apply claimed success without any role node"; else
    pass "missing role node reported as uncontrollable"
fi
grep -q 'role=uncontrollable' "$LOG" || fail "log does not record role=uncontrollable"

# Unknown vendor ABI must not be written blindly.
DWC3=$tmp/dwc3-vendor
mkdir -p "$DWC3"
printf '0\n' > "$DWC3/otg_mode"
before=$(cat "$DWC3/otg_mode")
role_apply || true
[ "$(cat "$DWC3/otg_mode")" = "$before" ] || fail "otg_mode was written with a guessed value"
[ "$(cat "$DWC3/otg_mode")" = "$before" ] && pass "unknown vendor ABI (otg_mode) never written blindly"

# --- 4. reset --------------------------------------------------------------
{
    i=1
    while [ "$i" -le 20 ]; do echo "field$i"; i=$((i + 1)); done
    echo adb
} > "$STATE_FILE"
PROPS_STORE="persist.sys.usb.config=none"
reset > /dev/null
[ "$(getprop persist.sys.usb.config)" = adb ] || fail "reset did not restore the captured default"
[ "$(getprop persist.sys.usb.config)" = adb ] && pass "reset restores persist default from usb-stock.state:21"

# `apply` composes the same three steps as sections 1-3; only its exit-code
# contract is worth pinning down, because the supervisor reads it: a kernel with
# no role node at all is "nothing to do here", not a failure, while a role node
# that exists and refuses the write is a real failure.
DWC3=$tmp/dwc3-empty
mkdir -p "$DWC3"
PROPS_STORE="sys.usb.configfs=carplay
sys.usb.config=carplay
persist.sys.usb.config=none"
rc=0
apply > /dev/null 2>&1 || rc=$?
[ "$rc" -eq 0 ] || fail "apply treated a missing role node as a failure (rc=$rc)"
[ "$rc" -eq 0 ] && pass "apply reports uncontrollable role as success (dts owns it)"

DWC3=$tmp/dwc3-readonly
mkdir -p "$DWC3"
printf 'otg\n' > "$DWC3/mode"
chmod 444 "$DWC3/mode"
rc=0
apply > /dev/null 2>&1 || rc=$?
[ "$rc" -ne 0 ] || fail "apply called an unwritable role node a success"
[ "$rc" -ne 0 ] && pass "apply fails when a role node exists but refuses the write"
chmod 644 "$DWC3/mode"
grep -q 'role=failed' "$LOG" || fail "log does not distinguish role=failed from uncontrollable"

if [ "$failures" -ne 0 ]; then
    echo "$failures check(s) failed" >&2
    exit 1
fi
echo "all usb_lock checks passed"
