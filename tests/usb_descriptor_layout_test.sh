#!/bin/sh
# Guards the USB interface layout iap2d hands to FunctionFS.
#
# The layout is not a free choice: it is what a real iPhone presents once it
# is in wired-CarPlay mode (captured from a real iPhone 8, see
# docs/evidence/iphone8-carplay-working-descriptors-2026-10-27.md):
#
#   0/0:06.01.01 x2   PTP                                 (Apple's is x3; see below)
#   1/0:ff.fe.02 x2   Apple USBMUX / "Apple Mobile Device"
#   2/0:ff.fd.01 x0   Apple USB Ethernet (ipheth), alt 0
#   2/1:ff.fd.01 x2       ... alt 1 carries the ipheth bulk pair
#   3/4               CDC-NCM control + data
#
# PTP is interface 0 on every sample that works -- iPhone 6, iPhone 8 on iOS 12
# and 16, and DDBox010 -- and iAP2 is interface 1 on all of them.  A head unit
# that indexes interfaces instead of matching class codes only finds iAP2 if
# this order is kept.  PTP here carries two bulk endpoints instead of Apple's
# three: the interrupt endpoint exists for MTP events no CarPlay session reads,
# and dwc3 has 8 IN / 8 OUT with the standby configuration's kernel ptp already
# holding two INs for the gadget's lifetime.
#
# Two things this layout encodes that the previous revision got wrong:
#
#   * Apple has no 0xFF/0x42/0x01 interface.  That combination was invented to
#     label a second, bare-iAP2 bulk pair, and a head unit looking for the
#     Apple Mobile Device function (ff/fe/02, interface 1, right behind PTP)
#     never found one.  The bare iAP2 link and the USBMUX link now share the
#     single ff/fe/02 pair and are demultiplexed by frame header, exactly like
#     a real phone.
#   * 0xFF/0xFD/0x01 must be present even though this box does not implement
#     ipheth.  Hosts use its presence to decide whether the configuration is
#     a CarPlay configuration at all -- DiPlay's IphoneCarPlayConfiguration
#     prefers exactly (usbmux && ncm && apple-ethernet) and falls back only
#     when it is missing.  Omitting it hands the head unit a configuration it
#     classifies as "charging + PTP".
#
# NOTE: a real iPhone also carries four CS_INTERFACE functional descriptors
# on the NCM control interface.  FunctionFS on this 4.4 kernel rejects any
# descriptor type other than INTERFACE/ENDPOINT (the ep0 write fails with
# EINVAL and no gadget comes up), so there are exactly two supported layouts
# and this test pins both:
#
#   kernel NCM  - configfs ncm.usb0 is linked into b.1 and emits the NCM
#                 interfaces (with the class descriptors); we describe only
#                 interfaces 0/1 and ep0 hands us two endpoints.
#   TUN         - no kernel function; we describe every interface ourselves and
#                 own the five endpoints.
#
# See docs/evidence/ncm-class-descriptors-2026-10-25.md.
#
# Run with sh from the project root.
set -eu
src=${1:-src/iap2d.c}
fail() { echo "FAIL: $*" >&2; exit 1; }

[ -f "$src" ] || fail "source not found: $src"

# Pull the interface/endpoint plan out of build_speed_block(), in the order it
# is written to ep0.  Comment lines are dropped so the prose in that function
# cannot satisfy an assertion.
plan=$(awk '/^static int build_speed_block/,/^}/' "$src" |
       tr -d '\r' |
       grep -E 'put_(intf|ep)\(' |
       sed -e 's/^[[:space:]]*//' -e 's/^s = //')

expected='put_intf(s, 0, 0, 2, 0x06, 0x01, 0x01, 0);
put_ep(s, 0x81, USB_ENDPOINT_XFER_BULK, bulk, 0);
put_ep(s, 0x04, USB_ENDPOINT_XFER_BULK, bulk, 0);
put_intf(s, 1, 0, 2, 0xFF, 0xFE, 0x02, 1);
put_ep(s, 0x83, USB_ENDPOINT_XFER_BULK, bulk, 0);
put_ep(s, 0x03, USB_ENDPOINT_XFER_BULK, bulk, 0);
put_intf(s, 2, 0, 0, 0xFF, 0xFD, 0x01, 2);
put_intf(s, 2, 1, 2, 0xFF, 0xFD, 0x01, 2);
put_ep(s, 0x86, USB_ENDPOINT_XFER_BULK, bulk, 0);
put_ep(s, 0x07, USB_ENDPOINT_XFER_BULK, bulk, 0);
put_intf(s, 3, 0, 1, 0x02, 0x0D, 0x00, 3);
put_ep(s, 0x84, USB_ENDPOINT_XFER_INT, intr, 8);
put_intf(s, 4, 0, 2, 0x0A, 0x00, 0x01, 4);
put_ep(s, 0x85, USB_ENDPOINT_XFER_BULK, bulk, 0);
put_ep(s, 0x05, USB_ENDPOINT_XFER_BULK, bulk, 0);'

[ "$plan" = "$expected" ] || {
    echo "--- actual ---" >&2
    printf '%s\n' "$plan" >&2
    echo "--- expected ---" >&2
    printf '%s\n' "$expected" >&2
    fail "descriptor plan drifted from the real-iPhone layout"
}

# The invented 0xFF/0x42/0x01 interface must never come back: it is the one
# interface combination no iPhone exposes, and it pushed the real ff/fe/02
# function from interface 1 to interface 2.
if grep -q '0xFF, 0x42, 0x01' "$src"; then
    fail 'ff/42/01 is back; no iPhone exposes that interface'
fi

# 6 interfaces (PTP, ff/fe/02, ff/fd/01 alt0, ff/fd/01 alt1, NCM control,
# NCM data) and 9 endpoints, advertised identically for FS and HS.  The TUN
# layout is the worst case, so it also sizes the staging buffer.
grep -q 'DESC_PER_SPEED (6 \* 9 + 9 \* 7)' "$src" ||
    fail 'DESC_PER_SPEED must account for 6 interfaces and 9 endpoints'
grep -q '#define DESC_IFACES_TUN        5' "$src" ||
    fail 'DESC_IFACES_TUN must be 5 (the alt1 descriptor is counted separately)'
grep -q '#define DESC_EPS_TUN           7' "$src" ||
    fail 'DESC_EPS_TUN must be 7'
grep -q '#define DESC_IFACES_ETH_ALT    1' "$src" ||
    fail 'DESC_IFACES_ETH_ALT must be 1'
grep -q '#define DESC_EPS_ETH_ALT       2' "$src" ||
    fail 'DESC_EPS_ETH_ALT must be 2'
if grep -q 'fs_count = 11' "$src"; then
    fail 'fs_count must follow the layout, not a fixed 11'
fi

# A rejected descriptor write must degrade to the marker-only layout instead of
# taking the gadget down: alt settings are exactly the thing older FunctionFS
# builds refuse, and a box that asks for a configuration it cannot describe
# would stop enumerating altogether.
grep -q 'retrying without the ff/fd/01 bulk pair' "$src" ||
    fail 'a refused alt descriptor must fall back to the alt0-only marker'
grep -q 'g_eth_alt = 0' "$src" ||
    fail 'the alt fallback must clear g_eth_alt before the retry'

# The NCM interfaces belong to the kernel whenever ncm.usb0 is linked in:
# describing them twice fails the bind. Both layouts must therefore exist, and
# the NCM block must sit behind the mode flag.
awk '/^static int build_speed_block/,/^}/' "$src" | tr -d '\r' |
    grep -q 'if (!g_kernel_ncm) {' ||
    fail 'the NCM interface block must be conditional on g_kernel_ncm'
grep -q 'g_kernel_ncm = ncm_bridge_kernel_available();' "$src" ||
    fail 'the NCM mode must be probed before the descriptors are written'

# Endpoint indices must match the plan above: the ff/fe/02 bulk pair comes
# first, then the ff/fd/01 ethernet pair, then the NCM trio in the TUN layout.
# A mismatch here maps the wrong fd onto iAP2.
grep -q '#define EP_COUNT      9' "$src" || fail 'EP_COUNT must be 9'
for pair in 'EP_PTP_IN:0' 'EP_PTP_OUT:1' 'EP_MUX_IN:2' 'EP_MUX_OUT:3' \
            'EP_ETH_IN:4' 'EP_ETH_OUT:5' 'EP_NCM_IRQ:6' 'EP_NCM_IN:7' \
            'EP_NCM_OUT:8'; do
    name=${pair%:*}
    idx=${pair#*:}
    grep -Eq "^#define $name[[:space:]]+$idx($|[[:space:]])" "$src" ||
        fail "missing endpoint index: $name=$idx"
done
grep -q 'const int expected_eps = g_kernel_ncm ? (g_eth_alt ? 6 : 4) : EP_COUNT;' "$src" ||
    fail 'expected_eps must be 6 (or 4 without the eth pair) in kernel-NCM mode'
if grep -q 'if (n >= EP_COUNT) {' "$src"; then
    fail 'the endpoint map is gated on EP_COUNT; it must use expected_eps'
fi

# Interface names, position-indexed from 1; f_fs rewrites iInterface to the
# string id at bind time, so the numbers above and this table must stay in
# step:
#   0 (unnamed)               -> interface 0 (PTP; Apple leaves it anonymous)
#   1 "Apple USB Multiplexor" -> interface 1 (ff/fe/02)
#   2 "Apple USB Ethernet"    -> interface 2 (ff/fd/01)
#   3 "NCM Control"           -> interface 3 (TUN only)
#   4 "NCM Data"              -> interface 4 (TUN only)
if grep -q '\*p++ = 0; /\* iInterface \*/' "$src"; then
    fail 'iInterface is hard-wired to zero again; no iPhone omits interface names'
fi
for name in 'Apple USB Multiplexor' 'Apple USB Ethernet' 'NCM Control' 'NCM Data'; do
    grep -q "\"$name\"" "$src" || fail "interface string missing: $name"
done
table=$(awk '/^static const char \*const g_intf_strings\[\]/,/^};/' "$src" |
        tr -d '\r' | grep -o '"[^"]*"')
printf '%s\n' "$table" | sed -n '1p' | grep -qx '"Apple USB Multiplexor"' ||
    fail 'string position 1 (ff/fe/02) must be "Apple USB Multiplexor"'
printf '%s\n' "$table" | sed -n '2p' | grep -qx '"Apple USB Ethernet"' ||
    fail 'string position 2 (ff/fd/01) must be "Apple USB Ethernet"'
grep -q 'INTF_STRING_COUNT' "$src" ||
    fail 'the interface string table must be sized by a named constant'
grep -q 'sh->str_count = INTF_STRING_COUNT' "$src" ||
    fail 'the string table is no longer advertised to FunctionFS'
grep -q 'sh->lang_count = 1' "$src" ||
    fail 'the string table must advertise exactly one language (0x0409)'
grep -q 'first write failed\|retrying without interface names' "$src" ||
    fail 'a rejected string table must fall back to the empty one, not kill the gadget'

# Both transports share the one ff/fe/02 pair, so the reader has to split the
# stream by frame header: iAP2 link frames start with 0x40 0x40, USBMUX frames
# with 0xFE 0xED 0xFA 0xCE.  A daemon that routes by endpoint again would drop
# whichever transport lost the arbitration.
grep -q 'g_native_active' "$src" || fail 'native transport arbitration missing'
grep -q 'IAP2_FRAME_MAGIC\|usbmux_frame_magic\|frame_is_iap2' "$src" ||
    fail 'the shared ff/fe/02 pair must be demultiplexed by frame header'
grep -q 'pump_locked' "$src" || fail 'pump_locked missing'

echo 'usb descriptor layout test passed'
