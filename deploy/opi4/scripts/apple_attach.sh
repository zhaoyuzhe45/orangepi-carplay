#!/system/bin/sh
# Manual control for the single supervised CarPlay daemon.

set -u

ROOT=/data/carplay
SUPERVISOR=$ROOT/bin/carplay_supervisor.sh
PID_FILE=$ROOT/supervisor.pid
DAEMON_PID_FILE=$ROOT/daemon.pid
LOCK_DIR=$ROOT/supervisor.lock

log_event() {
    log -t CarPlayBoxSupervisor "$*" 2>/dev/null || echo "Supervisor: $*" >&2
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

wait_for_supervisor() {
    tries=0
    while [ "$tries" -lt 100 ]; do
        if [ -f "$PID_FILE" ]; then
            supervisor_pid=$(cat "$PID_FILE" 2>/dev/null || true)
            if [ -n "$supervisor_pid" ] && kill -0 "$supervisor_pid" 2>/dev/null; then
                echo "$supervisor_pid"
                return 0
            fi
        fi
        sleep 0.1
        tries=$((tries + 1))
    done
    return 1
}

terminate_matching() {
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

stop_all() {
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
    terminate_matching carplay_supervisor.sh
    terminate_matching /data/carplay/bin/iap2d
    rm -f "$PID_FILE" "$DAEMON_PID_FILE" 2>/dev/null || true
    rm -rf "$LOCK_DIR" 2>/dev/null || true
    # Android init hard-stop bypasses supervisor traps on this vendor image.
    # Restore the captured gadget and release its USB property gate explicitly.
    /system/bin/sh "$SUPERVISOR" --restore-stock || return 1
}

start_all() {
    stop_all
    if [ -n "$(getprop init.svc.carplay_box 2>/dev/null || true)" ]; then
        start carplay_box >/dev/null 2>&1 || true
    else
        # The init rc is parsed on the next boot. Use one guarded manual launch
        # until then; the supervisor mkdir lock rejects any duplicate process.
        nohup /system/bin/sh "$SUPERVISOR" --force-attach >/dev/null 2>&1 </dev/null &
    fi
    wait_for_supervisor || { log_event "manual start failed"; return 1; }
}

case "${1:-start}" in
    status)
        pid=$(wait_for_supervisor) || { echo "stopped"; exit 1; }
        service_state=$(getprop init.svc.carplay_box 2>/dev/null || true)
        echo "running pid=$pid service=${service_state:-manual}"
        ;;
    stop)
        stop_all
        log_event "manual stop"
        echo "stopped"
        ;;
    start|restart)
        pid=$(start_all) || exit 1
        log_event "manual start pid=$pid"
        echo "started pid=$pid"
        ;;
    *)
        echo "usage: $0 [start|restart|stop|status]" >&2
        exit 64
        ;;
esac
