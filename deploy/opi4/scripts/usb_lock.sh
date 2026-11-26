#!/system/bin/sh
# usb_lock.sh — 把 USB 端口钉死在设备（peripheral）角色，并把 ADB 从 USB 总线上摘掉。
#
# 为什么分两层实现（本脚本 = 运行时，不刷机）：
#
#   1. 属性闸门
#      sys.usb.configfs=carplay            → init 的 configfs USB 分支全部要求 =1，整段失效
#      sys.usb.config    不含 adb           → /init.usb.configfs.rc 里
#                                             `sys.usb.ffs.ready=1 && sys.usb.config=adb`
#                                             这条链 ffs.adb 的规则永远不再匹配
#   2. 持久默认值
#      persist.sys.usb.config=none         → 下次开机 framework 不再要求 adb/mtp，
#                                             从源头消掉「ADB 先枚举」这一步
#   3. 配置里清场
#      configs/*/f* 里只允许指向 ffs.iap2。若同一配置里已经挂着 ffs.iap2，却还多出
#      ffs.adb / mtp / ptp / rndis / accessory，说明 init 又把安卓类接口塞回来了，
#      立刻摘掉（混合布局 = 车机读成「安卓手机」的直接原因）
#   4. 角色锁定
#      写内核导出的角色节点。这个内核到底导出哪种 ABI 只能实测，所以 apply 会先 probe，
#      只写语义确定的节点；一个可写节点都没有时会明确报 role=uncontrollable ——
#      那一项只能在 dts 里做（dr_mode = "peripheral"），见 docs/usb-lock-layers.md
#
# 明确不做的事：不 stop adbd。adbd 还扛着 TCP(5555) 远程通道，是这台设备唯一的维护入口。
# USB 上的 ADB 是靠「不把 ffs.adb 链进 gadget」断的，杀 adbd 属于误伤。
#
# 用法：
#   usb_lock.sh status      只读快照
#   usb_lock.sh probe       列出角色节点候选与可写性（只读）
#   usb_lock.sh apply       应用全部运行时措施（幂等）
#   usb_lock.sh reassert    只做便宜的漂移检查，用于周期巡检
#   usb_lock.sh watch       常驻巡检（supervisor 启动时拉起）
#   usb_lock.sh reset       把 persist 默认值恢复成 usb-stock.state 里记录的值
#
# 退出码：0 全部生效（含「角色由 dts 负责」这种已知不可控）/ 1 有项真的没做到

G=${USB_GADGET_DIR:-/config/usb_gadget/g1}
UDC_NAME=${USB_UDC_NAME:-fe800000.dwc3}
UDC_CLASS=${UDC_CLASS_DIR:-/sys/class/udc/$UDC_NAME}
# 这台镜像的 dwc3 平台设备并不在 /sys/devices/platform/<udc>，而是嵌在 usb0 控制器下：
#   /sys/devices/platform/usb0/fe800000.dwc3
# 写死任何一个路径都会在其他镜像上悄悄失效，所以经 /sys/class/udc 反解，
# 取不到再退回两个已知形态。
if [ -n "${DWC3_DIR:-}" ]; then
    DWC3=$DWC3_DIR
else
    DWC3=$(readlink -f "$UDC_CLASS" 2>/dev/null | sed 's#/udc/[^/]*$##')
    [ -n "$DWC3" ] && [ -d "$DWC3" ] || DWC3=/sys/devices/platform/usb0/$UDC_NAME
    [ -d "$DWC3" ] || DWC3=/sys/devices/platform/$UDC_NAME
fi
ROOT=${CARPLAY_ROOT:-/data/carplay}
LOG_DIR=${CARPLAY_LOG_DIR:-$ROOT/logs}
LOG=$LOG_DIR/usb-lock.log
STATE_FILE=${USB_STOCK_STATE:-$ROOT/usb-stock.state}
LOG_MAX_BYTES=${USB_LOCK_LOG_MAX:-262144}
WATCH_INTERVAL=${USB_LOCK_WATCH_INTERVAL:-2}

# getprop/setprop 在离线测试沙箱里不存在，缺了就当成「无属性系统」，不要因此失败。
prop_get() {
    command -v getprop >/dev/null 2>&1 || return 0
    getprop "$1" 2>/dev/null || true
}

prop_set() {
    command -v setprop >/dev/null 2>&1 || return 0
    setprop "$1" "$2" 2>/dev/null || true
}

log() {
    [ -d "$LOG_DIR" ] || mkdir -p "$LOG_DIR" 2>/dev/null || return 0
    {
        printf '%s %s\n' "$(date '+%Y-%m-%d %H:%M:%S' 2>/dev/null)" "$*"
    } >> "$LOG" 2>/dev/null || return 0
    # 巡检每 2 秒跑一次，日志必须自己封顶，否则能吃掉 /data。
    size=$(wc -c < "$LOG" 2>/dev/null || echo 0)
    if [ "${size:-0}" -gt "$LOG_MAX_BYTES" ]; then
        tail -n 400 "$LOG" > "$LOG.tmp" 2>/dev/null && mv -f "$LOG.tmp" "$LOG" 2>/dev/null
    fi
}

udc_state() {
    cat "$UDC_CLASS/state" 2>/dev/null || true
}

gadget_vid() {
    cat "$G/idVendor" 2>/dev/null || true
}

gadget_pid() {
    cat "$G/idProduct" 2>/dev/null || true
}

udc_bound() {
    cat "$G/UDC" 2>/dev/null || true
}

# Two one-line indirections so the guard's decision logic can be exercised
# without a real configfs tree (Windows cannot create unprivileged symlinks).
# Production behaviour is unchanged: `[ -L ]` plus readlink.
is_link() {
    [ -L "$1" ]
}

link_target() {
    readlink "$1" 2>/dev/null || true
}

# ---------------------------------------------------------------- 角色节点探测

# 只列语义已知的候选。otg_mode / dr_mode / role 这类名字在不同厂商核里含义不一致
# （有的 0=host，有的 1=host），盲写有可能把控制器锁进 host —— 那比不锁更糟，
# 所以只探测、记录，不写。
role_nodes() {
    for node in "$DWC3/mode" "$DWC3/extcon" "$DWC3/otg_mode" "$DWC3/dr_mode" "$DWC3/role"; do
        [ -e "$node" ] && printf '%s\n' "$node"
    done
}

role_describe() {
    node=$1
    value=$(cat "$node" 2>/dev/null || true)
    if [ -w "$node" ]; then writable=yes; else writable=no; fi
    printf '  %s = [%s] writable=%s\n' "$node" "$value" "$writable"
}

probe() {
    echo "dwc3 平台目录: $DWC3"
    if [ -d "$DWC3" ]; then
        echo "存在的候选角色节点:"
        found=0
        for node in $(role_nodes); do
            role_describe "$node"
            found=1
        done
        [ "$found" -eq 1 ] || echo "  （一个都没有 → 角色只能在 dts 里锁）"
        echo "dwc3 目录全部条目:"
        ls "$DWC3" 2>/dev/null | tr '\n' ' '
        echo
    else
        echo "  $DWC3 不存在"
    fi
    echo "UDC 类目录: $UDC_CLASS"
    ls "$UDC_CLASS" 2>/dev/null | tr '\n' ' '
    echo
    echo "udc.state = $(udc_state)"
}

# 返回值：0 = 已锁 / 1 = 没有候选节点（本机实测如此，由 dts 负责，不算失败）
#         / 2 = 有节点但写不进去（这才是真失败）
role_apply() {
    applied=
    saw_candidate=none
    for node in $(role_nodes); do
        want=
        case "$node" in
            */mode) want=peripheral ;;
            */extcon) want=USB ;;
            *) saw_candidate=unknown; continue ;;
        esac
        saw_candidate=known
        [ -w "$node" ] || { log "role: $node 不可写"; continue; }
        current=$(cat "$node" 2>/dev/null || true)
        case "$current" in
            *"$want"*) log "role: 已经是设备模式（$node=[$current]）"; return 0 ;;
        esac
        printf '%s\n' "$want" > "$node" 2>/dev/null || { log "role: 写 $node 失败"; continue; }
        sleep 0.2
        current=$(cat "$node" 2>/dev/null || true)
        case "$current" in
            *"$want"*)
                applied=$node
                log "role: 锁定设备模式 node=$node value=[$current]"
                return 0
                ;;
            *) log "role: 写入 $node 未生效（回读 [$current]）" ;;
        esac
    done
    if [ "$saw_candidate" = none ]; then
        log "role=uncontrollable: $DWC3 下没有 mode/extcon 节点，这一项只能靠 dts dr_mode"
        return 1
    fi
    return 2
}
# ---------------------------------------------------------------- 属性闸门

pin_props() {
    changed=0

    current=$(prop_get sys.usb.configfs)
    if [ "$current" != carplay ]; then
        prop_set sys.usb.configfs carplay
        log "prop: sys.usb.configfs [$current] -> carplay"
        changed=1
    fi

    current=$(prop_get sys.usb.config)
    case "$current" in
        adb*|mtp*|ptp*|rndis*|accessory*|midi*)
            prop_set sys.usb.config carplay
            log "prop: sys.usb.config [$current] -> carplay（拦下安卓类 USB 功能）"
            changed=1
            ;;
    esac

    # 持久值含 adb 时，下次开机 framework 会在早期就要求 adb，那正是 ffs.adb 被链进
    # 配置 1 的触发条件。改成 none 后这条路径从源头消失。
    current=$(prop_get persist.sys.usb.config)
    case "$current" in
        ''|none) ;;
        *)
            case "$current" in
                *adb*|*mtp*|*ptp*)
                    prop_set persist.sys.usb.config none
                    log "prop: persist.sys.usb.config [$current] -> none（开机不再请求 adb/mtp）"
                    changed=1
                    ;;
            esac
            ;;
    esac

    [ "$changed" -eq 1 ] && return 0
    return 1
}

# ---------------------------------------------------------------- 配置里摘掉 ADB

# 只在「同一配置里已经挂着 ffs.iap2」时动手：那是 CarPlay 附着中的特征。
# 拆机/--restore-stock 之后配置里只剩 ffs.adb，此时不能删，否则 b.1 变空、
# 内核会以 -EINVAL 拒绝整个 gadget。
evict_foreign_links() {
    changed=0
    for cfg in "$G"/configs/*; do
        [ -d "$cfg" ] || continue
        ours=0
        for slot in "$cfg"/f*; do
            [ -e "$slot" ] || is_link "$slot" || continue
            case "$(link_target "$slot")" in
                *ffs.iap2) ours=1 ;;
            esac
        done
        [ "$ours" -eq 1 ] || continue
        for slot in "$cfg"/f*; do
            [ -e "$slot" ] || is_link "$slot" || continue
            target=$(link_target "$slot")
            case "$target" in
                *ffs.iap2) continue ;;
            esac
            # PTP in slot f0 is ours, not a foreign Android interface. The
            # supervisor links the kernel ptp function there so a head unit
            # finds interface 0 (06/01/01) before it starts iAP2; evicting it
            # silently dropped interface 0 about a second after every attach.
            # Stock init never uses f0 (it works from f1 up), so a ptp/mtp link
            # anywhere else is still removed below.
            case "$target" in
                *ptp.*)
                    case "$slot" in
                        */f0) continue ;;
                    esac
                    ;;
            esac
            case "$target" in
                *ffs.adb|*adb.*|*mtp.*|*ptp.*|*rndis.*|*accessory.*|*midi.*)
                    if rm -f "$slot" 2>/dev/null; then
                        changed=1
                        log "link: 摘掉 $slot -> $target（安卓类接口不许和 iPhone 布局共存）"
                    fi
                    ;;
            esac
        done
    done
    [ "$changed" -eq 1 ] && return 0
    return 1
}

# ---------------------------------------------------------------- 命令

status() {
    echo "udc.state     = $(udc_state)"
    echo "UDC bound     = [$(udc_bound)]"
    echo "vid/pid       = $(gadget_vid) / $(gadget_pid)"
    echo "sys.usb.configfs      = [$(prop_get sys.usb.configfs)]"
    echo "sys.usb.config        = [$(prop_get sys.usb.config)]"
    echo "persist.sys.usb.config= [$(prop_get persist.sys.usb.config)]"
    echo "adbd          = [$(prop_get init.svc.adbd)]  tcp.port=[$(prop_get persist.adb.tcp.port)$(prop_get service.adb.tcp.port)]"
    echo "角色节点:"
    found=0
    for node in $(role_nodes); do
        role_describe "$node"
        found=1
    done
    [ "$found" -eq 1 ] || echo "  （无 → 角色只能在 dts 里锁）"
    echo "配置链接:"
    for cfg in "$G"/configs/*; do
        [ -d "$cfg" ] || continue
        printf '  %s:\n' "$cfg"
        ls -l "$cfg" 2>/dev/null | grep '\->' | sed 's/^.*[0-9] //' | sed 's/^/    /'
    done
}

reassert() {
    rc=0
    pin_props || true
    evict_foreign_links || true
    return $rc
}

apply() {
    rc=0
    pinned=no
    if pin_props; then pinned=yes; fi
    if evict_foreign_links; then evicted=yes; else evicted=no; fi
    # 角色这一项在 RK3399 这颗核上恒为 uncontrollable（实测：dwc3 下无 mode/extcon
    # 节点，extcon 的 state 全部只读）。那是「做不到」，不是「做失败了」：
    # 记为 warning，只有「有节点却写不进去」才算失败，否则每次启动都写一条假告警，
    # 真出问题时反而被淹掉。
    # `role_apply ||` and not a bare call: a bare non-zero return would take the
    # caller down if it runs under `set -e`.
    role_rc=0
    role_apply || role_rc=$?
    case "$role_rc" in
        0) role_result=locked ;;
        1) role_result=uncontrollable ;;
        *) role_result=failed; rc=1 ;;
    esac
    log "apply: props_changed=$pinned links_evicted=$evicted role=$role_result udc=[$(udc_bound)] state=$(udc_state)"
    return $rc
}

watch() {
    echo "$$" > "$ROOT/usb-lock.pid" 2>/dev/null || true
    log "watch: 启动 pid=$$ interval=${WATCH_INTERVAL}s"
    # 角色锁只在启动时做一次：每次巡检都写角色节点会让控制器反复重新枚举。
    role_apply || true
    last_vid=$(gadget_vid)
    while :; do
        pin_props || true
        evict_foreign_links || true
        current_vid=$(gadget_vid)
        if [ "$current_vid" != "$last_vid" ]; then
            log "watch: idVendor 变化 [$last_vid] -> [$current_vid] state=$(udc_state)"
            last_vid=$current_vid
        fi
        sleep "$WATCH_INTERVAL"
    done
}

reset() {
    # 只回滚 persist 默认值：把 --restore-stock 记录过的原值写回去。
    wanted=
    if [ -f "$STATE_FILE" ]; then
        wanted=$(sed -n '21p' "$STATE_FILE" 2>/dev/null || true)
    fi
    case "$wanted" in
        ''|none|carplay) wanted=adb ;;
    esac
    prop_set persist.sys.usb.config "$wanted"
    log "reset: persist.sys.usb.config -> $wanted（来源 ${STATE_FILE}:21）"
    echo "persist.sys.usb.config = [$(prop_get persist.sys.usb.config)]"
}

case "${1:-status}" in
    status)   status ;;
    probe)    probe ;;
    apply)    apply ;;
    reassert) reassert ;;
    watch)    watch ;;
    reset)    reset ;;
    *)
        echo "用法: $0 {status|probe|apply|reassert|watch|reset}" >&2
        exit 2
        ;;
esac
