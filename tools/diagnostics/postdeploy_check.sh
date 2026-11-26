#!/system/bin/sh
# Post-reboot verification + the failed connection attempt forensics.
echo "== gadget state / service"
cat /sys/class/udc/fe800000.dwc3/state 2>/dev/null
getprop init.svc.carplay_box
echo "== dmesg: vendor requests since boot"
dmesg | grep "box: vendor req" | tail -12
echo "== dmesg: stall warnings"
dmesg | grep -c "ep0.c:317" || true
echo "== iap2d log tail"
tail -n 40 /data/carplay/logs/carplay-active.log
