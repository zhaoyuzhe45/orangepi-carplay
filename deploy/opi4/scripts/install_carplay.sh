#!/system/bin/sh
# Install staged files on the CarPlay accessory board.
# Usage: install_carplay.sh <stage-dir> <install-boot 0|1>

set -eu
STAGE=${1:-/data/local/tmp/carplay-stage}
INSTALL_BOOT=${2:-0}
ROOT=/data/carplay
INIT_SRC=$STAGE/init.carplay.rc
INIT_DST=/system/etc/init/carplay_box.rc
PID_FILE=$ROOT/supervisor.pid
AUDIO_APK=$STAGE/CarPlayAudio.apk
PHONE_APK=$STAGE/CarPlayPhone.apk
AUDIO_APK_DST=/system/priv-app/CarPlayAudio/CarPlayAudio.apk
AUDIO_PERMISSIONS=$STAGE/carplay-audio-default-permissions.xml
AUDIO_PERMISSIONS_DST=/system/etc/default-permissions/carplay-audio.xml
DISPLAY_MODES_TSV=$ROOT/config/display-modes.tsv
DISPLAY_MODES_XML=$ROOT/config/resolution-white-modes.xml
DAEMON_PID_FILE=$ROOT/daemon.pid
LOCK_DIR=$ROOT/supervisor.lock
PROFILE_DIR=/cache/carplay

validate_display_boot_order() {
    # Do not install this early-boot integration onto an unverified init graph.
    # Android8 imports system services during mount_all in fs. The profile must
    # be applied after persist replay and before the first hal/core class start.
    [ "$(getprop ro.product.device)" = rk3399_mid ] &&
    [ "$(getprop ro.build.version.release)" = 8.1.0 -o "$(getprop ro.build.version.release)" = 8.1 ] || return 1
    /system/bin/busybox awk '
    /^[[:space:]]*#/ { next }
    /^on / { action=$0 }
    action=="on late-init" && $1=="trigger" {
        if ($2=="fs") fs=NR
        if ($2=="post-fs-data") data=NR
        if ($2=="zygote-start") zygote=NR
        if ($2=="load_persist_props_action") persist=NR
        if ($2=="early-boot") early=NR
        if ($2=="boot") boot=NR
    }
    $1=="class_start" && ($2=="hal" || $2=="core") {
        if (action!="on boot") bad=1
        if ($2=="hal") hal=1
        if ($2=="core") core=1
    }
    END { exit !(fs && data>fs && zygote>data && persist>zygote && early>persist && boot>early && hal && core && !bad) }
    ' /init.rc || return 1
    grep -q '^on fs' /init.rk30board.bootmode.emmc.rc &&
    grep -q '^[[:space:]]*mount_all fstab.rk30board' /init.rk30board.bootmode.emmc.rc &&
    /system/bin/busybox awk '$2=="/cache" && $3=="ext4" && $5 !~ /latemount/ { ok=1 } END { exit !ok }' /fstab.rk30board &&
    grep -q '^[[:space:]]*class hal' /vendor/etc/init/android.hardware.graphics.composer@2.1-service.rc &&
    grep -q '^[[:space:]]*class core' /system/etc/init/surfaceflinger.rc &&
    [ -x /system/bin/timeout ] && [ ! -L "$PROFILE_DIR" ]
}

backup_display_boot_state() {
    mkdir -p "$ROOT/backups" "$PROFILE_DIR"
    chmod 700 "$ROOT/backups" "$PROFILE_DIR"
    chown 0:0 "$PROFILE_DIR"
    for source_path in /init.rc /data/system/users/0/settings_global.xml /data/system/users/0/settings_secure.xml; do
        [ -f "$source_path" ] || continue
        case "$source_path" in
            /init.rc) backup_path=$ROOT/backups/graphics-init.original.rc ;;
            *) backup_path=$ROOT/backups/${source_path##*/}.pre-carplay-display ;;
        esac
        if [ ! -f "$backup_path" ]; then
            cp -p "$source_path" "$backup_path"
            chmod 600 "$backup_path"
            sha256sum "$backup_path" | cut -d ' ' -f1 > "$backup_path.sha256"
        fi
    done
    if [ -f "$PROFILE_DIR/display-profile.conf" ] && [ ! -L "$PROFILE_DIR/display-profile.conf" ] &&
       [ ! -f "$ROOT/backups/display-profile.pre-carplay.conf" ]; then
        cp "$PROFILE_DIR/display-profile.conf" "$ROOT/backups/display-profile.pre-carplay.conf"
        chmod 600 "$ROOT/backups/display-profile.pre-carplay.conf"
    fi
}

instance_pids() {
    needle=$1
    for entry in /proc/[0-9]*; do
        [ -r "$entry/cmdline" ] || continue
        pid=${entry##*/}
        [ "$pid" = "$$" ] && continue
        cmdline=$(tr "\0" " " < "$entry/cmdline" 2>/dev/null || true)
        case "$cmdline" in
            *"$needle"*) printf "%s\n" "$pid" ;;
        esac
    done
}

terminate_matching_instances() {
    needle=$1
    pids=$(instance_pids "$needle")
    [ -n "$pids" ] || return 0
    for pid in $pids; do
        kill -TERM "$pid" 2>/dev/null || true
    done
    tries=0
    while [ -n "$(instance_pids "$needle")" ] && [ "$tries" -lt 100 ]; do
        sleep 0.1
        tries=$((tries + 1))
    done
    pids=$(instance_pids "$needle")
    for pid in $pids; do
        kill -KILL "$pid" 2>/dev/null || true
    done
    sleep 0.2
}

stop_existing_instances() {
    # An Android init service is authoritative when present. Stopping it
    # prevents init from restarting the old supervisor while files are replaced.
    service_state=$(getprop init.svc.carplay_box 2>/dev/null || true)
    if [ -n "$service_state" ]; then
        stop carplay_box >/dev/null 2>&1 || true
        tries=0
        while [ "$tries" -lt 100 ]; do
            service_state=$(getprop init.svc.carplay_box 2>/dev/null || true)
            case "$service_state" in
                running|restarting|stopping) ;;
                *) break ;;
            esac
            sleep 0.1
            tries=$((tries + 1))
        done
    fi
    terminate_matching_instances carplay_supervisor.sh
    terminate_matching_instances /data/carplay/bin/iap2d
    rm -f "$PID_FILE" "$DAEMON_PID_FILE" 2>/dev/null || true
    rm -rf "$LOCK_DIR" 2>/dev/null || true
}

wait_for_supervisor() {
    tries=0
    while [ "$tries" -lt 100 ]; do
        if [ -f "$PID_FILE" ]; then
            supervisor_pid=$(cat "$PID_FILE" 2>/dev/null || true)
            if [ -n "$supervisor_pid" ] && kill -0 "$supervisor_pid" 2>/dev/null; then
                echo "supervisor started pid=$supervisor_pid source=$1"
                return 0
            fi
        fi
        sleep 0.1
        tries=$((tries + 1))
    done
    return 1
}

start_supervisor() {
    if [ -n "$(getprop init.svc.carplay_box 2>/dev/null || true)" ]; then
        start carplay_box >/dev/null 2>&1 || true
        if wait_for_supervisor init; then
            return 0
        fi
        # A freshly copied .rc is not visible to init until reboot. Fall back to
        # one guarded manual supervisor; its singleton lock prevents duplicates.
        stop carplay_box >/dev/null 2>&1 || true
        terminate_matching_instances carplay_supervisor.sh
        terminate_matching_instances /data/carplay/bin/iap2d
        rm -f "$PID_FILE" "$DAEMON_PID_FILE"
        rm -rf "$LOCK_DIR"
    fi
    nohup /system/bin/sh "$ROOT/bin/carplay_supervisor.sh" >/dev/null 2>&1 </dev/null &
    wait_for_supervisor manual
}

if [ "$(id -u)" != "0" ]; then
    echo "install requires root" >&2
    exit 77
fi
for required in iap2d carplay-encoder.jar carplay_supervisor.sh apple_attach.sh init.carplay.rc \
                usb_lock.sh usb_role_switch.sh apply_display_mode.sh \
                provision_display_modes.sh restore_display_modes.sh \
                display-modes.tsv resolution-white-modes.xml; do
    [ -f "$STAGE/$required" ] || { echo "missing staged file: $required" >&2; exit 66; }
done
if { [ -f "$AUDIO_APK" ] || [ -f "$AUDIO_PERMISSIONS" ] || [ -f "$PHONE_APK" ]; } &&
   { [ ! -f "$AUDIO_APK" ] || [ ! -f "$AUDIO_PERMISSIONS" ] || [ ! -f "$PHONE_APK" ]; }; then
    echo "audio/phone staging is incomplete; both APKs and permissions are required" >&2
    exit 66
fi

SYSTEM_WRITE=1

if [ "$INSTALL_BOOT" = 1 ]; then
    validate_display_boot_order || {
        echo 'unsupported early-display init graph or cache/timeout boundary; installation stopped before service teardown' >&2
        exit 65
    }
    backup_display_boot_state
fi

stop_existing_instances

if [ -f "$AUDIO_APK" ]; then
    am force-stop com.carplaybox.audio >/dev/null 2>&1 || true
    sleep 0.5
fi

mkdir -p "$ROOT/bin" "$ROOT/logs" "$ROOT/config"
chmod 700 "$ROOT" "$ROOT/logs" "$ROOT/config"
chmod 755 "$ROOT/bin"
cp "$STAGE/carplay-encoder.jar" "$ROOT/bin/carplay-encoder.jar.new"
chmod 644 "$ROOT/bin/carplay-encoder.jar.new"
mv -f "$ROOT/bin/carplay-encoder.jar.new" "$ROOT/bin/carplay-encoder.jar"
cp "$STAGE/iap2d" "$ROOT/bin/iap2d.new"
chmod 755 "$ROOT/bin/iap2d.new"
mv -f "$ROOT/bin/iap2d.new" "$ROOT/bin/iap2d"
cp "$STAGE/carplay_supervisor.sh" "$ROOT/bin/carplay_supervisor.sh"
cp "$STAGE/apple_attach.sh" "$ROOT/bin/apple_attach.sh"
cp "$STAGE/usb_lock.sh" "$ROOT/bin/usb_lock.sh"
cp "$STAGE/usb_role_switch.sh" "$ROOT/bin/usb_role_switch.sh"
cp "$STAGE/apply_display_mode.sh" "$ROOT/bin/apply_display_mode.sh"
cp "$STAGE/init.carplay.rc" "$ROOT/bin/init.carplay.rc"
cp "$STAGE/provision_display_modes.sh" "$ROOT/bin/provision_display_modes.sh"
cp "$STAGE/restore_display_modes.sh" "$ROOT/bin/restore_display_modes.sh"
cp "$STAGE/display-modes.tsv" "$DISPLAY_MODES_TSV"
cp "$STAGE/resolution-white-modes.xml" "$DISPLAY_MODES_XML"
chmod 755 "$ROOT/bin/carplay_supervisor.sh" "$ROOT/bin/apple_attach.sh" \
    "$ROOT/bin/usb_lock.sh" "$ROOT/bin/usb_role_switch.sh" \
    "$ROOT/bin/apply_display_mode.sh" \
    "$ROOT/bin/provision_display_modes.sh" "$ROOT/bin/restore_display_modes.sh"
chmod 644 "$ROOT/bin/init.carplay.rc" "$DISPLAY_MODES_TSV" "$DISPLAY_MODES_XML"
sh -n "$ROOT/bin/carplay_supervisor.sh"
sh -n "$ROOT/bin/apple_attach.sh"
sh -n "$ROOT/bin/usb_lock.sh"
sh -n "$ROOT/bin/usb_role_switch.sh"
sh -n "$ROOT/bin/apply_display_mode.sh"
sh -n "$ROOT/bin/provision_display_modes.sh"
sh -n "$ROOT/bin/restore_display_modes.sh"

if [ "$SYSTEM_WRITE" = "1" ]; then
    if grep -q " /system " /proc/mounts; then
        root_mount=/system
    else
        root_mount=/
    fi
    if ! mount -o remount,rw "$root_mount"; then
        echo "cannot remount $root_mount read-write" >&2
        exit 73
    fi
fi

if [ -f "$AUDIO_APK" ]; then
    mkdir -p /system/priv-app/CarPlayAudio /system/etc/default-permissions
    cp "$AUDIO_APK" "$AUDIO_APK_DST.new"
    chmod 644 "$AUDIO_APK_DST.new"
    mv -f "$AUDIO_APK_DST.new" "$AUDIO_APK_DST"
    cp "$AUDIO_PERMISSIONS" "$AUDIO_PERMISSIONS_DST.new"
    chmod 644 "$AUDIO_PERMISSIONS_DST.new"
    mv -f "$AUDIO_PERMISSIONS_DST.new" "$AUDIO_PERMISSIONS_DST"
    restorecon -RF /system/priv-app/CarPlayAudio /system/etc/default-permissions 2>/dev/null || true
    echo "installed CarPlay audio/vehicle APK and default runtime permissions"
    # Register the paired interface update now; keep the privileged system copy for boot.
    pm install -r "$AUDIO_APK"
    if [ -f "$PHONE_APK" ]; then pm install -r "$PHONE_APK"; fi
fi

if [ "$INSTALL_BOOT" = "1" ]; then
    mkdir -p /system/etc/init /system/etc/carplay
    # Android init parses every regular file in this directory, including
    # names ending in .backup. Keep the backup outside init's scan path.
    if [ -f "$INIT_DST" ] && [ ! -f "$ROOT/bin/carplay_box.rc.backup" ]; then
        cp "$INIT_DST" "$ROOT/bin/carplay_box.rc.backup"
    fi
    rm -f "$INIT_DST.backup"
    cp "$INIT_SRC" "$INIT_DST"
    chmod 644 "$INIT_DST"
    restorecon "$INIT_DST" 2>/dev/null || true
    # The early helper runs from /system and reads /cache before userdata is
    # unlocked. Replacing the rc removes the old asynchronous display boot
    # worker; init never restarts graphics for the remembered profile.
    cp "$STAGE/apply_display_mode.sh" /system/etc/carplay/apply_display_mode.sh
    chmod 755 /system/etc/carplay/apply_display_mode.sh
    restorecon -R /system/etc/carplay 2>/dev/null || true
    restorecon -R "$PROFILE_DIR" 2>/dev/null || true
    echo "installed boot service at $INIT_DST and /system/etc/carplay/apply_display_mode.sh; reboot required for init to parse it"
fi

if [ "$SYSTEM_WRITE" = "1" ]; then
    sync
    mount -o remount,ro "$root_mount" || true
fi
sh "$ROOT/bin/provision_display_modes.sh" "$DISPLAY_MODES_XML"

restorecon -RF "$ROOT" 2>/dev/null || true
if pm path com.carplaybox.audio >/dev/null 2>&1; then
    pm grant com.carplaybox.audio android.permission.RECORD_AUDIO ||
        echo "warning: RECORD_AUDIO grant will be applied during the next boot" >&2
    appops set com.carplaybox.audio RECORD_AUDIO allow ||
        echo "warning: RECORD_AUDIO app-op will be applied during the next boot" >&2
    pm grant com.carplaybox.audio android.permission.ACCESS_FINE_LOCATION || true
    pm grant com.carplaybox.audio android.permission.ACCESS_COARSE_LOCATION || true
    pm grant com.carplaybox.audio android.permission.READ_PHONE_STATE || true
    pm grant com.carplaybox.audio android.permission.READ_CONTACTS || true
    appops set com.carplaybox.audio MOCK_LOCATION allow ||
        echo "warning: enable the car GPS provider app-op after the APK is registered" >&2
    appops write-settings || echo "warning: app-op settings could not be flushed" >&2
    am start-foreground-service -n com.carplaybox.audio/.AudioCaptureService >/dev/null 2>&1 ||
        echo "warning: audio capture service could not be started yet" >&2
    am start-foreground-service -n com.carplaybox.audio/.VehicleLocationService >/dev/null 2>&1 ||
        echo "warning: vehicle location service could not be started yet" >&2
fi
if start_supervisor; then
    exit 0
fi
echo "supervisor failed to start" >&2
exit 70
