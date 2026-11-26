#!/system/bin/sh
# Usage: sh install_bluetooth_classic.sh /path/to/bluetooth.default.arm64.so
# This compatibility image is valid only for the exact Android 8.1 firmware below.
set -eu
[ "$(id -u)" = 0 ]
[ "$(getprop ro.product.device)" = rk3399_mid ]
[ "$(getprop ro.build.version.sdk)" = 27 ]
SOURCE=$1
TARGET=/system/lib64/hw/bluetooth.default.so
ORIGINAL=d33810d95ef68eb05cef71144b25bcf4c84117f5b83755d5324c97eb927f1077
PATCHED=7985c96fc09e864abc5f50d7e7b822a57fbe6294b12b23c38a759984872ee9b8
[ "$(sha256sum "$SOURCE" | cut -d ' ' -f 1)" = "$PATCHED" ]
CURRENT=$(sha256sum "$TARGET" | cut -d ' ' -f 1)
if [ "$CURRENT" = "$PATCHED" ]; then echo 'Classic Bluetooth guard already installed'; exit 0; fi
[ "$CURRENT" = "$ORIGINAL" ] || { echo 'Unsupported Bluetooth firmware; unchanged' >&2; exit 1; }
BACKUP=/data/carplay/backups/bluetooth-classic
mkdir -p "$BACKUP"
if [ ! -f "$BACKUP/bluetooth.default.arm64.so" ]; then cp "$TARGET" "$BACKUP/bluetooth.default.arm64.so"; fi
[ "$(sha256sum "$BACKUP/bluetooth.default.arm64.so" | cut -d ' ' -f 1)" = "$ORIGINAL" ]
mount -o remount,rw /system
cp "$SOURCE" "$TARGET.new"
chown root:root "$TARGET.new"
chmod 644 "$TARGET.new"
[ "$(sha256sum "$TARGET.new" | cut -d ' ' -f 1)" = "$PATCHED" ]
mv "$TARGET.new" "$TARGET"
restorecon "$TARGET"
sync
echo 'Classic Bluetooth guard installed. Reboot once to load it.'
