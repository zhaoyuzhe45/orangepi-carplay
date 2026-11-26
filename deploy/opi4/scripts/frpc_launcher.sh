#!/system/bin/sh
# Keeps frpc alive on the OPi4 so the device can be reached from outside the
# bench network without a USB cable or a port forward on the router.
#
# Two things this wrapper owns that frpc does not:
#   1. Booting happens before WiFi.  frpc is started only once a default route
#      exists, otherwise the first minutes are a connect-fail loop.
#   2. The token lives in the config, so an unfilled template must not be able
#      to spin.  While /data/carplay/frpc.toml still holds __FRP_* placeholders
#      this loop idles; writing the real config starts frpc without a restart.

ROOT=/data/carplay
BIN=$ROOT/bin/frpc
CONF=$ROOT/frpc.toml
LOG=$ROOT/logs/frpc-supervisor.log

mkdir -p "$ROOT/logs" 2>/dev/null || true

log() {
    printf '%s %s\n' "$(date '+%Y-%m-%d %H:%M:%S')" "$*" >> "$LOG" 2>/dev/null || true
}

config_ready() {
    [ -f "$CONF" ] || return 1
    grep -q '__FRP_' "$CONF" 2>/dev/null && return 1
    return 0
}

wait_for_network() {
    waited=0
    while [ "$waited" -lt 300 ]; do
        # Android keeps the default route in a per-interface table (policy
        # routing), so "ip route" on the main table has no default line even
        # when the link works.  An IPv4 address on an uplink is the reliable
        # "network is up" signal here; frpc retries the rest by itself.
        # iap2ncm0 is deliberately excluded: it always carries 169.254.0.1.
        for uplink in wlan0 eth0; do
            if ip addr show "$uplink" 2>/dev/null | grep -q 'inet '; then
                return 0
            fi
        done
        sleep 2
        waited=$((waited + 2))
    done
    return 1
}

if [ ! -x "$BIN" ]; then
    log "frpc binary missing at $BIN, staying idle"
fi

# Wait for a real config.  A correctly installed device gets past this on the
# first evaluation; a template-only device parks here instead of restarting.
while ! config_ready; do
    log "frpc.toml not configured yet (missing file or __FRP_* placeholder), idling"
    sleep 60
done

log "config accepted, waiting for network"

while :; do
    if ! wait_for_network; then
        log "wlan0 has no IPv4 address after 300s, retrying"
        sleep 10
        continue
    fi
    log "starting frpc -c $CONF"
    "$BIN" -c "$CONF" >> "$LOG" 2>&1
    rc=$?
    # frpc only returns on config error, auth rejection or fatal network state.
    # Back off a little so a bad token does not hammer the server.
    log "frpc exited rc=$rc, restarting in 20s"
    sleep 20
done
