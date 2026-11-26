#!/bin/sh
# Build and run the H.264 picture-boundary unit test on the host.
#
# The packetizer is the difference between "one AirPlay packet per picture" and
# "one per slice", and only the second one produces the rolling mosaic the user
# sees in the app grid. airplay_h264.c is pure C over a byte buffer, so the test
# needs no device: compiling it here keeps the check cheap enough to run on every
# change.
set -u

here=$(cd "$(dirname "$0")" && pwd)
source_file=$here/h264_slice_boundary_test.c
[ -f "$source_file" ] || { echo "FAIL: missing $source_file" >&2; exit 1; }

cc=${CC:-}
if [ -z "$cc" ]; then
    for candidate in gcc clang cc; do
        if command -v "$candidate" >/dev/null 2>&1; then
            cc=$candidate
            break
        fi
    done
fi
[ -n "$cc" ] || {
    echo "SKIP: no host C compiler (gcc/clang/cc) available"
    exit 0
}

out=$(mktemp "${TMPDIR:-/tmp}/h264_slice_boundary.XXXXXX") || {
    echo "FAIL: cannot create a scratch file" >&2
    exit 1
}
trap 'rm -f "$out"' EXIT HUP INT TERM

"$cc" -std=c11 -Wall -Wextra -Werror -O1 -o "$out" "$source_file" || {
    echo "FAIL: host build of the H.264 boundary test failed" >&2
    exit 1
}
"$out"
