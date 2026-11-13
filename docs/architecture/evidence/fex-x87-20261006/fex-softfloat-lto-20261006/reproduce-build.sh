#!/bin/bash
set -euo pipefail
cd /data/src/winehua
# Requires the already validated V2 staged source. Never reuses Wine objects.
experiment_out="$PWD/workspace_temp/fex-softfloat-lto-repro-20261006"
export BUILD_DIR="$PWD/build-fex-softfloat-lto-repro-20261006"
export FEX_SRC="$experiment_out/source"
test ! -e "$BUILD_DIR"
test ! -e "$FEX_SRC"
test -d workspace_temp/fex-fileload-v2-20261006/source
mkdir -p "$experiment_out"
cp -a workspace_temp/fex-fileload-v2-20261006/source "$FEX_SRC"
patch -d "$FEX_SRC" -p1 < workspace_temp/fex-softfloat-lto-20261006/thinlto-boundaries.patch
source scripts/env.sh
export PATH="$LLVM_MINGW/bin:$PATH"
cmake -S "$FEX_SRC" -B "$BUILD_DIR/fex-pe" \
    -DCMAKE_BUILD_TYPE=RelWithDebInfo \
    -DCMAKE_TOOLCHAIN_FILE="$FEX_SRC/Data/CMake/toolchain_mingw.cmake" \
    -DENABLE_LTO=True -DMINGW_TRIPLE=aarch64-w64-mingw32 -DBUILD_TESTING=False
require_cmake_not_debug "$BUILD_DIR/fex-pe/CMakeCache.txt" 'FEX ThinLTO experiment'
require_cmake_flag_var "$BUILD_DIR/fex-pe/CMakeCache.txt" CMAKE_CXX_FLAGS_RELWITHDEBINFO 'FEX ThinLTO experiment'
cmake --build "$BUILD_DIR/fex-pe" -j12 --target wow64fex > "$experiment_out/build.log" 2>&1
sha256sum "$BUILD_DIR/fex-pe/Bin/libwow64fex.dll" > "$experiment_out/built-dll.sha256"
