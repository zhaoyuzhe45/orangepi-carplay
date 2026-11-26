#!/bin/sh
# Build and run the H.264 SPS frame-cropping rewrite test on the host.
#
# Panel-mirror capture codes the padded macroblock frame and relies on the SPS
# frame_cropping fields to present the canvas-aspect region to the sink. A
# wrong rewrite shows up as black bars or a squashed picture on the head unit,
# so the rewrite is pinned here: field-preserving, emulation-prevention safe,
# and a no-op when the display size already matches.
set -u

here=$(cd "$(dirname "$0")" && pwd)
source_file=$here/h264_sps_crop_test.c
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

out=$(mktemp "${TMPDIR:-/tmp}/h264_sps_crop.XXXXXX") || {
    echo "FAIL: cannot create a scratch file" >&2
    exit 1
}
trap 'rm -f "$out"' EXIT HUP INT TERM

"$cc" -std=c11 -Wall -Wextra -Werror -O1 -o "$out" "$source_file" || {
    echo "FAIL: host build of the SPS crop test failed" >&2
    exit 1
}
"$out"
