#!/bin/sh
# Run with sh from the project root. Exercises production functions against
# temporary files; setprop emulates this board's two stock USB init branches.
set -eu
supervisor=${1:-deploy/opi4/scripts/carplay_supervisor.sh}
tmp=$(mktemp -d "${TMPDIR:-/tmp}/carplay-usb-ownership.XXXXXX")
trap 'rm -rf "$tmp"' EXIT HUP INT TERM
for fn in capture_stock_gadget write_stock_state stock_gadget_is_modified \
    restore_link restore_symlink restore_file ensure_carplay_directory \
    prepare_ffs ensure_carplay_config \
    attach_gadget restore_stock_gadget claim_usb_gadget release_usb_gadget \
    early_claim_gadget ensure_adb_transport evict_restocked_adb_function \
    setup_kernel_ncm release_kernel_ncm \
    setup_kernel_ptp release_kernel_ptp \
    detach_udc \
    udc_state udc_speed charger_type cc_state record_udc_state udc_observer \
    start_udc_observer stop_udc_observer; do
    sed -n "/^$fn() {/,/^}/p" "$supervisor" >> "$tmp/functions.sh"
done
. "$tmp/functions.sh"
log_event() { :; }
ROOT=$tmp/runtime
LAST_CONFIGURED_STAMP=$ROOT/last-configured.uptime
G=$tmp/g1
FFS=$tmp/ffs
STOCK_STATE_FILE=$ROOT/usb-stock.state
STOCK_CONFIG=$G/configs/b.1
# 1 when configuration 2 exists; prepare_ffs decides it and hands it to iap2d.
STANDBY_CONFIG=0
# Two configurations: b.1 = PTP-only standby, b.2 = CarPlay function set.
CARPLAY_CONFIG=$G/configs/b.2
OS_DESC=$G/os_desc
STOCK_OS_DESC_CONFIG_LINK=$OS_DESC/b.1
OS_DESC_CONFIG_LINK=$OS_DESC/b.2
LINK=$CARPLAY_CONFIG/f0
# Kernel NCM slots. The sandbox kernel has no "ncm" function registered, so
# setup_kernel_ncm() must fail cleanly and every caller keep going.
NCM_FUNC=$G/functions/ncm.usb0
NCM_LINK=$CARPLAY_CONFIG/f1
# Kernel PTP for interface 0 of the standby configuration. It lives in b.1
# alone: configfs binds every configuration at attach time, so a second PTP
# instance in b.2 would need three more endpoints the dwc3 core does not have.
PTP_FUNC=$G/functions/ptp.gs0
PTP_LINK=$STOCK_CONFIG/f0
# Rockchip's ptp function is alloc_inst_mtp_ptp(false): it reuses the MTP
# instance, so configfs refuses the link with "Create MTP function before
# linking PTP function with a gadget configuration" until functions/mtp.gs0
# exists. That instance stays unlinked -- it only provides /dev/mtp_usb.
PTP_MTP_FUNC=$G/functions/mtp.gs0
# Injected so setup_kernel_ncm() has values to write; the shipped constants are
# checked against Apple's OUI separately at the end of this file.
NCM_DEV_ADDR=00:a0:57:00:00:01
NCM_HOST_ADDR=00:a0:57:00:00:02
FFS_ADB_DEFAULT=../../../../usb_gadget/g1/functions/ffs.adb
STRINGS=$G/strings/0x409
UDC=fe800000.dwc3
APPLE_USB_MANUFACTURER='Apple Inc.'
APPLE_USB_PRODUCT=iPhone
APPLE_USB_SERIAL=0000802000000000000000000000000000000001
# Injected so attach_gadget() has a value to write; the shipped constant is
# checked against Apple's 500 mA ask separately at the end of this file.
APPLE_USB_MAX_POWER=250
# Injected so attach_gadget() has a value to write; the shipped constant is
# checked at the end of this file.
APPLE_BM_ATTRIBUTES=0xc0
ATTACHED=0
MODIFIED_GADGET=0
USB_CONFIGFS=1
USB_CONFIG=adb
LEGACY_ENABLES=0
FAIL_CLAIM=0
ORIGINAL_USB_CONFIGFS=
ORIGINAL_OS_DESC_USE=
ORIGINAL_OS_DESC_VENDOR_CODE=
ORIGINAL_OS_DESC_SIGNATURE=
ORIGINAL_OS_DESC_CONFIG=
ORIGINAL_STRING_MANUFACTURER=
ORIGINAL_STRING_PRODUCT=
ORIGINAL_STRING_SERIAL=
getprop() {
    case "$1" in
        sys.usb.configfs) printf '%s\n' "$USB_CONFIGFS" ;;
        sys.usb.config) printf '%s\n' "$USB_CONFIG" ;;
    esac
}
setprop() {
    case "$1" in
        sys.usb.configfs)
            [ "$FAIL_CLAIM" -eq 0 ] || return 1
            USB_CONFIGFS=$2 ;;
        sys.usb.config) USB_CONFIG=$2 ;;
    esac
    if [ "$USB_CONFIG" = adb ]; then
        case "$USB_CONFIGFS" in
            1) printf '0x0006\n' > "$G/idProduct" ;;
            0) LEGACY_ENABLES=$((LEGACY_ENABLES + 1)) ;;
        esac
    fi
}
log_event() { printf '%s\n' "$*"; }
mount() { return 0; }
umount() { return 0; }
mkdir -p "$ROOT" "$STOCK_CONFIG" "$OS_DESC" "$STRINGS" "$G/functions/ffs.adb" "$G/functions/ffs.iap2"
ln -s "$G/functions/ffs.adb" "$STOCK_CONFIG/f1"
ln -s "$STOCK_CONFIG" "$STOCK_OS_DESC_CONFIG_LINK"
printf '0x2207\n' > "$G/idVendor"
printf '0x0006\n' > "$G/idProduct"
printf '0x0200\n' > "$G/bcdUSB"
for attr in bDeviceClass bDeviceSubClass bDeviceProtocol; do printf '0x00\n' > "$G/$attr"; done
printf '%s\n' "$UDC" > "$G/UDC"
printf 'rockchip\n' > "$STRINGS/manufacturer"
printf 'rk3399-mid\n' > "$STRINGS/product"
printf 'original-serial\n' > "$STRINGS/serialnumber"
# The stock Android gadget asks the host for 1000 mA (bMaxPower 500); a real
# iPhone asks for 500 mA (bMaxPower 250). A head unit that budgets its port
# from this field can refuse the whole device, so the accessory path pins it
# to Apple's value and the restore must put the stock one back.
printf '500\n' > "$STOCK_CONFIG/MaxPower"
# Stock Android advertises 0x80 (bus-powered). A real iPhone and the DDBox010
# sample both advertise 0xc0 (self-powered, no remote wakeup). A head unit that
# budgets its downstream port reads this byte during enumeration, so the
# accessory path pins it and the restore must put the stock value back.
printf '0x80\n' > "$STOCK_CONFIG/bmAttributes"
# The stock image enables os_desc for Windows adb/RNDIS; the CarPlay attach
# must switch it off, otherwise Windows asks vendor code 0x52 for the MS OS
# compat descriptor and rejects the device with Code 10.
printf '1\n' > "$OS_DESC/use"
printf '1\n' > "$OS_DESC/b_vendor_code"
printf 'MSFT100\n' > "$OS_DESC/qw_sign"
capture_stock_gadget
# Recreate an existing 20-line snapshot to prove migration appends fields and
# preserves the old USB strings and OS descriptor values.
head -n 20 "$STOCK_STATE_FILE" > "$tmp/legacy.state"
cp "$tmp/legacy.state" "$STOCK_STATE_FILE"
ORIGINAL_USB_CONFIGFS=
capture_stock_gadget
prepare_ffs
attach_gadget
[ "$(cat "$OS_DESC/use")" = 0 ] || {
    echo 'FAIL: MS OS descriptors still advertised after attach' >&2; exit 1;
}
[ "$(cat "$OS_DESC/b_vendor_code")" = 1 ] || {
    echo 'FAIL: leftover CarPlay vendor code kept in os_desc' >&2; exit 1;
}
[ ! -L "$OS_DESC_CONFIG_LINK" ] || {
    echo 'FAIL: CarPlay config linked into os_desc' >&2; exit 1;
}
# Configuration 1 is the standby and must be PTP-only. That is the shape
# DDBox010 advertises first: the host selects it, recognises an Apple device,
# and only then asks for the CarPlay configuration by value 2. Leaving PTP,
# iAP2, ethernet and NCM in one configuration is what left the A4L head unit
# with nowhere to switch to -- it accepted configuration 1 and went silent.
[ ! -e "$STOCK_CONFIG/f1" ] && [ ! -L "$STOCK_CONFIG/f1" ] || {
    echo 'FAIL: stock adb function still attached to configuration 1' >&2; exit 1;
}
[ "$(readlink "$STOCK_CONFIG/f0")" = "$PTP_FUNC" ] || {
    echo 'FAIL: standby configuration 1 is not PTP-only' >&2; exit 1;
}
[ -d "$PTP_FUNC" ] || {
    echo 'FAIL: kernel ptp function was not created' >&2; exit 1;
}
# Configuration 2 carries the CarPlay function set and deliberately has no PTP
# of its own: configfs binds every configuration up front, and dwc3 exposes
# only 8 IN / 8 OUT -- a second 3-endpoint PTP would push the total past it.
[ -d "$CARPLAY_CONFIG" ] || {
    echo 'FAIL: CarPlay configuration 2 was not created' >&2; exit 1;
}
[ "$(readlink "$CARPLAY_CONFIG/f0")" = "$G/functions/ffs.iap2" ] || {
    echo 'FAIL: CarPlay function not advertised as configuration 2' >&2; exit 1;
}
# The MTP instance is ptp's prerequisite and must be created first, or the link
# above silently fails with EINVAL and the whole attach aborts.
[ -d "$PTP_MTP_FUNC" ] || {
    echo 'FAIL: ptp prerequisite MTP instance was not created' >&2; exit 1;
}
mtp_line=$(grep -n 'mkdir "$PTP_MTP_FUNC"' "$supervisor" | cut -d: -f1)
ptp_mkdir_line=$(grep -n 'mkdir "$PTP_FUNC"' "$supervisor" | cut -d: -f1)
[ -n "$mtp_line" ] && [ -n "$ptp_mkdir_line" ] && [ "$mtp_line" -lt "$ptp_mkdir_line" ] || {
    echo 'FAIL: MTP instance must be created before the ptp function' >&2; exit 1;
}
# A kernel without CONFIG_USB_CONFIGFS_F_PTP registers no "ptp" function and
# mkdir fails with ENOENT; that must degrade, not abort the attach.
grep -q 'setup_kernel_ptp || true' "$supervisor" || {
    echo 'FAIL: attach must survive a kernel without the ptp function' >&2; exit 1;
}
# Same for the whole multi-configuration path: if configuration 2 cannot be
# created the attach falls back to the single-configuration layout instead of
# leaving the gadget half-applied.
grep -q 'single-configuration fallback in b.1' "$supervisor" || {
    echo 'FAIL: attach must fall back to one configuration when b.2 cannot be created' >&2; exit 1;
}
# iap2d cannot probe configfs itself (its SELinux domain cannot stat
# /config/usb_gadget), so the layout has to be handed over. Without it the
# daemon reads "configuration 2 not enabled yet" as a dead session and reopens
# every endpoint file once a minute -- which delays the switch to CarPlay by a
# whole backoff step.
grep -q 'CARPLAY_STANDBY_CONFIG=$STANDBY_CONFIG' "$supervisor" || {
    echo 'FAIL: the daemon must be told which gadget layout it is describing' >&2; exit 1;
}
[ "$STANDBY_CONFIG" -eq 1 ] || {
    echo 'FAIL: prepare_ffs must record the two-configuration layout' >&2; exit 1;
}
[ "$(readlink "$CARPLAY_CONFIG/f1")" = "$NCM_FUNC" ] || {
    echo 'FAIL: kernel NCM function not linked after ffs.iap2' >&2; exit 1;
}
# Both configurations advertise the same power envelope; a stock 1000 mA ask on
# b.2 would contradict b.1 on the same device.
[ "$(cat "$CARPLAY_CONFIG/MaxPower")" = 250 ] || {
    echo 'FAIL: configuration 2 still asks for the stock 1000 mA' >&2; exit 1;
}
[ "$(cat "$CARPLAY_CONFIG/bmAttributes")" = 0xc0 ] || {
    echo 'FAIL: configuration 2 is still bus-powered' >&2; exit 1;
}
for cycle in 1 2 3; do setprop sys.usb.config adb; done
[ "$(cat "$G/idProduct")" = 0x12A8 ] || {
    echo 'FAIL: replaying stock adb property overwrote CarPlay PID' >&2; exit 1;
}
# bMaxPower is in 2 mA units: 500 => 1000 mA (the stock Android ask), 250 =>
# 500 mA (what an iPhone asks for). Leaving the stock value makes the box look
# like an Android device drawing twice the current a CarPlay phone ever does.
[ "$(cat "$STOCK_CONFIG/MaxPower")" = 250 ] || {
    echo 'FAIL: accessory path still asks the host for the stock 1000 mA' >&2; exit 1;
}
# bmAttributes bit 6 is the self-powered flag. The accessory path must claim
# self-powered (0xc0), because that is what both working samples report and
# because a port that measures the draw of a bus-powered device against a
# 500 mA budget will power it down when the two ends disagree.
[ "$(cat "$STOCK_CONFIG/bmAttributes")" = 0xc0 ] || {
    echo 'FAIL: accessory path still advertises the stock bus-powered bmAttributes' >&2; exit 1;
}
[ "$LEGACY_ENABLES" -eq 0 ] || { echo 'FAIL: legacy USB driver was enabled' >&2; exit 1; }
[ "$USB_CONFIGFS" = carplay ] || { echo 'FAIL: stock init still owns configfs' >&2; exit 1; }
head -n 20 "$STOCK_STATE_FILE" > "$tmp/migrated-prefix.state"
cmp "$tmp/legacy.state" "$tmp/migrated-prefix.state"
[ "$(sed -n '21p' "$STOCK_STATE_FILE")" = 1 ]
# Field 22 records the stock b.1/f1 target so restore can put adb back.
[ -n "$(sed -n '22p' "$STOCK_STATE_FILE")" ] || {
    echo 'FAIL: stock configuration-1 function target was not captured' >&2; exit 1;
}
# Field 23 records the stock bMaxPower; append-only, like every field after 20.
[ "$(sed -n '23p' "$STOCK_STATE_FILE")" = 500 ] || {
    echo 'FAIL: stock bMaxPower was not captured in the snapshot' >&2; exit 1;
}
# Field 24 records the stock bmAttributes, append-only like the rest.
[ "$(sed -n '24p' "$STOCK_STATE_FILE")" = 0x80 ] || {
    echo 'FAIL: stock bmAttributes was not captured in the snapshot' >&2; exit 1;
}
# A restart while already owned must retain the saved original gate.
ORIGINAL_USB_CONFIGFS=
capture_stock_gadget
[ "$ORIGINAL_USB_CONFIGFS" = 1 ]
restore_stock_gadget
[ "$USB_CONFIGFS" = 1 ]
[ "$(cat "$G/idVendor")" = 0x2207 ]
[ "$(cat "$G/idProduct")" = 0x0006 ]
[ "$(cat "$STRINGS/product")" = rk3399-mid ]
[ "$(cat "$OS_DESC/use")" = 1 ] || {
    echo 'FAIL: stock os_desc state was not restored' >&2; exit 1;
}
[ "$(readlink "$STOCK_OS_DESC_CONFIG_LINK")" = "$STOCK_CONFIG" ]
[ ! -L "$OS_DESC_CONFIG_LINK" ]
[ -L "$STOCK_CONFIG/f1" ] || {
    echo 'FAIL: stock adb function was not restored to configuration 1' >&2; exit 1;
}
[ "$(readlink "$STOCK_CONFIG/f1")" = "$G/functions/ffs.adb" ] || {
    echo 'FAIL: restored configuration 1 points at the wrong function' >&2; exit 1;
}
# The current budget is part of the stock layout: leaving 250 behind would make
# the adb gadget under-declare its draw after a restore.
[ "$(cat "$STOCK_CONFIG/MaxPower")" = 500 ] || {
    echo 'FAIL: stock bMaxPower was not restored' >&2; exit 1;
}
# Same for bmAttributes: leaving 0xc0 behind makes the restored adb gadget
# claim to be self-powered, which the stock image never does.
[ "$(cat "$STOCK_CONFIG/bmAttributes")" = 0x80 ] || {
    echo 'FAIL: stock bmAttributes was not restored' >&2; exit 1;
}
# Configuration 2 is ours entirely: the directory must be gone after a restore,
# or the next host would still be told bNumConfigurations 2.
[ ! -d "$CARPLAY_CONFIG" ] || {
    echo 'FAIL: CarPlay configuration 2 survived the stock restore' >&2; exit 1;
}
[ ! -L "$STOCK_CONFIG/f4" ] || {
    echo 'FAIL: stale function survived in configuration 1' >&2; exit 1;
}
[ ! -L "$STOCK_CONFIG/f5" ] || {
    echo 'FAIL: stale function survived in configuration 1' >&2; exit 1;
}
# The ptp function object must go too: a leftover instance would keep
# /dev/mtp_usb registered and make the next attach reuse a half-torn-down
# function.
[ ! -L "$STOCK_CONFIG/f0" ] || {
    echo 'FAIL: kernel ptp function survived the stock restore' >&2; exit 1;
}
[ ! -d "$PTP_FUNC" ] || {
    echo 'FAIL: kernel ptp function object was not released' >&2; exit 1;
}
# ...and its MTP prerequisite, which we created. Leaving it behind would keep
# /dev/mtp_usb registered for a function the stock gadget never had.
[ ! -d "$PTP_MTP_FUNC" ] || {
    echo 'FAIL: ptp prerequisite MTP instance was not released' >&2; exit 1;
}
# Failing to reserve ownership must not proceed to a USB rebind.
FAIL_CLAIM=1
if prepare_ffs; then echo 'FAIL: prepare accepted a rejected ownership property' >&2; exit 1; fi
[ "$(cat "$G/idProduct")" = 0x0006 ]

# /init.usb.configfs.rc:21 fires on sys.usb.ffs.ready=1 while configfs is still
# 1, about 16 s into boot. It links ffs.adb into configuration 1 and binds the
# UDC, so the head unit enumerates a 2207:0006 ADB gadget and settles on
# Android Auto / CarLife before the Apple descriptor exists. The claim must
# park the property and detach the UDC in that same window.
FAIL_CLAIM=0
USB_CONFIGFS=1
printf '%s\n' "$UDC" > "$G/UDC"
early_claim_gadget >/dev/null 2>&1 || {
    echo 'FAIL: early claim was rejected' >&2; exit 1;
}
[ "$(getprop sys.usb.configfs)" = carplay ] || {
    echo 'FAIL: early claim did not park configfs before adbd reported ready' >&2; exit 1;
}
# configfs detaches the controller only on an empty write. A non-empty token
# such as "none" is rejected and the UDC stays bound; idVendor then fails with
# EBUSY while bcdUSB (no driver check) still lands, leaving an iPhone VID with
# the stock 0x0210 bcdUSB and making the host ask for a BOS descriptor.
[ -z "$(cat "$G/UDC")" ] || {
    echo 'FAIL: early claim left the UDC bound, so the host could still enumerate adb' >&2; exit 1;
}
# A replayed stock branch must be evicted again, otherwise configuration 1
# advertises 0xFF/0x42/0x01 next to the Apple interfaces.
ln -s "$G/functions/ffs.adb" "$STOCK_CONFIG/f1"
evict_restocked_adb_function >/dev/null 2>&1 || {
    echo 'FAIL: restocked adb function was not evicted' >&2; exit 1;
}
[ ! -L "$STOCK_CONFIG/f1" ] || {
    echo 'FAIL: restocked adb function survived inside configuration 1' >&2; exit 1;
}

# A head unit that walks away leaves no error, so the observer must record how
# far enumeration got: one line per transition, never per poll.
UDC_SYSFS=$tmp/udc
EXTCON_SYSFS=$tmp/extcon
UDC_OBSERVE_INTERVAL=0.2
USB_STATE_LOG=$ROOT/logs/usb-state.log
UDC_OBSERVER=
mkdir -p "$UDC_SYSFS/$UDC"
printf 'not attached\n' > "$UDC_SYSFS/$UDC/state"
printf 'unknown\n' > "$UDC_SYSFS/$UDC/current_speed"
mkdir -p "$EXTCON_SYSFS/extcon1"
printf 'USB=1\nUSB_HOST=0\nSDP=0\nDCP=1\n' > "$EXTCON_SYSFS/extcon1/state"
start_udc_observer
[ -n "$UDC_OBSERVER" ] || { echo 'FAIL: observer did not start' >&2; exit 1; }
[ -f "$ROOT/udc-observer.pid" ] || { echo 'FAIL: observer pid file missing' >&2; exit 1; }
sleep 0.4
printf 'addressed\n' > "$UDC_SYSFS/$UDC/state"
sleep 0.4
printf 'configured\n' > "$UDC_SYSFS/$UDC/state"
printf 'high-speed\n' > "$UDC_SYSFS/$UDC/current_speed"
sleep 0.5
# A head unit that presents as a dedicated charger never moves the UDC out of
# "not attached"; the charger verdict is the only signal that anything happened.
printf 'USB=1\nUSB_HOST=0\nSDP=1\nDCP=0\n' > "$EXTCON_SYSFS/extcon1/state"
sleep 0.5
# CC is a separate wire pair from D+/D-.  Losing it while the BC1.2 verdict
# stays put is exactly the A-to-C cable without a pull-up: the host never sees
# us, yet the charger type keeps reporting a data-capable port.
printf 'USB=0\nUSB_HOST=0\nSDP=1\nDCP=0\n' > "$EXTCON_SYSFS/extcon1/state"
sleep 0.5
stop_udc_observer
[ -z "$UDC_OBSERVER" ] || { echo 'FAIL: observer not cleared' >&2; exit 1; }
[ ! -f "$ROOT/udc-observer.pid" ] || { echo 'FAIL: observer pid file left' >&2; exit 1; }
grep -q -- '--- session start' "$USB_STATE_LOG" || {
    echo 'FAIL: observer did not mark the session start' >&2; exit 1;
}
[ "$(grep -c 'state=not attached' "$USB_STATE_LOG")" = 1 ] || {
    echo 'FAIL: unchanged state was recorded more than once' >&2; exit 1;
}
grep -q 'state=not attached speed=unknown charger=DCP cc=on' "$USB_STATE_LOG" || {
    echo 'FAIL: charger type or CC state missing from the initial record' >&2; exit 1;
}
grep -q 'state=addressed' "$USB_STATE_LOG" || {
    echo 'FAIL: addressed transition missing' >&2; exit 1;
}
grep -q 'state=configured speed=high-speed' "$USB_STATE_LOG" || {
    echo 'FAIL: configured transition missing' >&2; exit 1;
}
grep -q 'state=configured speed=high-speed charger=SDP' "$USB_STATE_LOG" || {
    echo 'FAIL: charger change was not recorded' >&2; exit 1;
}
grep -q 'charger=SDP cc=off' "$USB_STATE_LOG" || {
    echo 'FAIL: CC loss was not recorded separately from the charger verdict' >&2; exit 1;
}
# Guard the detach helper at the source level: a non-empty token leaves the
# UDC bound, and a bound UDC accepts bcdUSB but rejects idVendor, so the whole
# descriptor set silently degrades to "iPhone VID + stock bcdUSB".
if grep -q 'echo none > "$G/UDC"' "$supervisor"; then
    echo 'FAIL: supervisor detaches the UDC with a non-empty token' >&2; exit 1
fi
# bcdDevice is BCD-encoded (each nibble 0..9, so 0x0A00 is rejected by
# configfs) with the iOS major version in the high byte. A head unit that
# reads 0x0100 sees an iOS 1.x phone, i.e. one predating CarPlay (iOS 7.1),
# and drops the device without ever issuing the 0xC0/0x53 probe.
#
# 0x0901 is measured, not assumed: it is what a DDBox010 reports, and that
# box drives CarPlay on the very A4L head unit that ignores us. This path
# shipped 0x1200 (iOS 12) until that sample was available, and the head unit's
# behaviour with 0x1200 is indistinguishable from its behaviour with 0x0100 --
# enumerates, accepts configuration 1, then goes silent. Track the sample.
grep -q 'echo 0x0901 > "$G/bcdDevice"' "$supervisor" || {
    echo 'FAIL: accessory path no longer pins bcdDevice to the DDBox010 value 0x0901' >&2; exit 1
}

# iSerialNumber is a 40-hex-digit UDID on every Apple device, and on the
# DDBox010 sample. The 12-character alphanumeric string this used to ship was
# chosen for Windows device-instance keys, and it is the one device-descriptor
# field a head unit can compare against a genuine phone.
shipped_serial=$(sed -n 's/^APPLE_USB_SERIAL="\(.*\)"$/\1/p' "$supervisor" | head -1)
case "$shipped_serial" in
    *[!0-9a-f]*) echo "FAIL: APPLE_USB_SERIAL '$shipped_serial' is not lowercase hex" >&2; exit 1 ;;
esac
[ ${#shipped_serial} -eq 40 ] || {
    echo "FAIL: APPLE_USB_SERIAL must be 40 hex digits (got ${#shipped_serial}: '$shipped_serial')" >&2; exit 1
}

# bmAttributes must be pinned and snapshotted; both key facts are asserted by
# behaviour above, these two keep the constants from drifting silently.
grep -qF 'echo "$APPLE_BM_ATTRIBUTES" > "$STOCK_CONFIG/bmAttributes"' "$supervisor" || {
    echo 'FAIL: accessory path does not declare the configuration self-powered' >&2; exit 1
}
grep -q 'STOCK_BM_ATTRIBUTES' "$supervisor" || {
    echo 'FAIL: bmAttributes is not snapshotted, so restore cannot put it back' >&2; exit 1
}

# The NCM interface must carry an Apple-assigned MAC. A real iPhone reports one
# through the CDC Ethernet iMacAddress string; the kernel default is a random
# locally-administered address (d6:a6:.., a fresh one every boot) that no Apple
# device ever shows, which is a fingerprint a strict head unit can act on.
# Values are injected here so the assertion tests the write path, then the
# shipped constants are checked against the OUI independently.
mkdir -p "$G/functions"
setup_kernel_ncm >/dev/null 2>&1 || true
[ "$(cat "$NCM_FUNC/dev_addr" 2>/dev/null || true)" = "$NCM_DEV_ADDR" ] || {
    echo 'FAIL: setup_kernel_ncm does not pin dev_addr' >&2; exit 1;
}
[ "$(cat "$NCM_FUNC/host_addr" 2>/dev/null || true)" = "$NCM_HOST_ADDR" ] || {
    echo 'FAIL: setup_kernel_ncm does not pin host_addr' >&2; exit 1;
}
for var in NCM_DEV_ADDR NCM_HOST_ADDR; do
    value=$(sed -n "s/^$var=//p" "$supervisor" | head -1)
    case "$value" in
        00:a0:57:*) ;;
        *) echo "FAIL: $var is not in Apple's 00:a0:57 OUI (got '$value')" >&2; exit 1 ;;
    esac
done
[ "$NCM_DEV_ADDR" != "$NCM_HOST_ADDR" ] || {
    echo 'FAIL: dev_addr and host_addr must differ, they are the two ends of the link' >&2; exit 1;
}
grep -q 'Apple OUI' "$supervisor" || {
    echo 'FAIL: the Apple OUI choice is no longer explained where it is applied' >&2; exit 1;
}

# bMaxPower in the shipped script must be Apple's 500 mA ask, not the stock
# Android 1000 mA. Anything else is a number a head unit can budget against
# and reject.
shipped_power=$(sed -n 's/^APPLE_USB_MAX_POWER=//p' "$supervisor" | head -1)
[ "$shipped_power" = 250 ] || {
    echo "FAIL: APPLE_USB_MAX_POWER is '$shipped_power', must be 250 (500 mA)" >&2; exit 1
}
grep -q 'STOCK_MAX_POWER' "$supervisor" || {
    echo 'FAIL: bMaxPower is no longer snapshotted, so restore cannot put it back' >&2; exit 1
}

echo 'supervisor USB ownership tests PASS (adb replay, migration, two configurations (b.1 PTP-only standby + b.2 CarPlay, layout flag passed to the daemon), PTP interface 0 + MTP prerequisite, os_desc off/restore, claim failure, early claim before adb enumeration, restocked adb eviction, udc detach, udc observer, charger type, cc state, NCM Apple OUI, bMaxPower 500 mA, bmAttributes self-powered, bcdDevice 0x0901, UDID serial)'
