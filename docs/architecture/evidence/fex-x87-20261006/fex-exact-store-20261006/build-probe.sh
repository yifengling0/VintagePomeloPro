#!/bin/bash
set -euo pipefail
cd /data/src/winehua
out=workspace_temp/fex-exact-store-20261006
source scripts/env.sh
"$LLVM_MINGW/bin/i686-w64-mingw32-clang" -O2 -Wall -Wextra -Werror "$out/exact-store-probe.c" -o "$out/exact-store-probe.exe"
gcc -O2 -Wall -Wextra -Werror "$out/exact-store-probe.c" -o "$out/exact-store-probe-native"
"$out/exact-store-probe-native" > "$out/exact-native.tsv"
