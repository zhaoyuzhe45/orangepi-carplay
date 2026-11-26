#!/bin/sh
# Source guard for what 0x40/0x51 actually means (A4L, 2026-10-29).
#
# The head unit sends 0x40 51 wValue=1 wLength=0.  Every reference agrees
# that shape is "switch the accessory to host":
#
#   MediaTek carplay.c   usb_control_msg(0x51, USB_TYPE_VENDOR, 1, 0, NULL, 0)
#                        "Apple Device to Host Mode Switch"
#   wiomoc/iap2          Request 0x51, Value 1 = CarPlay mode, Length 0
#   wired-CarPlay flow   40 51 01 00 00 00 00 00 = "iPhone USB role switch"
#   orangecar            wValue=1 CarPlay, wValue=0 the car's own phone-app mode
#
# So the rule these guards defend is:
#
#   1. wValue is the decision: 1 means take the bus.  wLength must not be
#      part of it -- it is 0 in *both* shapes, so gating on it silently
#      ignores every real head unit.
#   2. The takeover must remain overridable at runtime (policy file or env),
#      because a failed attempt costs a head-unit power cycle.
#   3. The ACK still comes before any policy decision: stalling here is what
#      makes a head unit abandon CarPlay entirely.
#
# Run with sh from the project root.
set -eu
source_file=${1:-src/iap2d.c}
[ -f "$source_file" ] || { echo "FAIL: missing $source_file" >&2; exit 1; }

fail() { echo "FAIL: $*" >&2; exit 1; }
pass() { echo "ok - $*"; }

code=$(sed 's://.*::' "$source_file" | sed '/^[[:space:]]*\*/d; /^[[:space:]]*\/\*/d')
line_of() { printf '%s\n' "$code" | grep -n "$1" | head -1 | cut -d: -f1; }

# ---- 1. the decision helper keys off wValue, not off the payload ---------
armed=$(line_of 'static int role_switch_armed')
[ -n "$armed" ] || fail "role_switch_armed() is gone; every 0x51 would switch"
tail_from=$(printf '%s\n' "$code" | tail -n +"$armed")
printf '%s\n' "$tail_from" | head -40 | grep -q 'return w_value == 1;' ||
	fail "role_switch_armed() no longer decides on wValue; the A4L's wLength=0 CarPlay request would be ignored again"
if printf '%s\n' "$tail_from" | head -40 | grep -q 'return w_length >= 4;'; then
	fail "role_switch_armed() is gating on wLength again (that is the refuted 2026-10-29 rule)"
fi
pass "wValue=1 counts as the CarPlay hand-off, wLength stays out of the test"

# ---- 2. the takeover stays overridable at runtime ------------------------
printf '%s\n' "$tail_from" | head -40 | grep -q 'ROLE_POLICY_PATH' ||
	fail "the role-switch.policy override is gone; pinning the role would need a rebuild"
printf '%s\n' "$tail_from" | head -40 | grep -q '"stay"' ||
	fail "the policy no longer understands 'stay'; the escape hatch only works one way"
pass "role-switch.policy can arm ('switch') or pin ('stay') the takeover"

# ---- 3. handle_role_swap passes wValue into the helper -------------------
handle=$(line_of 'static void handle_role_swap')
[ -n "$handle" ] || fail "handle_role_swap() is gone"
pending=$(printf '%s\n' "$code" | grep -n 'g_switch_pending = BOX_ROLE_HOST' | tail -1 | cut -d: -f1)
[ -n "$pending" ] || fail "the host switch is never scheduled"
guard=$(printf '%s\n' "$code" | grep -n '!role_switch_armed(' | head -1 | cut -d: -f1)
[ -n "$guard" ] || fail "handle_role_swap() schedules the switch without asking role_switch_armed()"
printf '%s\n' "$code" | sed -n "${guard}p" | grep -q 'setup->wValue' ||
	fail "role_switch_armed() is called without wValue; the decision would be blind"
[ "$guard" -lt "$pending" ] ||
	fail "role_switch_armed() is checked after the switch is scheduled (line $guard vs $pending)"
pass "the switch is scheduled only after the wValue/policy check"

# ---- 4. the ACK still happens before any policy decision -----------------
ack=$(printf '%s\n' "$code" | grep -n 'consume_control_out(ep0, setup->wLength)' | head -1 | cut -d: -f1)
[ -n "$ack" ] || fail "handle_role_swap() no longer ACKs the request"
[ "$ack" -lt "$guard" ] ||
	fail "the ACK moved after the policy check (line $ack vs $guard); a head unit that gets nothing back gives up on CarPlay"
pass "0x51 is acknowledged before the policy decides anything"

pass "all 0x51 semantics guards hold"
