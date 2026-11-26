#!/bin/sh
set -eu
script=${1:-deploy/opi4/scripts/usb_role_switch.sh}
tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT HUP INT TERM
mkdir -p "$tmp/g1/configs/b.1" "$tmp/udc/fe800000.dwc3" "$tmp/xhci" "$tmp/bus"
ln -s /not-needed "$tmp/g1/configs/b.1/f0"
echo peripheral > "$tmp/mode"
echo fe800000.dwc3 > "$tmp/g1/UDC"
echo 'not attached' > "$tmp/udc/fe800000.dwc3/state"
export MODE_PATH=$tmp/mode GADGET=$tmp/g1 UDC_DIR=$tmp/udc XHCI_DIR=$tmp/xhci USB_BUS=$tmp/bus
sh "$script" --host
sh "$script" --host
[ "$(cat "$tmp/mode")" = host ]
[ -z "$(cat "$tmp/g1/UDC")" ]
sh "$script" --device
sh "$script" --device
[ "$(cat "$tmp/g1/UDC")" = fe800000.dwc3 ]
echo 'role switches are idempotent'
