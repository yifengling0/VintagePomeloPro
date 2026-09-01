#!/usr/bin/env bash
set -euo pipefail
root=$(cd "$(dirname "$0")/.." && pwd)
out="$root/build/host_tests/text-input"
mkdir -p "$out"
# Compile the pinned Wayland server for the host; no OHOS/guest artifacts or
# submodule sources are modified. This test exercises real resources and pipes.
if [[ ! -f "$out/wayland/build.ninja" ]]; then
    CC=cc CXX=c++ meson setup "$out/wayland" "$root/thirdparty/wayland" \
        -Ddocumentation=false -Dtests=false -Ddtd_validation=false
fi
ninja -C "$out/wayland" -j4
cc -I "$root/entry/src/main/cpp" -c \
    "$root/entry/src/main/cpp/protocols/text-input-unstable-v3-protocol.c" \
    -o "$out/text-input-protocol.o"
c++ -std=c++17 -Wall -Wextra -Werror -pthread \
    -I "$root/entry/src/main/cpp/protocols" -I "$root/entry/src/main/cpp" \
    -I "$root/host_tests/stubs" \
    "$root/host_tests/text_input_lifecycle_test.cpp" \
    "$root/entry/src/main/cpp/input/text_input.cpp" "$out/text-input-protocol.o" \
    -L "$out/wayland/src" -Wl,-rpath,"$out/wayland/src" -lwayland-server \
    -Wl,--wrap=pipe2 -Wl,--wrap=wl_global_create -Wl,--wrap=wl_event_loop_add_fd \
    -o "$out/text_input_lifecycle_test"
"$out/text_input_lifecycle_test"
