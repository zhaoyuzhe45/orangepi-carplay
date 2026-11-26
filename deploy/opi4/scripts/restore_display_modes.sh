#!/system/bin/sh
# Restore the resolution_white.xml captured before CarPlay mode provisioning.
# Usage: restore_display_modes.sh

set -eu
ROOT=/data/carplay
RESOURCE=/system/usr/share/resolution_white.xml
BACKUP=$ROOT/backups/resolution_white.original.xml
MANIFEST=$ROOT/backups/resolution_white.original.sha256
MOUNT_TARGET=

if [ "$(id -u)" != "0" ]; then
    echo "display mode restore requires root" >&2
    exit 77
fi
if [ ! -f "$BACKUP" ]; then
    echo "no original display mode backup found; nothing to restore"
    exit 0
fi
if [ -f "$MANIFEST" ]; then
    expected=$(cat "$MANIFEST" 2>/dev/null || true)
    actual=$(sha256sum "$BACKUP" | cut -d ' ' -f1)
    [ "$expected" = "$actual" ] || {
        echo "display mode backup checksum mismatch" >&2
        exit 65
    }
fi
if grep -q ' /system ' /proc/mounts; then
    MOUNT_TARGET=/system
else
    MOUNT_TARGET=/
fi
mount -o remount,rw "$MOUNT_TARGET" || {
    echo "cannot remount $MOUNT_TARGET read-write" >&2
    exit 73
}
cp "$BACKUP" "$RESOURCE"
chmod 644 "$RESOURCE"
restorecon "$RESOURCE" >/dev/null 2>&1 || true
sync
mount -o remount,ro "$MOUNT_TARGET" >/dev/null 2>&1 || true
echo "restored original $RESOURCE"
