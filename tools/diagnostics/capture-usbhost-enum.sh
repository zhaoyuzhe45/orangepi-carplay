#!/system/bin/sh
# Observe a CarPlay handoff; never change role, PHY, VBUS or services.
# Usage: su 0 sh capture-usbhost-enum.sh [capture_seconds=30] [wait_seconds=180]
#        su 0 sh capture-usbhost-enum.sh --snapshot | --buses
# Start BEFORE plugging in. xHCI ADDRESS_DEVICE is a controller command,
# not a normal control URB: usbmon alone cannot diagnose its completion.
set -u
USB_BUS=${USB_BUS:-/sys/bus/usb/devices}
MON_ROOT=${MON_ROOT:-/sys/kernel/debug/usb/usbmon}
TRACE_ROOT=${TRACE_ROOT:-/sys/kernel/debug/tracing}
MODE_PATH=${MODE_PATH:-/sys/devices/platform/usb0/dwc3_mode}
UDC=${CARPLAY_OTG_UDC:-fe800000.dwc3}
UDC_STATE=${UDC_STATE:-/sys/class/udc/$UDC/state}
OUT=${OUT:-/data/local/tmp}

target_buses() {
    for hub in "$USB_BUS"/usb[0-9]*; do
        [ -e "$hub" ] || continue
        target=$(readlink -f "$hub" 2>/dev/null) || continue
        case "$target/" in
            */"$UDC"/*) basename "$hub" | sed 's/^usb//' ;;
        esac
    done | sort -n
}
snapshot() {
    echo "uptime=$(cut -d' ' -f1 /proc/uptime)"
    echo "mode=$(cat "$MODE_PATH" 2>/dev/null) udc=$(cat "$UDC_STATE" 2>/dev/null)"
    echo "controller=$UDC target_buses=$(target_buses | tr '\n' ' ')"
    for hub in "$USB_BUS"/usb[0-9]*; do
        [ -e "$hub" ] && echo "$(basename "$hub") -> $(readlink -f "$hub")"
    done
    for bus in $(target_buses); do
        for dev in "$USB_BUS/$bus-"*; do
            [ -r "$dev/idVendor" ] || continue
            echo "device=$(basename "$dev") vid=$(cat "$dev/idVendor") pid=$(cat "$dev/idProduct") speed=$(cat "$dev/speed" 2>/dev/null)"
        done
    done
    for node in /sys/class/extcon/extcon*/state; do
        [ -r "$node" ] || continue
        echo "$node"; cat "$node"
    done
    for node in /sys/devices/platform/ff770000.syscon/ff770000.syscon:usb2-phy@e450/otg_mode /sys/kernel/debug/gpio; do
        [ -r "$node" ] || continue
        echo "$node"; cat "$node"
    done
    return 0
}
case ${1:-} in
    --buses) target_buses; exit 0 ;;
    --snapshot) snapshot; exit 0 ;;
esac
SECS=${1:-30}
WAIT_SECS=${2:-180}
case "$SECS:$WAIT_SECS" in *[!0-9:]*|:*|*:) echo 'durations must be integers' >&2; exit 2;; esac
if [ "$SECS" -lt 1 ] || [ "$SECS" -gt 120 ] || [ "$WAIT_SECS" -gt 600 ]; then
    echo 'capture must be 1..120 s; wait must be 0..600 s' >&2; exit 2
fi
mkdir -p "$OUT" || exit 2
RUN=$(mktemp -d "$OUT/usbhost-$(date +%Y%m%d-%H%M%S)-XXXXXX") || exit 2
SUM=$RUN/summary.txt
PIDS=
SEEN=' '
INSTANCE=
log() { echo "$*" | tee -a "$SUM"; }
cleanup() {
    for pid in $PIDS; do kill "$pid" 2>/dev/null || true; done
    for pid in $PIDS; do wait "$pid" 2>/dev/null || true; done
    if [ -n "$INSTANCE" ]; then
        echo 0 > "$INSTANCE/tracing_on" 2>/dev/null || true
        cat "$INSTANCE/trace" > "$RUN/xhci-events.txt" 2>/dev/null || true
        rmdir "$INSTANCE" 2>/dev/null || true
    fi
    dmesg > "$RUN/dmesg-after.txt" 2>&1
    snapshot > "$RUN/state-after.txt" 2>&1
    log "saved=$RUN"
}
trap cleanup EXIT
trap 'exit 130' INT
trap 'exit 143' TERM HUP
log "started; controller=$UDC capture=${SECS}s wait=${WAIT_SECS}s"
snapshot > "$RUN/state-before.txt" 2>&1
dmesg > "$RUN/dmesg-before.txt" 2>&1
# A private trace instance preserves Wi-Fi/global tracing. These old kernel
# events lack a controller field: correlate timestamps with saved topology.
candidate=$TRACE_ROOT/instances/carplay-enum-$$
if [ -d "$TRACE_ROOT/instances" ] && mkdir "$candidate" 2>/dev/null; then
    INSTANCE=$candidate
    echo 0 > "$INSTANCE/tracing_on"
    echo 512 > "$INSTANCE/buffer_size_kb"
    enabled=0
    for event in xhci_handle_command xhci_handle_cmd_addr_dev xhci_handle_port_status xhci_setup_device xhci_dbg_address; do
        node=$INSTANCE/events/xhci-hcd/$event/enable
        if [ -w "$node" ] && echo 1 > "$node"; then enabled=$((enabled + 1)); fi
    done
    echo 1 > "$INSTANCE/tracing_on"
    log "xHCI events enabled=$enabled (all controllers; correlate topology)"
else
    log 'xHCI tracing unavailable; usbmon does not capture ADDRESS_DEVICE command completion'
fi
start_monitors() {
    for bus in $(target_buses); do
        case "$SEEN" in *" $bus "*) continue;; esac
        SEEN="$SEEN$bus "
        log "target bus=$bus path=$(readlink -f "$USB_BUS/usb$bus")"
        if [ -r "$MON_ROOT/${bus}u" ]; then
            head -c 4194304 "$MON_ROOT/${bus}u" > "$RUN/usbmon-$bus.txt" 2> "$RUN/usbmon-$bus.err" &
            PIDS="$PIDS $!"
        else
            log "usbmon unavailable for bus=$bus"
        fi
    done
}
i=0
limit=$((WAIT_SECS * 10))
while :; do
    mode=$(cat "$MODE_PATH" 2>/dev/null)
    buses=$(target_buses)
    if [ "$mode" = host ] && [ -n "$buses" ]; then break; fi
    if [ "$i" -ge "$limit" ]; then
        log "WAIT_TIMEOUT: no host bus for $UDC; mode=$mode udc=$(cat "$UDC_STATE" 2>/dev/null)"
        exit 1
    fi
    if [ $((i % 20)) -eq 0 ]; then
        echo "uptime=$(cut -d' ' -f1 /proc/uptime) mode=$mode udc=$(cat "$UDC_STATE" 2>/dev/null)" >> "$RUN/timeline.txt"
    fi
    sleep 0.1
    i=$((i + 1))
done
log "host observed after $i polls; starting capture"
start_monitors
snapshot > "$RUN/state-host.txt" 2>&1
i=0
while [ "$i" -lt "$((SECS * 5))" ]; do
    start_monitors
    if [ $((i % 5)) -eq 0 ]; then snapshot >> "$RUN/timeline.txt" 2>&1; fi
    sleep 0.2
    i=$((i + 1))
done
log 'capture complete; this is not a CarPlay success verdict'
