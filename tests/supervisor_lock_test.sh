#!/bin/sh
# Guards the singleton lock against recycled PIDs.
#
# The lock is a directory holding the owner's pid. Deciding "is that pid still
# the supervisor?" with `kill -0` alone is wrong: after a reboot the same number
# is handed to whichever Android process gets there first. When that happened
# on this box, every supervisor start found pid 1012 "alive" -- it was
# com.yunpan.appmanage:acc -- took the `exit 0` branch, and init restarted the
# service every 5 s forever. Each copy died in 60 ms without writing one log
# line, so the box looked healthy while CarPlay never started at all.
#
# The rule that fixes it: a pid counts only if its command line still names
# carplay_supervisor.sh. supervisor_pid_is_live() is the single place that
# answers the question, and both the lock and the pid file go through it.
#
# Run with sh from the project root. Needs /proc and kill, so the device or a
# POSIX host -- not MSYS.
set -eu
supervisor=${1:-deploy/opi4/scripts/carplay_supervisor.sh}
fail() { echo "FAIL: $*" >&2; exit 1; }

case "$(uname -s 2>/dev/null || echo unknown)" in
    MSYS*|MINGW*|CYGWIN*)
        echo "SKIP: needs a POSIX host (/proc/<pid>/cmdline); run on the device"
        exit 0
        ;;
esac

[ -f "$supervisor" ] || fail "not found: $supervisor"

tmp=tests/build/supervisor-lock-sandbox
mkdir -p "$tmp"
# Cleanup must not decide the exit status; rm may be wrapped by a shim here.
trap 'kill $unrelated $named 2>/dev/null || true' EXIT INT TERM

# Pull the predicate out of the production script so the test exercises the
# shipped code, not a copy of it.
sed -n '/^supervisor_pid_is_live() {/,/^}/p' "$supervisor" > "$tmp/functions.sh"
[ -s "$tmp/functions.sh" ] || fail "supervisor_pid_is_live() not found in $supervisor"
grep -q 'carplay_supervisor.sh' "$tmp/functions.sh" ||
    fail "the predicate does not look at the command line"
# The predicate reads command lines through read_cmdline(), which also owns the
# zombie and timeout guards. Pull that helper in as well, otherwise the sandbox
# exercises the predicate without the protection being tested.
sed -n '/^read_cmdline() {/,/^}/p' "$supervisor" > "$tmp/cmdline.sh"
[ -s "$tmp/cmdline.sh" ] || fail "read_cmdline() not found in $supervisor"
. "$tmp/cmdline.sh"
. "$tmp/functions.sh"

expect() { # want, pid, label
    if supervisor_pid_is_live "$2"; then got=0; else got=1; fi
    [ "$got" = "$1" ] || fail "$3: expected $1, got $got (cmdline='${SUPERVISOR_PID_CMDLINE:-}')"
    echo "ok   $3"
}

echo "== an empty or malformed pid is not live"
expect 1 "" "empty string"
expect 1 "not-a-pid" "garbage"

echo "== this test process is not the supervisor"
expect 1 "$$" "self, cmdline is the test"

echo "== a pid that no longer exists is not live"
expect 1 999999 "dead pid"

echo "== a live but unrelated process is not live (the reboot regression)"
sleep 60 &
unrelated=$!
expect 1 "$unrelated" "unrelated process (pid $unrelated)"

echo "== a live process that really is the supervisor is live"
# The predicate's contract is "the command line names the supervisor script".
# A stub with that exact name is enough to exercise it, and unlike copying the
# production script it cannot accidentally start a real supervisor in the
# sandbox. The name is the whole signal, so nothing else is being faked.
printf '#!/bin/sh\nsleep 60\n' > "$tmp/carplay_supervisor.sh"
chmod +x "$tmp/carplay_supervisor.sh"
sh "$tmp/carplay_supervisor.sh" &
named=$!
sleep 1
expect 0 "$named" "supervisor named process (pid $named)"

echo "== a zombie pid is not live and must not block the check"
# A child that exited but has not been reaped yet is the state that wedged boot:
# reading its /proc/<pid>/cmdline never returned, so the supervisor sat in
# acquire_singleton and never claimed the gadget. The read has to be skipped
# outright for corpses and capped for everything else.
sh -c 'exit 0' &
zombie=$!
sleep 1
zombie_start=$(date +%s)
expect 1 "$zombie" "zombie process (pid $zombie)"
zombie_elapsed=$(( $(date +%s) - zombie_start ))
[ "$zombie_elapsed" -le 3 ] || fail "zombie check blocked for ${zombie_elapsed}s"
echo "ok   zombie check returned in ${zombie_elapsed}s"

echo "== both call sites use the same predicate"
n=$(grep -c 'supervisor_pid_is_live "' "$supervisor")
[ "$n" -ge 2 ] || fail "expected the predicate to be used at the lock and the pid file, found $n use(s)"
echo "ok   predicate used at both call sites ($n)"
if grep -q 'kill -0 "\$held_pid"' "$supervisor"; then
    fail "the lock still trusts a bare kill -0"
fi
echo "ok   no bare kill -0 on the lock holder"

echo "PASS: a recycled pid can no longer wedge the supervisor"
