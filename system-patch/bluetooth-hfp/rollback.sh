#!/system/bin/sh
set -eu
[ "$(id -u)" = 0 ]
[ "$(getprop ro.product.device)" = rk3399_mid ]
BACKUP=/data/carplay/backups/bluetooth-20261112
TARGET=/vendor/overlay/CarPlayBluetoothHfpOverlay.apk
[ -f "$BACKUP/overlay-state" ]
mount -o remount,rw /vendor
case "$(cat "$BACKUP/overlay-state")" in
    present) cp "$BACKUP/previous-overlay.apk" "$TARGET"; chmod 644 "$TARGET"; restorecon "$TARGET" ;;
    absent) rm -f "$TARGET" ;;
    *) exit 1 ;;
esac
sync
echo 'Previous HFP configuration restored; reboot required.'
