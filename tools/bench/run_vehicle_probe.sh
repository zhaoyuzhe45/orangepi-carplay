#!/system/bin/sh
# Isolated acceptance only. Restore the user's settings and normal services.
set -eu
SCENARIO=${1:-gps}
OUT=/data/local/tmp/carplay-vehicle-probe
MARKER=/data/data/com.carplaybox.audio/files/vehicle-probe-ready
mkdir -p "$OUT"
MODE=$(settings get secure location_mode)
PROVIDERS=$(settings get secure location_providers_allowed)
cleanup() {
    if [ "$SCENARIO" = revocation ]; then appops set com.carplaybox.audio MOCK_LOCATION allow; fi
    if [ "$MODE" = null ]; then settings delete secure location_mode >/dev/null; else settings put secure location_mode "$MODE"; fi
    for restored in $(echo "$PROVIDERS" | tr ',' ' '); do
        settings put secure location_providers_allowed "+$restored"
    done
    am start-foreground-service -n com.carplaybox.audio/.AudioCaptureService >/dev/null 2>&1 || true
    am start-foreground-service -n com.carplaybox.audio/.VehicleLocationService >/dev/null 2>&1 || true
}
trap cleanup EXIT INT TERM
am stopservice -n com.carplaybox.audio/.VehicleLocationService > "$OUT/stop.log" || true
rm -f "$MARKER"
am instrument -w -e staged true -e scenario "$SCENARIO" com.carplaybox.vehicleprobe/com.carplaybox.audio.VehicleLocationProbe > "$OUT/result.txt" 2>&1 &
probe_pid=$!
n=0
while [ ! -f "$MARKER" ] && [ "$n" -lt 15 ] && kill -0 "$probe_pid" 2>/dev/null; do
    sleep 1
    n=$((n + 1))
done
if [ -f "$MARKER" ]; then
  if [ "$SCENARIO" = revocation ]; then
    appops set com.carplaybox.audio MOCK_LOCATION ignore
    sleep 3
    appops set com.carplaybox.audio MOCK_LOCATION allow
  else
    settings put secure location_mode 0
    settings put secure location_providers_allowed -gps
    settings put secure location_providers_allowed -network
    settings get secure location_providers_allowed > "$OUT/disabled-setting.txt"
  fi
fi
wait "$probe_pid" || true
dumpsys location > "$OUT/location-after.txt"
cat "$OUT/result.txt"
if [ "$SCENARIO" = revocation ]; then
    grep -q 'revokedUpdates=' "$OUT/result.txt"
else
    grep -q 'freshUpdatesWithGpsDisabled=4' "$OUT/result.txt"
fi
grep -q 'providerRemoved=true' "$OUT/result.txt"
grep -q 'INSTRUMENTATION_CODE: 0' "$OUT/result.txt"
