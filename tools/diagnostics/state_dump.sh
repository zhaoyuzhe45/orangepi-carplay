#!/system/bin/sh
# Snapshot of everything that explains "the car listed CarPlay but could not connect".
set -u
L=/data/carplay/logs
echo "===== 1. identity ====="
cat /proc/uptime
cat /sys/class/udc/fe800000.dwc3/state 2>/dev/null || echo "udc-state: (none)"
cat /sys/devices/platform/usb0/dwc3_mode 2>/dev/null || echo "dwc3_mode: (none)"
getprop init.svc.carplay_box; getprop init.svc.carplay_frpc

echo "===== 2. role bookkeeping ====="
for f in usb-role.state role-switch.failures role-request role-swap.stamp; do
	printf '%s=' "$f"; cat /data/carplay/$f 2>/dev/null || echo "(absent)"
done

echo "===== 3. carrier / vbus hints ====="
for p in /sys/class/udc/fe800000.dwc3/*; do
	n=$(basename "$p")
	case "$n" in state|soft_connect|current_speed|maximum_speed) printf '%s=' "$n"; cat "$p" 2>/dev/null;; esac
done
grep -E 'vbus|VBU S' /sys/kernel/debug/regulator/regulator_summary 2>/dev/null | head -10

echo "===== 4. usb bus now ====="
for d in /sys/bus/usb/devices/*; do
	[ -d "$d" ] || continue
	case "$(basename "$d")" in *:*) continue;; esac
	printf '%s vid=%s pid=%s prod=%s speed=%s\n' "$(basename "$d")" \
		"$(cat "$d/idVendor" 2>/dev/null)" "$(basename "$d")" \
		"$(cat "$d/product" 2>/dev/null)" "$(cat "$d/speed" 2>/dev/null)"
done

echo "===== 5. dmesg: usb core / xhci / dwc3 ====="
dmesg | grep -aE 'xhci|dwc3|usb 5-|usb 6-|new (high|full|super)-speed|device not accepting|set new mode|box: vendor' | tail -60

echo "===== 6. active log tail (last attempt) ====="
tail -n 90 $L/carplay-active.log 2>/dev/null | tr -d '\000'

echo "===== 7. supervisor tail ====="
tail -n 25 $L/supervisor.log 2>/dev/null | tr -d '\000'
