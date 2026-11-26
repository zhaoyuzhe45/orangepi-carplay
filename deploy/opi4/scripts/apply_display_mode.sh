#!/system/bin/sh
# Runtime preparation requires media teardown; early boot reads only a trusted
# successful-stream profile before graphics starts. No boot service restart.
# prepare MODE DENSITY REQUEST_ID --media-stopped
# status prints the atomic transaction snapshot; boot only marks compatibility.
set -u
ROOT=${CARPLAY_ROOT:-/data/carplay}
BOOT_ID_FILE=${CARPLAY_BOOT_ID_FILE:-/proc/sys/kernel/random/boot_id}
TIMEOUT=${CARPLAY_DISPLAY_TIMEOUT:-/system/bin/timeout}
WAIT_SECONDS=${CARPLAY_DISPLAY_WAIT_SECONDS:-30}
PROFILE_DIR=${CARPLAY_DISPLAY_PROFILE_DIR:-/cache/carplay}
PROFILE=$PROFILE_DIR/display-profile.conf
CACHE_HELD=0
SETTINGS_DIR=${CARPLAY_DISPLAY_SETTINGS_DIR:-/data/system/users/0}
BUSYBOX=${CARPLAY_BUSYBOX:-/system/bin/busybox}
STATUS=$ROOT/.display-mode-status
LOCK=
FAILED=$ROOT/.display-mode-failed-targets
PROP_MODE=persist.sys.framebuffer.main
PROP_RES=persist.sys.resolution.main
HELD=0
MUTATED=0
RESTARTED=0
SEQUENCE=0
MODE=
DENSITY=
REQUEST_ID=
BOOT_ID=$(cat "$BOOT_ID_FILE" 2>/dev/null)
LOCK=$ROOT/.display-mode-lock-$BOOT_ID
CACHE_LOCK=$PROFILE_DIR/.display-profile-lock-$BOOT_ID

valid_mode() {
    case "$1" in
        800x480@60|1024x480@60|1280x600@60|1280x720@60|1920x900@60|1920x1080@60) return 0 ;;
        *) return 1 ;;
    esac
}
valid_density() {
    case "$1" in ''|*[!0-9]*|0*) return 1 ;; esac
    [ "${#1}" -le 3 ] && [ "$1" -ge 120 ] && [ "$1" -le 480 ]
}
read_profile() {
    PROFILE_VALID=0
    PROFILE_MODE=800x480@60
    PROFILE_DENSITY=160
    [ ! -L "$PROFILE_DIR" ] && [ ! -L "$PROFILE" ] && [ -f "$PROFILE" ] || return 0
    [ "$(stat -c '%u:%a' "$PROFILE_DIR" 2>/dev/null)" = 0:700 ] &&
    [ "$(stat -c '%u:%a' "$PROFILE" 2>/dev/null)" = 0:600 ] || return 0
    profile_bytes=$(stat -c '%s' "$PROFILE" 2>/dev/null) || return 0
    [ "$profile_bytes" -le 96 ] || return 0
    profile_extra=
    {
        IFS= read -r profile_version && IFS= read -r profile_mode && IFS= read -r profile_density &&
        ! IFS= read -r profile_extra
    } < "$PROFILE" || return 0
    [ -z "$profile_extra" ] || return 0
    [ "$profile_version" = version=1 ] || return 0
    case "$profile_mode" in mode=*) profile_mode=${profile_mode#mode=} ;; *) return 0 ;; esac
    case "$profile_density" in density=*) profile_density=${profile_density#density=} ;; *) return 0 ;; esac
    valid_mode "$profile_mode" && valid_density "$profile_density" || return 0
    PROFILE_MODE=$profile_mode
    PROFILE_DENSITY=$profile_density
    PROFILE_VALID=1
}
cmd_early() {
    # /cache is mounted by fs; this action runs after persist replay and before
    # boot class_start hal/core on the verified Android8 RK3399 init graph.
    read_profile
    setprop sys.carplay.display.active_density "$PROFILE_DENSITY" &&
    setprop sys.carplay.display.active_mode "$PROFILE_MODE" &&
    setprop persist.sys.framebuffer.main "$PROFILE_MODE" &&
    setprop persist.sys.resolution.main Auto &&
    setprop persist.sys.sf.lcd_density "$PROFILE_DENSITY" &&
    setprop qemu.sf.lcd_density "$PROFILE_DENSITY"
}
cmd_early_settings() {
    # Only fixed WM settings are touched, before zygote-start. Android8 keeps
    # these entries in one-line <setting ... /> records. Never source XML.
    for settings_file in "$SETTINGS_DIR/settings_global.xml" "$SETTINGS_DIR/settings_secure.xml"; do
        [ -f "$settings_file" ] && [ ! -L "$settings_file" ] || continue
        grep -Eq 'name="display_(size|density)_forced"' "$settings_file" || continue
        settings_temp=$settings_file.carplay.$$.new
        "$BUSYBOX" awk '
        /name="display_size_forced"|name="display_density_forced"/ {
            if ($0 !~ /^[[:space:]]*<setting .*\/>[[:space:]]*$/) exit 2
            next
        }
        { print }
        ' "$settings_file" > "$settings_temp" || { rm -f "$settings_temp"; return 70; }
        settings_owner=$(stat -c '%u:%g' "$settings_file") || return 70
        settings_mode=$(stat -c '%a' "$settings_file") || return 70
        chown "$settings_owner" "$settings_temp" && chmod "$settings_mode" "$settings_temp" &&
        restorecon "$settings_temp" >/dev/null 2>&1 && mv -f "$settings_temp" "$settings_file" || {
            rm -f "$settings_temp"; return 70;
        }
    done
}
cmd_can_prepare() {
    [ "$#" = 2 ] && valid_mode "$1" && valid_density "$2" || return 64
    [ ! -d "$LOCK" ] || return 75
    grep -Fxq "$BOOT_ID $1" "$FAILED" 2>/dev/null && return 76
    return 0
}
cmd_remember() {
    [ "$#" = 2 ] && valid_mode "$1" && valid_density "$2" || return 64
    MODE=$1; DENSITY=$2
    [ -x "$TIMEOUT" ] || return 70
    [ ! -d "$LOCK" ] || return 75
    previous_transaction=$(cat "$STATUS" 2>/dev/null || true)
    # Never change Android graphics during an active stream. This final
    # readback prevents an old session from remembering a newer session's mode.
    DEADLINE=$(($(now_seconds) + 30))
    verify_target || return 70
    [ ! -d "$LOCK" ] && [ "$(cat "$STATUS" 2>/dev/null || true)" = "$previous_transaction" ] || return 70
    [ ! -L "$PROFILE_DIR" ] && [ ! -L "$PROFILE" ] || return 70
    read_profile
    if [ "$PROFILE_VALID" = 1 ] && [ "$PROFILE_MODE" = "$MODE" ] && [ "$PROFILE_DENSITY" = "$DENSITY" ]; then
        return 0
    fi
    mkdir -p "$PROFILE_DIR" && chown 0:0 "$PROFILE_DIR" && chmod 700 "$PROFILE_DIR" || return 70
    mkdir "$CACHE_LOCK" 2>/dev/null || return 75
    CACHE_HELD=1
    trap cleanup_cache EXIT
    trap 'cleanup_cache; exit 70' HUP INT TERM
    [ ! -d "$LOCK" ] && [ "$(cat "$STATUS" 2>/dev/null || true)" = "$previous_transaction" ] || return 70
    profile_temp=$PROFILE_DIR/.display-profile.$$.new
    printf 'version=1\nmode=%s\ndensity=%s\n' "$MODE" "$DENSITY" > "$profile_temp" &&
    chown 0:0 "$profile_temp" && chmod 600 "$profile_temp" &&
    restorecon "$profile_temp" >/dev/null 2>&1 &&
    [ ! -d "$LOCK" ] && [ "$(cat "$STATUS" 2>/dev/null || true)" = "$previous_transaction" ] &&
    mv -f "$profile_temp" "$PROFILE" || {
        rm -f "$profile_temp"; return 70;
    }
    # Runtime-only durability flush; cold boot never calls sync or polls.
    bounded sync
}
cleanup_cache() {
    [ "$CACHE_HELD" = 1 ] || return 0
    rm -f "$PROFILE_DIR/.display-profile.$$.new"
    rmdir "$CACHE_LOCK" 2>/dev/null || true
    CACHE_HELD=0
}
now_seconds() {
    read uptime ignored < /proc/uptime
    printf '%s\n' "${uptime%%.*}"
}
bounded() {
    current=$(now_seconds)
    remain=$((DEADLINE - current))
    [ "$remain" -gt 0 ] || return 124
    [ "$remain" -le 5 ] || remain=5
    "$TIMEOUT" -k 1 -s TERM "$remain" "$@"
}
log_line() {
    printf '%s %s\n' "$(date '+%Y-%m-%dT%H:%M:%S')" "$*" >> "$ROOT/logs/display-mode.log" 2>/dev/null || true
}
write_status() {
    SEQUENCE=$((SEQUENCE + 1))
    temp=$STATUS.$$.new
    {
        printf 'version=1\nboot_id=%s\nsequence=%s\nrequest_id=%s\n' "$BOOT_ID" "$SEQUENCE" "$REQUEST_ID"
        printf 'phase=%s\nmode=%s\ndensity=%s\nresult=%s\ndetail=%s\n' "$1" "$MODE" "$DENSITY" "$2" "$3"
    } > "$temp" && chmod 600 "$temp" && mv -f "$temp" "$STATUS"
}
mark_failed() {
    grep -Fxq "$BOOT_ID $MODE" "$FAILED" 2>/dev/null && return 0
    printf '%s %s\n' "$BOOT_ID" "$MODE" >> "$FAILED"
    chmod 600 "$FAILED"
}
cleanup() {
    [ "$HELD" = 1 ] || return 0
    rm -f "$LOCK/owner" "$STATUS.$$.new"
    rmdir "$LOCK" 2>/dev/null || true
    HELD=0
}
lock_transaction() {
    mkdir -p "$ROOT/logs" || return 70
    chmod 700 "$ROOT" "$ROOT/logs" || return 70
    # A kernel-boot-specific pathname expires stale locks without a racy
    # remove/recreate step. A dead worker in this boot remains blocked: its
    # last graphics transition must not race a replacement worker.
    mkdir "$LOCK" 2>/dev/null || return 75
    HELD=1
    printf '%s %s\n' "$BOOT_ID" "$$" > "$LOCK/owner" || return 70
    chmod 700 "$LOCK"
    if [ -f "$STATUS" ] && grep -Fxq "boot_id=$BOOT_ID" "$STATUS"; then
        SEQUENCE=$(sed -n 's/^sequence=//p' "$STATUS")
        case "$SEQUENCE" in ''|*[!0-9]*) SEQUENCE=0 ;; esac
    fi
}
service_state() { bounded getprop "init.svc.$1"; }
framework_alive() { [ -n "$(bounded pidof system_server 2>/dev/null)" ]; }
wait_service() {
    tries=0
    while [ "$tries" -lt "$WAIT_SECONDS" ]; do
        [ "$(service_state "$1")" = "$2" ] && return 0
        bounded sleep 1 || return 1
        tries=$((tries + 1))
    done
    return 1
}
live_size() {
    bounded dumpsys SurfaceFlinger 2>/dev/null |
        sed -n 's/.*layerStack=0,[[:space:]]*([[:space:]]*\([0-9][0-9]*\)[[:space:]]*x[[:space:]]*\([0-9][0-9]*\)[[:space:]]*).*/\1x\2/p' |
        head -n 1
}
wm_snapshot() {
    WM_SIZE=$(bounded wm size 2>/dev/null) || return 1
    WM_DENSITY=$(bounded wm density 2>/dev/null) || return 1
    WM_PHYSICAL=$(printf '%s\n' "$WM_SIZE" | sed -n 's/^Physical size: *\([0-9]*x[0-9]*\).*$/\1/p' | head -n 1)
    WM_OVERRIDE=$(printf '%s\n' "$WM_SIZE" | sed -n 's/^Override size: *\([0-9]*x[0-9]*\).*$/\1/p' | head -n 1)
    WM_DENSITY_OVERRIDE=$(printf '%s\n' "$WM_DENSITY" | sed -n 's/^Override density: *\([0-9]*\).*$/\1/p' | head -n 1)
    WM_EFFECTIVE_DENSITY=$WM_DENSITY_OVERRIDE
    [ -n "$WM_EFFECTIVE_DENSITY" ] || WM_EFFECTIVE_DENSITY=$(printf '%s\n' "$WM_DENSITY" | sed -n 's/^Physical density: *\([0-9]*\).*$/\1/p' | head -n 1)
    [ -n "$WM_PHYSICAL" ] && valid_density "$WM_EFFECTIVE_DENSITY"
}
verify_target() {
    [ "$(bounded getprop "$PROP_MODE")" = "$MODE" ] || return 1
    [ "$(service_state hwcomposer-2-1)" = running ] &&
    [ "$(service_state surfaceflinger)" = running ] && framework_alive || return 1
    [ "$(live_size)" = "${MODE%@*}" ] || return 1
    wm_snapshot || return 1
    [ "$WM_PHYSICAL" = "${MODE%@*}" ] && [ -z "$WM_OVERRIDE" ] && [ "$WM_EFFECTIVE_DENSITY" = "$DENSITY" ]
}
restart_hwc() {
    # Explicit stop/start avoids the vendor HAL's onrestart cascade.
    RESTARTED=1
    bounded setprop ctl.stop surfaceflinger && wait_service surfaceflinger stopped || return 1
    bounded setprop ctl.stop hwcomposer-2-1 && wait_service hwcomposer-2-1 stopped || return 1
    bounded setprop ctl.start hwcomposer-2-1 && wait_service hwcomposer-2-1 running || return 1
    bounded setprop ctl.start surfaceflinger && wait_service surfaceflinger running || return 1
}
restart_framework() {
    expected_physical=${1:-${MODE%@*}}
    previous_server=$(bounded pidof system_server 2>/dev/null)
    bounded setprop ctl.restart zygote || return 1
    tries=0
    while [ "$tries" -lt "$WAIT_SECONDS" ]; do
        current_server=$(bounded pidof system_server 2>/dev/null)
        if [ -n "$current_server" ] && [ "$current_server" != "$previous_server" ] &&
           wm_snapshot && [ "$WM_PHYSICAL" = "$expected_physical" ]; then return 0; fi
        bounded sleep 1 || return 1
        tries=$((tries + 1))
    done
    return 1
}
restore_previous() {
    DEADLINE=$(($(now_seconds) + 120))
    write_status rolling_back 70 "$1" || return 1
    bounded setprop sys.carplay.display.active_density "$PREVIOUS_DENSITY" &&
    bounded setprop sys.carplay.display.active_mode "$PREVIOUS_MODE" &&
    bounded setprop "$PROP_MODE" "$PREVIOUS_MODE" || return 1
    bounded setprop "$PROP_RES" "$PREVIOUS_RES" || return 1
    if [ "$RESTARTED" = 1 ]; then
        restart_hwc && restart_framework "$PREVIOUS_PHYSICAL" || return 1
    fi
    if [ -n "$PREVIOUS_OVERRIDE" ]; then
        bounded wm size "$PREVIOUS_OVERRIDE" || return 1
    else
        bounded wm size reset || return 1
    fi
    if [ -n "$PREVIOUS_DENSITY_OVERRIDE" ]; then
        bounded wm density "$PREVIOUS_DENSITY_OVERRIDE" || return 1
    else
        bounded wm density reset || return 1
    fi
    [ "$(live_size)" = "$PREVIOUS_PHYSICAL" ] && wm_snapshot &&
    [ "$WM_PHYSICAL" = "$PREVIOUS_PHYSICAL" ] &&
    [ "$WM_OVERRIDE" = "$PREVIOUS_OVERRIDE" ] &&
    [ "$WM_EFFECTIVE_DENSITY" = "$PREVIOUS_DENSITY" ] &&
    [ "$(service_state surfaceflinger)" = running ] &&
    [ "$(service_state hwcomposer-2-1)" = running ] && framework_alive
}
fail_transaction() {
    reason=$1
    mark_failed || true
    if [ "$MUTATED" = 1 ]; then
        if restore_previous "$reason"; then detail=${reason}_rolled_back; else detail=${reason}_rollback_failed; fi
    else
        detail=$reason
    fi
    write_status failed 70 "$detail" || true
    log_line "request=$REQUEST_ID mode=$MODE failed detail=$detail"
    return 70
}
interrupted() {
    trap '' HUP INT TERM
    fail_transaction interrupted
    cleanup
    exit 70
}
cmd_prepare() {
    [ "$#" = 4 ] || return 64
    MODE=$1; DENSITY=$2; REQUEST_ID=$3
    valid_mode "$MODE" && valid_density "$DENSITY" && [ "$4" = --media-stopped ] || return 64
    case "$REQUEST_ID" in ''|*[!A-Za-z0-9._-]*) return 64 ;; esac
    [ "${#REQUEST_ID}" -le 64 ] && [ -n "$BOOT_ID" ] || return 64
    case "$BOOT_ID" in *[!A-Za-z0-9._-]*) return 64 ;; esac
    [ -x "$TIMEOUT" ] || { echo 'bounded command tool unavailable' >&2; return 70; }
    lock_transaction || return $?
    trap cleanup EXIT
    trap interrupted HUP INT TERM
    if grep -Fxq "$BOOT_ID $MODE" "$FAILED" 2>/dev/null; then
        write_status failed 76 failed_this_boot || return 70
        return 76
    fi
    DEADLINE=$(($(now_seconds) + 120))
    write_status applying 0 preflight || return 70
    PREVIOUS_PHYSICAL=$(live_size)
    wm_snapshot && valid_mode "$PREVIOUS_PHYSICAL@60" &&
    [ "$WM_PHYSICAL" = "$PREVIOUS_PHYSICAL" ] && framework_alive &&
    [ "$(service_state surfaceflinger)" = running ] &&
    [ "$(service_state hwcomposer-2-1)" = running ] || { fail_transaction preflight_failed; return $?; }
    PREVIOUS_MODE=$(bounded getprop "$PROP_MODE") || { fail_transaction preflight_failed; return $?; }
    # Restore the verified actual geometry if an old persisted property disagrees.
    [ "$PREVIOUS_MODE" = "$PREVIOUS_PHYSICAL@60" ] || PREVIOUS_MODE=$PREVIOUS_PHYSICAL@60
    PREVIOUS_RES=$(bounded getprop "$PROP_RES") || { fail_transaction preflight_failed; return $?; }
    PREVIOUS_OVERRIDE=$WM_OVERRIDE
    PREVIOUS_DENSITY_OVERRIDE=$WM_DENSITY_OVERRIDE
    PREVIOUS_DENSITY=$WM_EFFECTIVE_DENSITY
    if verify_target; then
        write_status complete 0 noop || return 70
        return 0
    fi
    MUTATED=1
    bounded setprop sys.carplay.display.active_density "$DENSITY" &&
    bounded setprop sys.carplay.display.active_mode "$MODE" &&
    bounded setprop "$PROP_MODE" "$MODE" && bounded setprop "$PROP_RES" Auto || { fail_transaction switch_failed; return $?; }
    if [ "$PREVIOUS_PHYSICAL" != "${MODE%@*}" ]; then
        restart_hwc && restart_framework || { fail_transaction switch_failed; return $?; }
    fi
    bounded wm size reset && bounded wm density "$DENSITY" || { fail_transaction wm_failed; return $?; }
    verify_target || { fail_transaction verification_failed; return $?; }
    MUTATED=0
    write_status complete 0 verified || return 70
    log_line "request=$REQUEST_ID mode=$MODE density=$DENSITY complete"
}
cmd_boot() {
    # Compatibility for an older rc only: no properties, WM calls, waits, or
    # service transitions. The installer removes the old boot worker trigger.
    mkdir -p "$ROOT" || return 70
    printf '%s\n' "$BOOT_ID" > "$ROOT/.display-mode-boot-id"
}
case "${1:-status}" in
    prepare|now) shift; cmd_prepare "$@" ;;
    remember) shift; cmd_remember "$@" ;;
    can-prepare) shift; cmd_can_prepare "$@" ;;
    profile) read_profile; printf 'mode=%s\ndensity=%s\n' "$PROFILE_MODE" "$PROFILE_DENSITY" ;;
    early) cmd_early ;;
    early-settings) cmd_early_settings ;;
    status) [ ! -f "$STATUS" ] || cat "$STATUS" ;;
    boot) cmd_boot ;;
    *) echo "usage: $0 prepare MODE DENSITY REQUEST_ID --media-stopped | remember MODE DENSITY | can-prepare MODE DENSITY | profile | status | early | early-settings | boot" >&2; exit 64 ;;
esac
