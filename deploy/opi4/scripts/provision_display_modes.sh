#!/system/bin/sh
# Merge the six exact 60 Hz framebuffer presets without duplicating stock modes.
set -eu
ROOT=${CARPLAY_ROOT:-/data/carplay}
RESOURCE=${CARPLAY_DISPLAY_RESOURCE:-/system/usr/share/resolution_white.xml}
BUSYBOX=${CARPLAY_BUSYBOX:-/system/bin/busybox}
BACKUP_DIR=$ROOT/backups
BACKUP=$BACKUP_DIR/resolution_white.original.xml
MANIFEST=$BACKUP_DIR/resolution_white.original.sha256
FRAGMENT=${1:-$ROOT/config/resolution-white-modes.xml}
TEMP=$BACKUP_DIR/resolution_white.xml.new
MOUNT_TARGET=
MOUNTED_RW=0
cleanup() {
    rm -f "$TEMP"
    if [ "$MOUNTED_RW" = 1 ]; then mount -o remount,ro "$MOUNT_TARGET" >/dev/null 2>&1 || true; fi
}
trap cleanup EXIT HUP INT TERM
[ "$(id -u)" = 0 ] || { echo 'display mode provisioning requires root' >&2; exit 77; }
[ -f "$RESOURCE" ] && [ -f "$FRAGMENT" ] || { echo 'missing mode resource or fragment' >&2; exit 66; }
mkdir -p "$BACKUP_DIR"
chmod 700 "$ROOT" "$BACKUP_DIR"
if [ ! -f "$BACKUP" ]; then cp "$RESOURCE" "$BACKUP"; chmod 600 "$BACKUP"; fi
if [ ! -s "$MANIFEST" ]; then sha256sum "$BACKUP" | cut -d ' ' -f1 > "$MANIFEST"; fi
[ "$(sha256sum "$BACKUP" | cut -d ' ' -f1)" = "$(cat "$MANIFEST")" ] || {
    echo 'display mode backup checksum mismatch' >&2; exit 65;
}
# Inventory is based on resolution entries, including unmarked stock timings.
# Markers alone do not prove a valid mode and must not cause duplicate 720/1080.
all_present() {
    "$BUSYBOX" awk '
    function value(block, tag, out) {
        out=block; sub(".*<" tag ">", "", out); sub("</" tag ">.*", "", out); return out+0
    }
    /<resolution>/ { block=""; active=1 }
    active { block=block $0 }
    /<\/resolution>/ {
        if (active && value(block,"vrefresh")==60) seen[value(block,"hdisplay") "x" value(block,"vdisplay")]=1
        active=0
    }
    END { exit !(seen["800x480"] && seen["1024x480"] && seen["1280x600"] && seen["1280x720"] && seen["1920x900"] && seen["1920x1080"]) }
    ' "$RESOURCE"
}
if all_present; then echo 'six fixed display modes already provisioned'; exit 0; fi
"$BUSYBOX" awk -v resource="$RESOURCE" -v fragment="$FRAGMENT" '
function value(block, tag, out) {
    out=block; sub(".*<" tag ">", "", out); sub("</" tag ">.*", "", out); return out+0
}
BEGIN {
    while ((getline line < resource)>0) {
        if (line ~ /<resolution>/) { block=""; active=1 }
        if (active) block=block line
        if (line ~ /<\/resolution>/) {
            if (value(block,"vrefresh")==60) seen[value(block,"hdisplay") "x" value(block,"vdisplay")]=1
            active=0
        }
    }
    close(resource)
    block=""; active=0; marker=""
    while ((getline line < fragment)>0) {
        if (line ~ /CARPLAY-MODE:/) marker=line "\n"
        if (line ~ /<resolution>/) { block=""; active=1 }
        if (active) block=block line "\n"
        if (line ~ /<\/resolution>/) {
            key=value(block,"hdisplay") "x" value(block,"vdisplay")
            if (!seen[key]) { insert=insert marker block; seen[key]=1 }
            marker=""; active=0
        }
    }
    close(fragment)
}
{
    if ($0 ~ /<\/resolutions>/ && !inserted) { printf "%s", insert; inserted=1 }
    print
}
END { if (!inserted) exit 2 }
' "$RESOURCE" > "$TEMP" || { echo 'failed to merge display mode XML' >&2; exit 65; }
# Check the merged file before making /system writable.
original_resource=$RESOURCE
RESOURCE=$TEMP
all_present || { echo 'generated XML is missing a required mode' >&2; exit 65; }
RESOURCE=$original_resource
if grep -q ' /system ' /proc/mounts; then MOUNT_TARGET=/system; else MOUNT_TARGET=/; fi
mount -o remount,rw "$MOUNT_TARGET" || { echo 'cannot remount system writable' >&2; exit 73; }
MOUNTED_RW=1
cp "$TEMP" "$RESOURCE.new"
chmod 644 "$RESOURCE.new"
mv -f "$RESOURCE.new" "$RESOURCE"
restorecon "$RESOURCE" >/dev/null 2>&1 || true
sync
if ! all_present; then
    cp "$BACKUP" "$RESOURCE"
    chmod 644 "$RESOURCE"
    sync
    echo 'display mode readback failed; original restored' >&2
    exit 65
fi
printf 'six fixed display modes provisioned; original saved at %s\n' "$BACKUP"
