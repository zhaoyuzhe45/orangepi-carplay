#!/bin/sh
# Guards the endpoint reader in iap2d.
#
# f_fs epfiles implement no ->poll(), so poll() reports every endpoint ready
# whether or not data is queued, and a synchronous read() always blocks until a
# transfer completes (f_fs.c: ffs_epfile_io -> wait_for_completion_interruptible)
# even with O_NONBLOCK set.  That is why the reader must not poll and must own
# exactly one blocking read().
#
# The 2026-10-26 field failure was a single shared reader over two OUT
# endpoints: the head unit's 20-byte USBMUX VERSION frame woke it, the reply
# went out, it then moved on to the native pair's OUT endpoint, blocked there,
# and the 17-byte SETUP frame that followed 2 ms later was never collected.
#
# The fix at the time was one thread per OUT endpoint.  That is no longer
# possible: bare iAP2 and USBMUX now share the single ff/fe/02 bulk pair, so
# there is exactly one OUT endpoint and the split moved into
# dispatch_out_frame(), keyed on the frame header (0x40 0x40 vs 0xFE).
#
# Run with sh from the project root.
set -eu
src=${1:-src/iap2d.c}
fail() { echo "FAIL: $*" >&2; exit 1; }

[ -f "$src" ] || fail "source not found: $src"

# Print one function body, opened by its exact signature line and closed by the
# first column-0 brace. Prose elsewhere in the file cannot satisfy an assertion.
body() {
	awk -v name="$1" '
		index($0, name "(") > 0 && /^(static )?(void|int|size_t|ssize_t)/ {
			inside = 1
		}
		inside { print }
		inside && /^}/ { exit }
	' "$src" | tr -d '\r'
}

count() { printf '%s\n' "$2" | grep -c -- "$1" || true; }

echo "== the reader owns one blocking read and never polls =="
reader=$(body "endpoint_reader_main")
[ -n "$reader" ] || fail "endpoint_reader_main not found"
[ "$(count 'poll(' "$reader")" = 0 ] ||
	fail "endpoint_reader_main must not poll(): f_fs epfiles always report ready"
[ "$(count 'read(fd' "$reader")" = 1 ] ||
	fail "endpoint_reader_main should own exactly one read() call"
[ "$(count 'dispatch_out_frame' "$reader")" = 1 ] ||
	fail "endpoint_reader_main must hand frames to dispatch_out_frame()"

echo "== exactly one reader thread, on the one shared OUT endpoint =="
creates=$(grep -c 'pthread_create(&g_reader, NULL, endpoint_reader_main' "$src" || true)
[ "$creates" = 1 ] || fail "expected one reader thread, found $creates"
if grep -q 'g_native_reader' "$src"; then
	fail "the second (native) reader is back; ff/fe/02 has a single OUT endpoint"
fi

echo "== the shared pair is split by frame header, not by endpoint =="
dispatch=$(body "dispatch_out_frame")
[ -n "$dispatch" ] || fail "dispatch_out_frame not found"
printf '%s\n' "$dispatch" | grep -q 'frame_is_iap2(buf, n)' ||
	fail "dispatch_out_frame must classify the frame, not the endpoint"
printf '%s\n' "$dispatch" | grep -q 'usbmux_feed(g_mux' ||
	fail "USBMUX frames must still go to usbmux_feed()"
printf '%s\n' "$dispatch" | grep -q 'iap2_link_feed(g_link' ||
	fail "iAP2 frames must still go to iap2_link_feed()"

classifier=$(body "frame_is_iap2")
[ -n "$classifier" ] || fail "frame_is_iap2 not found"
printf '%s\n' "$classifier" | grep -q 'IAP2_FRAME_MAGIC' ||
	fail "the classifier must look at the iAP2 sync word"
printf '%s\n' "$classifier" | grep -q 'USBMUX_FRAME_MAGIC' ||
	fail "the classifier must look at the USBMUX magic"
printf '%s\n' "$classifier" | grep -q 'g_native_active' ||
	fail "a split frame must follow the transport that is already active"

echo "== the iAP2 link answers on the same IN endpoint USBMUX uses =="
grep -q 'g_native_in_fd = in_fd;' "$src" ||
	fail "the native link must transmit on the shared ff/fe/02 IN endpoint"
grep -q 'txq_set_fd(g_native_in_fd);' "$src" ||
	fail "the native link must redirect the tx queue when it takes the round"

echo "== the single reader is woken and joined on the way down =="
stop=$(body "stop_runtime")
[ -n "$stop" ] || fail "stop_runtime not found"
printf '%s\n' "$stop" | grep -q 'pthread_kill(g_reader, SIGUSR1)' ||
	fail "stop_runtime must interrupt the reader before joining it"
printf '%s\n' "$stop" | grep -q 'pthread_join(g_reader' ||
	fail "stop_runtime must join the reader"
teardown=$(grep -n 'link torn down' "$src" || true)
[ -n "$teardown" ] || fail "round teardown block not found"
tail_block=$(sed -n "$(printf '%s' "$teardown" | cut -d: -f1),+30p" "$src" | tr -d '\r')
printf '%s\n' "$tail_block" | grep -q 'pthread_kill(g_reader, SIGUSR1)' ||
	fail "round teardown must wake the reader before closing the endpoint files"

echo "== the reader and the timer loop share one condition, so it must broadcast =="
grep -q 'pthread_cond_signal(&g_link_cv);' "$src" &&
	fail "a single signal can strand the reader; use broadcast"
grep -q 'pthread_cond_broadcast(&g_link_cv);' "$src" ||
	fail "missing broadcast that releases the reader at round start"

echo "endpoint reader test passed"
