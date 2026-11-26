#!/system/bin/sh
echo "pid: $(pgrep -f carplay/bin/iap2d)"
cat /sys/class/udc/fe800000.dwc3/state
cat /sys/devices/platform/usb0/dwc3_mode
echo "--- last 6 log lines ---"
tail -n 6 /data/carplay/logs/carplay-active.log | tr -d '\000'
