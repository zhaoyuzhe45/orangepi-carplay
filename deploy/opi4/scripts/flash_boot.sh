#!/system/bin/sh
# flash_boot.sh -- write a verified image to the boot or kernel partition, on device.
#
# Why a script instead of "dd whatever": these are the two partitions that turn
# this box into a brick, and the recovery path (maskrom, or pulling the SD card)
# is far worse than a five-second check.  So this refuses to write unless the
# image passes the same structural checks the loader will apply, and it always
# keeps a copy of what it is about to overwrite.
#
# The image is written while Android is running.  Neither partition is mounted,
# so this is safe; the new image simply takes effect on the next start.
#
# The two partitions carry the same KRNL container but a different payload, so
# the payload check differs: boot holds a gzip ramdisk, kernel holds the raw
# arm64 Image.  Passing the wrong image to the wrong partition is exactly the
# mistake this catches.
#
# Usage:
#   flash_boot.sh --check  <img> [--part boot|kernel]   verify only, write nothing
#   flash_boot.sh --apply  <img> [--part boot|kernel]   verify, back up, write
#   flash_boot.sh --rollback [--part boot|kernel]       restore the saved backup
#   flash_boot.sh --status   [--part boot|kernel]       show partition and backup state
#
# --part defaults to boot, or to $FLASH_PART when that is exported.
#
# Exit codes: 0 ok / 1 usage or verification failure / 2 write failure

set -eu

say()  { echo "[flash_boot] $*"; }
die()  { echo "[flash_boot] ERROR: $*" >&2; exit 1; }

require_root() {
    # Every path below writes to a block device; failing cleanly beats failing
    # halfway through a dd.
    [ "$(id -u 2>/dev/null || echo 1)" = 0 ] ||
        die "needs root: run it as  su 0 sh $0 $cmd ${IMG:-} --part $TARGET"
}

cmd=${1:-}
[ -n "$cmd" ] || die "usage: $0 --check|--apply <img> [--part boot|kernel] | --rollback | --status"
shift || true

TARGET=${FLASH_PART:-boot}
IMG=
while [ $# -gt 0 ]; do
    case "$1" in
        --part) [ -n "${2:-}" ] || die "--part needs a value"; TARGET=$2; shift 2 ;;
        -*) die "unknown option: $1" ;;
        *) IMG=$1; shift ;;
    esac
done

case "$TARGET" in
    boot|kernel) ;;
    *) die "unknown partition '$TARGET' (expected boot or kernel)" ;;
esac

PART_DEV=/dev/block/by-name/$TARGET
BACKUP_DIR=/data/local/tmp/$TARGET-backup
BACKUP_IMG=$BACKUP_DIR/$TARGET.img

part_size_bytes() {
    # blockdev is the direct answer; sysfs is the fallback for older toybox.
    if blockdev --getsize64 "$PART_DEV" 2>/dev/null; then
        return 0
    fi
    dev=$(basename "$(readlink "$PART_DEV" 2>/dev/null)" 2>/dev/null) || return 1
    [ -n "$dev" ] || return 1
    sectors=$(cat "/sys/class/block/$dev/size" 2>/dev/null) || return 1
    [ -n "$sectors" ] || return 1
    echo $((sectors * 512))
}

check_payload() {
    img=$1
    if [ "$TARGET" = boot ]; then
        # A ramdisk is gzip, and gzip always starts with 1f 8b.
        gz=$(od -An -tx1 -j 8 -N 2 "$img" 2>/dev/null | tr -d ' \n')
        [ "$gz" = "1f8b" ] || die "payload does not start with a gzip header (got '$gz'); is this the kernel image?"
        say "check   : magic, length, fit and gzip header all OK"
        return 0
    fi
    # kernel: a raw arm64 Image, which must NOT be compressed and carries the
    # ASCII magic "ARM\x64" (0x644d5241) at offset 0x38 of the payload, i.e.
    # bytes 41 52 4d 64 at absolute 8 + 0x38 = 64.  Comparing bytes instead of
    # a number keeps this independent of how od renders word order.
    gz=$(od -An -tx1 -j 8 -N 2 "$img" 2>/dev/null | tr -d ' \n')
    [ "$gz" != "1f8b" ] || die "payload is gzip compressed; the kernel partition wants a raw Image"
    arm=$(od -An -tx1 -j 64 -N 4 "$img" 2>/dev/null | tr -d ' \n')
    [ "$arm" = "41524d64" ] ||
        die "no ARM\\x64 magic at payload offset 0x38 (got '$arm'); not a kernel Image"
    say "check   : magic, length, fit and arm64 Image header all OK"
}

check_image() {
    img=$1
    [ -f "$img" ] || die "no such file: $img"

    size=$(wc -c < "$img" | tr -d ' ')
    [ "$size" -ge 1048576 ] || die "image is only $size bytes, far too small"

    magic=$(dd if="$img" bs=4 count=1 2>/dev/null)
    [ "$magic" = "KRNL" ] || die "bad magic '$magic', expected KRNL"

    plen=$(od -An -tu4 -j 4 -N 4 "$img" 2>/dev/null | tr -d ' ')
    [ -n "$plen" ] || die "cannot read the payload length field"
    total=$((8 + plen))
    [ "$total" -le "$size" ] || die "payload claims $plen bytes but the file holds only $size"
    say "image    : $img ($size bytes, payload $plen)"
    say "partition: $TARGET ($PART_DEV)"

    part_bytes=$(part_size_bytes 2>/dev/null || echo "")
    if [ -n "${part_bytes:-}" ]; then
        say "capacity : $part_bytes bytes"
        [ "$total" -le "$part_bytes" ] ||
            die "payload $total bytes does not fit the $part_bytes byte partition"
    else
        say "capacity : size unknown, skipping the fit check"
    fi

    check_payload "$img"
}

do_status() {
    say "partition: $TARGET ($PART_DEV -> $(readlink -f "$PART_DEV" 2>/dev/null || echo '?'))"
    say "capacity : $(part_size_bytes 2>/dev/null || echo '?') bytes"
    if [ -f "$BACKUP_IMG" ]; then
        say "backup   : $BACKUP_IMG ($(wc -c < "$BACKUP_IMG" | tr -d ' ') bytes, \
$(od -An -tx1 -j 0 -N 4 "$BACKUP_IMG" 2>/dev/null | tr -d ' \n'))"
    else
        say "backup   : none yet"
    fi
}

do_backup() {
    mkdir -p "$BACKUP_DIR"
    if [ -f "$BACKUP_IMG" ]; then
        say "backup   : already present, keeping the original ($BACKUP_IMG)"
        return 0
    fi
    say "backup   : dumping $PART_DEV -> $BACKUP_IMG"
    dd if="$PART_DEV" of="$BACKUP_IMG" bs=1048576 2>/dev/null || die "backup dump failed"
    chmod 644 "$BACKUP_IMG"
    say "backup   : $(wc -c < "$BACKUP_IMG" | tr -d ' ') bytes saved"
}

do_apply() {
    [ -n "${IMG:-}" ] || die "usage: $0 --apply <img> [--part boot|kernel]"
    require_root "$@"
    check_image "$IMG"
    do_backup

    cur=$(od -An -tx1 -j 0 -N 4 "$PART_DEV" 2>/dev/null | tr -d ' \n')
    say "current  : first 4 bytes of the partition are '$cur'"

    say "writing  : $IMG -> $PART_DEV"
    dd if="$IMG" of="$PART_DEV" bs=1048576 2>/dev/null || die "write failed (partition untouched?)"

    # Read it back: a silent short write is the difference between booting and
    # not booting, and dd would have reported success either way.
    newmagic=$(od -An -tx1 -j 0 -N 4 "$PART_DEV" 2>/dev/null | tr -d ' \n')
    [ "$newmagic" = "4b524e4c" ] || die "verify failed: partition now starts with '$newmagic'"
    newlen=$(od -An -tu4 -j 4 -N 4 "$PART_DEV" 2>/dev/null | tr -d ' ')
    want=$(od -An -tu4 -j 4 -N 4 "$IMG" 2>/dev/null | tr -d ' ')
    [ "$newlen" = "$want" ] || die "verify failed: payload length $newlen != $want"
    say "verify   : partition read-back matches the image"
    say ""
    say "Done. Reboot to apply:  su 0 reboot"
    say "Rollback if needed:     su 0 sh $0 --rollback --part $TARGET"
}

do_rollback() {
    require_root "$@"
    [ -f "$BACKUP_IMG" ] || die "no backup at $BACKUP_IMG, cannot roll back"
    check_image "$BACKUP_IMG"
    say "rollback : $BACKUP_IMG -> $PART_DEV"
    dd if="$BACKUP_IMG" of="$PART_DEV" bs=1048576 2>/dev/null || die "rollback write failed"
    say "rollback : done, reboot to apply"
}

case "$cmd" in
    --check)    [ -n "${IMG:-}" ] || die "usage: $0 --check <img> [--part boot|kernel]"
                check_image "$IMG" ;;
    --apply)    do_apply ;;
    --rollback) do_rollback ;;
    --status)   do_status ;;
    *) echo "usage: $0 --check|--apply <img> [--part boot|kernel] | --rollback | --status" >&2; exit 1 ;;
esac
