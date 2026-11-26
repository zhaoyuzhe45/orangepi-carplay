#!/system/bin/sh
set -eu
NODE=/sys/kernel/debug/dri/0/HDMI-A-1
dd if=/data/local/tmp/carplay-edid.bin of="$NODE/edid_override" bs=128 count=1
printf on > "$NODE/force"
sleep 2
echo "force=$(cat "$NODE/force")"
echo "status=$(cat /sys/class/drm/card0-HDMI-A-1/status)"
echo modes=
cat /sys/class/drm/card0-HDMI-A-1/modes
echo size=
wm size
echo density=
wm density
