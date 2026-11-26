#!/system/bin/sh
# Usage: install.sh /data/local/tmp/overlay.apk expected-sha256
# Only this verified Android 8.1 board. Original Bluetooth APK stays intact.
set -eu
[ "$(id -u)" = 0 ]
[ "$(getprop ro.product.device)" = rk3399_mid ]
[ "$(getprop ro.build.version.sdk)" = 27 ]
SOURCE=$1
EXPECTED=$2
[ "$(sha256sum "$SOURCE" | cut -d ' ' -f 1)" = "$EXPECTED" ]
TARGET=/vendor/overlay/CarPlayBluetoothHfpOverlay.apk
BACKUP=/data/carplay/backups/bluetooth-20261112
mkdir -p "$BACKUP"
if [ ! -f "$BACKUP/overlay-state" ]; then
    if [ -f "$TARGET" ]; then
        cp "$TARGET" "$BACKUP/previous-overlay.apk"
        echo present > "$BACKUP/overlay-state"
    else
        echo absent > "$BACKUP/overlay-state"
    fi
    cp /system/app/Bluetooth/Bluetooth.apk "$BACKUP/Bluetooth-original.apk"
fi
mount -o remount,rw /vendor
mkdir -p /vendor/overlay
cp "$SOURCE" "$TARGET.new"
chmod 644 "$TARGET.new"
chown root:root "$TARGET.new"
[ "$(sha256sum "$TARGET.new" | cut -d ' ' -f 1)" = "$EXPECTED" ]
mv "$TARGET.new" "$TARGET"
restorecon "$TARGET"
sync
echo 'HFP overlay installed. Reboot for resource/profile rescan.'
