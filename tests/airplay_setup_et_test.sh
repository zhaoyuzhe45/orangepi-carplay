#!/bin/sh
# Guard for the `et` field in the AirPlay SETUP plist.
#
# AirPlay's encryption type is an enum with two interesting values:
#
#   kAirPlayEncryptionType_Undefined = 0
#   kAirPlayEncryptionType_None      = 1
#
# The key is a statement about the streams that follow, so it is tied to the
# session state and both mistakes are silent until the picture fails:
#
#   unpaired session -> `et` MUST be present and MUST be the integer 1.
#     An absent key reads back as Undefined, the receiver goes looking for a
#     FairPlay key we never send and answers 466 Key Management Error -- which
#     on the wire looks exactly like "this head unit requires pairing".
#
#   paired session (pair-setup + pair-verify done) -> `et` MUST be absent.
#     The frames are ChaCha20-Poly1305 keyed from the pair-verify shared
#     secret, so advertising et = 1 tells the receiver to expect plaintext and
#     it fails to open every frame. catplay sets req.et = None in that case,
#     which is why a real iPhone never carries the key here.
#
# This box runs the paired path, and the F-PACE head unit accepted RECORD with
# no `et` in the plist (2026-10-26 sessions), so a hard-coded et = 1 is a
# regression, not a fix. The encoding side is also covered by
# tests/airplay_protocol_test.c::test_bplist_setup_encryption_type().
#
# Run with sh from the project root.
set -eu
source_file=${1:-src/airplay_bplist.c}
[ -f "$source_file" ] || { echo "FAIL: missing $source_file" >&2; exit 1; }

fail() { echo "FAIL: $*" >&2; exit 1; }
pass() { echo "ok - $*"; }

code=$(sed 's://.*::' "$source_file" | sed '/^[[:space:]]*\*/d; /^[[:space:]]*\/\*/d')

fn=$(printf '%s\n' "$code" | grep -n 'int airplay_bplist_encode_setup' | head -1 | cut -d: -f1)
[ -n "$fn" ] || fail "airplay_bplist_encode_setup() is gone"
body=$(printf '%s\n' "$code" | tail -n +"$fn" | head -100)

printf '%s\n' "$body" | grep -q '"timingPort"' ||
	fail "timingPort vanished -- this guard is reading the wrong plist function"

# The encoder must take the session state, otherwise it cannot be conditional
# at all and someone will hard-code one of the two values again.
printf '%s\n' "$body" | grep -q 'int encrypted' ||
	fail "airplay_bplist_encode_setup() no longer takes the session state; et cannot be conditional"

et_line=$(printf '%s\n' "$body" | grep -n '"et"' | head -1 | cut -d: -f1)
[ -n "$et_line" ] || fail "the unpaired branch lost its 'et'; that path now answers 466"

# The `et` entry must sit inside the `if (!encrypted)` block: find the guard and
# require that the entry comes after it and before the next closing brace at the
# same indent level. A simpler and stricter check: the immediately preceding
# non-blank line of code must belong to that branch.
guard_line=$(printf '%s\n' "$body" | grep -n 'if (!encrypted)' | head -1 | cut -d: -f1)
[ -n "$guard_line" ] ||
	fail "there is no 'if (!encrypted)' branch; et is unconditional, so the paired path sends et = 1"
[ "$et_line" -gt "$guard_line" ] ||
	fail "et is emitted before the encrypted check, i.e. on the paired path too"

val=$(printf '%s\n' "$body" | sed -n "${et_line}p")
printf '%s' "$val" | grep -q 'add_integer(&builder, 1)' ||
	fail "et is not the integer 1 (kAirPlayEncryptionType_None); got: $val"
printf '%s' "$val" | grep -q 'add_string' &&
	fail "et is a string; the receiver compares it as an integer"
printf '%s' "$val" | grep -q 'add_boolean' &&
	fail "et is a boolean; the receiver compares it as an integer"

# Production selects encrypted HAP or plaintext legacy for the negotiated profile.
caller=${2:-src/airplay_client.c}
[ -f "$caller" ] || fail "missing $caller"
# Read the complete condition so formatting the arguments on multiple lines
# does not hide the final encrypted argument from this source guard.
caller_call=$(awk '
    /if[[:space:]]*\(airplay_bplist_encode_setup/ { capture = 1 }
    capture { printf "%s ", $0; if (index($0, "{")) exit }
' "$caller")
printf '%s\n' "$caller_call" | grep -Eq ',[[:space:]]*client->profile[[:space:]]*==[[:space:]]*AIRPLAY_PROFILE_HAP[[:space:]]*\)[[:space:]]*<[[:space:]]*0[[:space:]]*\)' ||
	fail "send_setup() must select encrypted SETUP exactly for the HAP profile: $caller_call"

pass "unpaired SETUP advertises et = 1 (no encryption)"
pass "paired SETUP omits et, matching the ChaCha20 stream cipher"
pass "all AirPlay SETUP guards hold"
