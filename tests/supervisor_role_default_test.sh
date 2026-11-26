#!/bin/sh
# ensure_default_role() has to pin the Type-C port back to peripheral at start,
# because dr_mode = "otg" alone leaves the role undecided and every consumer of
# this port that is not CarPlay expects a device.
#
# The four cases below are the ones that actually happen in the field: a stale
# "host" left behind by a role switch, the normal "already peripheral" start, a
# kernel without the otg patch (file absent), and a read-only file.
#
# Run with sh from the project root.
set -eu
sup=${1:-deploy/opi4/scripts/carplay_supervisor.sh}
tmp=$(mktemp -d "${TMPDIR:-/tmp}/supervisor-role-default.XXXXXX")
trap 'rm -rf "$tmp"' EXIT HUP INT TERM

fail() { echo "FAIL: $*" >&2; exit 1; }
pass() { echo "ok - $*"; }

sed -n '/^ensure_default_role() {/,/^}/p' "$sup" > "$tmp/fn.sh"
[ -s "$tmp/fn.sh" ] || fail "could not extract ensure_default_role"

LOG=$tmp/log
: > "$LOG"
log_event() { printf '%s\n' "$*" >> "$LOG"; }
. "$tmp/fn.sh"

# 1. a leftover host role is taken back
DWC3_MODE=$tmp/mode
echo host > "$DWC3_MODE"
ensure_default_role || fail "ensure_default_role returned non-zero"
[ "$(cat "$DWC3_MODE")" = peripheral ] || fail "role stayed at $(cat "$DWC3_MODE")"
grep -q 'pinned to peripheral' "$LOG" || fail "no log line for the pin"
pass "takes the port back from host"

# 2. already peripheral: nothing written, nothing logged
: > "$LOG"
ensure_default_role
[ ! -s "$LOG" ] || fail "logged on the already-correct path: $(cat "$LOG")"
pass "silent when already peripheral"

# 3. kernel without the otg patch: the file does not exist
: > "$LOG"
DWC3_MODE=$tmp/absent
ensure_default_role || fail "missing file must not be fatal"
pass "survives a kernel without dwc3_mode"

# 4. unwritable target: report, but do not fail the supervisor. A directory is
#    used instead of chmod because these tests run as root, where 0444 is still
#    writable and the misbehaving path would never be reached.
: > "$LOG"
DWC3_MODE=$tmp/not-a-file
mkdir -p "$DWC3_MODE"
ensure_default_role || fail "unwritable target must not be fatal"
grep -q 'could not pin the dwc3 role' "$LOG" ||
	fail "an unwritable target is not reported"
pass "reports an unwritable target without failing"

echo "all role default checks passed"
