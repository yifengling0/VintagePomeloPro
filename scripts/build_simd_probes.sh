#!/bin/bash
set -euo pipefail
SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
source "$SCRIPT_DIR/env.sh"
out="$BUILD_DIR/simd-zero-probes"
mkdir -p "$out"
for triple in x86_64 i686; do
    "$LLVM_MINGW/bin/$triple-w64-mingw32-gcc" -O2 -Wall -Wextra -Werror -msse2 \
        "$ROOT/tests/runtime/fex_simd_zero_probe.c" -o "$out/simd-zero-$triple.exe"
done
"$LLVM_MINGW/bin/x86_64-w64-mingw32-gcc" -O2 -Wall -Wextra -Werror \
    "$ROOT/tests/runtime/fex_jit_fp_probe.c" -o "$out/jit-fp-x86_64.exe"
for triple in x86_64 i686; do
    "$LLVM_MINGW/bin/$triple-w64-mingw32-gcc" -O2 -Wall -Wextra -Werror \
        "$ROOT/tests/runtime/icu_probe.c" -o "$out/icu-$triple.exe"
done
