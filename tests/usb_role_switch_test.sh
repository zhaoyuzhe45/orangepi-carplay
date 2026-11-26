#!/bin/sh
# Exercises usb_role_switch.sh against a fake sysfs/configfs tree.
#
# The point is the ordering, not the device: the gadget has to be unbound before
# the port is handed to the host stack, and it has to be rebound on the way back
# when (and only when) a configuration still links a function.  Both failures are
# invisible in dmesg and only show up as "the car sees nothing", so they are
# cheaper to catch here.
#
# Run with sh from the project root.
set -eu
script=${1:-deploy/opi4/scripts/usb_role_switch.sh}
tmp=$(mktemp -d "${TMPDIR:-/tmp}/usb-role-switch.XXXXXX")
trap 'rm -rf "$tmp"' EXIT HUP INT TERM

fail() { echo "FAIL: $*" >&2; exit 1; }
pass() { echo "ok - $*"; }

# ---- fake hardware -------------------------------------------------------
mkdir -p "$tmp/udc/fe800000.dwc3" "$tmp/xhci" "$tmp/usbbus"
echo "not attached" > "$tmp/udc/fe800000.dwc3/state"
echo peripheral > "$tmp/mode"
mkdir -p "$tmp/g1/configs/b.1" "$tmp/g1/configs/b.2"
ln -s /nonexistent "$tmp/g1/configs/b.2/f0"
echo fe800000.dwc3 > "$tmp/g1/UDC"

run() {
	MODE_PATH=$tmp/mode GADGET=$tmp/g1 UDC_DIR=$tmp/udc \
	XHCI_DIR=$tmp/xhci USB_BUS=$tmp/usbbus SWITCH_TRIES=3 \
	sh "$script" "$@"
}

# ---- status --------------------------------------------------------------
out=$(run --status)
case $out in
*"mode      : peripheral"*) ;;
*) fail "status did not report peripheral: $out" ;;
esac
case $out in
*"udc       : fe800000.dwc3"*) pass "status reports the bound UDC" ;;
*) fail "status lost the UDC binding: $out" ;;
esac

# ---- host ----------------------------------------------------------------
run --host >/dev/null
[ "$(cat "$tmp/mode")" = host ] || fail "mode is $(cat "$tmp/mode"), expected host"
[ -z "$(cat "$tmp/g1/UDC")" ] || fail "gadget still bound after switching to host"
pass "--host unbinds the gadget, then switches the port"

# ---- device --------------------------------------------------------------
run --device >/dev/null
[ "$(cat "$tmp/mode")" = peripheral ] || fail "mode is $(cat "$tmp/mode"), expected peripheral"
[ "$(cat "$tmp/g1/UDC")" = fe800000.dwc3 ] ||
	fail "gadget was not rebound on the way back"
pass "--device switches back and rebinds the gadget"

# ---- refusal -------------------------------------------------------------
echo "" > "$tmp/g1/UDC"
rm -f "$tmp/g1/configs/b.2/f0"          # no function anywhere: must not bind
mkdir -p "$tmp/g1/configs/b.3"
run --host >/dev/null
run --device >/dev/null
[ -z "$(cat "$tmp/g1/UDC")" ] ||
	fail "bound the UDC although no configuration links a function"
pass "leaves the UDC free when no configuration has a function"

# ---- unwritable mode -----------------------------------------------------
chmod 444 "$tmp/mode"
if run --host 2>/dev/null; then
	fail "switching to host succeeded although dwc3_mode was read-only"
fi
pass "fails loudly when the kernel will not accept the switch"

echo "all role switch checks passed"
