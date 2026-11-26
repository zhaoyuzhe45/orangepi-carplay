#!/system/bin/sh
# Explicit LAN fixture runner. Never installed as a boot/service script.
set -eu
PEER=$1
OUT=/data/local/tmp/carplay-services-bench
mkdir -p "$OUT"
cleanup() {
    am force-stop com.carplaybox.probe >/dev/null 2>&1 || true
    if [ -n "${bench_pid:-}" ]; then
        kill -TERM "$bench_pid" 2>/dev/null || true
        remaining=30
        while kill -0 "$bench_pid" 2>/dev/null && [ "$remaining" -gt 0 ]; do
            sleep 0.1
            remaining=$((remaining - 1))
        done
        # This is the exact child started below, never a production daemon.
        kill -KILL "$bench_pid" 2>/dev/null || true
        wait "$bench_pid" 2>/dev/null || true
    fi
    start carplay_box >/dev/null 2>&1 || true
}
trap cleanup EXIT INT TERM
stop carplay_box
sleep 2
CARPLAY_AIRPLAY_PROFILE=legacy /data/local/tmp/iap2d-services \
    --fg --airplay-endpoint "$PEER:7011" > "$OUT/native.log" 2>&1 &
bench_pid=$!
# The receiver timestamps scenarios from the accepted TCP connection. Align
# this runner with that boundary too; connection retries must not shorten the
# observation window or sample night mode before the receiver sends it.
attempt=0
until grep -q 'AirPlay TCP CONNECTED endpoint=' "$OUT/native.log"; do
    kill -0 "$bench_pid" 2>/dev/null || exit 1
    attempt=$((attempt + 1))
    [ "$attempt" -lt 150 ] || exit 1
    sleep 0.1
done
sleep 7
am start-foreground-service -n com.carplaybox.probe/com.carplaybox.ProbeService \
    --es action play --ei seconds 36 > "$OUT/play-start.log" 2>&1
sleep 14
cmd uimode night > "$OUT/night-at-21s.txt" 2>&1
sleep 3
cmd uimode night > "$OUT/night-at-24s.txt" 2>&1
sleep 2
am start-foreground-service -n com.carplaybox.probe/com.carplaybox.ProbeService \
    --es action record --ei seconds 8 > "$OUT/record-start.log" 2>&1
sleep 5
dumpsys media.audio_policy > "$OUT/audio-policy-during-mic.txt"
dumpsys media.audio_flinger > "$OUT/audio-flinger-during-mic.txt"
sleep 14
cp /data/data/com.carplaybox.probe/files/microphone.pcm "$OUT/microphone.pcm"
logcat -d -v threadtime -s CarMicrophoneBridge:I CarPlayAudioCapture:I \
    CarServicesProbe:I CarPlayVehicleLocation:I AndroidRuntime:E > "$OUT/java.log"
cat /proc/sys/kernel/random/boot_id > "$OUT/boot-id.txt"
