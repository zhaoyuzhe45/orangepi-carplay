#!/system/bin/sh
# Persistent supervisor for the CarPlay USB accessory daemon.
# It owns the gadget attachment, restarts the daemon after exit 75, and uses
# bounded exponential backoff for unexpected crashes.

set -u

ROOT=/data/carplay
BIN=$ROOT/bin/iap2d
CONFIG=$ROOT/config.json
PID_FILE=$ROOT/supervisor.pid
DAEMON_PID_FILE=$ROOT/daemon.pid
LOCK_DIR=$ROOT/supervisor.lock
LOCK_OWNED=0
STOCK_STATE_FILE=$ROOT/usb-stock.state
FFS=/dev/usb-ffs/iap2
G=/config/usb_gadget/g1
UDC=fe800000.dwc3
UDC_SYSFS=${UDC_SYSFS:-/sys/class/udc}
EXTCON_SYSFS=${EXTCON_SYSFS:-/sys/class/extcon}
UDC_OBSERVE_INTERVAL=${UDC_OBSERVE_INTERVAL:-1}
USB_STATE_LOG=$ROOT/logs/usb-state.log
# Runtime USB hardening: keep the port in device role and keep ADB off the bus.
# Split between this guard and the kernel dts on purpose -- see usb_lock.sh and
# docs/usb-lock-layers.md. Anything the guard cannot reach is a dts/init change.
USB_LOCK=$ROOT/bin/usb_lock.sh
USB_LOCK_PID_FILE=$ROOT/usb-lock.pid
STOCK_CONFIG=$G/configs/b.1
# 1 when configuration 2 exists and iap2d was told so (see prepare_ffs).
STANDBY_CONFIG=0
# Role arbitration.  The daemon sees 0x40/0x51 on ep0 and leaves a note here;
# this script owns dwc3 and performs the switch, because the daemon cannot
# unbind the gadget it is reading from.  Persisted as a file (not a signal) so
# a crash between the two halves still leaves a readable decision.
ROLE_REQUEST=$ROOT/role_request
ROLE_STATE=$ROOT/usb-role.state
ROLE_SWITCH_FAILURES=$ROOT/role-switch.failures
# Uptime (seconds) at which a head unit last completed SET_CONFIGURATION.  The
# UDC observer writes it; attach_grace_wait() reads it to stay out of the head
# unit's cold-boot decision window.  Kept in a file because the observer is a
# subshell: a variable would not survive the fork.
LAST_CONFIGURED_STAMP=$ROOT/last-configured.uptime
# Minimum gap between two presentations of the gadget to the same head unit.
# 0 disables the hold (bench use).
ATTACH_GRACE_MS=${CARPLAY_ATTACH_GRACE_MS:-45000}
# Touch this file and the next daemon start runs the in-process role-switch
# self-test once (the flag is consumed at launch).  On a bench this exercises
# device -> host -> give-up -> device without a head unit sending 0x51.
SELFTEST_FLAG=$ROOT/selftest_role_switch
# Three strikes, then stay a device until the next boot.  Without a budget a
# peer that keeps sending 0x51 but never becomes a device (DiPlay does exactly
# that when probing) would flip the port every reconnect and never settle.
ROLE_SWITCH_MAX_ATTEMPTS=${ROLE_SWITCH_MAX_ATTEMPTS:-3}
ROLE_SWITCH=${ROLE_SWITCH:-$ROOT/bin/usb_role_switch.sh}
# Two configurations, deliberately:
#
#   b.1  bConfigurationValue 1  PTP only (06/01/01, 3 endpoints)  -- standby
#   b.2  bConfigurationValue 2  iAP2 + Apple ethernet + NCM      -- CarPlay
#
# DDBox010 -- the one Android-based box known to drive this A4L head unit --
# advertises four configurations and its configuration 1 is PTP-only with no
# driver bound. The host selects 1, decides "this is an Apple device", and only
# then asks for the CarPlay configuration. We used to put PTP+iAP2+eth+NCM into
# the single configuration 1, so the host had nowhere to switch to: it accepted
# configuration 1 and went silent.
#
# Endpoint budget is the reason b.2 does NOT carry its own PTP: dwc3 exposes
# 8 IN / 8 OUT, and b.1's kernel ptp already holds 2 IN + 1 OUT for the whole
# gadget lifetime (configfs binds every configuration up front, not on switch).
# The current split costs exactly what the single-configuration layout cost --
# 6 IN / 4 OUT -- so this change does not touch the endpoint budget at all.
CARPLAY_CONFIG=$G/configs/b.2
OS_DESC=$G/os_desc
STOCK_OS_DESC_CONFIG_LINK=$OS_DESC/b.1
OS_DESC_CONFIG_LINK=$OS_DESC/b.2
LINK=$CARPLAY_CONFIG/f0
# Kernel-side CDC-NCM, when the kernel was built with CONFIG_USB_CONFIGFS_NCM.
# FunctionFS rejects CS_INTERFACE, so interfaces 2/3 could never carry the four
# functional descriptors a real iPhone advertises; the kernel function emits
# them itself. It goes into the *next* slot after ffs.iap2 because interface
# numbering follows bind order: FunctionFS takes 0/1, this takes 2/3.
NCM_FUNC=$G/functions/ncm.usb0
NCM_LINK=$CARPLAY_CONFIG/f1
# Kernel-side PTP (CONFIG_USB_CONFIGFS_F_PTP=y on this image). Interface 0 of
# every configuration a real iPhone advertises is 06/01/01 with three
# endpoints, and that is the first thing an OEM head unit looks at: without it
# the A4L MIB2 enumerated us, accepted the only configuration on offer and then
# never started a single iAP2 transfer. The kernel function supplies Apple's
# exact descriptor (06/01/01, bulk in/out + interrupt in) with no userspace
# daemon, so it costs one mkdir and one link.
#
# Slot f0 of b.1 (the standby configuration), not f6: composite numbers
# interfaces in bind order on this 4.4 kernel, and newer kernels order by name.
# f0 is first either way, which is what puts PTP at interface 0.
PTP_FUNC=$G/functions/ptp.gs0
PTP_LINK=$STOCK_CONFIG/f0
# Rockchip's ptp is alloc_inst_mtp_ptp(false) (f_ptp.c), i.e. the same driver as
# MTP with the ptp flag set. configfs refuses the link with
#   "Create MTP function before linking PTP function with a gadget configuration"
# unless functions/mtp.gs0 already exists, because that instance is what creates
# /dev/mtp_usb. It stays unlinked on purpose: linking it would add a second
# 06/01/01 interface plus three more endpoints for no benefit.
PTP_MTP_FUNC=$G/functions/mtp.gs0
# Both ends of the NCM link report an Apple-assigned MAC. A real iPhone carries
# one (the head unit reads it from the CDC Ethernet iMacAddress string); the
# kernel default is a random locally-administered address such as 9e:56:..,
# which no Apple device ever shows. 00:a0:57 is Apple's OUI.
NCM_DEV_ADDR=00:a0:57:00:00:01
NCM_HOST_ADDR=00:a0:57:00:00:02
# The device tree describes the Type-C port as dr_mode = "otg" so that dwc3_mode
# accepts a switch to host (the car asks for exactly that with vendor request
# 0x51). "otg" is also an undecided start: the extcon property that used to pick
# the role is gone, so nothing chooses peripheral at boot. Every consumer of this
# port that is not CarPlay expects a device, so the supervisor pins it here.
DWC3_MODE=${DWC3_MODE:-/sys/devices/platform/usb0/dwc3_mode}
FFS_ADB_DEFAULT=../../../../usb_gadget/g1/functions/ffs.adb
STRINGS=$G/strings/0x409
APPLE_USB_MANUFACTURER="Apple Inc."
APPLE_USB_PRODUCT="iPhone"
# bMaxPower is in 2 mA units. Stock Android asks for 500 (= 1000 mA), which is
# how a phone that fast-charges identifies itself; a real iPhone asks for 250
# (= 500 mA) and negotiates the rest over the charging protocol. A head unit
# that budgets its downstream port from bMaxPower -- they typically source
# 500 mA on a data port -- can refuse or current-limit a device that asks for
# twice that, and the failure happens at enumeration, before any vendor
# request. Pin the accessory path to Apple's value.
APPLE_USB_MAX_POWER=250
# A real iPhone reports a 40-hex-digit UDID here, and so does the DDBox010
# sample that drives CarPlay on the very head unit which ignores this box.
# This path shipped a 12-character alphanumeric serial for a while: Windows
# keys the USB device instance on VID/PID/serial, and a constant non-Apple
# value made every re-enumeration reuse cached hardware IDs (including the
# Rockchip MS OS compatible ID "RESTORE_MODE" that used to be advertised).
# That was a Windows-side workaround. iSerialNumber is the one
# device-descriptor field a head unit can compare against a genuine phone, so
# the accessory path goes back to the UDID shape. The value is deliberately
# constant -- rotating it per boot would leak a mask ROM serial to the host
# and break Windows caching on every replug.
APPLE_USB_SERIAL="a3f19c7d4e2b80f6c5d31a9e7b04f82c6d15e3a9"
# bmAttributes bit 6 is the self-powered flag. Stock Android advertises 0x80
# (bus-powered, no remote wakeup); a real iPhone and the DDBox010 both
# advertise 0xc0 (self-powered, no remote wakeup). The box has its own supply,
# and a port that budgeted the draw of a bus-powered device against 500 mA
# would power it down -- so the accessory path claims what the working samples
# claim.
APPLE_BM_ATTRIBUTES=0xc0
FORCE_ATTACH=0
RESTORE_ONLY=0
STOPPING=0
CHILD=
UDC_OBSERVER=
USB_LOCK_WATCHER=
ATTACHED=0
MODIFIED_GADGET=0
ORIGINAL_USB_CONFIG=
ORIGINAL_USB_CONFIGFS=
ORIGINAL_UDC=
PRE_DETACH_UDC=
ORIGINAL_VID=
ORIGINAL_PID=
ORIGINAL_BCDUSB=
ORIGINAL_DEVICE_CLASS=
ORIGINAL_DEVICE_SUBCLASS=
ORIGINAL_DEVICE_PROTOCOL=
ORIGINAL_STRING_MANUFACTURER=
ORIGINAL_STRING_PRODUCT=
ORIGINAL_STRING_SERIAL=
STOCK_F2=
STOCK_F3=
STOCK_F4=
STOCK_F5=
STOCK_F6=
STOCK_MAX_POWER=
STOCK_BM_ATTRIBUTES=
ORIGINAL_OS_DESC_USE=
ORIGINAL_OS_DESC_VENDOR_CODE=
ORIGINAL_OS_DESC_SIGNATURE=
ORIGINAL_OS_DESC_CONFIG=

case "${1:-}" in
    --force-attach) FORCE_ATTACH=1 ;;
    --restore-stock) RESTORE_ONLY=1 ;;
    "") ;;
    *) echo "usage: $0 [--force-attach|--restore-stock]" >&2; exit 64 ;;
esac

# Android Auto mode gate.
#
# init starts this service at sys.boot_completed=1, so any reboot -- and a head
# unit power-cycling the USB port is enough -- would rebuild the gadget from
# scratch (configfs is RAM-backed) and hand USB ownership straight back to
# CarPlay, silently undoing the AA identity. When the marker file exists this
# supervisor must not touch the gadget at all: attach the AA identity instead
# and park here forever, so init still sees a live service and never restarts
# it. Removing the marker plus `ctl.stop`/`ctl.start` returns to CarPlay.
#
# --restore-stock deliberately bypasses the gate: that is the escape hatch and
# must keep working even while AA mode is armed.
if [ "$RESTORE_ONLY" -eq 0 ] && [ -f "$ROOT/mode.aa" ]; then
    printf '%s AA mode marker present: refusing CarPlay takeover\n' \
        "$(date -u +%Y-%m-%dT%H:%M:%SZ 2>/dev/null || echo -)" \
        >> "$ROOT/logs/supervisor.log" 2>/dev/null || true
    /system/bin/sh "$ROOT/bin/aa_boot_attach.sh" \
        >> "$ROOT/logs/supervisor.log" 2>&1 || true
    while :; do sleep 3600; done
fi

log_event() {
    # `log -t` is a no-op on this image: the binary exists but logd never
    # shows the tag, so every supervisor decision was invisible. Keep a plain
    # file next to the daemon log; it is the only trace of who touched the
    # gadget and in what order.
    printf '%s %s\n' "$(date -u +%Y-%m-%dT%H:%M:%SZ 2>/dev/null || echo -)" "$*" \
        >> "${LOG_DIR:-/data/carplay/logs}/supervisor.log" 2>/dev/null || true
    log -t CarPlayBoxSupervisor "$*" 2>/dev/null || echo "Supervisor: $*" >&2
}

# Runtime USB hardening lives in its own script so it can be run by hand while
# the supervisor is down (`usb_lock.sh status|probe|apply`). A missing script is
# not fatal: the supervisor still owns the gadget, just without the extra guard.
usb_lock_available() {
    [ -x "$USB_LOCK" ]
}

usb_lock_apply() {
    usb_lock_available || return 0
    "$USB_LOCK" apply >> "${LOG_DIR:-/data/carplay/logs}/supervisor.log" 2>&1 || \
        log_event "usb lock reported a failure; see logs/usb-lock.log"
}

# Only used when the stock layout (and stock defaults) are being handed back:
# `--restore-stock` and a clean stop. Never on the management-only path, which
# still wants ADB held off the bus.
usb_lock_reset_defaults() {
    usb_lock_available || return 0
    "$USB_LOCK" reset >> "${LOG_DIR:-/data/carplay/logs}/supervisor.log" 2>&1 || true
}

start_usb_lock_watcher() {
    usb_lock_available || return 0
    # A previous supervisor killed mid-flight can leave a watcher behind. Reap it
    # first, otherwise two guards race on the same nodes and the pid file only
    # tracks the newer one.
    stop_usb_lock_watcher
    "$USB_LOCK" watch >> "${LOG_DIR:-/data/carplay/logs}/supervisor.log" 2>&1 &
    USB_LOCK_WATCHER=$!
    printf '%s\n' "$USB_LOCK_WATCHER" > "$USB_LOCK_PID_FILE" 2>/dev/null || true
    log_event "usb lock watcher started pid=$USB_LOCK_WATCHER"
}

# /proc/<pid>/cmdline can block forever on this kernel when the target is a
# zombie still mid-exit. Observed 2026-10-27: a stale pid left in the singleton
# lock wedged the supervisor inside acquire_singleton, so claim_usb_gadget never
# ran, configfs stayed empty, and the head unit enumerated nothing at all. The
# read must therefore never be unbounded: skip corpses outright (their cmdline
# is empty anyway) and cap the read so a wedged entry costs two seconds instead
# of the whole boot. `cat` opens the file inside the timed child on purpose -
# a shell redirection would open it in this process, where timeout cannot help.
read_cmdline() {
    read_cmdline_pid=${1:-}
    [ -n "$read_cmdline_pid" ] || return 0
    # `awk` is absent from this image's shell, so the state field comes out of
    # sed: the greedy prefix ends at the last ')' and the next char is the
    # state. A missing file (dead pid) leaves it empty, which is also "not live".
    read_cmdline_state=$(sed -n 's/^.*) \([A-Za-z]\).*/\1/p' \
        "/proc/$read_cmdline_pid/stat" 2>/dev/null)
    case "$read_cmdline_state" in
        ''|Z|X) return 0 ;;
    esac
    if command -v timeout >/dev/null 2>&1; then
        timeout 2 cat "/proc/$read_cmdline_pid/cmdline" 2>/dev/null | \
            tr '\0' ' ' 2>/dev/null || true
    else
        cat "/proc/$read_cmdline_pid/cmdline" 2>/dev/null | \
            tr '\0' ' ' 2>/dev/null || true
    fi
}

# True only when the pid really is our background watcher. The pid file outlives
# the process, so a recycled pid must not be handed to kill(1).
pid_is_usb_lock_watcher() {
    case "$(read_cmdline "$1")" in
        *usb_lock.sh*watch*) return 0 ;;
    esac
    return 1
}

# `--restore-stock` runs in its own process, where USB_LOCK_WATCHER is empty.
# Falling back to the pid file matters: an orphaned watcher would re-pin
# persist.sys.usb.config within two seconds and silently undo the restore.
stop_usb_lock_watcher() {
    victim=${USB_LOCK_WATCHER:-}
    if [ -z "$victim" ] && [ -f "$USB_LOCK_PID_FILE" ]; then
        victim=$(cat "$USB_LOCK_PID_FILE" 2>/dev/null || true)
    fi
    if [ -n "$victim" ] && pid_is_usb_lock_watcher "$victim"; then
        kill "$victim" 2>/dev/null || true
        log_event "usb lock watcher stopped pid=$victim"
    fi
    USB_LOCK_WATCHER=
    rm -f "$USB_LOCK_PID_FILE" 2>/dev/null || true
}

# Detach the gadget controller from configfs.
#
# configfs only unbinds on an empty write. Writing "none" is rejected with
# -ENODEV on this kernel and the UDC quietly stays bound, which is worse than
# a plain failure: the following gadget attribute writes then hit -EBUSY and
# are skipped, except bcdUSB, which lacks the "driver already bound" check and
# still goes through. That asymmetry is how the box ended up advertising
# 05ac:12a8 with the stock bcdUSB 0x0210 -- a value above 0x0200 makes the
# host ask for a BOS descriptor that an accessory-mode iPhone never offers.
detach_udc() {
    # A single newline, not a zero-byte write. configfs's store() returns
    # early on count==0 without ever looking at the payload, so `printf '%s' ''`
    # is a no-op; one newline trims down to the empty string that triggers the
    # unbind. Both a wrong token and a zero-byte write fail the same way and
    # silently leave the UDC bound.
    echo "" > "$G/UDC" 2>/dev/null || true
}

# Is <pid> a supervisor that is still running?
#
# A bare `kill -0` is not enough. PIDs are recycled across reboots, so the
# number recorded before a restart can belong to any unrelated Android process
# on the next boot. Both the singleton lock and the pid file used to trust the
# number alone: the lock path then hit its `exit 0`, init restarted the service
# every 5 s, and every copy exited in 60 ms without writing a single log line --
# the box looked alive while CarPlay was simply never started. Requiring the
# command line to match is what distinguishes a real duplicate from a recycled
# number, and both call sites share this one predicate so they cannot drift.
supervisor_pid_is_live() {
    p=${1:-}
    # Always reset: callers log this, and a leftover value from an earlier probe
    # would blame the wrong process (the first version did exactly that).
    SUPERVISOR_PID_CMDLINE=
    [ -n "$p" ] || return 1
    [ "$p" != "$$" ] || return 1
    kill -0 "$p" 2>/dev/null || return 1
    cmdline=$(read_cmdline "$p")
    SUPERVISOR_PID_CMDLINE=$cmdline
    case "$cmdline" in
        *carplay_supervisor.sh*) return 0 ;;
    esac
    return 1
}

# Win the enumeration race before anything else runs.
#
# /init.usb.configfs.rc:21 is queued the instant sys.usb.config flips to adb
# while sys.usb.configfs is still 1, and adbd reaches sys.usb.ffs.ready=1 about
# 16 s into boot. Anything slow placed before this block -- restorecon over a
# multi-megabyte log directory, the singleton lock, the stock capture -- loses
# that race, and the branch then links ffs.adb into configuration 1 next to
# ffs.iap2. A head unit that enumerates that mixed layout sees 0xFF/0x42/0x01
# (ADB) among the interfaces and classifies the box as an Android phone.
# Park the gate and detach the UDC here; the capture below still records the
# stock values because it only reads them.
if [ "$RESTORE_ONLY" -eq 0 ]; then
    # Take the singleton slot *before* touching the UDC. init has three
    # on-property triggers for this service, and a second instance used to
    # reach this block, detach the UDC the first instance had just bound, and
    # only then discover it had no business running -- which yanks the device
    # out of the middle of the host's enumeration. mkdir is atomic, so this
    # costs microseconds and still leaves the expensive work (restorecon, the
    # stock capture) after the claim.
    mkdir -p "$ROOT" 2>/dev/null || true
    if ! mkdir "$LOCK_DIR" 2>/dev/null; then
        held_pid=$(cat "$LOCK_DIR/pid" 2>/dev/null || true)
        if supervisor_pid_is_live "$held_pid"; then
            exit 0
        fi
        # Holder is gone, or the number now belongs to something else. Say so:
        # a silent takeover here would hide the reboot-time pid recycling that
        # the first version of this code tripped over.
        log_event "clearing stale lock pid=${held_pid:-unknown} cmdline=${SUPERVISOR_PID_CMDLINE:-none}"
        rm -rf "$LOCK_DIR" 2>/dev/null || true
        mkdir "$LOCK_DIR" 2>/dev/null || exit 0
    fi
    printf '%s\n' "$$" > "$LOCK_DIR/pid" 2>/dev/null || true
    chmod 600 "$LOCK_DIR/pid" 2>/dev/null || true
    LOCK_OWNED=1

    if [ "$(getprop sys.usb.configfs 2>/dev/null || true)" != carplay ]; then
        setprop sys.usb.configfs carplay 2>/dev/null || true
    fi
    # capture_stock_gadget() runs after this block, so remember the bound UDC
    # name first: it is the one stock value the detach destroys, and a snapshot
    # written without it can never re-bind the stock gadget on stop.
    PRE_DETACH_UDC=$(cat "$G/UDC" 2>/dev/null || true)
    detach_udc
    # Lock the port to device role here, while the UDC is deliberately unbound.
    # Writing the role node can reset the controller; doing it after attachment
    # would drop the session the host was in the middle of enumerating.
    usb_lock_apply
fi

wait_for_usb_settle() {
    stable=0
    last=
    tries=0
    # Android's USB framework can briefly transition adb -> none -> adb
    # during boot. Once claim_usb_gadget() has parked the property at
    # "carplay" the stock branch can no longer run, so this is only a short
    # confirmation that the property really holds before the descriptors are
    # written.
    while [ "$stable" -lt 5 ] && [ "$tries" -lt 50 ]; do
        sleep 0.2
        current=$(getprop sys.usb.config 2>/dev/null || true)
        if [ -n "$current" ] && [ "$current" = "$last" ]; then
            stable=$((stable + 1))
        else
            stable=0
            last=$current
        fi
        tries=$((tries + 1))
    done
    log_event "USB configuration settled config=${last:-unknown} stable_ticks=$stable"
}

mkdir -p "$ROOT" "$ROOT/bin" "$ROOT/logs" "$FFS"
chmod 700 "$ROOT"
chmod 755 "$ROOT/bin"
# Only the executables need a label; -RF over $ROOT also walks logs/, which
# holds megabytes of daemon output and used to delay the gadget claim by ~12 s
# -- long enough for init to bind ffs.adb into configuration 1 first.
restorecon "$ROOT" "$ROOT/bin" 2>/dev/null || true
restorecon "$ROOT/bin/"* 2>/dev/null || true

cleanup_lock() {
    if [ "$LOCK_OWNED" -eq 1 ]; then
        rm -f "$LOCK_DIR/pid" 2>/dev/null || true
        rmdir "$LOCK_DIR" 2>/dev/null || true
        LOCK_OWNED=0
    fi
}

acquire_singleton() {
    # The early block at the top of the file already holds the slot when it
    # won the race; adopt it instead of treating our own lock as a rival.
    [ "$LOCK_OWNED" -eq 1 ] && return 0
    # mkdir is atomic on this filesystem. The PID file alone is insufficient:
    # init and a manual deployment can both observe it before either writes.
    tries=0
    while [ "$tries" -lt 20 ]; do
        if mkdir "$LOCK_DIR" 2>/dev/null; then
            printf "%s\n" "$$" > "$LOCK_DIR/pid"
            chmod 600 "$LOCK_DIR/pid" 2>/dev/null || true
            LOCK_OWNED=1
            trap cleanup_lock 0
            return 0
        fi
        lock_pid=$(cat "$LOCK_DIR/pid" 2>/dev/null || true)
        if [ -n "$lock_pid" ] && kill -0 "$lock_pid" 2>/dev/null; then
            lock_cmdline=$(read_cmdline "$lock_pid")
            case "$lock_cmdline" in
                *carplay_supervisor.sh*)
                    log_event "already running pid=$lock_pid (singleton lock)"
                    return 1
                    ;;
            esac
        fi
        sleep 0.1
        tries=$((tries + 1))
    done
    # A process can die between creating the lock and recording its PID.
    rm -rf "$LOCK_DIR" 2>/dev/null || true
    if mkdir "$LOCK_DIR" 2>/dev/null; then
        printf "%s\n" "$$" > "$LOCK_DIR/pid"
        chmod 600 "$LOCK_DIR/pid" 2>/dev/null || true
        LOCK_OWNED=1
        trap cleanup_lock 0
        return 0
    fi
    log_event "another supervisor holds $LOCK_DIR"
    return 1
}

capture_stock_gadget() {
    if [ -s "$STOCK_STATE_FILE" ]; then
        {
            IFS= read -r ORIGINAL_USB_CONFIG
            IFS= read -r ORIGINAL_UDC
            IFS= read -r ORIGINAL_VID
            IFS= read -r ORIGINAL_PID
            IFS= read -r ORIGINAL_BCDUSB
            IFS= read -r ORIGINAL_DEVICE_CLASS
            IFS= read -r ORIGINAL_DEVICE_SUBCLASS
            IFS= read -r ORIGINAL_DEVICE_PROTOCOL
            IFS= read -r STOCK_F2
            IFS= read -r STOCK_F3
            IFS= read -r STOCK_F4
            IFS= read -r STOCK_F5
            IFS= read -r STOCK_F6
            IFS= read -r ORIGINAL_STRING_MANUFACTURER || true
            IFS= read -r ORIGINAL_STRING_PRODUCT || true
            IFS= read -r ORIGINAL_STRING_SERIAL || true
            IFS= read -r ORIGINAL_OS_DESC_USE || true
            IFS= read -r ORIGINAL_OS_DESC_VENDOR_CODE || true
            IFS= read -r ORIGINAL_OS_DESC_SIGNATURE || true
            IFS= read -r ORIGINAL_OS_DESC_CONFIG || true
            IFS= read -r ORIGINAL_USB_CONFIGFS || true
            IFS= read -r STOCK_F1 || true
            IFS= read -r STOCK_MAX_POWER || true
            IFS= read -r STOCK_BM_ATTRIBUTES || true
        } < "$STOCK_STATE_FILE"
        # Older snapshots predate USB string capture. They were created before
        # this supervisor changed those files, so the current values are stock.
        [ -n "$ORIGINAL_STRING_MANUFACTURER" ] || \
            ORIGINAL_STRING_MANUFACTURER=$(cat "$STRINGS/manufacturer" 2>/dev/null || true)
        [ -n "$ORIGINAL_STRING_PRODUCT" ] || \
            ORIGINAL_STRING_PRODUCT=$(cat "$STRINGS/product" 2>/dev/null || true)
        [ -n "$ORIGINAL_STRING_SERIAL" ] || \
            ORIGINAL_STRING_SERIAL=$(cat "$STRINGS/serialnumber" 2>/dev/null || true)
        # Older snapshots did not include OS descriptor fields. Capture the
        # current stock values when those fields are absent.
        [ -n "$ORIGINAL_OS_DESC_USE" ] || ORIGINAL_OS_DESC_USE=$(cat "$OS_DESC/use" 2>/dev/null || true)
        [ -n "$ORIGINAL_OS_DESC_VENDOR_CODE" ] || ORIGINAL_OS_DESC_VENDOR_CODE=$(cat "$OS_DESC/b_vendor_code" 2>/dev/null || true)
        [ -n "$ORIGINAL_OS_DESC_SIGNATURE" ] || ORIGINAL_OS_DESC_SIGNATURE=$(cat "$OS_DESC/qw_sign" 2>/dev/null || true)
        [ -n "$ORIGINAL_OS_DESC_CONFIG" ] || ORIGINAL_OS_DESC_CONFIG=$(readlink "$STOCK_OS_DESC_CONFIG_LINK" 2>/dev/null || true)
        # Append after all 20 legacy fields; never shift existing snapshots.
        if [ -z "$ORIGINAL_USB_CONFIGFS" ]; then
            ORIGINAL_USB_CONFIGFS=$(getprop sys.usb.configfs 2>/dev/null || true)
            case "$ORIGINAL_USB_CONFIGFS" in
                0|1) ;;
                *) ORIGINAL_USB_CONFIGFS=1 ;;
            esac
        fi
        # Field 22 (stock b.1/f1 -> ffs.adb) arrives with the configuration-1
        # swap. Snapshots written before it, or written while CarPlay still
        # owned b.1, fall back to the current link and then to the stock path.
        if [ -z "$STOCK_F1" ]; then
            STOCK_F1=$(readlink "$STOCK_CONFIG/f1" 2>/dev/null || true)
        fi
        [ -n "$STOCK_F1" ] || STOCK_F1=$FFS_ADB_DEFAULT
        # Field 23 (stock bMaxPower) arrives with the current-budget fix.
        # Snapshots written before it were taken while bMaxPower still held
        # its stock value, so the live file is the stock value unless a
        # previous run already pinned it.
        if [ -z "$STOCK_MAX_POWER" ]; then
            STOCK_MAX_POWER=$(cat "$STOCK_CONFIG/MaxPower" 2>/dev/null || true)
        fi
        case "$STOCK_MAX_POWER" in
            ''|*[!0-9]*) STOCK_MAX_POWER=500 ;;
            # A snapshot taken mid-attach would record our own 250 as stock.
            "$APPLE_USB_MAX_POWER") STOCK_MAX_POWER=500 ;;
        esac
        # Field 24 (stock bmAttributes) arrives with the self-powered fix.
        # Snapshots written before it were taken while the byte still held its
        # stock value, so the live file is stock unless a previous run pinned
        # it -- in which case it is ours and must not be adopted as stock.
        if [ -z "$STOCK_BM_ATTRIBUTES" ]; then
            STOCK_BM_ATTRIBUTES=$(cat "$STOCK_CONFIG/bmAttributes" 2>/dev/null || true)
        fi
        case "$STOCK_BM_ATTRIBUTES" in
            '') STOCK_BM_ATTRIBUTES=0x80 ;;
            "$APPLE_BM_ATTRIBUTES") STOCK_BM_ATTRIBUTES=0x80 ;;
        esac
        write_stock_state
        return
    fi
    ORIGINAL_USB_CONFIG=$(getprop sys.usb.config 2>/dev/null || true)
    ORIGINAL_USB_CONFIGFS=$(getprop sys.usb.configfs 2>/dev/null || true)
    case "$ORIGINAL_USB_CONFIGFS" in
        0|1) ;;
        *) ORIGINAL_USB_CONFIGFS=1 ;;
    esac
    ORIGINAL_UDC=${PRE_DETACH_UDC:-$(cat "$G/UDC" 2>/dev/null || true)}
    ORIGINAL_VID=$(cat "$G/idVendor" 2>/dev/null || true)
    ORIGINAL_PID=$(cat "$G/idProduct" 2>/dev/null || true)
    ORIGINAL_BCDUSB=$(cat "$G/bcdUSB" 2>/dev/null || true)
    ORIGINAL_DEVICE_CLASS=$(cat "$G/bDeviceClass" 2>/dev/null || true)
    ORIGINAL_DEVICE_SUBCLASS=$(cat "$G/bDeviceSubClass" 2>/dev/null || true)
    ORIGINAL_DEVICE_PROTOCOL=$(cat "$G/bDeviceProtocol" 2>/dev/null || true)
    STOCK_F1=$(readlink "$STOCK_CONFIG/f1" 2>/dev/null || true)
    STOCK_F2=$(readlink "$STOCK_CONFIG/f2" 2>/dev/null || true)
    STOCK_F3=$(readlink "$STOCK_CONFIG/f3" 2>/dev/null || true)
    STOCK_F4=$(readlink "$STOCK_CONFIG/f4" 2>/dev/null || true)
    STOCK_F5=$(readlink "$STOCK_CONFIG/f5" 2>/dev/null || true)
    STOCK_F6=$(readlink "$STOCK_CONFIG/f6" 2>/dev/null || true)
    ORIGINAL_STRING_MANUFACTURER=$(cat "$STRINGS/manufacturer" 2>/dev/null || true)
    ORIGINAL_STRING_PRODUCT=$(cat "$STRINGS/product" 2>/dev/null || true)
    ORIGINAL_STRING_SERIAL=$(cat "$STRINGS/serialnumber" 2>/dev/null || true)
    ORIGINAL_OS_DESC_USE=$(cat "$OS_DESC/use" 2>/dev/null || true)
    ORIGINAL_OS_DESC_VENDOR_CODE=$(cat "$OS_DESC/b_vendor_code" 2>/dev/null || true)
    ORIGINAL_OS_DESC_SIGNATURE=$(cat "$OS_DESC/qw_sign" 2>/dev/null || true)
    ORIGINAL_OS_DESC_CONFIG=$(readlink "$STOCK_OS_DESC_CONFIG_LINK" 2>/dev/null || true)
    STOCK_MAX_POWER=$(cat "$STOCK_CONFIG/MaxPower" 2>/dev/null || true)
    case "$STOCK_MAX_POWER" in
        ''|*[!0-9]*) STOCK_MAX_POWER=500 ;;
        "$APPLE_USB_MAX_POWER") STOCK_MAX_POWER=500 ;;
    esac
    STOCK_BM_ATTRIBUTES=$(cat "$STOCK_CONFIG/bmAttributes" 2>/dev/null || true)
    case "$STOCK_BM_ATTRIBUTES" in
        '') STOCK_BM_ATTRIBUTES=0x80 ;;
        "$APPLE_BM_ATTRIBUTES") STOCK_BM_ATTRIBUTES=0x80 ;;
    esac
    if [ "$ORIGINAL_VID" = "0x05ac" ] && [ "$ORIGINAL_PID" = "0x12a8" ]; then
        ORIGINAL_UDC=
        ORIGINAL_VID=0x2207
        ORIGINAL_PID=0x0006
        ORIGINAL_BCDUSB=0x0210
        ORIGINAL_DEVICE_CLASS=0x00
        ORIGINAL_DEVICE_SUBCLASS=0x00
        ORIGINAL_DEVICE_PROTOCOL=0x00
        # The gadget was left impersonating an iPhone, so b.1/f1 may already be
        # detached. Restore the stock adb function from the known stock path.
        STOCK_F1=$FFS_ADB_DEFAULT
        STOCK_F2=
        STOCK_F3=
        STOCK_F4=
        STOCK_F5=
        STOCK_F6=
    fi
    [ -n "$STOCK_F1" ] || STOCK_F1=$FFS_ADB_DEFAULT
    write_stock_state
}

write_stock_state() {
    # The init gate in system/etc/init/carplay_box.rc flips sys.usb.config to
    # "carplay" as soon as the stock rules ask for adb, and this service starts
    # before /init.rockchip.rc has set it at all, so an early capture can
    # observe "carplay" or "none" instead of the stock intent. The authoritative
    # source is persist.sys.usb.config, which /init.rk30board.rc:23 copies into
    # sys.usb.config. --restore-stock must not leave adb down.
    case "$ORIGINAL_USB_CONFIG" in
        ''|none)
            persisted=$(getprop persist.sys.usb.config 2>/dev/null || true)
            case "$persisted" in
                ''|none) ORIGINAL_USB_CONFIG=adb ;;
                *) ORIGINAL_USB_CONFIG=$persisted ;;
            esac
            ;;
        adb|mtp*|ptp*|midi*|rndis*|accessory*) ;;
        *) ORIGINAL_USB_CONFIG=adb ;;
    esac
    {
        printf '%s\n' "$ORIGINAL_USB_CONFIG"
        printf '%s\n' "$ORIGINAL_UDC"
        printf '%s\n' "$ORIGINAL_VID"
        printf '%s\n' "$ORIGINAL_PID"
        printf '%s\n' "$ORIGINAL_BCDUSB"
        printf '%s\n' "$ORIGINAL_DEVICE_CLASS"
        printf '%s\n' "$ORIGINAL_DEVICE_SUBCLASS"
        printf '%s\n' "$ORIGINAL_DEVICE_PROTOCOL"
        printf '%s\n' "$STOCK_F2"
        printf '%s\n' "$STOCK_F3"
        printf '%s\n' "$STOCK_F4"
        printf '%s\n' "$STOCK_F5"
        printf '%s\n' "$STOCK_F6"
        printf '%s\n' "$ORIGINAL_STRING_MANUFACTURER"
        printf '%s\n' "$ORIGINAL_STRING_PRODUCT"
        printf '%s\n' "$ORIGINAL_STRING_SERIAL"
        printf '%s\n' "$ORIGINAL_OS_DESC_USE"
        printf '%s\n' "$ORIGINAL_OS_DESC_VENDOR_CODE"
        printf '%s\n' "$ORIGINAL_OS_DESC_SIGNATURE"
        printf '%s\n' "$ORIGINAL_OS_DESC_CONFIG"
        printf '%s\n' "$ORIGINAL_USB_CONFIGFS"
        printf '%s\n' "$STOCK_F1"
        printf '%s\n' "$STOCK_MAX_POWER"
        printf '%s\n' "$STOCK_BM_ATTRIBUTES"
    } > "$STOCK_STATE_FILE"
    chmod 600 "$STOCK_STATE_FILE"
}

stock_gadget_is_modified() {
    [ -d "$CARPLAY_CONFIG" ] || \
    [ "$(cat "$G/idVendor" 2>/dev/null || true)" != "$ORIGINAL_VID" ] || \
    [ "$(cat "$G/idProduct" 2>/dev/null || true)" != "$ORIGINAL_PID" ] || \
    [ "$(readlink "$STOCK_CONFIG/f1" 2>/dev/null || true)" != "$STOCK_F1" ] || \
    [ "$(readlink "$G/f4" 2>/dev/null || true)" != "$STOCK_F4" ] || \
    [ "$(cat "$STRINGS/manufacturer" 2>/dev/null || true)" != "$ORIGINAL_STRING_MANUFACTURER" ] || \
    [ "$(cat "$STRINGS/product" 2>/dev/null || true)" != "$ORIGINAL_STRING_PRODUCT" ] || \
    [ "$(cat "$STRINGS/serialnumber" 2>/dev/null || true)" != "$ORIGINAL_STRING_SERIAL" ] || \
    [ "$(cat "$OS_DESC/use" 2>/dev/null || true)" != "$ORIGINAL_OS_DESC_USE" ] || \
    [ "$(cat "$OS_DESC/b_vendor_code" 2>/dev/null || true)" != "$ORIGINAL_OS_DESC_VENDOR_CODE" ] || \
    [ "$(cat "$OS_DESC/qw_sign" 2>/dev/null || true)" != "$ORIGINAL_OS_DESC_SIGNATURE" ] || \
    [ "$(readlink "$STOCK_OS_DESC_CONFIG_LINK" 2>/dev/null || true)" != "$ORIGINAL_OS_DESC_CONFIG" ] || \
    [ "$(readlink "$OS_DESC_CONFIG_LINK" 2>/dev/null || true)" != "" ] || \
    [ "$(getprop sys.usb.configfs 2>/dev/null || true)" != "$ORIGINAL_USB_CONFIGFS" ] || \
    [ "$(cat "$STOCK_CONFIG/MaxPower" 2>/dev/null || true)" != "${STOCK_MAX_POWER:-500}" ] || \
    [ "$(cat "$STOCK_CONFIG/bmAttributes" 2>/dev/null || true)" != "${STOCK_BM_ATTRIBUTES:-0x80}" ]
}

claim_usb_gadget() {
    # Both stock init paths key on this property: 1 writes configfs and 0
    # enables legacy android_usb. A third, temporary value reserves g1 for
    # this supervisor without activating either path. Keep adbd and its TCP
    # transport running; do not rewrite sys.usb.config or persistent defaults.
    if [ "$(getprop sys.usb.configfs 2>/dev/null || true)" != carplay ]; then
        setprop sys.usb.configfs carplay || return 1
        [ "$(getprop sys.usb.configfs 2>/dev/null || true)" = carplay ] || return 1
        log_event "USB gadget ownership acquired; stock init configuration suspended"
    fi
    MODIFIED_GADGET=1
}

release_usb_gadget() {
    [ -n "$ORIGINAL_USB_CONFIGFS" ] || return 0
    setprop sys.usb.configfs "$ORIGINAL_USB_CONFIGFS" || return 1
    [ "$(getprop sys.usb.configfs 2>/dev/null || true)" = "$ORIGINAL_USB_CONFIGFS" ] || return 1
    log_event "USB gadget ownership released; configfs=$ORIGINAL_USB_CONFIGFS"
}

# /init.usb.configfs.rc:21 is gated on sys.usb.configfs=1:
#
#   on property:sys.usb.ffs.ready=1 && property:sys.usb.config=adb && \
#      property:sys.usb.configfs=1
#       symlink .../functions/ffs.adb .../configs/b.1/f1
#       write .../g1/UDC ${sys.usb.controller}
#
# adbd reports ffs.ready about 16 s into boot, long before boot_completed
# starts this service. That branch therefore publishes a 2207:0006 ADB
# gadget on the UDC first, the head unit enumerates it as an Android phone
# and offers Android Auto / CarLife, and it does not re-probe CarPlay when
# the Apple descriptor replaces it later. Park the property and detach the
# UDC in the same early window, before that branch can run.
early_claim_gadget() {
    claim_usb_gadget || return 1
    detach_udc
    ensure_adb_transport
    log_event "early USB claim: configfs=${ORIGINAL_USB_CONFIGFS}->carplay, UDC detached"
    return 0
}

# Suppressing the stock branch also suppresses its `start adbd` line, and
# adbd is what serves the TCP transport remote debugging depends on. USB
# adb stays unavailable while CarPlay owns the gadget; the network transport
# must not be collateral damage.
ensure_adb_transport() {
    case "$(getprop init.svc.adbd 2>/dev/null || true)" in
        running|restarting) return 0 ;;
    esac
    setprop ctl.start adbd 2>/dev/null || true
    log_event "adbd was down after claiming USB; requested start for the TCP transport"
    return 0
}

# The stock init branch can still be replayed by a property change we do not
# control (for example a framework-driven sys.usb.config flip). Whenever the
# adb function reappears inside configuration 1 the gadget once again
# advertises 0xFF/0x42/0x01, so drop it and force a re-bind.
evict_restocked_adb_function() {
    [ -e "$STOCK_CONFIG/f1" ] || [ -L "$STOCK_CONFIG/f1" ] || return 1
    rm -f "$STOCK_CONFIG/f1" 2>/dev/null || return 1
    detach_udc
    log_event "init re-created the stock adb function in configuration 1; evicted it"
    return 0
}

restore_link() {
    slot=$1
    target=$2
    [ -n "$target" ] || return 0
    [ -e "$G/$slot" ] || [ -L "$G/$slot" ] || \
        ln -s "$target" "$G/$slot" || true
}

restore_symlink() {
    link=$1
    target=$2
    rm -f "$link" 2>/dev/null || true
    [ -n "$target" ] || return 0
    ln -s "$target" "$link" 2>/dev/null || true
}

restore_file() {
    file=$1
    value=$2
    [ -n "$value" ] || return 0
    printf '%s\n' "$value" > "$file" 2>/dev/null || true
}

if ! acquire_singleton; then
    [ "$RESTORE_ONLY" -eq 0 ] && exit 0
    exit 1
fi
[ "$RESTORE_ONLY" -eq 1 ] || capture_stock_gadget
if [ "$RESTORE_ONLY" -eq 0 ]; then
    # Capture reads the untouched gadget, so it must run before the claim.
    # The claim must then happen immediately: every second spent settling is
    # a second in which adbd can hand the UDC to the stock adb gadget and the
    # head unit can classify the box as an Android phone.
    early_claim_gadget || log_event "early USB claim failed; stock init may still own the gadget"
    wait_for_usb_settle
fi

if [ -f "$PID_FILE" ]; then
    old_pid=$(cat "$PID_FILE" 2>/dev/null)
    if supervisor_pid_is_live "$old_pid"; then
        # A real second instance must fail loudly: init marks the service
        # crashed and the operator sees it, instead of a silent no-op.
        log_event "already running pid=$old_pid (pid file)"
        exit 1
    fi
    if [ -n "${old_pid:-}" ] && [ "$old_pid" != "$$" ]; then
        log_event "ignoring stale supervisor pid=$old_pid cmdline=${SUPERVISOR_PID_CMDLINE:-none}"
        rm -f "$PID_FILE" 2>/dev/null || true
    fi
fi
echo $$ > "$PID_FILE"
chmod 600 "$PID_FILE"

on_stop() {
    STOPPING=1
    if [ -n "$CHILD" ] && kill -0 "$CHILD" 2>/dev/null; then
        kill -TERM "$CHILD" 2>/dev/null || true
    fi
    stop_udc_observer
    stop_usb_lock_watcher
}
trap on_stop INT TERM HUP

json_setting() {
    key=$1
    fallback=$2
    line=$(grep -F "\"$key\"" "$CONFIG" 2>/dev/null | head -n 1)
    case "$line" in
        *true*) echo 1 ;;
        *false*) echo 0 ;;
        *) echo "$fallback" ;;
    esac
}

json_integer() {
    key=$1
    fallback=$2
    line=$(grep -F "\"$key\"" "$CONFIG" 2>/dev/null | head -n 1)
    value=$(printf '%s' "$line" | sed 's/[^0-9]//g')
    case "$value" in
        ''|*[!0-9]*) echo "$fallback" ;;
        *) echo "$value" ;;
    esac
}

# Which side of the USB link we are meant to be on.  "device" is the shape
# DiPlay drives us in and stays the default; "host" is what a head unit asks
# for with 0x51 and is entered only after a successful switch.
current_role() {
    if [ -r "$ROLE_STATE" ]; then
        _role=$(cat "$ROLE_STATE" 2>/dev/null)
        # Whitelist, not a fallback: a truncated or garbage file (crash while
        # writing, hand edit) must never be read as "host" -- that would leave
        # the port with no gadget bound and nothing driving the bus.
        case "$_role" in
            host) echo host; return 0 ;;
        esac
    fi
    echo device
}

set_role_state() {
    printf '%s\n' "$1" > "$ROLE_STATE" 2>/dev/null ||
        log_event "could not persist role $1"
}

host_role_active() {
    [ "$(current_role)" = host ]
}

# Save the kernel evidence before teardown changes the role. Keep two rounds;
# boot ID and uptime identify them even when the wall clock resets on power loss.
save_role_diagnostics() {
    _diag_dir=${LOG_DIR:-$ROOT/logs}
    _diag=$_diag_dir/usb-role-latest.log
    mkdir -p "$_diag_dir" || return 0
    [ ! -f "$_diag" ] || mv -f "$_diag" "$_diag_dir/usb-role-previous.log"
    {
        echo "boot_id=$(cat /proc/sys/kernel/random/boot_id) uptime=$(cat /proc/uptime)"
        echo "daemon_exit=$status role=$(current_role) request=$(cat "$ROLE_REQUEST" 2>/dev/null)"
        echo "mode=$(cat "$DWC3_MODE" 2>/dev/null) selected_udc=$UDC"
        for _hub in /sys/bus/usb/devices/usb[0-9]*; do
            [ -e "$_hub" ] && echo "$_hub -> $(readlink -f "$_hub")"
        done
        echo "kernel ring buffer (last 2000 lines):"
        dmesg 2>&1 | tail -n 2000
    } > "$_diag" 2>&1
    chmod 600 "$_diag" 2>/dev/null || true
    sync
    log_event "USB role diagnostics saved: $_diag"
}

# Budget check, kept separate so the test harness can exercise it without a
# dwc3 file.  A missing or garbled counter means "no failures yet".
role_switch_allowed() {
    _f=$(cat "$ROLE_SWITCH_FAILURES" 2>/dev/null || echo 0)
    case "$_f" in
        ''|*[!0-9]*) _f=0 ;;
    esac
    [ "$_f" -lt "$ROLE_SWITCH_MAX_ATTEMPTS" ]
}

record_role_switch_failure() {
    _f=$(cat "$ROLE_SWITCH_FAILURES" 2>/dev/null || echo 0)
    case "$_f" in
        ''|*[!0-9]*) _f=0 ;;
    esac
    _f=$((_f + 1))
    printf '%s\n' "$_f" > "$ROLE_SWITCH_FAILURES" 2>/dev/null || true
    log_event "role switch failures since boot: $_f/$ROLE_SWITCH_MAX_ATTEMPTS"
}

should_attach() {
    # In host mode we are not a gadget at all: attaching would fight our own
    # host controller for the bus.
    host_role_active && return 1
    [ "$FORCE_ATTACH" -eq 1 ] && return 0
    [ "$(json_setting enabled 1)" -eq 1 ] || return 1
    [ "$(json_setting bootStart 1)" -eq 1 ] || return 1
    return 0
}

udc_state() {
    cat "$UDC_SYSFS/$UDC/state" 2>/dev/null || true
}

# Stay off the bus for a while after the head unit last configured us.
#
# A head unit decides whether to offer CarPlay inside a window that starts when
# it powers up.  On a cold start the A4L's sibling units send 0x40/0x51 only
# 22-27 s after SET_CONFIGURATION, with no traffic in between (orangecar,
# docs/protocol.md 7.2).  Re-presenting the gadget inside that window wedges the
# head unit's CarPlay stack: it keeps enumerating us afterwards but never sends
# 0x51 again, and only a head-unit bus reset clears it -- measured through ten
# re-presentations and two board reboots (7.3).  Our own symptom is the same:
# after one failed attempt the car stops listing the box until it is power
# cycled.
#
# So the retry ladder has to be slower than the thing it is retrying.  A failed
# attempt already costs a head-unit power cycle; 45 s is cheap.  Nothing is held
# back when the head unit never got as far as configuring us -- there is no
# window to protect then, and a cable that was just plugged in should enumerate
# immediately.
attach_grace_wait() {
    [ "$ATTACH_GRACE_MS" -gt 0 ] || return 0
    [ -r "$LAST_CONFIGURED_STAMP" ] || return 0
    _last=$(cat "$LAST_CONFIGURED_STAMP" 2>/dev/null || echo '')
    case "$_last" in ''|*[!0-9]*) return 0 ;; esac
    _now=$(cut -d. -f1 /proc/uptime 2>/dev/null || echo '')
    case "$_now" in ''|*[!0-9]*) return 0 ;; esac
    _grace=$((ATTACH_GRACE_MS / 1000))
    _elapsed=$((_now - _last))
    [ "$_elapsed" -ge 0 ] || return 0
    [ "$_elapsed" -ge "$_grace" ] && return 0
    _remaining=$((_grace - _elapsed))
    log_event "holding the gadget off the bus ${_remaining}s more: the head unit was configured ${_elapsed}s ago and re-presenting inside its boot window wedges CarPlay"
    sleep "$_remaining"
}

udc_speed() {
    cat "$UDC_SYSFS/$UDC/current_speed" 2>/dev/null || true
}

# BC1.2 result the Type-C controller reports for whatever is plugged in.
# DCP is the one that matters: a Dedicated Charging Port never enumerates, so
# the UDC stays "not attached" forever and the daemon logs "function never
# bound".  Without this field that looks identical to a dead cable, and the
# host gets blamed for something the connector decided before Android booted.
charger_type() {
    for state_file in "$EXTCON_SYSFS"/*/state; do
        [ -r "$state_file" ] || continue
        while IFS='=' read -r key value; do
            [ "$value" = 1 ] || continue
            case "$key" in
                DCP) echo DCP; return 0 ;;
                CDP) echo CDP; return 0 ;;
                SDP) echo SDP; return 0 ;;
                FAST-CHARGER) echo FAST; return 0 ;;
                SLOW-CHARGER) echo SLOW; return 0 ;;
            esac
        done < "$state_file"
    done
    echo none
}

# Type-C connection state from the CC controller, i.e. whether the cable's
# Rp/Rd negotiation ever succeeded.  This is a different wire pair from the
# BC1.2 verdict above: a cable can carry D+/D- (so the charger is detected as
# CDP) while CC is not connected at all.  dwc3 only enters device mode on the
# CC event, so "charger=CDP cc=off" means the host never even saw us -- the
# classic symptom of an A-to-C cable missing its 56k pull-up.
cc_state() {
    for state_file in "$EXTCON_SYSFS"/*/state; do
        [ -r "$state_file" ] || continue
        while IFS='=' read -r key value; do
            case "$key" in
                USB)
                    if [ "$value" = 1 ]; then
                        echo on
                        return 0
                    fi
                    ;;
            esac
        done < "$state_file"
    done
    echo off
}

record_udc_state() {
    printf '%s state=%s speed=%s charger=%s cc=%s\n' \
        "$(cut -d. -f1 /proc/uptime 2>/dev/null || echo 0)" \
        "${1:-unknown}" "${2:-unknown}" "${3:-unknown}" "${4:-unknown}" >> "$USB_STATE_LOG" 2>/dev/null || true
}

# Head units give no error message when they walk away from a device, so record
# how far enumeration actually got.  The UDC state ladder is
#   not attached -> powered -> default -> addressed -> configured
# "addressed" without "configured" means the host read descriptors and refused
# the configuration; "powered"/"default" means it never started enumeration.
# The charger type is tracked alongside because a DCP verdict freezes the ladder
# at "not attached".  The cc field separates the two ways of getting stuck:
# a DCP charger is a slow-charge port, while "charger=CDP cc=off" is a broken
# CC path and no cable swap will be needed on the host side.
udc_observer() {
    previous=$(udc_state)
    previous_charger=$(charger_type)
    previous_cc=$(cc_state)
    record_udc_state "$previous" "$(udc_speed)" "$previous_charger" "$previous_cc"
    log_event "UDC observer started state=${previous:-unknown} charger=${previous_charger} cc=${previous_cc}"
    while :; do
        sleep "$UDC_OBSERVE_INTERVAL"
        current=$(udc_state)
        charger=$(charger_type)
        cc=$(cc_state)
        if [ "$cc" != "$previous_cc" ]; then
            record_udc_state "${current:-unknown}" "$(udc_speed)" "$charger" "$cc"
            log_event "cc ${previous_cc} -> ${cc} (udc state ${current:-unknown} charger ${charger})"
            if [ "$cc" = on ]; then
                log_event "cc established: $(dmesg 2>/dev/null | grep -c 'connection has connected') connected / $(dmesg 2>/dev/null | grep -c 'connection has disconnected') disconnected so far"
            fi
            previous_cc=$cc
            [ -n "$current" ] && previous=$current
            continue
        fi
        if [ "$charger" != "$previous_charger" ]; then
            record_udc_state "${current:-unknown}" "$(udc_speed)" "$charger" "$cc"
            log_event "charger ${previous_charger} -> ${charger} (udc state ${current:-unknown})"
            previous_charger=$charger
            [ -n "$current" ] && previous=$current
            continue
        fi
        # The charger re-read the cable and landed on the same verdict; skip.
        if [ -z "$current" ]; then
            continue
        fi
        if [ "$current" = "$previous" ]; then
            continue
        fi
        record_udc_state "$current" "$(udc_speed)" "$charger" "$cc"
        log_event "UDC state ${previous:-unknown} -> ${current:-unknown}"
        if [ "$current" = configured ]; then
            cut -d. -f1 /proc/uptime > "$LAST_CONFIGURED_STAMP" 2>/dev/null || true
            {
                echo '--- dmesg (usb only) ---'
                dmesg 2>/dev/null | \
                    grep -iE 'dwc3|gadget|ffs|usb_state|config #|ep0|reset' | \
                    tail -n 20
            } >> "$USB_STATE_LOG" 2>/dev/null || true
        fi
        previous=$current
    done
}

start_udc_observer() {
    mkdir -p "$ROOT/logs" 2>/dev/null || true
    printf '\n--- session start uptime=%s ---\n' \
        "$(cut -d. -f1 /proc/uptime 2>/dev/null || echo 0)" \
        >> "$USB_STATE_LOG" 2>/dev/null || true
    udc_observer &
    UDC_OBSERVER=$!
    printf '%s\n' "$UDC_OBSERVER" > "$ROOT/udc-observer.pid" 2>/dev/null || true
}

stop_udc_observer() {
    [ -n "$UDC_OBSERVER" ] || return 0
    kill "$UDC_OBSERVER" 2>/dev/null || true
    UDC_OBSERVER=
    rm -f "$ROOT/udc-observer.pid" 2>/dev/null || true
}

teardown_gadget() {
    if [ "$ATTACHED" -ne 1 ]; then
        return
    fi
    detach_udc
    rm -f "$OS_DESC_CONFIG_LINK" 2>/dev/null || true
    for slot in f0 f1 f2 f3 f4 f5 f6; do
        rm -f "$CARPLAY_CONFIG/$slot" 2>/dev/null || true
    done
    rmdir "$CARPLAY_CONFIG" 2>/dev/null || true
    # b.1 must never be left empty: the kernel rejects a gadget whose only
    # configuration has no function. b.1 keeps its ptp link (if this run got
    # that far) and gets the stock adb link back immediately.
    rm -f "$STOCK_CONFIG/f1" 2>/dev/null || true
    ln -s "${STOCK_F1:-$FFS_ADB_DEFAULT}" "$STOCK_CONFIG/f1" 2>/dev/null || true
    ATTACHED=0
}

restore_stock_gadget() {
    [ "$MODIFIED_GADGET" -eq 1 ] || return 0
    log_event "restoring stock gadget layout (target vid=$ORIGINAL_VID pid=$ORIGINAL_PID bcdUSB=$ORIGINAL_BCDUSB)"
    detach_udc
    rm -f "$OS_DESC_CONFIG_LINK" 2>/dev/null || true
    rm -f "$STOCK_OS_DESC_CONFIG_LINK" 2>/dev/null || true
    # Configuration 2 is ours and must disappear entirely; a leftover empty
    # configuration would still be advertised to the next host as
    # bNumConfigurations 2.
    for slot in f0 f1 f2 f3 f4 f5 f6; do
        rm -f "$CARPLAY_CONFIG/$slot" 2>/dev/null || true
    done
    # configfs drops its virtual attributes with the directory; the rm only
    # matters where these are real files (the test sandbox).
    rm -f "$CARPLAY_CONFIG/MaxPower" "$CARPLAY_CONFIG/bmAttributes" 2>/dev/null || true
    rmdir "$CARPLAY_CONFIG/strings" 2>/dev/null || true
    rmdir "$CARPLAY_CONFIG" 2>/dev/null || true
    ATTACHED=0
    restore_file "$OS_DESC/use" "$ORIGINAL_OS_DESC_USE"
    restore_file "$OS_DESC/b_vendor_code" "$ORIGINAL_OS_DESC_VENDOR_CODE"
    if [ -n "$ORIGINAL_OS_DESC_SIGNATURE" ]; then
        printf '%s\n' "$ORIGINAL_OS_DESC_SIGNATURE" > "$OS_DESC/qw_sign" 2>/dev/null || true
    fi
    restore_symlink "$STOCK_OS_DESC_CONFIG_LINK" "$ORIGINAL_OS_DESC_CONFIG"
    # attach_gadget detaches the stock adb function from b.1 and puts ffs.iap2
    # there instead. Recreate the captured b.1 layout, always leaving exactly
    # one function so the kernel does not reject the configuration.
    # The kernel NCM function object outlives its b.1 link; drop it before the
    # slots are rewritten, or it would be re-created as default and a later
    # start would silently re-enter kernel NCM mode after an explicit restore.
    release_kernel_ncm
    # Same for the PTP function, which additionally holds /dev/mtp_usb open. It
    # sits in the uncaptured f0 slot, so the loop below would leave it behind.
    release_kernel_ptp
    for slot in f0 f1 f2 f3 f4 f5 f6; do
        rm -f "$STOCK_CONFIG/$slot" 2>/dev/null || true
    done
    ln -s "${STOCK_F1:-$FFS_ADB_DEFAULT}" "$STOCK_CONFIG/f1" 2>/dev/null || true
    restore_link f2 "$STOCK_F2"
    restore_link f3 "$STOCK_F3"
    restore_link f4 "$STOCK_F4"
    restore_link f5 "$STOCK_F5"
    restore_link f6 "$STOCK_F6"
    restore_file "$G/idVendor" "$ORIGINAL_VID"
    restore_file "$G/idProduct" "$ORIGINAL_PID"
    restore_file "$G/bcdUSB" "$ORIGINAL_BCDUSB"
    restore_file "$G/bDeviceClass" "$ORIGINAL_DEVICE_CLASS"
    restore_file "$G/bDeviceSubClass" "$ORIGINAL_DEVICE_SUBCLASS"
    restore_file "$G/bDeviceProtocol" "$ORIGINAL_DEVICE_PROTOCOL"
    restore_file "$STRINGS/manufacturer" "$ORIGINAL_STRING_MANUFACTURER"
    restore_file "$STRINGS/product" "$ORIGINAL_STRING_PRODUCT"
    restore_file "$STRINGS/serialnumber" "$ORIGINAL_STRING_SERIAL"
    # The current budget belongs to the stock layout too: leaving 250 behind
    # would make the adb gadget under-declare its draw.
    restore_file "$STOCK_CONFIG/MaxPower" "${STOCK_MAX_POWER:-500}"
    # Same for the self-powered bit: the stock adb gadget is bus-powered, and
    # leaving 0xc0 behind would misreport a device that is not on its own rail.
    restore_file "$STOCK_CONFIG/bmAttributes" "${STOCK_BM_ATTRIBUTES:-0x80}"
    if [ -n "$ORIGINAL_UDC" ]; then
        printf '%s\n' "$ORIGINAL_UDC" > "$G/UDC" 2>/dev/null || true
    fi
    release_usb_gadget || log_event "warning: could not restore stock USB ownership"
    if [ -n "$ORIGINAL_USB_CONFIG" ] && \
       [ "$(getprop sys.usb.config 2>/dev/null)" != "$ORIGINAL_USB_CONFIG" ]; then
        setprop sys.usb.config "$ORIGINAL_USB_CONFIG" 2>/dev/null || true
    fi
    log_event "stock gadget restored vid=$(cat "$G/idVendor" 2>/dev/null) pid=$(cat "$G/idProduct" 2>/dev/null) bcdUSB=$(cat "$G/bcdUSB" 2>/dev/null) UDC=[$(cat "$G/UDC" 2>/dev/null)]"
    MODIFIED_GADGET=0
}

if [ "$RESTORE_ONLY" -eq 1 ]; then
    # init stop kills the service process group immediately on this image.
    # Manual control calls this after it has stopped all runtime processes;
    # the singleton lock prevents restoring under a live supervisor.
    MODIFIED_GADGET=1
    restore_stock_gadget
    # Order matters: kill the guard first, then hand the default back. Reversed,
    # the watcher would restore the pinning within one interval.
    stop_usb_lock_watcher
    # Hand the persisted USB default back as well, otherwise the next boot would
    # still come up with ADB held off the bus after an explicit restore.
    usb_lock_reset_defaults
    rm -f "$PID_FILE" "$DAEMON_PID_FILE"
    cleanup_lock
    exit 0
fi

# Creating the function *is* the probe: without CONFIG_USB_CONFIGFS_NCM there is
# no "ncm" function registered in configfs and mkdir fails with ENOENT. The
# daemon runs the same check before writing descriptors, so a missing symbol
# leaves both sides on the six-descriptor TUN layout instead of half-applying
# the kernel one.
setup_kernel_ncm() {
    [ -d "$NCM_FUNC" ] && return 0
    mkdir "$NCM_FUNC" 2>/dev/null || {
        log_event "kernel NCM unavailable (no CONFIG_USB_CONFIGFS_NCM); daemon keeps the TUN layout"
        return 1
    }
    # Do NOT try to name the netdev. f_ncm.c exposes `ifname` as a read-only
    # attribute (show only) and gether_setup_default() has already fixed the
    # name to "usb%d" by the time mkdir returns, so a write here fails silently
    # and only makes the next reader believe the name was chosen. The daemon
    # reads the attribute itself; this side just records what the kernel said.
    actual=$(cat "$NCM_FUNC/ifname" 2>/dev/null | tr -d ' \r\n')
    log_event "kernel NCM function created netdev=${actual:-unknown} (named by the kernel; ifname is read-only)"
    # MAC before linking: f_ncm exports dev_addr as the CDC Ethernet
    # iMacAddress string, so the host sees it in the descriptor tree, and the
    # default (random local address) is a fingerprint no Apple device has.
    if printf '%s\n' "$NCM_DEV_ADDR" > "$NCM_FUNC/dev_addr" 2>/dev/null; then
        log_event "ncm: dev_addr pinned to $NCM_DEV_ADDR (Apple OUI)"
    else
        log_event "ncm: dev_addr rejected; kernel default MAC kept (not an Apple OUI)"
    fi
    if printf '%s\n' "$NCM_HOST_ADDR" > "$NCM_FUNC/host_addr" 2>/dev/null; then
        log_event "ncm: host_addr pinned to $NCM_HOST_ADDR (Apple OUI)"
    else
        log_event "ncm: host_addr rejected; kernel default MAC kept (not an Apple OUI)"
    fi
    return 0
}

release_kernel_ncm() {
    rm -f "$NCM_LINK" 2>/dev/null || true
    rmdir "$NCM_FUNC" 2>/dev/null || true
}

# Same probe-by-mkdir idiom as setup_kernel_ncm: without
# CONFIG_USB_CONFIGFS_F_PTP no "ptp" function is registered in configfs and
# mkdir fails with ENOENT. That is a degraded layout, not a fatal one -- the
# attach continues without interface 0 and logs it.
setup_kernel_ptp() {
    [ -d "$PTP_FUNC" ] && return 0
    # Prerequisite, in this order. ptp_alloc_inst() -> alloc_inst_mtp_ptp(false)
    # reuses the MTP instance's /dev/mtp_usb; without mtp.gs0 present the ptp
    # mkdir succeeds but the later configfs link fails with EINVAL and the whole
    # attach aborts (observed 2026-10-27: three crash-loop cycles, USB dead).
    if [ ! -d "$PTP_MTP_FUNC" ]; then
        mkdir "$PTP_MTP_FUNC" 2>/dev/null || {
            log_event "kernel PTP unavailable (mtp prerequisite missing); interface 0 stays absent"
            return 1
        }
        log_event "kernel MTP instance created as ptp prerequisite (left unlinked)"
    fi
    mkdir "$PTP_FUNC" 2>/dev/null || {
        log_event "kernel PTP unavailable (no CONFIG_USB_CONFIGFS_F_PTP); interface 0 stays absent"
        return 1
    }
    log_event "kernel PTP function created (interface 0: 06/01/01, 3 endpoints)"
    return 0
}

# Teardown order is the mirror of setup: unlink, then ptp, then the MTP
# prerequisite we created. rmdir on a still-referenced instance fails, so the
# order matters if this ever has to run without the link already gone.
release_kernel_ptp() {
    rm -f "$PTP_LINK" 2>/dev/null || true
    rmdir "$PTP_FUNC" 2>/dev/null || true
    rmdir "$PTP_MTP_FUNC" 2>/dev/null || true
}

prepare_ffs() {
    claim_usb_gadget || return 1
    detach_udc
    rm -f "$OS_DESC_CONFIG_LINK" 2>/dev/null || true
    # b.2 is torn down and rebuilt on every attach so a half-written slot from
    # an interrupted run can never reach the bind. Its functions (ffs.iap2,
    # ncm.usb0) deliberately outlive the directory: re-creating them costs a
    # udev-style name change for NCM and a remount for ffs.
    for slot in f0 f1 f2 f3 f4 f5 f6; do
        rm -f "$CARPLAY_CONFIG/$slot" 2>/dev/null || true
    done
    # configfs drops its virtual attributes with the directory; the rm only
    # matters where these are real files (the test sandbox).
    rm -f "$CARPLAY_CONFIG/MaxPower" "$CARPLAY_CONFIG/bmAttributes" 2>/dev/null || true
    rmdir "$CARPLAY_CONFIG/strings" 2>/dev/null || true
    rmdir "$CARPLAY_CONFIG" 2>/dev/null || true
    umount -l "$FFS" 2>/dev/null || true
    # Release the FunctionFS device name after a supervised stop. A lazy
    # unmount alone can leave ffs.iap2 registered and make the next mount EBUSY.
    rmdir "$G/functions/ffs.iap2" 2>/dev/null || true
    mkdir -p "$G/functions/ffs.iap2" "$FFS"
    mount -t functionfs iap2 "$FFS" || return 1
    # Must run before the daemon starts: iap2d decides whether to describe
    # interfaces 2/3 by looking for this directory.
    setup_kernel_ncm || true
    # Decided here, before the daemon starts: iap2d has to know whether it is
    # describing the CarPlay configuration of a two-configuration gadget or the
    # only configuration there is, and it cannot look (SELinux). Pass it in.
    if ensure_carplay_directory; then
        STANDBY_CONFIG=1
    else
        STANDBY_CONFIG=0
    fi
    return 0
}

wait_for_descriptors() {
    tries=0
    while [ "$tries" -lt 100 ]; do
        [ -e "$FFS/ep1" ] && return 0
        kill -0 "$CHILD" 2>/dev/null || return 1
        sleep 0.1
        tries=$((tries + 1))
    done
    return 1
}

# Creates configuration 2 and pins its per-configuration attributes.
#
# Called twice on purpose: once from prepare_ffs, before the daemon starts (so
# iap2d can be told which layout it is describing -- it cannot probe configfs
# itself, its SELinux domain cannot even stat /config/usb_gadget), and again
# from attach_gadget so a half-written directory can never reach the bind.
ensure_carplay_directory() {
    if [ ! -d "$CARPLAY_CONFIG" ]; then
        mkdir "$CARPLAY_CONFIG" 2>/dev/null || {
            log_event "attach: cannot create $CARPLAY_CONFIG (multi-config unavailable)"
            return 1
        }
        log_event "attach: created configuration 2 ($CARPLAY_CONFIG)"
    fi
    # bMaxPower and bmAttributes are per configuration. Leaving b.2 at the
    # stock 1000 mA / bus-powered defaults would advertise two different power
    # envelopes on the same device, and the host reads b.1 first.
    echo "$APPLE_USB_MAX_POWER" > "$CARPLAY_CONFIG/MaxPower" 2>/dev/null || true
    echo "$APPLE_BM_ATTRIBUTES" > "$CARPLAY_CONFIG/bmAttributes" 2>/dev/null || true
    return 0
}

ensure_carplay_config() {
    # Both configurations are rewritten from scratch on every attach:
    #   b.1 = PTP only               (standby, bConfigurationValue 1)
    #   b.2 = ffs.iap2 + ncm.usb0    (CarPlay, bConfigurationValue 2)
    # A configfs configuration with no function makes the kernel reject the
    # whole gadget with -EINVAL, so each of them must end up with at least one
    # function before the UDC is bound. b.1's ptp is linked in attach_gadget
    # (after setup_kernel_ptp), b.2's pair right after it.
    detach_udc
    rm -f "$OS_DESC_CONFIG_LINK" 2>/dev/null || true
    rm -f "$STOCK_OS_DESC_CONFIG_LINK" 2>/dev/null || true
    # Stock init fills f1..f6 with adb/rndis/mtp-style functions. All of them
    # have to go: a leftover mtp link would add a *second* 06/01/01 interface
    # next to our ptp and make the kernel refuse the bind.
    for slot in f0 f1 f2 f3 f4 f5 f6; do
        rm -f "$STOCK_CONFIG/$slot" 2>/dev/null || true
    done
    for slot in f0 f1 f2 f3 f4 f5 f6; do
        rm -f "$CARPLAY_CONFIG/$slot" 2>/dev/null || true
    done
    ensure_carplay_directory || return 1
    echo "$APPLE_USB_MAX_POWER" > "$STOCK_CONFIG/MaxPower" 2>/dev/null || true
    echo "$APPLE_BM_ATTRIBUTES" > "$STOCK_CONFIG/bmAttributes" 2>/dev/null || true
    return 0
}

attach_gadget() {
    MODIFIED_GADGET=1
    ensure_carplay_config || return 1
    # Every attribute below is writable only while no driver owns the UDC.
    # Bail out instead of continuing into a half-applied descriptor set: a
    # still-bound UDC rejects idVendor but happily accepts bcdUSB, which is
    # exactly how this box ended up as 05ac:12a8 with the stock bcdUSB.
    [ -z "$(cat "$G/UDC" 2>/dev/null)" ] || return 1
    echo 0x05AC > "$G/idVendor" || return 1
    echo 0x12A8 > "$G/idProduct" || return 1
    echo 0x0200 > "$G/bcdUSB" || return 1
    # A real iPhone reports bcdUSB 0x0200. Anything above 0x0200 makes the
    # host request a BOS descriptor; the stock 0x0310 bcdDevice is kept out
    # of the accessory path for the same reason (it is not part of the stock
    # snapshot, so nothing restores it).
    #
    # bcdDevice is BCD-encoded, so each nibble must be 0..9: configfs rejects
    # 0x0A00 outright (A is not a decimal digit) and the whole attach aborts.
    # Its high byte tracks the iOS major version in BCD -- 0x0700 = 7.00,
    # 0x0900 = 9.00, 0x1000 = 10.00, 0x1200 = 12.00. CarPlay starts at
    # iOS 7.1, so 0x0100 tells an OEM head unit that the device is an iOS 1.x
    # phone from before CarPlay existed: it enumerates, accepts configuration
    # 1, and then silently stops -- no 0xC0/0x53 probe, no iAP2, nothing in
    # the logs. A userspace sink (DiPlay) never reads this field, which is why
    # 0x0100 worked there and never on the A4L MIB unit.
    # iOS 9.01 (BCD 09.01). This is a measured value, not a guess: it is what
    # a DDBox010 reports, and that box drives CarPlay on the same A4L head unit
    # that ignores us. The accessory path carried 0x1200 (iOS 12) until that
    # sample was captured, and the head unit's behaviour with 0x1200 is
    # indistinguishable from its behaviour with 0x0100 -- enumerates, accepts
    # configuration 1, then goes silent. Track the device known to work.
    echo 0x0901 > "$G/bcdDevice" || { log_event "attach: bcdDevice write failed"; return 1; }
    echo 0x00 > "$G/bDeviceClass" || { log_event "attach: bDeviceClass write failed"; return 1; }
    echo 0x00 > "$G/bDeviceSubClass" || { log_event "attach: bDeviceSubClass write failed"; return 1; }
    echo 0x00 > "$G/bDeviceProtocol" || { log_event "attach: bDeviceProtocol write failed"; return 1; }
    # Current budget, not a capability: a real iPhone asks for 500 mA here and
    # negotiates charging separately, while the stock Android gadget asks for
    # 1000 mA. See APPLE_USB_MAX_POWER for why the head unit cares.
    echo "$APPLE_USB_MAX_POWER" > "$STOCK_CONFIG/MaxPower" ||
        { log_event "attach: bMaxPower write failed"; return 1; }
    # Self-powered, no remote wakeup -- the byte both working samples report.
    echo "$APPLE_BM_ATTRIBUTES" > "$STOCK_CONFIG/bmAttributes" ||
        { log_event "attach: bmAttributes write failed"; return 1; }
    mkdir -p "$STRINGS"
    printf '%s\n' "$APPLE_USB_MANUFACTURER" > "$STRINGS/manufacturer" ||
        { log_event "attach: manufacturer string failed"; return 1; }
    printf '%s\n' "$APPLE_USB_PRODUCT" > "$STRINGS/product" ||
        { log_event "attach: product string failed"; return 1; }
    printf '%s\n' "$APPLE_USB_SERIAL" > "$STRINGS/serialnumber" ||
        { log_event "attach: serial string failed"; return 1; }
    # Configuration 1 is the standby: PTP only, the shape DDBox010 advertises
    # as its first configuration. A head unit selects it, recognises an Apple
    # device, and only then asks for the CarPlay configuration by value 2.
    #
    # If the kernel exposes no ptp function we cannot offer a standby that
    # looks Apple, so fall back to the old single-configuration layout rather
    # than leaving b.1 empty -- the kernel rejects a gadget whose configuration
    # has no function at all.
    MULTI_CONFIG=0
    setup_kernel_ptp || true
    if [ -d "$PTP_FUNC" ] && ln -s "$PTP_FUNC" "$PTP_LINK" 2>/dev/null; then
        MULTI_CONFIG=1
        log_event "attach: config 1 = PTP only, standby (3 endpoints)"
    else
        release_kernel_ptp
        log_event "attach: no kernel ptp; single-configuration fallback in b.1"
    fi

    if [ "$MULTI_CONFIG" -eq 1 ]; then
        ln -s "$G/functions/ffs.iap2" "$LINK" ||
            { log_event "attach: iap2 function link failed"; return 1; }
        # Kernel NCM goes in last on purpose: composite assigns interface
        # numbers in link order inside a configuration, so ffs.iap2 takes 0/1
        # and ncm takes 2/3.
        if [ -d "$NCM_FUNC" ]; then
            ln -s "$NCM_FUNC" "$NCM_LINK" ||
                { log_event "attach: ncm function link failed"; return 1; }
        fi
        log_event "attach: config 2 = iAP2 + ethernet + ncm, CarPlay (4 interfaces)"
    else
        rmdir "$CARPLAY_CONFIG" 2>/dev/null || true
        ln -s "$G/functions/ffs.iap2" "$STOCK_CONFIG/f1" ||
            { log_event "attach: iap2 function link failed"; return 1; }
        if [ -d "$NCM_FUNC" ]; then
            ln -s "$NCM_FUNC" "$STOCK_CONFIG/f2" ||
                { log_event "attach: ncm function link failed"; return 1; }
        fi
    fi
    # Do not advertise Microsoft OS descriptors while impersonating an
    # accessory. A real iPhone answers nothing for string 0xEE; advertising
    # "MSFT100" with b_vendor_code 0x52 makes Windows fetch the MS OS extended
    # compat ID through the very request code (0xC0/0x52) our CarPlay handler
    # answers with a single 0x00 byte. Windows then rejects the device with
    # Code 10, "the Microsoft OS extended configuration descriptor returned by
    # the USB device is invalid". Leave os_desc disabled and unlinked; the
    # stock values are restored by restore_stock_gadget on stop.
    rm -f "$STOCK_OS_DESC_CONFIG_LINK" || { log_event "attach: os_desc stock link unlink failed"; return 1; }
    rm -f "$OS_DESC_CONFIG_LINK" || { log_event "attach: os_desc config link unlink failed"; return 1; }
    echo 0 > "$OS_DESC/use" || { log_event "attach: os_desc use=0 failed"; return 1; }
    # Put the vendor code and signature back so a later debug session never
    # sees a leftover "0x52 / MSFT100" pair and mistakes it for an active
    # Microsoft OS descriptor advertisement.
    if [ -n "$ORIGINAL_OS_DESC_VENDOR_CODE" ]; then
        printf '%s\n' "$ORIGINAL_OS_DESC_VENDOR_CODE" > "$OS_DESC/b_vendor_code" ||
            { log_event "attach: os_desc vendor code restore failed"; return 1; }
    fi
    if [ -n "$ORIGINAL_OS_DESC_SIGNATURE" ]; then
        printf '%s\n' "$ORIGINAL_OS_DESC_SIGNATURE" > "$OS_DESC/qw_sign" ||
            { log_event "attach: os_desc signature restore failed"; return 1; }
    fi
    echo "$UDC" > "$G/UDC" || { log_event "attach: UDC bind failed udc=$UDC"; return 1; }
    # Own cleanup as soon as bind succeeds, even if the daemon unbinds in
    # response to a fast 0x51 during the verification delay below.
    ATTACHED=1
    sleep 0.5
    # Re-assert the Microsoft OS descriptor state after binding.  Some vendor
    # kernels latch the flag at bind time, and a stale "use=1" is exactly what
    # makes Windows append a vendor compatible ID to our hardware IDs, which
    # vehicle head units then read as "Android/Rockchip" instead of "iPhone".
    if [ "$(cat "$OS_DESC/use" 2>/dev/null || echo 1)" != "0" ]; then
        echo 0 > "$OS_DESC/use" 2>/dev/null || true
    fi
    rm -f "$STOCK_OS_DESC_CONFIG_LINK" "$OS_DESC_CONFIG_LINK" 2>/dev/null || true
    log_event "os_desc use=$(cat "$OS_DESC/use" 2>/dev/null) entries=$(ls "$OS_DESC" 2>/dev/null | tr '\n' ' ')"
    if [ "$MULTI_CONFIG" -eq 1 ]; then
        MAIN_LINK=$LINK
    else
        MAIN_LINK=$STOCK_CONFIG/f1
    fi
    if [ "$(cat "$G/UDC" 2>/dev/null)" != "$UDC" ] || [ ! -L "$MAIN_LINK" ]; then
        if [ -L "$MAIN_LINK" ] && [ -z "$(cat "$G/UDC" 2>/dev/null)" ] &&
           [ "$(cat "$ROLE_REQUEST" 2>/dev/null)" = host ] &&
           [ -n "$CHILD" ] && kill -0 "$CHILD" 2>/dev/null; then
            log_event "attach: daemon accepted host handoff during post-bind check; preserving child pid=$CHILD"
            return 0
        fi
        log_event "attach: post-bind verify failed udc=[$(cat "$G/UDC" 2>/dev/null)] want=[$UDC] link=$([ -L "$MAIN_LINK" ] && echo yes || echo no)"
        return 1
    fi
    ATTACHED=1
    MODIFIED_GADGET=1
    log_event "gadget attached vid=$(cat "$G/idVendor" 2>/dev/null) pid=$(cat "$G/idProduct" 2>/dev/null) bcdUSB=$(cat "$G/bcdUSB" 2>/dev/null) bcdDevice=$(cat "$G/bcdDevice" 2>/dev/null) maxpower=$(cat "$STOCK_CONFIG/MaxPower" 2>/dev/null) configs=$(ls "$G/configs" 2>/dev/null | tr '\n' ',') multi=$MULTI_CONFIG"
    return 0
}

if [ -f "$DAEMON_PID_FILE" ]; then
    old_daemon=$(cat "$DAEMON_PID_FILE" 2>/dev/null || true)
    if [ -n "$old_daemon" ] && kill -0 "$old_daemon" 2>/dev/null; then
        old_cmdline=$(read_cmdline "$old_daemon")
        case "$old_cmdline" in
            *"/data/carplay/bin/iap2d"*)
                log_event "terminating orphaned daemon pid=$old_daemon"
                kill -TERM "$old_daemon" 2>/dev/null || true
                tries=0
                while kill -0 "$old_daemon" 2>/dev/null && [ "$tries" -lt 50 ]; do
                    sleep 0.1
                    tries=$((tries + 1))
                done
                kill -KILL "$old_daemon" 2>/dev/null || true
                ;;
        esac
    fi
    rm -f "$DAEMON_PID_FILE"
fi

ensure_default_role() {
    # dwc3_mode stands at "otg" after boot because that is what the device tree
    # says, and "otg" with no extcon means "nobody decided". Ask for peripheral
    # explicitly; when the switch is unsupported the file is absent or read-only
    # and the write is skipped, which is the pre-2026-10-28 behaviour anyway.
    [ -w "$DWC3_MODE" ] || return 0
    current=$(cat "$DWC3_MODE" 2>/dev/null || true)
    [ "$current" = "peripheral" ] && return 0
    if { echo peripheral > "$DWC3_MODE"; } 2>/dev/null; then
        log_event "usb: dwc3 role pinned to peripheral on start (was ${current:-unknown})"
    else
        log_event "usb: could not pin the dwc3 role (was ${current:-unknown})"
    fi
    return 0
}

if stock_gadget_is_modified; then
    MODIFIED_GADGET=1
    if ! should_attach; then
        restore_stock_gadget
    fi
fi

log_event "starting pid=$$ force_attach=$FORCE_ATTACH"
ensure_default_role
# A role swap is a per-session decision.  After a reboot dwc3 comes up in
# peripheral mode (pinned above), and carrying a stale "host" forward would
# leave the port with no gadget bound and no host controller driving it --
# the head unit would see a dead port.  Start every boot as a device.
set_role_state device
rm -f "$ROLE_REQUEST"
rm -f "$ROLE_SWITCH_FAILURES"
initial_delay=$(json_integer reconnectInitialMs 1000)
maximum_delay=$(json_integer reconnectMaxMs 30000)
case "$initial_delay" in *[!0-9]*|'') initial_delay=1000 ;; esac
case "$maximum_delay" in *[!0-9]*|'') maximum_delay=30000 ;; esac
[ "$initial_delay" -ge 100 ] || initial_delay=100
[ "$maximum_delay" -ge "$initial_delay" ] || maximum_delay="$initial_delay"
backoff=$initial_delay
start_udc_observer
# Re-assert the ADB/role guard for as long as this supervisor owns the port:
# a framework-driven sys.usb.config flip or a replayed init branch can undo it
# long after the early claim.
start_usb_lock_watcher

while [ "$STOPPING" -eq 0 ]; do
    if should_attach; then
        attach_grace_wait
        # A replayed stock init branch re-links ffs.adb into b.1 and re-binds
        # the UDC. Leaving it in place would advertise both the ADB interface
        # and the Apple interfaces in one configuration, which is exactly the
        # signature a head unit reads as "Android phone".
        evict_restocked_adb_function || true
        if ! prepare_ffs; then
            log_event "FunctionFS mount failed; retrying in ${backoff}ms"
            sleep $((backoff / 1000))
            backoff=$((backoff * 2))
            [ "$backoff" -le "$maximum_delay" ] || backoff="$maximum_delay"
            continue
        fi
    fi

    mode_arg=
    selftest_arg=
    if [ -f "$SELFTEST_FLAG" ]; then
        rm -f "$SELFTEST_FLAG"
        selftest_arg=--selftest-role-switch
        log_event "role-switch self-test requested; this daemon run will switch to host once"
    fi
    if host_role_active; then
        mode_arg=--host
        log_event "USB host role active; daemon enumerates the head unit instead of being enumerated"
    elif ! should_attach; then
        mode_arg=--management-only
        log_event "USB attach disabled; daemon remains available for management"
    fi

    started=$(cut -d. -f1 /proc/uptime)
    # CARPLAY_OTG_UDC tells the daemon which USB bus is ours once it enumerates
    # the head unit.  Every other bus on this board carries unrelated hardware
    # (two EHCI/OHCI pairs and the always-present USB3 host controller), and a
    # stick or a 4G dongle with a vendor interface would otherwise be claimed as
    # "the car" within milliseconds of the switch, while the head unit that
    # arrives a few hundred ms later is ignored.  The UDC is gone from
    # /sys/class/udc by then, so it has to be passed in from here.
    CARPLAY_OTG_UDC=$UDC \
    CARPLAY_STANDBY_CONFIG=$STANDBY_CONFIG "$BIN" --fg $mode_arg $selftest_arg >/dev/null 2>&1 &
    CHILD=$!
    echo "$CHILD" > "$DAEMON_PID_FILE"
    chmod 600 "$DAEMON_PID_FILE"
    CHILD_FAILURE=0
    log_event "daemon started pid=$CHILD mode=${mode_arg:-usb}"

    if should_attach; then
        if ! wait_for_descriptors; then
            log_event "daemon descriptors were not ready"
            CHILD_FAILURE=1
            kill -TERM "$CHILD" 2>/dev/null || true
        elif ! attach_gadget; then
            log_event "Apple gadget attachment failed"
            CHILD_FAILURE=1
            kill -TERM "$CHILD" 2>/dev/null || true
        else
            log_event "Apple gadget attached; USB replug recovery is handled in-process"
        fi
    fi

    wait "$CHILD"
    status=$?
    CHILD=
    rm -f "$DAEMON_PID_FILE"
    finished=$(cut -d. -f1 /proc/uptime)
    runtime=$((finished - started))
    if host_role_active || [ -f "$ROLE_REQUEST" ]; then
        save_role_diagnostics
    fi
    teardown_gadget
    if ! should_attach && ! host_role_active; then
        restore_stock_gadget
    fi

    # 0x40/0x51 asked for the host role.  The gadget is unbound by now, which
    # is the precondition for dwc3 to accept a mode change at all, so the
    # switch happens here rather than in the daemon.
    if [ -f "$ROLE_REQUEST" ]; then
        _requested=$(cat "$ROLE_REQUEST" 2>/dev/null)
        rm -f "$ROLE_REQUEST"
        case "$_requested" in
            host)
                if ! role_switch_allowed; then
                    log_event "ignoring host request: switch budget exhausted since boot; staying a device"
                else
                    log_event "head unit asked for the USB host role; switching"
                    if [ -x "$ROLE_SWITCH" ] && "$ROLE_SWITCH" --host; then
                        set_role_state host
                        log_event "dwc3 is in host mode; next start enumerates the head unit"
                        status=76
                    else
                        set_role_state device
                        record_role_switch_failure
                        log_event "host switch failed; falling back to device so the port still enumerates"
                        [ -x "$ROLE_SWITCH" ] && GADGET_REBIND=0 "$ROLE_SWITCH" --device \
                            >> "${LOG_DIR:-$ROOT/logs}/supervisor.log" 2>&1 || true
                    fi
                fi
                ;;
            device)
                # We were in host mode and the daemon could not find the peer.
                # Count it, or the next 0x51 would flip the port straight back.
                if host_role_active; then
                    record_role_switch_failure
                fi
                # The previous daemon has closed FunctionFS. Only change
                # the data role here; the next daemon supplies descriptors
                # before attach_gadget binds the new session.
                if [ -x "$ROLE_SWITCH" ] && GADGET_REBIND=0 "$ROLE_SWITCH" --device \
                    >> "${LOG_DIR:-$ROOT/logs}/supervisor.log" 2>&1; then
                    set_role_state device
                    log_event "role returned to device"
                else
                    log_event "device role recovery failed; see role helper output above"
                fi
                ;;
            *)
                log_event "ignoring unknown role request: ${_requested:-<empty>}"
                ;;
        esac
    fi
    log_event "daemon exited status=$status runtime=${runtime}s"

    [ "$STOPPING" -ne 0 ] && break
    if [ "$CHILD_FAILURE" -eq 1 ] && [ "$status" -eq 0 ]; then
        status=1
    fi
    case "$status" in
        0) log_event "clean stop requested"; break ;;
        64) log_event "configuration error; supervisor exit 64"; break ;;
        75) backoff=100; continue ;;
        76) backoff=$initial_delay; continue ;;
    esac
    if [ "$runtime" -ge 30 ]; then
        backoff=$initial_delay
    else
        backoff=$((backoff * 2))
        [ "$backoff" -le "$maximum_delay" ] || backoff="$maximum_delay"
    fi
    log_event "restarting in ${backoff}ms"
    sleep $((backoff / 1000))
done

teardown_gadget
restore_stock_gadget
stop_udc_observer
stop_usb_lock_watcher
usb_lock_reset_defaults
rm -f "$PID_FILE"
rm -f "$DAEMON_PID_FILE"
cleanup_lock
log_event "stopped"
