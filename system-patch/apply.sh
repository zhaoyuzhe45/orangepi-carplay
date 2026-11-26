#!/bin/sh
# Apply the product's USB hardening to an OrangePi 4 LTS Android 8.1 tree.
#
#   sh apply.sh <source-root> [--dry-run] [--adb-secure] [--force] [--kernel-only]
#
# <source-root> is the directory holding system/, device/ and kernel/
# (e.g. D:/Android/OrangePi_4_LTS_Android_8.1/src/RK3399-Android8.1).
#
# Every step is idempotent: a second run reports "already applied" and changes
# nothing. Each step is also verified after the fact, and the script exits
# non-zero if anything did not land, so a build never silently proceeds with a
# half-patched tree.
#
# Why these changes exist (see README.md for the full reasoning):
#   S1  system/core/rootdir/init.usb.configfs.rc   no USB function switching
#   S2  device/rockchip/rk3399/init.rk30board.usb.rc  no os_desc / MS OS ids
#   S3  device/rockchip/common/device.mk           persist.sys.usb.config=none
#   S4  device/rockchip/common/system.prop         adbd TCP transport only
#   S5  kernel/.../rk3399-orangepi-4-lts.dts       dr_mode = "peripheral"
#   S6  kernel/arch/arm64/configs/rockchip_defconfig  CONFIG_USB_CONFIGFS_NCM=y
#   S7  kernel/drivers/usb/gadget/composite.c      stop forcing bcdUSB 0x0210
set -eu

ROOT=
DRY_RUN=0
ADB_SECURE=0
FORCE=0
KERNEL_ONLY=0
for arg in "$@"; do
    case "$arg" in
        --dry-run) DRY_RUN=1 ;;
        --adb-secure) ADB_SECURE=1 ;;
        --force) FORCE=1 ;;
        --kernel-only) KERNEL_ONLY=1 ;;
        -*) echo "unknown option: $arg" >&2; exit 2 ;;
        *) ROOT=$arg ;;
    esac
done
[ -n "$ROOT" ] || { echo "usage: sh apply.sh <source-root> [--dry-run] [--adb-secure] [--force] [--kernel-only]" >&2; exit 2; }
[ -d "$ROOT" ] || { echo "not a directory: $ROOT" >&2; exit 2; }

HERE=$(cd "$(dirname "$0")" && pwd)
FILES=$HERE/files
CHANGED=0
FAILED=0

say() { printf '%s\n' "$*"; }
apply_note() {
    if [ "$DRY_RUN" -eq 1 ]; then
        say "  would apply: $*"
    else
        say "  applied: $*"
        CHANGED=$((CHANGED + 1))
    fi
}
skip_note() { say "  already applied: $*"; }
fail_note() { say "  FAILED: $*"; FAILED=$((FAILED + 1)); }

sha() {
    if command -v sha256sum >/dev/null 2>&1; then sha256sum "$1" | cut -d' ' -f1
    else shasum -a 256 "$1" | cut -d' ' -f1; fi
}

backup_once() {
    [ "$DRY_RUN" -eq 1 ] && return 0
    [ -f "$1.orig" ] || cp -p "$1" "$1.orig"
}

# ---------------------------------------------------------------- S1 / S2
# Whole-file replacement, guarded by a hash of the expected original. A vendor
# tree that differs from the SDK this was written against must not be silently
# overwritten: the operator gets a clear message instead.
replace_file() {
    src=$1
    dst=$2
    expect=$3
    label=$4
    [ -f "$dst" ] || { fail_note "$label: missing $dst"; return 0; }
    want_sha=$(sha "$src")
    [ "$(sha "$dst")" = "$want_sha" ] && { skip_note "$label"; return 0; }
    have_sha=$(sha "$dst")
    if [ -n "$expect" ] && [ "$have_sha" != "$expect" ] && [ "$FORCE" -ne 1 ]; then
        fail_note "$label: $dst is not the expected original"
        say "        expected $expect"
        say "        found    $have_sha"
        say "        re-run with --force only after diffing $(basename "$dst") against $src"
        return 0
    fi
    backup_once "$dst"
    if [ "$DRY_RUN" -eq 1 ]; then
        say "  would replace: $label"
    else
        cp "$src" "$dst"
        [ "$(sha "$dst")" = "$want_sha" ] || { fail_note "$label: copy did not verify"; return 0; }
        apply_note "$label"
    fi
}

# --kernel-only exists because the kernel partition is the only artifact that
# still has to be rebuilt: S1-S4 already ship inside the ramdisk we reflashed,
# so a tree that carries nothing but kernel/ must still be able to run this
# script and land S5-S7 instead of reporting four "missing file" failures.
if [ "$KERNEL_ONLY" -eq 1 ]; then
    say "== S1-S4  skipped (--kernel-only; they live in the boot ramdisk)"
else
say "== S1  init.usb.configfs.rc"
replace_file "$FILES/init.usb.configfs.rc" \
    "$ROOT/system/core/rootdir/init.usb.configfs.rc" \
    91c43bba03244570734928474571b069c59256a0f1eee71934ffa99954e1bf4a \
    "no USB function switching (adb/mtp/ptp/rndis/midi branches removed)"

say "== S2  init.rk30board.usb.rc"
replace_file "$FILES/init.rk30board.usb.rc" \
    "$ROOT/device/rockchip/rk3399/init.rk30board.usb.rc" \
    3cf83253e281fc4d69aac05315c0f1170a6751e48d4bf940e58a72ae5bcfbdec \
    "no os_desc / MS OS compatible ID, no sys.usb.config handlers"

# ---------------------------------------------------------------- S3
say "== S3  device.mk: persist.sys.usb.config=none"
MK=$ROOT/device/rockchip/common/device.mk
if [ ! -f "$MK" ]; then
    fail_note "S3: missing $MK"
elif ! grep -qE '^[[:space:]]*persist\.sys\.usb\.config=(adb|mtp|ptp|mass_storage|rndis|midi)' "$MK" \
     && grep -qE '^[[:space:]]*persist\.sys\.usb\.config=none' "$MK"; then
    # Every assignment (this makefile has one per UMS/CDROM branch) is already
    # none: nothing left to do.
    skip_note "all persist.sys.usb.config lines already none"
else
    # Preserve a trailing continuation backslash: only the value is replaced.
    backup_once "$MK"
    sed -E 's|^([[:space:]]*persist\.sys\.usb\.config=)[^\\]*|\1none  |' "$MK" > "$MK.new"
    if [ "$DRY_RUN" -eq 1 ]; then say "  would set persist.sys.usb.config=none"
    else
        mv "$MK.new" "$MK"
        if grep -q '^[[:space:]]*persist\.sys\.usb\.config=none' "$MK"; then
            apply_note "persist.sys.usb.config -> none (framework never asks for adb/mtp)"
        else
            fail_note "S3: replacement did not verify"
        fi
    fi
fi

# ---------------------------------------------------------------- S4
say "== S4  system.prop: adbd TCP transport, insecure adb note"
SP=$ROOT/device/rockchip/common/system.prop
MARKER='# --- box: USB ownership ---'
if [ ! -f "$SP" ]; then
    fail_note "S4: missing $SP"
elif grep -qF "$MARKER" "$SP"; then
    skip_note "USB ownership block present"
else
    backup_once "$SP"
    if [ "$DRY_RUN" -eq 1 ]; then
        say "  would append USB ownership block"
    else
        {
            printf '\n%s\n' "$MARKER"
            printf '%s\n' '# USB is owned by carplay_supervisor.sh; the framework must not ask'
            printf '%s\n' '# for adb or mtp. Every such request used to be a chance for init to'
            printf '%s\n' '# relink ffs.adb into configuration 1 while a head unit was enumerating.'
            printf '%s\n' 'persist.sys.usb.config=none'
            printf '%s\n' '# adbd keeps only its TCP transport; frp publishes 5555 for maintenance.'
            printf '%s\n' 'persist.adb.tcp.port=5555'
        } >> "$SP"
        apply_note "persist.adb.tcp.port=5555 (and persist.sys.usb.config=none)"
    fi
fi

if [ "$ADB_SECURE" -eq 1 ]; then
    say "== S4b ro.adb.secure=1 (opt-in)"
    if grep -q '^[[:space:]]*ro\.adb\.secure[[:space:]]*=[[:space:]]*1' "$SP"; then
        skip_note "ro.adb.secure already 1"
    else
        backup_once "$SP"
        if [ "$DRY_RUN" -eq 1 ]; then say "  would set ro.adb.secure=1"
        else
            sed -E 's|^[[:space:]]*ro\.adb\.secure[[:space:]]*=.*|ro.adb.secure=1|' "$SP" > "$SP.new"
            mv "$SP.new" "$SP"
            apply_note "ro.adb.secure=1 (requires /data/misc/adb/adb_keys to already hold your key)"
        fi
    fi
fi

fi

# ---------------------------------------------------------------- S5
say "== S5  dts: &usbdrd_dwc3_0 dr_mode = peripheral"
DTS=$ROOT/kernel/arch/arm64/boot/dts/rockchip/rk3399-orangepi-4-lts.dts
if [ ! -f "$DTS" ]; then
    fail_note "S5: missing $DTS"
elif awk '/^&usbdrd_dwc3_0 \{$/{b=1;next} b&&/^\};/{b=0} b&&/dr_mode/{print "yes"; exit}' "$DTS" | grep -q yes; then
    skip_note "dr_mode already set on usbdrd_dwc3_0"
else
    backup_once "$DTS"
    if [ "$DRY_RUN" -eq 1 ]; then say "  would lock the OTG controller to peripheral"
    else
        awk '
            /^&usbdrd_dwc3_0 \{$/ { inblk = 1; print; next }
            inblk && /^\};$/ {
                print "\t/* box: always the USB device. The Type-C fusb302 extcon can"
                print "\t * promote this controller to host depending on CC orientation,"
                print "\t * and a host-mode controller has no gadget at all -- the head"
                print "\t * unit would see nothing rather than a failed enumeration."
                print "\t */"
                print "\tdr_mode = \"peripheral\";"
                inblk = 0
                print
                next
            }
            { print }
        ' "$DTS" > "$DTS.new"
        mv "$DTS.new" "$DTS"
        if awk '/^&usbdrd_dwc3_0 \{$/{b=1;next} b&&/^\};/{b=0} b&&/dr_mode/{print "yes"; exit}' "$DTS" | grep -q yes; then
            apply_note 'dr_mode = "peripheral"'
        else
            fail_note "S5: dr_mode insertion did not verify"
        fi
    fi
fi

# ---------------------------------------------------------------- S6
say "== S6  rockchip_defconfig: CONFIG_USB_CONFIGFS_NCM=y"
DEF=$ROOT/kernel/arch/arm64/configs/rockchip_defconfig
if [ ! -f "$DEF" ]; then
    fail_note "S6: missing $DEF"
elif grep -qx 'CONFIG_USB_CONFIGFS_NCM=y' "$DEF"; then
    skip_note "CONFIG_USB_CONFIGFS_NCM already enabled"
else
    backup_once "$DEF"
    if [ "$DRY_RUN" -eq 1 ]; then say "  would enable CONFIG_USB_CONFIGFS_NCM"
    else
        # USB_CONFIGFS_NCM selects USB_U_ETHER and USB_F_NCM, so one line is enough.
        sed 's|^# CONFIG_USB_CONFIGFS_NCM is not set$|CONFIG_USB_CONFIGFS_NCM=y|' "$DEF" > "$DEF.new"
        mv "$DEF.new" "$DEF"
        if grep -qx 'CONFIG_USB_CONFIGFS_NCM=y' "$DEF"; then
            apply_note "CONFIG_USB_CONFIGFS_NCM=y (kernel NCM function, needed for CarPlay)"
        else
            fail_note "S6: CONFIG_USB_CONFIGFS_NCM line not found; check the defconfig"
        fi
    fi
fi

# ---------------------------------------------------------------- S7
say "== S7  composite.c: stop forcing bcdUSB 0x0210"
CC=$ROOT/kernel/drivers/usb/gadget/composite.c
MARK='/* box: do not force 0x0210 at high speed.'
if [ ! -f "$CC" ]; then
    fail_note "S7: missing $CC"
elif grep -qF "$MARK" "$CC"; then
    skip_note "bcdUSB clamp already present"
else
    backup_once "$CC"
    if [ "$DRY_RUN" -eq 1 ]; then say "  would clamp bcdUSB to the configured 0x0200"
    else
        awk -v mark="$MARK" '
            $0 ~ /cdev->desc\.bcdUSB = cpu_to_le16\(0x0210\);/ && !done {
                match($0, /^[ \t]*/); ind = substr($0, 1, RLENGTH)
                print ind mark
                print ind " * 0x0210 tells the host to read a BOS descriptor, and Windows"
                print ind " * then fetched the Rockchip MS OS"
                print ind " * compatible ID (\"RESTORE_MODE\"), enumerating this box as an"
                print ind " * Android ADB device -- five nodes, three with a yellow bang."
                print ind " * A real iPhone reports 0x0200 and offers no BOS, so keep the"
                print ind " * value configfs configured. */"
                print ind "if (cdev->desc.bcdUSB > cpu_to_le16(0x0200))"
                print ind "\tcdev->desc.bcdUSB = cpu_to_le16(0x0200);"
                done = 1
                next
            }
            { print }
        ' "$CC" > "$CC.new"
        mv "$CC.new" "$CC"
        if grep -qF "$MARK" "$CC"; then
            apply_note "bcdUSB stays at the configured 0x0200 at high speed"
        else
            fail_note "S7: the 0x0210 assignment was not found; kernel version differs"
        fi
    fi
fi

say ""
if [ "$FAILED" -ne 0 ]; then
    say "== $FAILED step(s) failed, $CHANGED applied"
    exit 1
fi
if [ "$DRY_RUN" -eq 1 ]; then
    say "== dry run complete"
else
    say "== $CHANGED step(s) applied"
    say "== now run: sh verify.sh $ROOT"
fi
