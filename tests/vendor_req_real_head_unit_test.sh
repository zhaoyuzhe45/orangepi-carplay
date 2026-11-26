#!/bin/sh
# Source guard for the 0x51 path with a real head unit (A4L, 2026-10-29).
#
# The car never selects a configuration before it starts talking: it sends
# 0x40/0x51 (wValue=1, wLength=0) while the device is still addressed, and
# re-probes it every minute from standby.  Two things must hold for the
# daemon to see and ACK that request at all, and both failed on the first
# real car:
#
#   1. The ep0 event thread must be running *before* the round loop parks in
#      wait_bound().  If it is spawned only after the host selects the
#      CarPlay configuration, a 0x51 arriving during standby sits in the
#      kernel event queue with no reader and the car times out.
#
#   2. consume_control_out() must finish a zero-length control-OUT with a
#      read() on ep0.  ffs_ep0_write() stalls every pending OUT setup it is
#      handed, so the old "zero-length write" completion STALLed exactly the
#      request the car was waiting on.
#
# Run with sh from the project root.
set -eu
source_file=${1:-src/iap2d.c}
[ -f "$source_file" ] || { echo "FAIL: missing $source_file" >&2; exit 1; }

fail() { echo "FAIL: $*" >&2; exit 1; }
pass() { echo "ok - $*"; }

# Strip comments: the rationale paragraphs quote the same identifiers the
# guards match on, and a comment-only match would pass vacuously.
code=$(sed 's://.*::' "$source_file" | sed '/^[[:space:]]*\*/d; /^[[:space:]]*\/\*/d')

line_of() { printf '%s\n' "$code" | grep -n "$1" | head -1 | cut -d: -f1; }

# ---- 1. the ep0 thread exists before the round loop ----------------------
ep0_spawn=$(line_of 'pthread_create(&g_ep0thread')
round_loop=$(line_of 'if (serve_round(round) < 0)')
[ -n "$ep0_spawn" ] || fail "the ep0 event thread is never spawned"
[ -n "$round_loop" ] || fail "the round loop is missing"
[ "$ep0_spawn" -lt "$round_loop" ] ||
	fail "the ep0 thread is spawned after the round loop (line $ep0_spawn vs $round_loop); a standby 0x51 would sit unread until the car times out"
pass "the ep0 thread is running while the daemon waits for the host"

# ---- 2. zero-length control-OUT is completed with read(), not write() ----
zeroline=$(printf '%s\n' "$code" | grep -n 'if (left == 0)' | head -1 | cut -d: -f1)
[ -n "$zeroline" ] || fail "consume_control_out() lost its zero-length branch"
tail_from=$(printf '%s\n' "$code" | tail -n +"$zeroline")
printf '%s\n' "$tail_from" | head -8 | grep -q 'read(ep0, buf, sizeof(buf))' ||
	fail "the zero-length OUT is no longer completed with read(ep0); write() stalls pending OUT setups in ffs_ep0_write()"
printf '%s\n' "$tail_from" | head -8 | grep -q 'write(ep0, buf, 0)' &&
	fail "the zero-length OUT completion is back to write(ep0, buf, 0); ffs_ep0_write() stalls every pending OUT setup"
pass "a wLength=0 control-OUT is ACKed with a zero-length ep0 queue (read)"

# ---- 3. the 0x51 role decision still routes through the budget -----------
printf '%s\n' "$code" | grep -q 'role_switch_failures() >= ROLE_SWITCH_MAX_ATTEMPTS' ||
	fail "handle_role_swap() no longer honours the supervisor's failure budget"
pass "the role switch still honours the failure budget"

pass "all 0x51 real-head-unit guards hold"
