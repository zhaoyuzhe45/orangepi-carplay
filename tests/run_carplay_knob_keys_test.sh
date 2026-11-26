#!/bin/sh
set -eu
root=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
output="$root/build/physical-presets-20261113"
mkdir -p "$output"
cc=${CC:-cc}
"$cc" -std=c11 -Wall -Wextra -Werror -I"$root/src" \
  "$root/tests/carplay_knob_test.c" "$root/src/carplay_knob.c" -o "$output/knob-test"
"$output/knob-test"
"$cc" -std=c11 -Wall -Wextra -Werror -I"$root/src" \
  "$root/tests/carplay_keys_test.c" "$root/src/carplay_keys.c" -pthread -o "$output/keys-test"
"$output/keys-test"
