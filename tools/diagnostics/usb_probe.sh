#!/system/bin/sh
# 持续采样 OTG 口的物理层状态，用于捕获插拔瞬间的 CC / VBUS 反应。
# 只在状态变化时落日志，避免刷盘。
#
# 观察对象：
#   extcon1 (fusb302) 的 state  -> USB/CDP/DCP/... 反映 CC 与 BC1.2 判定
#   extcon0 (usb2-phy) 的 state  -> USB_HOST / USB_VBUS_EN 反映 phy 侧 vbus 开关
#   UDC state                   -> not attached / powered / default / configured
#   dwc3 中断计数                -> 为 0 表示控制器从未收到任何总线事件（无 VBUS）
#
# 用法：setsid sh usb_probe.sh &     （停止：kill $(cat /data/local/tmp/usb_probe.pid)）

LOG=/data/carplay/logs/usb-probe.log
PID=/data/local/tmp/usb_probe.pid

echo $$ > "$PID"
echo "--- probe start uptime=$(cut -d' ' -f1 /proc/uptime)" >> "$LOG"

last=""
while :; do
    e1=$(tr '\n' ',' < /sys/class/extcon/extcon1/state 2>/dev/null)
    e0=$(tr '\n' ',' < /sys/class/extcon/extcon0/state 2>/dev/null)
    u=$(cat /sys/class/udc/fe800000.dwc3/state 2>/dev/null)
    irq=$(awk '/dwc3/{s=0; for(i=2;i<=NF-2;i++) s+=$i; print s}' /proc/interrupts 2>/dev/null)
    [ -n "$irq" ] || irq=0
    cur="$e1|$e0|$u|$irq"
    if [ "$cur" != "$last" ]; then
        t=$(cut -d' ' -f1 /proc/uptime)
        echo "$t udc=$u dwc3_irq=$irq" >> "$LOG"
        echo "$t    extcon1[$e1]" >> "$LOG"
        echo "$t    extcon0[$e0]" >> "$LOG"
        last="$cur"
    fi
    sleep 0.2
done
