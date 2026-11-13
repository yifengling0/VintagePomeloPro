#!/bin/bash
# Reproduce standalone target probes with the same materialized FEX / SoftFloat.
set -euo pipefail
repo="$(cd "$(dirname "$0")/.." && pwd)"
fex_source="${1:?materialized patched FEX source}"
probe_toolchain="${2:?llvm-mingw root}"
probe_out="${3:?independent probe output directory}"
test -f "$fex_source/FEXCore/Source/Common/StrictMul24.h"
test -x "$probe_toolchain/bin/aarch64-w64-mingw32-clang"
mkdir -p "$probe_out"
sf="$fex_source/External/SoftFloat-3e"
files=()
for name in extF80_mul s_roundPackToExtF80 s_normSubnormalExtF80Sig s_propagateNaNExtF80UI softfloat_raiseFlags; do
    files+=("$sf/src/$name.c")
done
flags=(-O2 -Wall -Wextra -DFEXCORE_PRESERVE_ALL_ATTR= -DSOFTFLOAT_FAST_INT64=1
       -DSOFTFLOAT_BUILTIN_CLZ=1 '-DINLINE=static inline' -DINLINE_LEVEL=4
       -I"$fex_source/FEXCore/Source/Common" -I"$sf/include" -I"$sf/include/SoftFloat-3e")
for arch in aarch64 x86_64; do
    "$probe_toolchain/bin/$arch-w64-mingw32-clang" "${flags[@]}" \
        "$repo/smoke/fex_strict_mul24_test.c" "${files[@]}" -o "$probe_out/vp-mul24-differential-$arch.exe"
done
"$probe_toolchain/bin/aarch64-w64-mingw32-clang" "${flags[@]}" \
    "$repo/smoke/fex_strict_mul24_bench.c" "${files[@]}" -o "$probe_out/vp-mul24-direct-bench.exe"
"$probe_toolchain/bin/aarch64-w64-mingw32-clang" -O2 -Wall -Wextra \
    "$repo/smoke/fex_arm64_timer_probe.c" -o "$probe_out/vp-arm64-timer-probe.exe"
for kind in abi threads; do
    "$probe_toolchain/bin/i686-w64-mingw32-clang" -O2 -Wall -Wextra \
        "$repo/smoke/fex_x87_${kind}_probe.c" "$repo/smoke/fex_x87_abi_probe.S" -o "$probe_out/vp-x87-$kind-probe.exe"
done
"$probe_toolchain/bin/i686-w64-mingw32-clang" -O2 -Wall -Wextra \
    "$repo/smoke/fex_strict_mul24_bench.c" "$repo/smoke/fex_strict_mul24_guest_loop.S" -o "$probe_out/vp-mul24-guest-bench.exe"
printf 'Target probes built in %s\n' "$probe_out"
