#!/bin/bash
# Build the box's arm64 kernel Image inside a Rockchip SDK kernel tree.
#
#   bash build-kernel-image.sh <sdk-root> [output-dir]
#
# <sdk-root> is the directory that holds kernel/ and prebuilts/, e.g.
#   /path/to/RK3399-Android8.1
#
# What it guarantees
# ------------------
# * Uses the toolchain the vendor shipped (gcc-linaro-6.3.1-2017.05) instead of
#   whatever happens to be in PATH, because the kernel was released with it.
# * Re-checks the resulting .config for CONFIG_USB_CONFIGFS_NCM=y after
#   rockchip_defconfig runs.  defconfig silently drops options whose
#   dependencies are unset, and a kernel without NCM would flash fine and then
#   quietly keep the old TUN-only descriptor layout.
# * Verifies the produced Image really is an uncompressed arm64 Image before
#   handing it on, so a bad build never reaches rk_kernelimg.py.
#
# The result is a plain Image; wrap it with tools/rk_kernelimg.py.
set -eu

SDK=${1:-}
OUT=${2:-$PWD/kernel-out}
if [ -z "$SDK" ] || [ ! -d "$SDK/kernel" ]; then
    echo "usage: bash build-kernel-image.sh <sdk-root> [output-dir]" >&2
    echo "  <sdk-root> must contain kernel/ (got: ${SDK:-<empty>})" >&2
    exit 2
fi
SDK=$(cd "$SDK" && pwd)
K=$SDK/kernel

TC=$SDK/prebuilts/gcc/linux-x86/aarch64/gcc-linaro-6.3.1-2017.05-x86_64_aarch64-linux-gnu/bin
if [ ! -x "$TC/aarch64-linux-gnu-gcc" ]; then
    echo "cross toolchain not found under $TC" >&2
    exit 2
fi
# flex/bison are only needed if kconfig regenerates its parser; a local prefix
# keeps the host from needing apt.
export PATH=$TC:$PATH
export ARCH=arm64
export CROSS_COMPILE=aarch64-linux-gnu-
export LC_ALL=C

# arm64 routes every compile through scripts/gcc-wrapper.py, which runs under
# the bare name `python`.  Ubuntu 20.04 ships python2 but not that name, and
# without it *every* cc-option probe fails -- the build then silently decides
# the compiler lacks flags it actually supports.  A private shim keeps the fix
# local instead of rewriting /usr/bin.
mkdir -p "$OUT/.bin"
if ! command -v python >/dev/null 2>&1; then
    for py in python2 python2.7; do
        if command -v "$py" >/dev/null 2>&1; then
            ln -sf "$(command -v "$py")" "$OUT/.bin/python"
            break
        fi
    done
fi
export PATH=$OUT/.bin:$PATH
command -v python >/dev/null 2>&1 ||
    { echo "no python2 available for scripts/gcc-wrapper.py (install python2)" >&2; exit 2; }

echo "== toolchain"
"$TC/aarch64-linux-gnu-gcc" --version | head -1
echo "== kernel"
head -4 "$K/Makefile" | tr '\n' ' '; echo

cd "$K"
echo "== defconfig"
make rockchip_defconfig

for sym in CONFIG_USB_CONFIGFS_NCM CONFIG_USB_F_NCM CONFIG_USB_U_ETHER CONFIG_USB_CONFIGFS; do
    if grep -qx "$sym=y" .config; then
        echo "   $sym=y"
    else
        echo "   FAILED: $sym is not enabled after rockchip_defconfig" >&2
        grep -n "$sym" .config >&2 || true
        exit 1
    fi
done

echo "== build ($(nproc) jobs)"
make Image -j"$(nproc)"

IMG=$K/arch/arm64/boot/Image
[ -f "$IMG" ] || { echo "no $IMG produced" >&2; exit 1; }

echo "== verify Image header"
python3 - "$IMG" <<'PY'
import struct, sys, pathlib
p = pathlib.Path(sys.argv[1])
d = p.read_bytes()
if d[:2] == b"\x1f\x8b":
    sys.exit("Image is gzip compressed; the bootloader expects it raw")
if len(d) < 0x3C:
    sys.exit("Image is too small to carry an arm64 header")
magic = struct.unpack_from("<I", d, 0x38)[0]
if magic != 0x644D5241:
    sys.exit(f"bad arm64 magic at 0x38: {magic:#x}")
print(f"   size        : {len(d)} bytes")
print(f"   text_offset : {struct.unpack_from('<Q', d, 0x08)[0]:#x}")
print(f"   image_size  : {struct.unpack_from('<Q', d, 0x10)[0]}")
PY

mkdir -p "$OUT"
cp -f "$IMG" "$OUT/Image"
cp -f "$K/.config" "$OUT/config-used"
echo "== done"
echo "   $OUT/Image"
echo "   $OUT/config-used"
echo "   next: python tools/rk_kernelimg.py -i <original-kernel.img> -o <new-kernel.img> --image $OUT/Image"
