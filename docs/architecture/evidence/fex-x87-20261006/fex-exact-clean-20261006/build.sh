#!/bin/bash
set -euo pipefail
cd /data/src/winehua
experiment_out="$PWD/workspace_temp/fex-exact-clean-20261006"
export BUILD_DIR="$PWD/build-fex-exact-clean-20261006"
export FEX_SRC="$experiment_out/source"
test ! -e "$BUILD_DIR"
source scripts/env.sh
export PATH="$LLVM_MINGW/bin:$PATH"
cmake -S "$FEX_SRC" -B "$BUILD_DIR/fex-pe" \
  -DCMAKE_BUILD_TYPE=RelWithDebInfo \
  -DCMAKE_TOOLCHAIN_FILE="$FEX_SRC/Data/CMake/toolchain_mingw.cmake" \
  -DENABLE_LTO=True -DMINGW_TRIPLE=aarch64-w64-mingw32 -DBUILD_TESTING=False
require_cmake_not_debug "$BUILD_DIR/fex-pe/CMakeCache.txt" 'FEX float diagnostic'
require_cmake_flag_var "$BUILD_DIR/fex-pe/CMakeCache.txt" CMAKE_CXX_FLAGS_RELWITHDEBINFO 'FEX float diagnostic'
cmake --build "$BUILD_DIR/fex-pe" -j12 --target wow64fex > "$experiment_out/float-build.log" 2>&1
sha256sum "$BUILD_DIR/fex-pe/Bin/libwow64fex.dll" > "$experiment_out/float-built-dll.sha256"
