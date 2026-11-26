#!/bin/sh
# Guards the one mistake flash_boot.sh exists to prevent: writing an image to
# the wrong partition.
#
# boot and kernel share the same KRNL container, so the header checks alone
# cannot tell them apart -- only the payload can.  A gzip ramdisk on the kernel
# partition, or a raw Image on the boot partition, both pass the magic and
# length checks and both brick the box.  This test pins the payload rule:
#
#   boot   -> must start with the gzip header 1f 8b
#   kernel -> must NOT be gzip, and must carry "ARMd" at payload offset 0x38
#             (the arm64 Image magic 0x644d5241, little endian)
#
# The images are synthesized so the test needs no partition, no root and no
# device: it runs on Windows, in WSL and on the box itself.
#
# Run with sh from the project root.
set -eu
script=${1:-deploy/opi4/scripts/flash_boot.sh}
fail() { echo "FAIL: $*" >&2; exit 1; }

[ -f "$script" ] || fail "script not found: $script"

tmp=${TMPDIR:-/tmp}/flashpart.$$
# Cleanup must never decide the exit status: this workspace wraps `rm` in a
# safe-delete shim that can refuse the path, and a trap that returns non-zero
# would turn a passing run into a failure.
trap 'rm -rf "$tmp" 2>/dev/null || true' EXIT INT TERM
mkdir -p "$tmp"

# 1100 KiB of zeros with a KRNL header claiming a 1 MiB payload: big enough to
# clear the "far too small" guard, small enough to be instant.
mk_img() {
    dd if=/dev/zero of="$1" bs=1024 count=1100 2>/dev/null || fail "dd failed"
    printf 'KRNL' | dd of="$1" bs=1 seek=0 conv=notrunc 2>/dev/null || fail "header write failed"
    # 0x00100000 little endian
    printf '\000\000\020\000' | dd of="$1" bs=1 seek=4 conv=notrunc 2>/dev/null || fail "length write failed"
}

run() { # expected_exit, then the rest is the command line
    want=$1; shift
    set +e
    out=$("$@" 2>&1)
    got=$?
    set -e
    [ "$got" = "$want" ] || fail "expected exit $want, got $got from: $* [ $out ]"
    printf '%s\n' "$out"
}

boot_img=$tmp/boot.img
kernel_img=$tmp/kernel.img

mk_img "$boot_img"
printf '\037\213' | dd of="$boot_img" bs=1 seek=8 conv=notrunc 2>/dev/null || fail "gzip header write failed"

mk_img "$kernel_img"
# "ARMd" is exactly the four bytes of 0x644d5241 in little endian, at 8 + 0x38.
printf 'ARMd' | dd of="$kernel_img" bs=1 seek=64 conv=notrunc 2>/dev/null || fail "arm64 magic write failed"

echo "== boot image into the boot partition"
run 0 sh "$script" --check "$boot_img" --part boot >/dev/null

echo "== kernel image into the kernel partition"
run 0 sh "$script" --check "$kernel_img" --part kernel >/dev/null

echo "== boot image into the kernel partition must be refused"
out=$(run 1 sh "$script" --check "$boot_img" --part kernel)
echo "$out" | grep -q 'gzip' || fail "refusal did not mention gzip: $out"

echo "== kernel image into the boot partition must be refused"
out=$(run 1 sh "$script" --check "$kernel_img" --part boot)
echo "$out" | grep -q 'gzip' || fail "refusal did not mention gzip: $out"

echo "== a kernel payload without the ARMd magic must be refused"
printf 'XXXX' | dd of="$kernel_img" bs=1 seek=64 conv=notrunc 2>/dev/null
out=$(run 1 sh "$script" --check "$kernel_img" --part kernel)
echo "$out" | grep -q 'ARM' || fail "refusal did not mention the arm64 magic: $out"

echo "== an unknown partition name must be refused"
run 1 sh "$script" --check "$boot_img" --part system >/dev/null

echo "== a payload longer than the file must be refused"
mk_img "$tmp/short.img"
printf '\377\377\377\377' | dd of="$tmp/short.img" bs=1 seek=4 conv=notrunc 2>/dev/null
run 1 sh "$script" --check "$tmp/short.img" --part boot >/dev/null

echo "== a file that is not a KRNL container must be refused"
dd if=/dev/zero of="$tmp/plain.img" bs=1024 count=1100 2>/dev/null
printf 'NOPE' | dd of="$tmp/plain.img" bs=1 seek=0 conv=notrunc 2>/dev/null
run 1 sh "$script" --check "$tmp/plain.img" --part boot >/dev/null

echo "== default partition is boot"
run 0 sh "$script" --check "$boot_img" >/dev/null

echo "PASS: flash_boot.sh tells boot and kernel payloads apart"
