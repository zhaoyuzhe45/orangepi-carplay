#!/system/bin/sh
# usb_role_switch.sh -- move the Type-C port between peripheral and host.
#
# Why a script
# ------------
# Wired CarPlay starts with the car as host and ends with the car as a device:
# it sends the Apple vendor request 0x51 and both sides swap roles.  Until the
# device tree said dr_mode = "otg", the kernel refused the switch outright
# ("Not support set mode!"), so this board could never be the host half.
#
# Switching has three moving parts that are easy to get wrong in the wrong
# order, and all three are handled here:
#
#   1. the gadget must be unbound *first*; dwc3 cannot hand the port to the
#      host stack while ffs still owns the UDC,
#   2. dwc3_mode only accepts "host"/"peripheral" once, so the script checks
#      the mode it actually ends up in instead of trusting the write,
#   3. coming back needs the gadget rebound, otherwise the box stays invisible
#      to the next car even though the port looks fine.
#
# Usage
#   usb_role_switch.sh --status    print the current mode and who owns the UDC
#   usb_role_switch.sh --host      detach the gadget, switch the port to host
#   usb_role_switch.sh --device    switch back and rebind the gadget
#
# Exit codes: 0 ok, 1 usage, 2 the kernel refused the switch.
set -eu

MODE_PATH=${MODE_PATH:-/sys/devices/platform/usb0/dwc3_mode}
GADGET=${GADGET:-/config/usb_gadget/g1}
# The supervisor restarts FunctionFS before binding; manual callers retain
# the legacy rebind behavior. This controls gadget binding, not VBUS power.
GADGET_REBIND=${GADGET_REBIND:-1}
UDC_NAME=${UDC_NAME:-fe800000.dwc3}
UDC_DIR=${UDC_DIR:-/sys/class/udc}
XHCI_DIR=${XHCI_DIR:-/sys/bus/platform/drivers/xhci-hcd}
USB_BUS=${USB_BUS:-/sys/bus/usb/devices}
# 0.2 s per try, so 25 tries is the 5 s the USB spec allows a port to settle.
SWITCH_TRIES=${SWITCH_TRIES:-25}

say() { echo "[role] $*"; }
die() { echo "[role] $*" >&2; exit 2; }

role_read() {
	[ -r "$MODE_PATH" ] || return 1
	tr -d ' \n\r' < "$MODE_PATH"
}

role_write() {
	[ -w "$MODE_PATH" ] || return 1
	# The kernel rejects this with -EINVAL unless dr_mode was "otg" at boot.
	echo "$1" > "$MODE_PATH" 2>/dev/null || return 1
	return 0
}

wait_for_role() {
	want=$1
	i=0
	while [ "$i" -lt "$SWITCH_TRIES" ]; do
		if [ "$(role_read 2>/dev/null || true)" = "$want" ]; then
			return 0
		fi
		i=$((i + 1))
		sleep 0.2 2>/dev/null || sleep 1
	done
	return 1
}

gadget_udc() {
	[ -r "$GADGET/UDC" ] || return 1
	tr -d ' \n\r' < "$GADGET/UDC"
}

gadget_has_function() {
	# A configuration without a single function is rejected by the kernel
	# with -EINVAL, so rebinding blindly can leave the port dead.
	for cfg in "$GADGET"/configs/*; do
		[ -d "$cfg" ] || continue
		for slot in "$cfg"/*; do
			[ -L "$slot" ] || continue
			return 0
		done
	done
	return 1
}

detach_gadget() {
	[ -w "$GADGET/UDC" ] || return 0
	[ -n "$(gadget_udc 2>/dev/null || true)" ] || return 0
	say "detaching gadget from $UDC_NAME"
	echo "" > "$GADGET/UDC" || die "cannot unbind the UDC"
	i=0
	while [ -n "$(gadget_udc 2>/dev/null || true)" ] && [ "$i" -lt 25 ]; do
		i=$((i + 1))
		sleep 0.2 2>/dev/null || sleep 1
	done
	[ -z "$(gadget_udc 2>/dev/null || true)" ] || die "UDC stayed bound"
}

attach_gadget() {
	[ "$GADGET_REBIND" = 1 ] || return 0
	[ -w "$GADGET/UDC" ] || return 0
	[ -z "$(gadget_udc 2>/dev/null || true)" ] || return 0
	if ! gadget_has_function; then
		say "no function linked in any configuration; leaving the UDC free"
		return 0
	fi
	say "rebinding gadget to $UDC_NAME"
	echo "$UDC_NAME" > "$GADGET/UDC" || die "cannot bind the UDC"
}

count_hosts() {
	[ -d "$XHCI_DIR" ] || { echo 0; return 0; }
	find "$XHCI_DIR" -maxdepth 1 -name 'xhci-hcd*' 2>/dev/null | wc -l | tr -d ' '
}

switch_host() {
	if [ "$(role_read 2>/dev/null || true)" = "host" ]; then
		say "host mode already active"
		return 0
	fi
	[ "$(role_read 2>/dev/null || true)" = "otg" ] ||
		[ "$(role_read 2>/dev/null || true)" = "peripheral" ] ||
		die "unexpected mode '$(role_read 2>/dev/null || echo missing)'; dr_mode is probably not otg"
	detach_gadget
	say "switching $MODE_PATH -> host"
	role_write host || die "kernel refused the switch (dr_mode is not otg?)"
	wait_for_role host || die "mode did not become host"
	say "host mode active, xhci controllers: $(count_hosts)"
}

switch_device() {
	if [ "$(role_read 2>/dev/null || true)" = "peripheral" ]; then
		attach_gadget
		say "peripheral mode already active"
		return 0
	fi
	[ "$(role_read 2>/dev/null || true)" = "host" ] ||
		[ "$(role_read 2>/dev/null || true)" = "otg" ] ||
		die "unexpected mode '$(role_read 2>/dev/null || echo missing)'"
	say "switching $MODE_PATH -> peripheral"
	role_write peripheral || die "kernel refused the switch"
	wait_for_role peripheral || die "mode did not become peripheral"
	attach_gadget
	say "peripheral mode active, udc state: $(cat "$UDC_DIR/$UDC_NAME/state" 2>/dev/null || echo unknown)"
}

show_status() {
	say "mode      : $(role_read 2>/dev/null || echo 'unreadable (no otg support?)')"
	say "udc       : $(gadget_udc 2>/dev/null || echo 'unbound')"
	say "udc state : $(cat "$UDC_DIR/$UDC_NAME/state" 2>/dev/null || echo unknown)"
	say "host ctrl : $(count_hosts) xhci"
	say "usb devs  : $(ls "$USB_BUS" 2>/dev/null | grep -c ':' || true) interfaces"
}

case ${1:-} in
--status) show_status ;;
--host) switch_host ;;
--device) switch_device ;;
*) die "usage: $0 --status|--host|--device" ;;
esac
