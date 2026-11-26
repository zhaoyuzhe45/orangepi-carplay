#!/system/bin/sh
# flash_resource.sh -- write a resource image on device, with a backup and a
# read-back check.
#
# The resource partition holds rk-kernel.dtb.  Getting it wrong means the board
# no longer boots, and unlike the kernel partition there is no equivalent
# "load the old one on the next start" path, so this keeps a copy of the current
# contents first and refuses to report success without comparing what the
# partition actually returns against the file that was written.
#
# Usage:  su 0 sh flash_resource.sh <image>
set -eu

PART=/dev/block/by-name/resource
BK=/data/local/tmp/resource-backup
SRC=${1:-}

[ -n "$SRC" ] || { echo "usage: $0 <image>" >&2; exit 1; }
[ -f "$SRC" ] || { echo "no such image: $SRC" >&2; exit 1; }

SRC_SIZE=$(wc -c < "$SRC" | tr -d ' ')
PART_SIZE=$(wc -c < "$PART" | tr -d ' ')
echo "[flash_resource] image    : $SRC ($SRC_SIZE bytes)"
echo "[flash_resource] partition: $PART ($PART_SIZE bytes)"
[ "$SRC_SIZE" -le "$PART_SIZE" ] || { echo "image larger than partition" >&2; exit 1; }
[ "$(dd if="$SRC" bs=4 count=1 2>/dev/null)" = "RSCE" ] || {
	echo "not an RSCE image" >&2; exit 1; }

mkdir -p "$BK"
if [ -f "$BK/resource.img" ]; then
	echo "[flash_resource] backup   : already present, keeping the original"
else
	dd if="$PART" of="$BK/resource.img" bs=1048576 2>/dev/null
	echo "[flash_resource] backup   : saved $BK/resource.img"
fi

BLOCKS=$(( (SRC_SIZE + 1048575) / 1048576 ))
echo "[flash_resource] writing  : $SRC -> $PART"
dd if="$SRC" of="$PART" bs=1048576 count="$BLOCKS" 2>/dev/null
sync

echo "[flash_resource] verify   : reading back $BLOCKS MiB"
dd if="$PART" of=/data/local/tmp/resource-verify.img bs=1048576 count="$BLOCKS" 2>/dev/null
if cmp -s "$SRC" /data/local/tmp/resource-verify.img; then
	echo "[flash_resource] ok       : read-back matches the image"
else
	echo "[flash_resource] MISMATCH : read-back differs, rolling back" >&2
	dd if="$BK/resource.img" of="$PART" bs=1048576 2>/dev/null
	sync
	exit 2
fi
rm -f /data/local/tmp/resource-verify.img
echo "[flash_resource] reboot to apply: su 0 reboot"
