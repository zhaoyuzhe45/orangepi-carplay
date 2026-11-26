#!/system/bin/sh
# Pre-deploy check: device state, flash tooling, current kernel build.
echo "== uptime / date"
cat /proc/uptime
date
echo "== flash tools on device"
ls -l /data/local/tmp/flash_boot.sh /data/local/tmp/flash_resource.sh 2>/dev/null || echo "flash_boot.sh MISSING"
echo "== current kernel"
uname -a
echo "== gadget state"
cat /sys/class/udc/fe800000.dwc3/state 2>/dev/null
echo "== iap2d / supervisor"
ps -A 2>/dev/null | grep -E "iap2d|carplay_supervisor" | head -5
getprop init.svc.carplay_box
