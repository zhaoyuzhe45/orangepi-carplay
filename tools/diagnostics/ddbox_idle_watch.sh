#!/system/bin/sh
# Zero-interference observation: watch whether DDBox stays on the bus when the host
# touches nothing at all. Reads sysfs and dmesg only.
#
# No SET_CONFIGURATION, no interface claim, no vendor request, no endpoint I/O.
# Purpose: separate "firmware watchdog resets itself" from "our probes reset it".

BUS=/sys/bus/usb/devices
ITER=24
STEP=5

dump_table() {
    for f in "$BUS"/*/idVendor; do
        d=${f%/idVendor}
        v=$(cat "$f" 2>/dev/null)
        p=$(cat "$d/idProduct" 2>/dev/null)
        c=$(cat "$d/bConfigurationValue" 2>/dev/null)
        n=$(cat "$d/bNumConfigurations" 2>/dev/null)
        printf '  %-10s %s:%s cfg=%s ncfg=%s\n' "$(basename "$d")" "$v" "$p" "${c:-?}" "${n:-?}"
    done
}

count_apple() {
    c=0
    for f in "$BUS"/*/idVendor; do
        if [ "$(cat "$f" 2>/dev/null)" = "05ac" ]; then c=$((c + 1)); fi
    done
    echo "$c"
}

count_disc() {
    dmesg 2>/dev/null | grep -c "USB disconnect"
}

echo "=== BASELINE ==="
dump_table
echo "apple_devices=$(count_apple) disconnects=$(count_disc)"

echo "=== WATCH ${ITER}x${STEP}s, host does nothing ==="
i=0
while [ $i -lt $ITER ]; do
    echo "t=$((i * STEP))s apple=$(count_apple) disc=$(count_disc)"
    sleep $STEP
    i=$((i + 1))
done

echo "=== FINAL ==="
dump_table
echo "apple_devices=$(count_apple) disconnects=$(count_disc)"
echo "=== DMESG USB TAIL ==="
dmesg 2>/dev/null | grep -iE "usb [0-9]-|ipheth" | tail -20
