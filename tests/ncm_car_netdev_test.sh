#!/bin/sh
# Guard for how the host role finds the head unit's network interface.
#
# Two different netdevs exist in this product and only one of them is ever
# relevant at a time:
#
#   device role  our gadget's kernel NCM function (functions/ncm.usb0) makes
#                usb%d, which is what the *car's* cdc_ncm binds to;
#   host role    the car's own CDC-NCM function is bound by *our* kernel's
#                cdc_ncm driver, and the netdev is the car's.
#
# Reusing the device-role lookup on the host path is wrong twice over: it
# returns early whenever our gadget function directory is absent (which is
# always, once the gadget has been torn down for the role switch), and
# otherwise it configures whichever interface happens to be called usb0.  The
# head unit then has no addressed interface and AirPlay has no transport, while
# the log says nothing more useful than "no netdev yet".
#
# So the host path identifies the interface through sysfs: the device link of
# /sys/class/net/<if> must resolve to an interface of the USB device we just
# claimed, and the driver must be cdc_ncm.  These guards pin that.
#
# Run with sh from the project root.
set -eu
source_file=${1:-src/ncm_bridge.c}
caller=${2:-src/iap2d.c}
[ -f "$source_file" ] || { echo "FAIL: missing $source_file" >&2; exit 1; }
[ -f "$caller" ] || { echo "FAIL: missing $caller" >&2; exit 1; }

fail() { echo "FAIL: $*" >&2; exit 1; }
pass() { echo "ok - $*"; }

code=$(sed 's://.*::' "$source_file" | sed '/^[[:space:]]*\*/d; /^[[:space:]]*\/\*/d')
caller_code=$(sed 's://.*::' "$caller" | sed '/^[[:space:]]*\*/d; /^[[:space:]]*\/\*/d')

fn=$(printf '%s\n' "$code" | grep -n 'int ncm_bridge_car_netdev_up' | head -1 | cut -d: -f1)
[ -n "$fn" ] || fail "ncm_bridge_car_netdev_up() is gone; the host role has no way to address the car's netdev"

# The identification is sysfs + driver, not a name.
body=$(printf '%s\n' "$code" | tail -n +1 | grep -n 'netdev_belongs_to' | head -1 | cut -d: -f1)
[ -n "$body" ] || fail "netdev_belongs_to() is gone; the netdev would be picked by name again"
printf '%s\n' "$code" | grep -q 'cdc_ncm' ||
	fail "the driver check is gone; eth0/wlan0 would be configurable"
printf '%s\n' "$code" | grep -q '/sys/class/net/%s/device' ||
	fail "the netdev is no longer matched back to its USB device"
printf '%s\n' "$code" | grep -q 'strstr(target, prefix)' ||
	fail "the claimed device name is not compared against the interface's sysfs name"

# It must not be gated on our own gadget layout, which is what made the old
# path return "no transport" in host mode.
start=$(printf '%s\n' "$code" | grep -n 'int ncm_bridge_car_netdev_up' | head -1 | cut -d: -f1)
tail=$(printf '%s\n' "$code" | sed -n "${start},/^}/p")
printf '%s\n' "$tail" | grep -q 'g_kernel_ncm' &&
	fail "ncm_bridge_car_netdev_up() consults g_kernel_ncm; that flag describes our gadget, not the car"

# And host mode must actually call it, with the claimed device name.  Scoped to
# run_host_mode() because the device path legitimately keeps the old lookup.
host=$(printf '%s\n' "$caller_code" | grep -n 'static int run_host_mode' | head -1 | cut -d: -f1)
[ -n "$host" ] || fail "run_host_mode() is gone"
host_body=$(printf '%s\n' "$caller_code" | sed -n "${host},/^}/p")
printf '%s\n' "$host_body" | grep -q 'ncm_bridge_car_netdev_up(g_host.sysfs_name' ||
	fail "run_host_mode() does not call ncm_bridge_car_netdev_up() with the claimed device"
printf '%s\n' "$host_body" | grep -q 'ncm_bridge_netdev_up()' &&
	fail "run_host_mode() still uses the device-role lookup"

pass "the host role addresses the head unit's cdc_ncm netdev, found through sysfs"
pass "all host NCM guards hold"
