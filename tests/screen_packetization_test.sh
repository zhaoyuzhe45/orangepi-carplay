#!/bin/sh
# Guard: the screen packetizer must send one AirPlay packet per picture.
#
# Observed symptom: rolling a long list on the phone produced a partial mosaic
# that healed by itself a few seconds later. Two independent defects in the
# packetizer produce exactly that, both of them by handing the sink a picture it
# cannot fully reconstruct:
#
#   1. One packet per *slice*. The Android encoder codes most pictures as a
#      single slice but switches to several for expensive ones - scrolling a full
#      app grid is the classic case - so a picture was split across packets and
#      the sink decoded only the part it had.
#   2. Re-sending a *stale* keyframe. The 1 fps "still picture" refresh could fire
#      long after newer pictures had gone out, rewinding the decoder to a
#      reference the following deltas do not build on.
#
# Both only clear at the encoder's next IDR, hence "recovers after a few seconds".
set -u

here=$(cd "$(dirname "$0")" && pwd)
root=$(cd "$here/.." && pwd)
src=$root/src/airplay_client.c
stripped=

failures=0
fail() {
    echo "FAIL: $1"
    failures=$((failures + 1))
}

cleanup() {
    [ -n "$stripped" ] && rm -f "$stripped"
}
trap cleanup EXIT HUP INT TERM

[ -f "$src" ] || {
    echo "FAIL: cannot read $src"
    exit 1
}
stripped=$(mktemp "${TMPDIR:-/tmp}/airplay_client.XXXXXX") || {
    echo "FAIL: cannot create a scratch file"
    exit 1
}

# Drop comments so prose about a forbidden pattern cannot satisfy an assertion.
awk '
    {
        line=$0
        while (length(line)) {
            if (comment) {
                end=index(line,"*/")
                if (!end) break
                line=substr(line,end+2); comment=0
            } else {
                begin=index(line,"/*")
                if (!begin) { sub(/\/\/.*/,"",line); printf "%s",line; break }
                printf "%s",substr(line,1,begin-1)
                line=substr(line,begin+2); comment=1
            }
        }
        print ""
    }
' "$src" >"$stripped"

body() {
    awk -v sig="$1" '
        index($0, sig) == 1 { seeking = 1; next }
        seeking && /;/ { seeking = 0; next }
        seeking && /\{/ { inside = 1; seeking = 0; next }
        inside && /^\}/     { inside = 0 }
        inside              { print }
    ' "$stripped"
}

frame_body=$(body 'static int send_h264_frame')
[ -n "$frame_body" ] || fail "send_h264_frame is missing"

# 1. Picture boundaries come from first_mb_in_slice, not from the NAL type.
echo "$frame_body" | grep -q 'airplay_h264_slice_starts_picture' ||
    fail "send_h264_frame must detect picture boundaries via airplay_h264_slice_starts_picture"
echo "$frame_body" | grep -q 'picture_start && client->video_batch_len' ||
    fail "send_h264_frame must flush the previous picture when a new one starts"

# 2. SPS/PPS travel once in the separate cleartext VideoConfig packet. The
#    encrypted access unit carries picture data only.
echo "$frame_body" | grep -q 'parser->sps_len' &&
    fail "send_h264_frame must not duplicate SPS/PPS inside the encrypted IDR"
keyframe_flag_line=$(echo "$frame_body" | grep -n 'video_batch_keyframe =' | head -1 | cut -d: -f1)
[ -n "$keyframe_flag_line" ] ||
    fail "send_h264_frame must record whether the buffered picture is a keyframe"

# 3. The synthetic IDR used while MediaCodec starts must also carry the
#    keyframe flag; otherwise the receiver may retain its decoder state.
placeholder_body=$(body 'static int send_placeholder_frame')
[ -n "$placeholder_body" ] || fail "send_placeholder_frame is missing"
echo "$placeholder_body" | grep -q 'sender_ntp, 1' ||
    fail "send_placeholder_frame must mark its IDR fixture as a keyframe"

# 4. The keyframe snapshot must be taken when the picture is complete, i.e. at
#    flush time. Snapshotting on arrival keeps only the first slice.
echo "$frame_body" | grep -q 'save_last_keyframe' &&
    fail "send_h264_frame must not snapshot the keyframe before the picture is complete"
flush_body=$(body 'static int flush_video_batch')
[ -n "$flush_body" ] || fail "flush_video_batch is missing"
echo "$flush_body" | grep -q 'save_last_keyframe' ||
    fail "flush_video_batch must snapshot the keyframe once the whole picture is buffered"

# 5. A keyframe may only be re-sent while it is still the newest picture.
resend_body=$(body 'static int resend_last_keyframe')
[ -n "$resend_body" ] || fail "resend_last_keyframe is missing"
echo "$resend_body" | grep -q 'last_keyframe_current' ||
    fail "resend_last_keyframe must refuse to rewind the decoder to a stale keyframe"
echo "$resend_body" | grep -q 'client->last_keyframe_nals, 1' ||
    fail "resend_last_keyframe must mark the resent IDR as a keyframe"

# 6. The MediaCodec packet is a complete access unit: release its final NAL
#    and complete picture at that boundary. Behavior is covered separately by
#    mediacodec_pipeline_test.py with a multi-slice access unit.
unit_body=$(body 'static int consume_encoder_packet')
[ -n "$unit_body" ] || fail "consume_encoder_packet is missing"
echo "$unit_body" | grep -q 'airplay_h264_flush' ||
    fail "access-unit boundary must release the final NAL"
echo "$unit_body" | grep -q 'flush_video_batch' ||
    fail "access-unit boundary must release the complete picture"

# 7. Encode the logical canvas directly. Some CarPlay receivers accept an SPS
# crop on a larger coded frame but use the coded frame's row stride, producing
# regular horizontal stripes. The physical capture dimensions remain available
# for touch mapping, but they must not choose the encoder surface size.
size_body=$(body 'static void screen_output_size')
[ -n "$size_body" ] || fail "screen_output_size is missing"
echo "$size_body" | grep -q 'client->output_width' ||
    fail "screen_output_size must choose the receiver-negotiated canvas"
echo "$size_body" | grep -q 'client->output_height' ||
    fail "screen_output_size must retain the negotiated output height"
echo "$size_body" | grep -q 'client->capture_width' &&
    fail "the logical canvas encoder must not be forced to the physical capture width"
echo "$size_body" | grep -q 'carplay_capabilities_capture_size' &&
    fail "the encoder must not aspect-fit a separate capture canvas"

# The encoder scales the entire physical preset into the negotiated canvas.
# The H.264 parser removes allocation padding only; it validates SPS bounds.
encoder_body=$(body 'static void *encoder_main')
[ -n "$encoder_body" ] || fail "encoder_main is missing"
echo "$encoder_body" | grep -q 'airplay_h264_parser_set_stream_crop(&parser, client->output_width' ||
    fail "encoder_main must crop codec padding to the negotiated output width"
echo "$encoder_body" | grep -q 'client->output_height' ||
    fail "encoder_main must crop codec padding to the negotiated output height"
echo "$encoder_body" | grep -q 'airplay_h264_parser_set_stream_crop_rect' &&
    fail "encoder_main must not crop an old mirror viewport out of the preset"

if [ "$failures" -ne 0 ]; then
    echo "screen packetization: $failures failure(s)"
    exit 1
fi
echo "screen packetization: ok"
