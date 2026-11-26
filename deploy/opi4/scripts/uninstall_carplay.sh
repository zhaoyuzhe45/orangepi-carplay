#!/system/bin/sh
# Remove the CarPlay boot service and optional runtime data.
# Usage: uninstall_carplay.sh [--purge-data]

set -u
ROOT=/data/carplay
INIT_DST=/system/etc/init/carplay_box.rc
INIT_BACKUP=$ROOT/bin/carplay_box.rc.backup
AUDIO_APK=/system/priv-app/CarPlayAudio/CarPlayAudio.apk
AUDIO_PERMISSIONS=/system/etc/default-permissions/carplay-audio.xml
PURGE=0

case "$#" in
    0) ;;
    1)
        if [ "$1" = "--purge-data" ]; then PURGE=1
        else echo "usage: $0 [--purge-data]" >&2; exit 64
        fi
        ;;
    *) echo "usage: $0 [--purge-data]" >&2; exit 64 ;;
esac

if [ "$(id -u)" != "0" ]; then
    echo "uninstall requires root" >&2
    exit 77
fi

stop_pid() {
    pid_file=$1
    [ -f "$pid_file" ] || return 0
    pid=$(cat "$pid_file" 2>/dev/null || true)
    [ -n "$pid" ] || return 0
    kill -TERM "$pid" 2>/dev/null || true
    tries=0
    while kill -0 "$pid" 2>/dev/null && [ "$tries" -lt 100 ]; do
        sleep 0.1
        tries=$((tries + 1))
    done
    kill -KILL "$pid" 2>/dev/null || true
}

stop_pid "$ROOT/supervisor.pid"
stop_pid "$ROOT/daemon.pid"
am force-stop com.carplaybox.audio >/dev/null 2>&1 || true
if [ -x "$ROOT/bin/restore_display_modes.sh" ]; then
    sh "$ROOT/bin/restore_display_modes.sh" || {
        echo "warning: original display mode file could not be restored" >&2
    }
fi

if grep -q " /system " /proc/mounts; then
    root_mount=/system
else
    root_mount=/
fi
if ! mount -o remount,rw "$root_mount"; then
    echo "cannot remount $root_mount read-write" >&2
    exit 73
fi

if [ -f "$INIT_BACKUP" ]; then
    cp "$INIT_BACKUP" "$INIT_DST"
    chmod 644 "$INIT_DST"
else
    rm -f "$INIT_DST"
fi
rm -f "$AUDIO_APK" "$AUDIO_PERMISSIONS"
rmdir /system/priv-app/CarPlayAudio 2>/dev/null || true
restorecon -RF /system/etc/init /system/etc/default-permissions /system/priv-app 2>/dev/null || true
sync
mount -o remount,ro "$root_mount" || true

if [ "$PURGE" = "1" ]; then
    rm -rf "$ROOT"
    echo "Removed boot service, audio helper and runtime data."
else
    rm -f "$ROOT/supervisor.pid" "$ROOT/daemon.pid"
    echo "Removed boot service and audio helper; kept $ROOT configuration and logs."
fi
echo "Reboot the board to finish removal from Android init."
