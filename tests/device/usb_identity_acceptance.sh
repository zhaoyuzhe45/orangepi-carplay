#!/system/bin/sh
# Run as root while the phone/head unit is disconnected. Replays the init
# property that used to overwrite PID, then verifies stop/start restoration.
set -eu
[ "$(id -u)" = 0 ] || { echo 'root required'; exit 1; }
[ "$(getprop ro.product.device)" = rk3399_mid ] || { echo 'wrong target'; exit 1; }
root=/data/carplay
g=/config/usb_gadget/g1
wait_attached() {
    tries=0
    while [ "$tries" -lt 150 ]; do
        if [ "$(cat "$g/idVendor")" = 0x05ac ] &&
           [ "$(cat "$g/idProduct")" = 0x12a8 ] &&
           [ "$(getprop sys.usb.configfs)" = carplay ] &&
           [ "$(cat "$g/UDC")" = fe800000.dwc3 ]; then
            return 0
        fi
        sleep 0.1
        tries=$((tries + 1))
    done
    echo 'CarPlay identity did not become ready' >&2
    return 1
}
wait_attached
[ "$(getprop sys.usb.config)" = adb ] || { echo 'test expects stock adb config'; exit 1; }
[ "$(cat /sys/class/udc/fe800000.dwc3/state)" != configured ] || {
    echo 'disconnect USB host before running this test'; exit 1;
}
supervisor_pid=$(cat "$root/supervisor.pid")
daemon_pid=$(cat "$root/daemon.pid")
for cycle in 1 2 3; do
    setprop sys.usb.config adb
    sleep 1
    [ "$(cat "$g/idProduct")" = 0x12a8 ]
    [ "$(cat "$g/idVendor")" = 0x05ac ]
    [ "$(getprop sys.usb.configfs)" = carplay ]
    [ "$(cat "$root/supervisor.pid")" = "$supervisor_pid" ]
    [ "$(cat "$root/daemon.pid")" = "$daemon_pid" ]
    kill -0 "$supervisor_pid"
    kill -0 "$daemon_pid"
    echo "replay=$cycle identity=05ac:12a8 supervisor=$supervisor_pid daemon=$daemon_pid"
done
original_gate=$(sed -n '21p' "$root/usb-stock.state")
original_vid=$(sed -n '3p' "$root/usb-stock.state")
original_pid=$(sed -n '4p' "$root/usb-stock.state")
# Always bring the service back if any restore assertion fails.
trap 'start carplay_box' EXIT HUP INT TERM
stop carplay_box
tries=0
while [ "$(getprop init.svc.carplay_box)" != stopped ] && [ "$tries" -lt 150 ]; do
    sleep 0.1
    tries=$((tries + 1))
done
[ "$(getprop init.svc.carplay_box)" = stopped ]
sh "$root/bin/carplay_supervisor.sh" --restore-stock
[ "$(getprop sys.usb.configfs)" = "$original_gate" ]
[ "$(cat "$g/idVendor")" = "$original_vid" ]
[ "$(cat "$g/idProduct")" = "$original_pid" ]
echo "restore=PASS configfs=$original_gate identity=$original_vid:$original_pid"
start carplay_box
wait_attached
trap - EXIT HUP INT TERM
echo 'USB identity acceptance PASS (three stock adb replays, stock restore, service restart)'
