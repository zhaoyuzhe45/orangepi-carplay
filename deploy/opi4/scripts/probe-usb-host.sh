#!/system/bin/sh
# Host-side enumeration check, meant to be run on the box *after* plugging the
# USB port into a PC or head unit. Everything is graded, because "Windows shows
# a yellow mark" and "the accessory never enumerated" are completely different
# failures and look similar from the sofa.
#
#   su 0 sh /data/local/tmp/probe-usb-host.sh
#
# Three levels, each one a prerequisite for the next:
#   L1 enumeration  -- the host accepted our descriptor set and configured us
#   L2 NCM bind     -- the host side bound a NCM driver, so carrier came up
#   L3 data plane   -- bytes actually moved on usb0
# L1 failing is ours to fix. L2 failing is often the *host* lacking a NCM
# driver (plain Windows does), so it is reported as WARN, not FAIL.

G=/config/usb_gadget/g1
LOG=/data/carplay/logs/carplay-active.log
PASS=0; FAIL=0; WARN=0

ok()   { PASS=$((PASS+1)); echo "  [PASS]  $*"; }
bad()  { FAIL=$((FAIL+1)); echo "  [FAIL]  $*"; }
warn() { WARN=$((WARN+1)); echo "  [WARN]  $*"; }

echo "===== 0. gadget 是否就绪 ====="
svc=$(getprop init.svc.carplay_box 2>/dev/null)
echo "  carplay_box = ${svc:-unknown}"
[ "$svc" = "running" ] || bad "supervisor 没在跑，先解决它再看 USB（stop/start carplay_box）"

echo "  UDC      = $(cat "$G/UDC" 2>/dev/null)"
echo "  vid:pid  = $(cat "$G/idVendor" 2>/dev/null):$(cat "$G/idProduct" 2>/dev/null)"
echo "  bcdUSB   = $(cat "$G/bcdUSB" 2>/dev/null)"
echo "  os_desc  = use $(cat "$G/os_desc/use" 2>/dev/null), b.1 $(ls "$G/os_desc/b.1" 2>/dev/null || echo absent)"
echo "  b.1 链接 : $(ls "$G/configs/b.1" 2>/dev/null | grep '^f' | tr '\n' ' ')"

state=$(cat /sys/class/udc/*/state 2>/dev/null | head -1)
speed=$(cat /sys/class/udc/*/current_speed 2>/dev/null | head -1)
echo "  udc state = ${state:-?}   speed = ${speed:-?}"

if [ "$state" = "not attached" ]; then
    echo
    echo ">> 还没有检测到主机（state=not attached，插的是充电器或线只通供电）。"
    echo ">> 插到 PC / 车机后重跑本脚本。下面是当前基线，不做判定。"
    echo
    ip addr show usb0 2>/dev/null | grep -E 'link/ether|inet ' | tr -s ' ' | sed 's/^/  /'
    exit 0
fi

echo
echo "===== 1. 枚举（L1）====="
case "$state" in
    configured) ok "udc state = configured（主机已完成 SetConfiguration）" ;;
    addressed|default|powered|attached)
        bad "udc state = $state —— 主机只走到枚举中途就停了，看第 5 节的 ep0 SETUP" ;;
    *)  warn "udc state = $state（未知取值，仅供参考）" ;;
esac

[ "$speed" = "UNKNOWN" ] && bad "current_speed 仍是 UNKNOWN" || ok "current_speed = $speed"

# Endpoint files live under the FunctionFS mount, not in configfs: configfs only
# carries the "dev" attribute. ep0 is the control channel, ep1..epN the data
# endpoints the daemon opened, so count only the numbered ones.
nep=$(ls /dev/usb-ffs/iap2/ep[1-9]* 2>/dev/null | wc -l)
[ "$nep" -ge 4 ] && ok "FunctionFS 数据端点已创建（$nep 个，/dev/usb-ffs/iap2）" \
                 || bad "只有 $nep 个数据端点，描述符没被接受或没 bind"

for pat in "descriptors accepted" "FUNCTIONFS_BIND"; do
    if grep -q "$pat" "$LOG" 2>/dev/null; then
        ok "iap2d 日志有 \"$pat\""
    else
        bad "iap2d 日志缺 \"$pat\""
    fi
done

echo
echo "===== 2. NCM 绑定与 carrier（L2）====="
if [ ! -d /sys/class/net/usb0 ]; then
    bad "没有 usb0 网卡（内核 NCM 函数没起来？）"
else
    nm=$(cat "$G/functions/ncm.usb0/ifname" 2>/dev/null)
    echo "  kernel netdev = ${nm:-?}"
    car=$(cat /sys/class/net/usb0/carrier 2>/dev/null)
    oper=$(cat /sys/class/net/usb0/operstate 2>/dev/null)
    flags=$(ip link show usb0 2>/dev/null | sed -n '1p' | sed 's/.*<\(.*\)>.*/\1/')
    echo "  carrier=$car operstate=$oper flags=<$flags>"
    if [ "$car" = "1" ]; then
        ok "carrier=1 —— 主机侧 NCM 驱动已绑定，链路通了"
    else
        warn "carrier=0 —— 主机没绑 NCM 驱动。纯 Windows 常常如此（无自带 NCM INF），"
        warn "        请改用 DiPlay / Linux 主机复核；这不算我们这边的失败"
    fi
    ip addr show usb0 2>/dev/null | grep -E 'inet |inet6 ' | sed 's/^/  /'
fi

echo
echo "===== 3. 数据面（L3）====="
rx=$(cat /sys/class/net/usb0/statistics/rx_bytes 2>/dev/null)
tx=$(cat /sys/class/net/usb0/statistics/tx_bytes 2>/dev/null)
echo "  usb0 rx=$rx tx=$tx"
[ "${rx:-0}" -gt 0 ] 2>/dev/null && ok "rx_bytes > 0，主机发过包" || warn "rx_bytes = 0（主机没发过包，正常：NCM 要等 AirPlay 握手才走流量）"
[ "${tx:-0}" -gt 0 ] 2>/dev/null && ok "tx_bytes > 0，我们发过包" || warn "tx_bytes = 0"

echo
echo "===== 4. 内核侧证据 ====="
dmesg 2>/dev/null | grep -iE "usb0|ncm|dwc3.*(reset|speed|connect)|gadget.*(bind|config)" | tail -12 | sed 's/^/  /'

echo
echo "===== 5. ep0 上主机发来的 vendor 请求（CarPlay 识别特征）====="
# 0x53 answered with 00000001 then 0x51/0x52 -> a head unit that recognised us.
# Absent on a plain PC: it only reads descriptors and stops there.
grep -E "ep0 SETUP|ep0: 0x|unhandled vendor" "$LOG" 2>/dev/null | tail -10 | sed 's/^/  /'
if grep -q "bRequest=0x53" "$LOG" 2>/dev/null; then
    ok "主机发过 0xC0/0x53（Apple 识别握手）"
else
    warn "没见到 0x53 —— PC 一般只读描述符就停；只有车机/DiPlay 会走这套握手"
fi

echo
echo "===== 结论 ====="
echo "  PASS=$PASS  WARN=$WARN  FAIL=$FAIL"
if [ "$FAIL" -eq 0 ]; then
    echo "  枚举这一关过了。接下来看 carrier 与 AirPlay 能否建立会话。"
else
    echo "  有 FAIL：问题在我们这侧（描述符 / gadget 状态 / daemon），把上面全文发我。"
fi
