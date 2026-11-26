#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/.."
build_dir="${SHARED_MIC_BUILD_DIR:-build/shared-mic-20261119}"
cc="${CC:-cc}"
common=(-std=gnu11 -Wall -Wextra -Werror -I"$build_dir/include")
"$cc" "${common[@]}" -fPIC -shared -DFAKE_HAL tests/shared_microphone_hal_test.c -o "$build_dir/fake_submix.so" -pthread
"$cc" "${common[@]}" tests/shared_microphone_hal_test.c -o "$build_dir/hal_test" -ldl -pthread
factory=$(realpath "$build_dir/fake_submix.so")
if [ "${1:-}" = red ]; then
    timeout 12s "$build_dir/hal_test" "$factory" "$factory"
else
    "$cc" "${common[@]}" -fPIC -shared -fvisibility=hidden "-DORIGINAL_SUBMIX_PATH=\"$factory\"" \
        system-patch/shared-microphone/audio_r_submix_fanout.c -o "$build_dir/host_submix.so" -pthread -ldl
    timeout 12s "$build_dir/hal_test" "$factory" "$(realpath "$build_dir/host_submix.so")"
fi
