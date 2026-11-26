#!/system/bin/sh
set -eu
[ "$(id -u)" = 0 ]
TARGET=/system/lib64/hw/bluetooth.default.so
SOURCE=/data/carplay/backups/bluetooth-classic/bluetooth.default.arm64.so
[ "$(sha256sum "$TARGET" | cut -d ' ' -f 1)" = 7985c96fc09e864abc5f50d7e7b822a57fbe6294b12b23c38a759984872ee9b8 ]
[ "$(sha256sum "$SOURCE" | cut -d ' ' -f 1)" = d33810d95ef68eb05cef71144b25bcf4c84117f5b83755d5324c97eb927f1077 ]
mount -o remount,rw /system
cp "$SOURCE" "$TARGET.new"
chown root:root "$TARGET.new"
chmod 644 "$TARGET.new"
mv "$TARGET.new" "$TARGET"
restorecon "$TARGET"
sync
echo 'Original Bluetooth stack restored. Reboot once.'
