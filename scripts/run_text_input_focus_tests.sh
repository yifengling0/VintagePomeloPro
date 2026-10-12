#!/usr/bin/env bash
set -euo pipefail
root=$(cd "$(dirname "$0")/.." && pwd)
out="$root/build/host_tests/text-input"
source=${WAYLAND_TEST_SOURCE:-"$root/thirdparty/wayland"}
mkdir -p "$out"
if [[ ! -f "$out/wayland/build.ninja" ]]; then
    CC=cc CXX=c++ meson setup "$out/wayland" "$source" \
        -Ddocumentation=false -Dtests=false -Ddtd_validation=false
fi
ninja -C "$out/wayland" -j4
cc -I "$root/entry/src/main/cpp" -c \
    "$root/entry/src/main/cpp/protocols/text-input-unstable-v3-protocol.c" \
    -o "$out/text-input-protocol.o"
c++ -std=c++17 -Wall -Wextra -Werror -Wno-unused-parameter -Wno-unused-variable -pthread \
    -I "$root/entry/src/main/cpp/protocols" -I "$root/entry/src/main/cpp" \
    -I "$root/host_tests/stubs" \
    "$root/host_tests/text_input_focus_reset_test.cpp" \
    "$root/entry/src/main/cpp/input/text_input.cpp" "$out/text-input-protocol.o" \
    -L "$out/wayland/src" -Wl,-rpath,"$out/wayland/src" -lwayland-server \
    -o "$out/text_input_focus_reset_test"
timeout 20 "$out/text_input_focus_reset_test"
