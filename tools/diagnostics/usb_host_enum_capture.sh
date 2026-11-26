#!/system/bin/sh
# 抓取"外来设备插到盒子 USB host 口"时的描述符快照。
#
# 目的：拿一份「能被真车机接受」的 CarPlay 盒子的 USB 描述符（例如喵驾 DDBox010），
# 和 iap2d 自造的 0xFF/0x42/0x01 + NCM 那套逐项对比。
#
# 这类盒子插到非车机的普通 host 上，因为等不到 iAP2 会话，会反复 detach/attach
# 甚至整机重启。所以这里不追求"连上就一直读"，而是：每次设备节点变化就立刻整段
# dump 一次，能捞到几次算几次——反复枚举反而是多给几次机会。
#
# 采集对象：
#   /sys/kernel/debug/usb/devices  完整的 lsusb -t + 描述符文本（厂商/产品/端点/配置/电量）
#   sysfs idVendor/idProduct/...   兜底：调试文件系统没挂上时也能拿到关键字段
#
# 用法：
#   adb push tools/diagnostics/usb_host_enum_capture.sh /data/local/tmp/
#   adb shell "chmod 755 /data/local/tmp/usb_host_enum_capture.sh && setsid sh /data/local/tmp/usb_host_enum_capture.sh 300 &"
#   插拔设备，然后：
#   adb shell cat /data/carplay/logs/usb-host-enum.log
#   停止：adb shell "kill \$(cat /data/local/tmp/usb_host_enum.pid)"

LOG=/data/carplay/logs/usb-host-enum.log
PID=/data/local/tmp/usb_host_enum.pid
DUR=${1:-300}

[ -r /sys/kernel/debug/usb/devices ] || mount -t debugfs none /sys/kernel/debug 2>/dev/null

echo $$ > "$PID"
t0=$(cut -d' ' -f1 /proc/uptime)
end=$(awk -v t="$t0" -v d="$DUR" 'BEGIN{printf "%d", t+d}')
echo "--- capture start t=$t0 dur=${DUR}s" >> "$LOG"
echo "--- root hubs ---" >> "$LOG"
cat /sys/kernel/debug/usb/devices >> "$LOG" 2>/dev/null

# 下游设备节点形如 1-1 / 1-1.2，root hub 是 usb1、1-0:1.0 这种，排除掉
list_devs() {
    ls /sys/bus/usb/devices 2>/dev/null | grep -E '^[0-9]+-[0-9]+(\.[0-9]+)*$' | tr '\n' ' '
}

last=$(list_devs)
n=0
while :; do
    now=$(cut -d' ' -f1 /proc/uptime)
    [ "$now" -ge "$end" ] && break
    cur=$(list_devs)
    if [ "$cur" != "$last" ]; then
        n=$((n+1))
        echo "===== [$n] t=$now devices=[$cur] =====" >> "$LOG"

        # 先落 dmesg 里的枚举行，"new high-speed USB device number"/"New USB device found"
        # 能看出它是 USB2 还是 USB3、VID/PID、以及是否被 reset
        dmesg | grep -iE "new .*USB device|USB disconnect|reset .*USB device|device descriptor" | tail -n 8 >> "$LOG"

        # 关键：整段描述符
        cat /sys/kernel/debug/usb/devices >> "$LOG" 2>/dev/null

        # 兜底字段，调试文件系统缺失时靠这个
        for d in $cur; do
            p=/sys/bus/usb/devices/$d
            echo "--- sysfs $d ---" >> "$LOG"
            for f in idVendor idProduct bDeviceClass bDeviceSubClass bDeviceProtocol \
                     bcdUSB bcdDevice bNumConfigurations speed manufacturer product serial; do
                [ -r "$p/$f" ] && echo "  $f=$(cat "$p/$f" 2>/dev/null)" >> "$LOG"
            done
            for c in "$p"/*/bMaxPower; do
                [ -r "$c" ] && echo "  bMaxPower=$(cat "$c" 2>/dev/null) ($c)" >> "$LOG"
            done
        done
        grep -E "^(I|E):" /sys/kernel/debug/usb/devices 2>/dev/null | tail -n 40 >> "$LOG"
        last="$cur"
    fi
    sleep 0.1
done

echo "--- capture end t=$(cut -d' ' -f1 /proc/uptime) events=$n ---" >> "$LOG"
rm -f "$PID"
