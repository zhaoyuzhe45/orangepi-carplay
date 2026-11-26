#!/bin/sh
# Run with sh from the project root.
#
# The kernel names the NCM netdev, not us: f_ncm.c's "ifname" is a read-only
# attribute and gether_setup_default() fixes the name to "usb%d" when the
# function directory is created. Writing to it fails silently, and a daemon
# that looks for a name it chose itself waits five seconds and then configures
# nothing -- which is exactly how NCM data never flowed.
#
# This pins both halves of the fix: the daemon reads configfs instead of
# trusting a constant, and the supervisor stopped pretending to pin the name.
set -eu

case "$(uname -s 2>/dev/null || echo unknown)" in
    MINGW*|MSYS*|CYGWIN*)
        echo "SKIP: needs a POSIX filesystem; run this on the device or in WSL"
        exit 0
        ;;
esac

src=src/ncm_bridge.c
hdr=src/ncm_bridge.h
sup=deploy/opi4/scripts/carplay_supervisor.sh

fail() { echo "FAIL: $*" >&2; exit 1; }
pass() { echo "ok: $*"; }

[ -f "$src" ] || fail "missing $src"
[ -f "$hdr" ] || fail "missing $hdr"
[ -f "$sup" ] || fail "missing $sup"

echo "== static: the daemon reads the name from configfs"

grep -qF 'NCM_KERNEL_IFNAME_ATTR' "$src" ||
    fail "no attribute path defined; the kernel's own value is unreachable"
pass "attribute path is defined"

grep -qF 'fopen(NCM_KERNEL_IFNAME_ATTR' "$src" ||
    fail "configfs attribute is never opened"
pass "configfs attribute is opened"

grep -qF 'kernel_ifname_from_configfs(out, len) == 0' "$src" ||
    fail "configfs is not consulted first; a fallback must not win"
pass "configfs is tried before any fallback"

grep -qF 'if_nametoindex(g_kernel_ifname)' "$src" &&
    fail "a lookup still uses the compiled-in name; the constant must not be authoritative"
pass "no lookup trusts the compiled-in name"

grep -qF 'ncm_bridge_netdev_prepare();' "$src" ||
    fail "ncm_bridge_open does not configure the netdev at startup"
pass "startup configures the netdev without waiting for BOUND"

grep -qF 'int ncm_bridge_netdev_prepare(void);' "$hdr" ||
    fail "prepare is not exported"
grep -qF 'int ncm_bridge_kernel_ifname(char *out, size_t len);' "$hdr" ||
    fail "the name accessor is not exported"
pass "both entry points are declared"

echo
echo "== static: the supervisor no longer writes ifname"

grep -qF '> "$NCM_FUNC/ifname"' "$sup" &&
    fail "supervisor still writes the read-only ifname attribute"
pass "no write to the read-only attribute"

grep -qF 'ifname is read-only' "$sup" ||
    fail "nothing explains why the name is not ours to choose"
pass "the reason is recorded next to the code"

echo
echo "== dynamic: setup_kernel_ncm creates the function and leaves the name alone"

tmp=$(mktemp -d "${TMPDIR:-/tmp}/carplay-ncm-name.XXXXXX")
trap 'rm -rf "$tmp"' EXIT HUP INT TERM
sed -n '/^setup_kernel_ncm() {/,/^}/p' "$sup" > "$tmp/fn.sh"
[ -s "$tmp/fn.sh" ] || fail "could not extract setup_kernel_ncm"

G=$tmp/g1
NCM_FUNC=$G/functions/ncm.usb0
# setup_kernel_ncm pins both NCM addresses to the Apple OUI; those constants live
# at the top of the supervisor, which this harness does not source, so copy them
# across.  Without this `set -u` aborts the dynamic half of the test.
NCM_DEV_ADDR=$(sed -n 's/^NCM_DEV_ADDR=//p' "$sup" | head -1)
NCM_HOST_ADDR=$(sed -n 's/^NCM_HOST_ADDR=//p' "$sup" | head -1)
[ -n "$NCM_DEV_ADDR" ] && [ -n "$NCM_HOST_ADDR" ] ||
    fail "could not read the NCM addresses out of the supervisor"
mkdir -p "$G/functions"
LOG=$tmp/log
: > "$LOG"
log_event() { printf '%s\n' "$*" >> "$LOG"; }
. "$tmp/fn.sh"

setup_kernel_ncm
[ -d "$NCM_FUNC" ] || fail "ncm.usb0 was not created"
[ ! -e "$NCM_FUNC/ifname" ] ||
    fail "setup_kernel_ncm created an ifname file; it is writing again"
pass "function created, ifname untouched"

[ "$(cat "$NCM_FUNC/dev_addr" 2>/dev/null)" = "$NCM_DEV_ADDR" ] ||
    fail "dev_addr was not pinned to the Apple OUI"
pass "dev_addr pinned to $NCM_DEV_ADDR"

grep -q 'named by the kernel' "$LOG" ||
    fail "the log does not say who owns the name"
pass "log line states the kernel named it"

: > "$LOG"
setup_kernel_ncm
[ ! -s "$LOG" ] || fail "second call is not a no-op; it would spam every restart"
pass "second call is a no-op"

echo
echo "all assertions passed"
