#!/bin/sh
# Guard: a refused handshake step must end the session, not stall in the poll loop.
#
# The RTSP ladder is a chain -- pair-setup -> pair-verify -> auth-setup -> SETUP
# -> stream SETUP -> RECORD -- and each request is only queued from the success
# arm of the previous one.  When a step comes back non-200 nothing else is ever
# queued, so before this guard the client kept a live socket, polled for 200 ms
# at a time and never wrote another byte.  In the log that is a bare
#
#     airplay: RTSP cseq=2 failed status=470 body=0
#
# followed by silence, which reads as "the receiver went quiet" when the
# receiver had in fact answered immediately.  Two things are required instead:
#
#   1. the step is named (rtsp_step_name) so the message says which rung of the
#      ladder was refused, and the client remembers the failure;
#   2. the caller notices airplay_client_failed() and tears the session down.
#
# Non-200 on a post-handshake request (feedback, keepalive) stays non-fatal:
# those answers are not part of the ladder and the session is already live.
#
# Run with sh from the project root.  Optional positional arguments override
# the three sources, which is how the mutations below are checked:
#   sh tests/airplay_fail_fast_test.sh <client.c> <bridge.c> <client.h>
set -eu
client=${1:-src/airplay_client.c}
bridge=${2:-src/ncm_bridge.c}
header=${3:-src/airplay_client.h}
for f in "$client" "$bridge" "$header"; do
	[ -f "$f" ] || { echo "FAIL: missing $f" >&2; exit 1; }
done

fail() { echo "FAIL: $*" >&2; exit 1; }
pass() { echo "ok - $*"; }

strip() { sed 's://.*::' "$1" | sed '/^[[:space:]]*\*/d; /^[[:space:]]*\/\*/d'; }

# ---- 1. the refusal is recorded, and only for handshake steps --------------
code=$(strip "$client")
start=$(printf '%s\n' "$code" | grep -n '^static int process_response' | head -1 | cut -d: -f1)
[ -n "$start" ] || fail "process_response() is gone"
body=$(printf '%s\n' "$code" | tail -n +"$start" | sed -n '1,/^}/p')

printf '%s\n' "$body" | grep -q 'client->failed = 1' ||
	fail "a refused handshake step no longer marks the client failed; the session will stall"

printf '%s\n' "$body" | grep -q 'cseq >= 1 && cseq <= 10' ||
	fail "the failure is no longer limited to the handshake ladder (cseq 1..10); a stray feedback status would kill a live session"

printf '%s\n' "$body" | grep -q 'rtsp_step_name(cseq)' ||
	fail "the aborted session does not name the step that was refused"

names=$(strip "$client" | grep -n '^static const char \*rtsp_step_name' | head -1 | cut -d: -f1)
[ -n "$names" ] || fail "rtsp_step_name() is gone"
named=$(strip "$client" | tail -n +"$names" | sed -n '1,/^}/p')
for step in 'pair-setup' 'pair-verify' 'SETUP' 'RECORD' 'GET /info'; do
	printf '%s\n' "$named" | grep -q "$step" ||
		fail "rtsp_step_name() has no entry for $step"
done

# ---- 2. the accessors exist ------------------------------------------------
grep -q 'int airplay_client_failed(const struct airplay_client \*client);' "$header" ||
	fail "airplay_client_failed() is not declared"
grep -q 'const char \*airplay_client_failure(const struct airplay_client \*client);' "$header" ||
	fail "airplay_client_failure() is not declared"

# ---- 3. the connect loop acts on it ---------------------------------------
bcode=$(strip "$bridge")
loop=$(printf '%s\n' "$bcode" | grep -nF 'while (!connect_cancelled()' | head -1 | cut -d: -f1)
[ -n "$loop" ] || fail "the AirPlay connect loop is gone"
loop_body=$(printf '%s\n' "$bcode" | tail -n +"$loop" | head -40)

printf '%s\n' "$loop_body" | grep -q 'airplay_client_failed(airplay)' ||
	fail "the connect loop never checks airplay_client_failed(); a refused session still polls forever"

printf '%s\n' "$loop_body" | grep -q 'airplay_client_failure(airplay)' ||
	fail "the connect loop tears down without logging which step was refused"

# The check must be at the top of the loop, before the poll() -- placing it
# after the poll would delay the teardown by one timeout and hide nothing.
check=$(printf '%s\n' "$loop_body" | grep -n 'airplay_client_failed(airplay)' | head -1 | cut -d: -f1)
poll=$(printf '%s\n' "$loop_body" | grep -n 'struct pollfd pfd\[3\]' | head -1 | cut -d: -f1)
[ -n "$poll" ] || fail "the poll set moved; this guard needs updating"
[ "$check" -lt "$poll" ] ||
	fail "airplay_client_failed() is checked after the poll set was built; move it to the top of the loop"

pass "a refused handshake marks the client failed and names the step"
pass "the connect loop tears the session down instead of polling an idle socket"
pass "all AirPlay fail-fast guards hold"
