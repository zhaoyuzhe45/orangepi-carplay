#!/bin/sh
# Role arbitration between the daemon (which sees 0x40/0x51 on ep0) and the
# supervisor (which owns dwc3).
#
# The cases worth locking down are the boring ones: no state file yet, an empty
# file left by a crash mid-write, and the fact that host mode must make
# should_attach() say "do not bind a gadget" -- binding one while our own host
# controller drives the bus would be two hosts on one wire.
#
# Run with sh from the project root.
set -eu
sup=${1:-deploy/opi4/scripts/carplay_supervisor.sh}
tmp=$(mktemp -d "${TMPDIR:-/tmp}/supervisor-role-request.XXXXXX")
trap 'rm -rf "$tmp"' EXIT HUP INT TERM

fail() { echo "FAIL: $*" >&2; exit 1; }
pass() { echo "ok - $*"; }

for fn in current_role set_role_state host_role_active; do
	sed -n "/^$fn() {/,/^}/p" "$sup" > "$tmp/$fn.sh"
	[ -s "$tmp/$fn.sh" ] || fail "could not extract $fn"
done

LOG=$tmp/log
: > "$LOG"
log_event() { printf '%s\n' "$*" >> "$LOG"; }
ROLE_STATE=$tmp/usb-role.state

. "$tmp/current_role.sh"
. "$tmp/set_role_state.sh"
. "$tmp/host_role_active.sh"

# 1. no state file at all (first boot after the update)
[ "$(current_role)" = device ] || fail "missing state must read as device"
host_role_active && fail "missing state must not look like host"
pass "defaults to device with no state file"

# 2. a normal hand-off
set_role_state host
[ "$(current_role)" = host ] || fail "host was not persisted"
host_role_active || fail "host_role_active missed a host state"
[ "$(cat "$ROLE_STATE")" = host ] || fail "on-disk value is not 'host'"
pass "persists host"

# 3. and back
set_role_state device
[ "$(current_role)" = device ] || fail "device was not persisted"
host_role_active && fail "host_role_active still true after device"
pass "persists device"

# 4. an empty file means "we were interrupted while writing", not "host".
#    Reading it as host would leave the port with no gadget bound.
: > "$ROLE_STATE"
[ "$(current_role)" = device ] || fail "empty state must read as device"
pass "empty state file reads as device"

# 5. garbage must not be trusted either
printf 'HOST\n' > "$ROLE_STATE"
[ "$(current_role)" = device ] || fail "uppercase host must not match"
pass "only the exact word host switches the role"

# 6. host mode must suppress gadget attachment
sed -n '/^should_attach() {/,/^}/p' "$sup" > "$tmp/should_attach.sh"
[ -s "$tmp/should_attach.sh" ] || fail "could not extract should_attach"
. "$tmp/should_attach.sh"
FORCE_ATTACH=1
json_setting() { echo 1; }
set_role_state device
should_attach || fail "device mode with force_attach must attach"
set_role_state host
if should_attach; then
	fail "host mode must not bind a gadget"
fi
pass "host mode suppresses gadget attachment"

# 7. The switch budget: a peer that keeps asking for host but never becomes a
#    device (DiPlay probes with 0x51 and cannot swap) must not be able to flip
#    the port on every reconnect.
for fn in role_switch_allowed record_role_switch_failure; do
	sed -n "/^$fn() {/,/^}/p" "$sup" > "$tmp/$fn.sh"
	[ -s "$tmp/$fn.sh" ] || fail "could not extract $fn"
done
ROLE_SWITCH_FAILURES=$tmp/failures
ROLE_SWITCH_MAX_ATTEMPTS=3
. "$tmp/role_switch_allowed.sh"
. "$tmp/record_role_switch_failure.sh"
rm -f "$ROLE_SWITCH_FAILURES"
role_switch_allowed || fail "no failures must allow a switch"
record_role_switch_failure
record_role_switch_failure
role_switch_allowed || fail "two failures must still allow a switch"
record_role_switch_failure
if role_switch_allowed; then
	fail "three failures must stop further switches"
fi
pass "switch budget stops after three failures"

# 8. A garbled counter (crash mid-write) must read as zero, not block forever.
printf 'x\n' > "$ROLE_SWITCH_FAILURES"
role_switch_allowed || fail "garbled counter must be treated as zero"
pass "garbled counter is treated as zero"

echo "supervisor_role_request_test: all checks passed"
