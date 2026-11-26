#!/bin/sh
# Guard for the attach grace period in the supervisor.
#
# A head unit decides whether to offer CarPlay inside a window that opens when
# it powers up: on a cold start the measured gap from SET_CONFIGURATION to
# 0x40/0x51 is 22-27 s, with no traffic in between.  Re-presenting the gadget
# inside that window wedges the head unit's CarPlay stack -- it keeps
# enumerating us but never sends 0x51 again, and only a head-unit bus reset
# clears it (orangecar, docs/protocol.md 7.3: ten re-presentations, two board
# reboots, no recovery).
#
# Our own retry ladder used to re-present after 1 s, 3 s, 7 s and 15 s, i.e.
# squarely inside that window, and the symptom matches: after one failure the
# car stops listing the box until it is power cycled.  So these guards pin:
#
#   1. a grace period exists and is at least the measured window;
#   2. it is measured from the last *configured* event, not from boot -- a
#      cable that was just plugged in must enumerate at once;
#   3. the wait happens before the gadget is presented again, and only on the
#      attach path (host mode must never be delayed);
#   4. it can be disabled from the environment for bench work.
#
# Run with sh from the project root.
set -eu
source_file=${1:-deploy/opi4/scripts/carplay_supervisor.sh}
[ -f "$source_file" ] || { echo "FAIL: missing $source_file" >&2; exit 1; }

fail() { echo "FAIL: $*" >&2; exit 1; }
pass() { echo "ok - $*"; }

line_of() { grep -n "$1" "$source_file" | head -1 | cut -d: -f1; }

# ---- 1. the knob exists and is long enough -------------------------------
grace=$(line_of '^ATTACH_GRACE_MS=')
[ -n "$grace" ] || fail "ATTACH_GRACE_MS is gone; the retry ladder is back inside the head unit's boot window"
grep -q 'CARPLAY_ATTACH_GRACE_MS' "$source_file" ||
	fail "the grace is not overridable; every bench run would pay 45 s"
ms=$(sed -n "${grace}p" "$source_file" | sed 's/.*:-\([0-9]*\).*/\1/')
case "$ms" in ''|*[!0-9]*) fail "cannot read the default grace from: $(sed -n "${grace}p" "$source_file")" ;; esac
[ "$ms" -ge 45000 ] ||
	fail "the default grace is ${ms}ms; the measured cold-boot window alone is 22-27 s"
pass "attach grace defaults to ${ms}ms and stays overridable"

# ---- 2. measured from the last configured event --------------------------
stamp=$(line_of '^LAST_CONFIGURED_STAMP=')
[ -n "$stamp" ] || fail "there is no record of when the head unit last configured us"
grep -q '> "\$LAST_CONFIGURED_STAMP"' "$source_file" ||
	fail "nothing ever writes LAST_CONFIGURED_STAMP; the grace would never arm"
writer=$(grep -n '> "\$LAST_CONFIGURED_STAMP"' "$source_file" | head -1 | cut -d: -f1)
conf=$(grep -n 'if \[ "\$current" = configured \]' "$source_file" | head -1 | cut -d: -f1)
[ -n "$conf" ] || fail "the UDC observer no longer watches for 'configured'"
[ "$writer" -gt "$conf" ] ||
	fail "the stamp is written outside the configured branch (line $writer vs $conf)"
pass "the window opens on 'configured', not on boot"

# ---- 3. the wait runs before the gadget is presented --------------------
fn=$(line_of '^attach_grace_wait()')
[ -n "$fn" ] || fail "attach_grace_wait() is gone"
call=$(grep -n '^        attach_grace_wait$' "$source_file" | head -1 | cut -d: -f1)
[ -n "$call" ] || fail "attach_grace_wait() is never called; the function alone does nothing"
attach=$(line_of 'elif ! attach_gadget; then')
[ -n "$attach" ] || fail "attach_gadget() is no longer called from the loop"
[ "$call" -lt "$attach" ] ||
	fail "the grace runs after the attach (line $call vs $attach)"
# Only on the attach path: host mode owns the bus and must not be delayed.
prev=$(sed -n "$((call - 1))p" "$source_file")
printf '%s' "$prev" | grep -q 'if should_attach; then' ||
	fail "attach_grace_wait() is not gated on should_attach; host mode would sleep through the role switch"
pass "the grace sits on the attach path, before the presentation"

# ---- 4. the sleep uses the remaining time, not a fixed delay ------------
grep -q '_remaining=\$((_grace - _elapsed))' "$source_file" ||
	fail "the wait does not use the remaining time; a late retry would still sleep the full grace"
grep -q 'sleep "\$_remaining"' "$source_file" ||
	fail "the wait does not sleep the remaining time"
pass "only the remaining part of the window is waited out"

pass "all attach-grace guards hold"
