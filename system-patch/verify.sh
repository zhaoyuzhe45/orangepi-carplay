#!/bin/sh
# Read-only check that the USB hardening is present in a source tree.
#
#   sh verify.sh <source-root> [--kernel-only]
#
# Prints one line per change and exits non-zero if any is missing. Safe to run
# at any time, including right before a build.
set -eu

ROOT=${1:-}
[ -n "$ROOT" ] || { echo "usage: sh verify.sh <source-root> [--kernel-only]" >&2; exit 2; }
[ -d "$ROOT" ] || { echo "not a directory: $ROOT" >&2; exit 2; }

# --kernel-only mirrors apply.sh: S1-S4 are ramdisk content, so a tree that
# carries only kernel/ is judged on S5-S7 alone.
KERNEL_ONLY=0
case "${2:-}" in --kernel-only) KERNEL_ONLY=1 ;; esac

FAILED=0
ok() { printf '  OK    %s\n' "$*"; }
no() { printf '  MISS  %s\n' "$*"; FAILED=$((FAILED + 1)); }

# check <label> <file> <test-expr-description> ; the test is supplied as a
# command so each check can be precise instead of one grep fits all.
check() {
    label=$1
    file=$2
    shift 2
    if [ ! -f "$file" ]; then
        no "$label (missing $(basename "$file"))"
        return 0
    fi
    if "$@" "$file"; then ok "$label"; else no "$label"; fi
}

# Comment lines explain why something is absent, so they must not satisfy a
# positive check nor defeat a negative one. Everything below looks at code only.
code() { grep -vE '^[[:space:]]*#' "$1" || true; }
has() { code "$2" | grep -qF "$1"; }
hasnt() { ! code "$2" | grep -qF "$1"; }
has_re() { code "$2" | grep -qE "$1"; }
hasnt_re() { ! code "$2" | grep -qE "$1"; }
# Marker checks that legitimately live in a comment.
raw_has() { grep -qF "$1" "$2"; }

if [ "$KERNEL_ONLY" -eq 1 ]; then
    echo "== S1-S4  skipped (--kernel-only; they live in the boot ramdisk)"
else
echo "== S1  system/core/rootdir/init.usb.configfs.rc"
F=$ROOT/system/core/rootdir/init.usb.configfs.rc
check "no adb/mtp/ptp/rndis function is linked" "$F" hasnt_re 'symlink .*functions/(ffs\.adb|mtp|ptp|rndis|midi)'
check "no stock branch binds the UDC" "$F" hasnt_re 'write /config/usb_gadget/g1/UDC'
check "no stock function is selected by property" "$F" hasnt_re 'sys\.usb\.config=(adb|mtp|ptp|rndis|midi)'
check "adbd is started for its TCP transport" "$F" has 'setprop service.adb.tcp.port 5555'
check "adbd is not stopped by the none branch" "$F" hasnt 'stop adbd'

echo "== S2  device/rockchip/rk3399/init.rk30board.usb.rc"
F=$ROOT/device/rockchip/rk3399/init.rk30board.usb.rc
check "no Microsoft OS descriptor vendor code" "$F" hasnt 'b_vendor_code'
check "no MSFT100 signature" "$F" hasnt 'MSFT100'
check "no os_desc/b.1 symlink" "$F" hasnt 'os_desc/b.1'
check "no sys.usb.config handlers" "$F" hasnt_re 'on property:sys\.usb\.config=(adb|mtp|ptp|rndis|midi)'
check "no mtp/ptp/rndis/midi function objects" "$F" hasnt_re 'functions/(mtp|ptp|rndis|midi)'
check "gadget skeleton still created" "$F" has 'mkdir /config/usb_gadget/g1/configs/b.1'
check "bcdUSB default is 0x0200" "$F" has 'write /config/usb_gadget/g1/bcdUSB 0x0200'

echo "== S3  device/rockchip/common/device.mk"
F=$ROOT/device/rockchip/common/device.mk
check "persist.sys.usb.config=none" "$F" has_re '^[[:space:]]*persist\.sys\.usb\.config=none'
check "no other persist.sys.usb.config value" "$F" hasnt_re '^[[:space:]]*persist\.sys\.usb\.config=(adb|mtp|ptp|mass_storage|rndis|midi)'

echo "== S4  device/rockchip/common/system.prop"
F=$ROOT/device/rockchip/common/system.prop
check "USB ownership block present" "$F" raw_has '# --- box: USB ownership ---'
check "adbd TCP port pinned" "$F" has 'persist.adb.tcp.port=5555'

fi

echo "== S5  rk3399-orangepi-4-lts.dts"
F=$ROOT/kernel/arch/arm64/boot/dts/rockchip/rk3399-orangepi-4-lts.dts
if [ -f "$F" ] && awk '/^&usbdrd_dwc3_0 \{$/{b=1;next} b&&/^\};/{b=0} b&&/dr_mode = "peripheral"/{print "yes"; exit}' "$F" | grep -q yes; then
    ok 'usbdrd_dwc3_0 locked to dr_mode = "peripheral"'
else
    no 'usbdrd_dwc3_0 is not locked to peripheral'
fi

echo "== S6  kernel/arch/arm64/configs/rockchip_defconfig"
F=$ROOT/kernel/arch/arm64/configs/rockchip_defconfig
check "CONFIG_USB_CONFIGFS_NCM=y" "$F" has_re '^CONFIG_USB_CONFIGFS_NCM=y$'
check "no stale 'not set' line" "$F" hasnt '# CONFIG_USB_CONFIGFS_NCM is not set'

echo "== S7  kernel/drivers/usb/gadget/composite.c"
F=$ROOT/kernel/drivers/usb/gadget/composite.c
check "bcdUSB clamp present" "$F" has '/* box: do not force 0x0210'
check "no unconditional 0x0210 assignment left" "$F" hasnt 'cdev->desc.bcdUSB = cpu_to_le16(0x0210);'

echo ""
if [ "$FAILED" -ne 0 ]; then
    echo "== $FAILED check(s) missing; run apply.sh"
    exit 1
fi
echo "== all checks passed"
