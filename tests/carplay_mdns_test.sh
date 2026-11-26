#!/bin/sh
set -eu
cd "$(dirname "$0")/.."
out=build/compat-20261111
mkdir -p "$out"
cc=${CC:-cc}
flags="-std=c11 -Wall -Wextra -Werror -pthread"
if [ "${MDNS_TEST_ASAN:-0}" = 1 ]; then
    flags="$flags -fsanitize=address,undefined -fno-omit-frame-pointer"
fi
# Tests include the implementation to exercise its private parser without
# adding test-only entry points to the public Android API.
$cc $flags tests/carplay_mdns_test.c -o "$out/mdns-test"
"$out/mdns-test"
$cc $flags tests/carplay_mdns_runtime_test.c -o "$out/mdns-runtime-test"
if [ "$(id -u)" -ne 0 ]; then
    echo 'Runtime verification requires root for an isolated network namespace.' >&2
    exit 1
fi
unshare -n sh -c 'ip link set lo up; exec ./build/compat-20261111/mdns-runtime-test'
