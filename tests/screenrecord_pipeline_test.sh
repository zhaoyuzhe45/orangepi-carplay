#!/bin/sh
# Historical filename retained; behavioral AU coverage is mediacodec_pipeline_test.py.
set -eu
src=${1:-src/airplay_client.c}
fail() { echo "FAIL: $*" >&2; exit 1; }
grep -q 'carplay_encoder_request_keyframe' "$src" || fail 'keyframe command missing'
grep -q 'consume_encoder_packet' "$src" || fail 'access-unit consumer missing'
grep -q 'ENCODER_RESTART_MIN_MS' "$src" || fail 'restart throttle missing'
grep -q 'has_real_frame' "$src" || fail 'still frame protection missing'
grep -q 'encoder.err' src/carplay_encoder.c || fail 'encoder stderr log missing'
if grep -q 'execl.*screenrecord\|time-limit\|PARSER_IDLE_FLUSH_MS' "$src"; then
    fail 'legacy timed screenrecord or partial-NAL idle flush remains'
fi
echo 'MediaCodec capture guards: ok'
