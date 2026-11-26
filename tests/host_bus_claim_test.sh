#!/bin/sh
# Guards for how the host side picks and claims the head unit.
#
# Three hard-won rules, all of them the same shape: an action that looks
# harmless costs a whole session when it lands at the wrong moment.
#
#   1. Only the bus of the role-switched controller may be claimed.  This
#      board has six USB buses; a stick or a 4G dongle that exposes a vendor
#      interface with a bulk pair would be claimed as "the car" milliseconds
#      after the switch, and the real head unit arriving 0.5-1.1 s later
#      would then be ignored (orangecar, docs/protocol.md 2.2).
#   2. Read the current alternate setting first; SET_INTERFACE(1) only when
#      that read finds no bulk pair.  A SET_INTERFACE landing right after
#      SET_CONFIGURATION is answered with -71 and takes the peer off the bus
#      (same file, 4.1/7.7).  Setting 0 is allowed to be zero-bandwidth, so
#      the ask still has to exist -- it just must not be the first move.
#   3. Disable autosuspend once claimed.  A head unit that suspends
#      mid-handshake is indistinguishable from one that walked away.
#
# Run with sh from the project root.
set -eu
source_file=${1:-src/usbhost.c}
[ -f "$source_file" ] || { echo "FAIL: missing $source_file" >&2; exit 1; }

fail() { echo "FAIL: $*" >&2; exit 1; }
pass() { echo "ok - $*"; }

code=$(sed 's://.*::' "$source_file" | sed '/^[[:space:]]*\*/d; /^[[:space:]]*\/\*/d')
line_of() { printf '%s\n' "$code" | grep -n "$1" | head -1 | cut -d: -f1; }

# ---- 1. the bus filter ---------------------------------------------------
filter=$(line_of 'static int bus_is_role_switched')
[ -n "$filter" ] || fail "bus_is_role_switched() is gone; every bus would be claimable"
tail_from=$(printf '%s\n' "$code" | tail -n +"$filter")
printf '%s\n' "$tail_from" | head -25 | grep -q 'CARPLAY_OTG_UDC' ||
	fail "the bus filter no longer reads CARPLAY_OTG_UDC; it cannot know which controller it is on"
printf '%s\n' "$tail_from" | head -25 | grep -q 'return 1;' ||
	fail "an unset CARPLAY_OTG_UDC must mean 'do not filter', or the box goes blind"

call=$(printf '%s\n' "$code" | grep -n 'bus_is_role_switched((int)busnum)' | head -1 | cut -d: -f1)
[ -n "$call" ] || fail "usbhost_open() never consults the bus filter"
readb=$(line_of 'sysfs_uint(device_entry->d_name, NULL, "busnum", 10, &busnum)')
[ -n "$readb" ] || fail "busnum is no longer read; the filter has nothing to test"
[ "$call" -gt "$readb" ] || fail "the filter runs before busnum exists (line $call vs $readb)"
pass "only the role-switched controller's bus is claimable"

# ---- 2. alternate setting: look first, ask only if needed ----------------
alt_use=$(printf '%s\n' "$code" | grep -n 'select_bulk_alt(fd, interface_number)' | head -1 | cut -d: -f1)
[ -n "$alt_use" ] || fail "SET_INTERFACE(1) is never issued; a zero-bandwidth setting 0 would never come up"
collect_first=$(printf '%s\n' "$code" | grep -n 'collect_endpoints(device_entry->d_name' | head -1 | cut -d: -f1)
[ -n "$collect_first" ] || fail "the endpoints are never read"
[ "$collect_first" -lt "$alt_use" ] ||
	fail "SET_INTERFACE(1) is issued before the current setting is read (line $alt_use vs $collect_first); that is the -71 that drops the peer off the bus"
# Two reads: the first unconditional, the second after the alt switch.
reads=$(printf '%s\n' "$code" | grep -c 'collect_endpoints(device_entry->d_name' || true)
[ "$reads" -ge 2 ] ||
	fail "only one endpoint read exists; the retry after SET_INTERFACE(1) is what makes setting 0 usable"
pass "endpoints are read from the current setting, SET_INTERFACE is the fallback"

# ---- 3. no autosuspend on the claimed device -----------------------------
printf '%s\n' "$code" | grep -q 'power/control' ||
	fail "power/control is never set; the head unit may autosuspend mid-handshake"
printf '%s\n' "$code" | grep -q '"on", 2' ||
	fail "power/control is not written as 'on'; anything else leaves autosuspend enabled"
pw=$(printf '%s\n' "$code" | grep -n 'power/control' | head -1 | cut -d: -f1)
claimed=$(printf '%s\n' "$code" | grep -n 'claimed = 1;' | head -1 | cut -d: -f1)
[ -n "$claimed" ] || fail "the claim flag is gone"
[ "$pw" -lt "$claimed" ] || fail "power/control is set after the claim is recorded"
pass "autosuspend is disabled on the claimed device"

# ---- 4. the sysfs name is kept for the NCM lookup ------------------------
printf '%s\n' "$code" | grep -q 'host->sysfs_name' ||
	fail "the sysfs device name is not stored; the NCM netdev cannot be matched back to this device"
pass "the sysfs device name is carried into struct usbhost"

# ---- 5. host mode readiness ----------------------------------------------
printf '%s\n' "$code" | grep -q 'usbhost_role_bus_count' ||
	fail "the host path does not observe xHCI root-bus registration"
pass "host role readiness is checked separately from downstream enumeration"

pass "all host claim guards hold"
