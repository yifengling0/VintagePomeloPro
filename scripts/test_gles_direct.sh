#!/usr/bin/env bash
set -euo pipefail
source "$(dirname "$0")/env.sh"
mkdir -p "$BUILD_DIR/host_tests"
# Compile production target against real SDK declarations, with fake calls in
# the test. -idirafter keeps host libc headers ahead of the cross SDK's libc.
"$OHOS_SDK/native/llvm/bin/clang++" --target=x86_64-linux-gnu \
    -std=c++17 -Wall -Wextra -Werror \
    -idirafter "$OHOS_SDK/native/sysroot/usr/include" \
    -I "$ROOT/entry/src/main/cpp" \
    "$ROOT/host_tests/native_window_gles_target_test.cpp" \
    "$ROOT/entry/src/main/cpp/graphics/native_window_gles_target.cpp" \
    -o "$BUILD_DIR/host_tests/native_window_gles_target_test"
"$BUILD_DIR/host_tests/native_window_gles_target_test"
for triple in aarch64-linux-ohos x86_64-linux-ohos; do
    "$OHOS_SDK/native/llvm/bin/clang++" --target="$triple" --sysroot="$SYSROOT" \
        -std=c++17 -D__OHOS_API__=23 -fsyntax-only -I "$ROOT/entry/src/main/cpp" \
        "$ROOT/entry/src/main/cpp/graphics/native_window_gles_target.cpp" \
        "$ROOT/entry/src/main/cpp/graphics/virgl_surface_presenter.cpp"
    echo "GLES Direct API 23 syntax PASS: $triple"
done
