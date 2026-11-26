#!/system/bin/sh
# One-shot installer for the verified RK3399 Android 8.1 audio pair.
# Usage: sh install_shared_microphone.sh check|apply|update|verify|rollback MODULE_SHA APK_SHA
set -eu
mode=${1:-check}
module_sha=${2:-}
apk_sha=${3:-}
refresh_framework=${4:-no}
stock_sha=4f27805e6f5203001679810d69f3061e02725f31feaa748f892ed9047a04c221
stage=/data/local/tmp/shared-mic-stage-20261119
backup=/data/carplay/backups/shared-mic-20261119
module=/vendor/lib/hw/audio.r_submix.default.so
original=/vendor/lib/hw/audio.r_submix.carplay-original.so
base_apk=/system/priv-app/CarPlayAudio/CarPlayAudio.apk
file_sha() { sha256sum "$1" | cut -d ' ' -f 1; }
fail() { echo "ERROR: $*" >&2; exit 1; }
valid_hash() { [ ${#1} -eq 64 ] && case "$1" in *[!0-9a-f]*) return 1;; *) return 0;; esac; }
mount_mode() {
    while read -r mount_device mount_target mount_type mount_options mount_rest; do
        if [ "$mount_target" = "$1" ]; then
            printf '%s\n' "${mount_options%%,*}"
            return 0
        fi
    done < /proc/mounts
}
system_mount=/system
[ -n "$(mount_mode /system)" ] || system_mount=/
system_mode=$(mount_mode "$system_mount")
vendor_mode=$(mount_mode /vendor)
[ -n "$system_mode" ] && [ -n "$vendor_mode" ] || fail 'cannot determine mount states'
restore_mounts() {
    result=0
    mount -o "remount,$vendor_mode" /vendor || result=1
    mount -o "remount,$system_mode" "$system_mount" || result=1
    return "$result"
}
replace_file() {
    src=$1 dst=$2 label=$3
    cp "$src" "$dst.new" && chmod 644 "$dst.new" && chown 0:0 "$dst.new" &&
        chcon "$label" "$dst.new" && mv "$dst.new" "$dst"
}
start_bridges() {
    for component in AudioCaptureService VehicleLocationService AppIntegrationService; do
        am start-foreground-service -n "com.carplaybox.audio/.$component" >/dev/null || return 1
    done
}
restart_audio() {
    old_server=$(pidof audioserver || true)
    old_hal=$(pidof android.hardware.audio@2.0-service || true)
    setprop ctl.restart audioserver || return 1
    attempts=0
    while [ "$attempts" -lt 20 ]; do
        sleep 1
        new_server=$(pidof audioserver || true)
        new_hal=$(pidof android.hardware.audio@2.0-service || true)
        if [ -n "$new_server" ] && [ "$new_server" != "$old_server" ] &&
           [ -n "$new_hal" ] && [ "$new_hal" != "$old_hal" ] &&
           [ "$(getprop init.svc.audioserver)" = running ] &&
           [ "$(getprop init.svc.audio-hal-2-0)" = running ]; then
            sleep 1
            return 0
        fi
        attempts=$((attempts + 1))
    done
    return 1
}
verify_loaded_module() {
    hal_pid=$(pidof android.hardware.audio@2.0-service || true)
    [ -n "$hal_pid" ] || return 1
    inode=$(stat -c %i "$module") || return 1
    # Check the current inode, not an old unlinked image at the same path.
    mapped=0
    while read -r map_addr map_perms map_offset map_dev map_inode map_path map_extra; do
        if [ "$map_inode" = "$inode" ] && [ "$map_path" = "$module" ] && [ -z "$map_extra" ]; then
            mapped=1
            break
        fi
    done < "/proc/$hal_pid/maps"
    [ "$mapped" = 1 ] || return 1
    timeout 8 env CLASSPATH="$stage/shared-mic-status.jar" app_process /system/bin com.carplaybox.audio.SharedMicrophoneStatusProbe "$1"
}
[ "$(id -u)" = 0 ] || fail 'root required'
[ "$(getprop ro.product.device)" = rk3399_mid ] || fail 'wrong device'
[ "$(getprop ro.build.version.sdk)" = 27 ] || fail 'wrong Android version'
[ "$(getenforce)" = Permissive ] || fail 'SELinux policy differs from tested device'
case "$mode" in check|apply|update|verify|rollback) ;; *) fail 'unknown mode';; esac
if [ "$mode" = rollback ]; then
    [ -f "$backup/complete" ] || fail 'verified backup missing'
    (cd "$backup" && sha256sum -c SHA256SUMS) || fail 'backup integrity failed'
    [ "$(file_sha "$backup/audio.r_submix.default.so")" = "$stock_sha" ] || fail 'original module backup mismatch'
    system_mount=$(sed -n '1p' "$backup/mount-state")
    system_mode=$(sed -n '2p' "$backup/mount-state")
    vendor_mode=$(sed -n '3p' "$backup/mount-state")
    case "$system_mount:$system_mode:$vendor_mode" in /system:rw:ro|/system:ro:ro|/:rw:ro|/:ro:ro|/system:rw:rw|/system:ro:rw|/:rw:rw|/:ro:rw) ;; *) fail 'invalid saved mount state';; esac
    trap 'restore_mounts' EXIT
    failed=0
    am force-stop com.carplaybox.audio || failed=1
    if mount -o remount,rw /vendor; then
        replace_file "$backup/audio.r_submix.default.so" "$module" u:object_r:vendor_file:s0 || failed=1
    else failed=1; fi
    if mount -o remount,rw "$system_mount"; then
        replace_file "$backup/base.apk" "$base_apk" u:object_r:system_file:s0 || failed=1
    else failed=1; fi
    pm install -r -d "$backup/active.apk" | grep -q '^Success' || failed=1
    sync || failed=1
    # Always attempt restoring runtime audio even if the APK operation failed.
    restart_audio || failed=1
    verify_loaded_module stock || failed=1
    [ "$(file_sha "$module")" = "$stock_sha" ] || failed=1
    active_apk=$(pm path com.carplaybox.audio | sed -n 's/^package://p')
    [ "$(file_sha "$active_apk")" = "$(file_sha "$backup/active.apk")" ] || failed=1
    [ "$(file_sha "$base_apk")" = "$(file_sha "$backup/base.apk")" ] || failed=1
    start_bridges || failed=1
    restore_mounts || failed=1
    trap - EXIT
    [ "$failed" = 0 ] || fail 'rollback incomplete; backup preserved for recovery'
    echo ROLLED_BACK
    exit 0
fi
valid_hash "$module_sha" && valid_hash "$apk_sha" || fail 'invalid expected hashes'
[ "$(file_sha "$stage/audio.r_submix.shared.so")" = "$module_sha" ] || fail 'staged module hash mismatch'
[ "$(file_sha "$stage/CarPlayAudio.apk")" = "$apk_sha" ] || fail 'staged APK hash mismatch'
[ -s "$stage/shared-mic-status.jar" ] || fail 'framework status probe missing'
if [ "$mode" = verify ]; then
    [ "$(file_sha "$module")" = "$module_sha" ] || fail 'installed HAL mismatch'
    active_apk=$(pm path com.carplaybox.audio | sed -n 's/^package://p')
    [ "$(file_sha "$active_apk")" = "$apk_sha" ] || fail 'active APK mismatch'
    [ "$(file_sha "$base_apk")" = "$apk_sha" ] || fail 'base APK mismatch'
    dumpsys package com.carplaybox.audio | grep 'CAPTURE_AUDIO_HOTWORD: granted=true' >/dev/null || fail 'collector permission missing'
    verify_loaded_module shared || fail 'loaded module mismatch'
    start_bridges
    echo INSTALLED_PENDING_FUNCTIONAL_VERIFICATION
    exit 0
fi
if [ "$mode" = update ]; then
    [ -f "$backup/complete" ] || fail 'original rollback backup missing'
    [ "$(file_sha "$backup/audio.r_submix.default.so")" = "$stock_sha" ] || fail 'original backup hash mismatch'
    verify_loaded_module shared || fail 'not updating a running shared module'
else
    [ "$(file_sha "$module")" = "$stock_sha" ] || fail 'active module is not the validated original'
fi
if [ -e "$original" ]; then
    [ "$(file_sha "$original")" = "$stock_sha" ] || fail 'original dependency path occupied by another library'
fi
active_apk=$(pm path com.carplaybox.audio | sed -n 's/^package://p')
[ -n "$active_apk" ] && [ -f "$active_apk" ] || fail 'active audio APK missing'
[ -f "$base_apk" ] || fail 'privileged base APK missing'
if [ "$mode" = check ]; then
    verify_loaded_module stock || fail 'original running module could not be verified'
    echo PREFLIGHT_PASS
    exit 0
fi
if [ -e "$backup" ]; then
    [ -f "$backup/complete" ] || fail 'incomplete previous backup'
    (cd "$backup" && sha256sum -c SHA256SUMS) || fail 'backup integrity failed'
    if [ "$mode" != update ]; then
        [ "$(file_sha "$module")" = "$(file_sha "$backup/audio.r_submix.default.so")" ] || fail 'module differs from reusable backup'
        [ "$(file_sha "$active_apk")" = "$(file_sha "$backup/active.apk")" ] || fail 'active APK differs from reusable backup'
        [ "$(file_sha "$base_apk")" = "$(file_sha "$backup/base.apk")" ] || fail 'base APK differs from reusable backup'
    fi
    [ "$(file_sha /data/carplay/config.json)" = "$(file_sha "$backup/config.json")" ] || fail 'config differs from reusable backup'
else
    mkdir -p "$backup"; chmod 700 "$backup"
    cp "$module" "$backup/audio.r_submix.default.so"
    cp "$active_apk" "$backup/active.apk"
    cp "$base_apk" "$backup/base.apk"
    cp /data/carplay/config.json "$backup/config.json"
    printf '%s\n' "$system_mount" "$system_mode" "$vendor_mode" > "$backup/mount-state"
    (cd "$backup" && sha256sum audio.r_submix.default.so active.apk base.apk config.json mount-state > SHA256SUMS)
    (cd "$backup" && sha256sum -c SHA256SUMS) || fail 'backup integrity failed'
    touch "$backup/complete"
fi
sync
rollback_on_error() {
    code=$?
    trap - EXIT
    if [ "$code" != 0 ]; then
        echo 'Installation failed; restoring verified backup' >&2
        if ! sh "$stage/install_shared_microphone.sh" rollback; then
            echo 'ERROR: Automatic rollback incomplete; original files remain in backup' >&2
            restore_mounts || true
            exit 2
        fi
    fi
    exit "$code"
}
trap rollback_on_error EXIT
am force-stop com.carplaybox.audio
# Make both destinations writable before replacing either installed component.
mount -o remount,rw /vendor
mount -o remount,rw "$system_mount"
replace_file "$backup/audio.r_submix.default.so" "$original" u:object_r:vendor_file:s0
replace_file "$stage/audio.r_submix.shared.so" "$module" u:object_r:vendor_file:s0
[ "$(file_sha "$module")" = "$module_sha" ] || fail 'installed module hash mismatch'
[ "$(file_sha "$original")" = "$stock_sha" ] || fail 'retained original module hash mismatch'
sync
restart_audio || fail 'new audio services did not restart'
verify_loaded_module shared || fail 'audio framework did not load the shared module'
replace_file "$stage/CarPlayAudio.apk" "$base_apk" u:object_r:system_file:s0
pm install -r -g "$stage/CarPlayAudio.apk" | grep -q '^Success' || fail 'new audio APK install failed'
active_apk=$(pm path com.carplaybox.audio | sed -n 's/^package://p')
[ "$(file_sha "$active_apk")" = "$apk_sha" ] || fail 'installed APK hash mismatch'
[ "$(file_sha "$base_apk")" = "$apk_sha" ] || fail 'base APK hash mismatch'
if ! dumpsys package com.carplaybox.audio | grep 'CAPTURE_AUDIO_HOTWORD: granted=true' >/dev/null; then
    [ "$refresh_framework" = refresh-framework ] || fail 'collector permission was not granted'
    # The host must perform the restart: zygote recreation may end this shell.
    sync
    restore_mounts || fail 'mount restoration before framework restart failed'
    trap - EXIT
    echo FRAMEWORK_REFRESH_REQUIRED
    exit 0
fi
start_bridges
sync
restore_mounts || fail 'mount state restoration failed'
trap - EXIT
echo INSTALLED_PENDING_FUNCTIONAL_VERIFICATION
