#!/bin/sh
# Source guard for usbhost.c: orderings that fail silently.
#
# Both bugs this guards produce the same symptom -- "host mode says there is no
# head unit on the bus" -- with no error anywhere, because nothing is broken
# enough to report.  They are cheap to reintroduce during a refactor, so they
# are pinned here.
#
#   1. Endpoints are read from the setting the device is *already* in, and
#      USBDEVFS_SETINTERFACE is only a fallback when that read finds nothing.
#      Two opposite mistakes live here:
#        - reading before any alt handling looks empty, because sysfs only
#          exports the active setting and MFi allows setting 0 to carry no
#          endpoints at all;
#        - issuing SET_INTERFACE unconditionally, which on a gadget that was
#          configured milliseconds ago takes the peer off the bus with -71
#          (the head unit then never enumerates again during that ignition
#          cycle).  So the fallback must exist, and it must be reachable only
#          from the "read found nothing" arm -- with a second read after it.
#
#   2. The 05ac:12a8 filter must apply only while our own gadget is bound.
#      In host mode the gadget is unbound, so nothing on the bus is us -- and a
#      head unit may legitimately present the same IDs.
#
# Run with sh from the project root.
set -eu
source_file=${1:-src/usbhost.c}
[ -f "$source_file" ] || { echo "FAIL: missing $source_file" >&2; exit 1; }

fail() { echo "FAIL: $*" >&2; exit 1; }
pass() { echo "ok - $*"; }

# Comments carry both the phrase "collect_endpoints" and the rationale for the
# ordering, so strip them before matching or every check passes trivially.
code=$(sed 's://.*::' "$source_file" | sed '/^[[:space:]]*\*/d; /^[[:space:]]*\/\*/d')

line_of() { printf '%s\n' "$code" | grep -n "$1" | head -1 | cut -d: -f1; }

# ---- 1. read first, SET_INTERFACE only as a fallback ---------------------
alt_line=$(line_of 'select_bulk_alt(fd,')
eps_first=$(printf '%s\n' "$code" | grep -n 'collect_endpoints(' | head -1 | cut -d: -f1)
eps_last=$(printf '%s\n' "$code" | grep -n 'collect_endpoints(' | tail -1 | cut -d: -f1)
eps_count=$(printf '%s\n' "$code" | grep -c 'collect_endpoints(')

[ -n "$alt_line" ] || fail "select_bulk_alt() is no longer called; an interface idling on setting 0 will look empty"
[ -n "$eps_first" ] || fail "collect_endpoints() call not found in usbhost_open; did the probe get rewritten"

[ "$eps_first" -lt "$alt_line" ] ||
	fail "select_bulk_alt() runs before the first collect_endpoints(); an unconditional SET_INTERFACE is the -71 that takes the peer off the bus"

[ "$eps_count" -ge 2 ] ||
	fail "collect_endpoints() is only called once; after the fallback sets the alt the endpoints must be read again"
[ "$alt_line" -lt "$eps_last" ] ||
	fail "the alt setting is selected but the endpoints are never read again (alt line $alt_line, last read $eps_last)"

# The fallback must sit inside the "the read found nothing" arm, not beside it.
falls_back=0
printf '%s\n' "$code" | grep -A6 'collect_endpoints(' | grep -q 'select_bulk_alt(fd,' &&
	falls_back=1
[ "$falls_back" -eq 1 ] ||
	fail "select_bulk_alt() is not reachable from the failed-collect branch; it is unconditional again"

alt_def=$(line_of 'static void select_bulk_alt')
[ -n "$alt_def" ] || fail "select_bulk_alt() is not defined"
printf '%s\n' "$code" | grep -q 'USBDEVFS_SETINTERFACE' ||
	fail "select_bulk_alt() no longer issues USBDEVFS_SETINTERFACE"
pass "endpoints are read before any SET_INTERFACE, and the switch is a fallback"


# ---- 2. the VID/PID filter is conditional --------------------------------
printf '%s\n' "$code" | grep -q 'gadget_is_bound() && vid == exclude_vid' ||
	fail "the 05ac:12a8 filter is unconditional again; in host mode it would skip the head unit"
printf '%s\n' "$code" | grep -q 'static int gadget_is_bound' ||
	fail "gadget_is_bound() is missing"
pass "the self-filter only applies while our own gadget is bound"

# ---- 3. no leftover fixed-latency sleep on the enumeration path ----------
# The head unit gives us 1 s before it gives up, so a hard-coded half-second
# poll would burn most of the budget by itself.
printf '%s\n' "$code" | grep -q 'usleep(500000)' &&
	fail "usbhost_open() polling is back to a 500 ms sleep; that is half the 1 s budget"
pass "no half-second poll on the enumeration path"

echo "usbhost order guard: all checks passed"
