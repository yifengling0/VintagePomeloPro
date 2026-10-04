#!/bin/bash
set -euo pipefail
cd /data/src/winehua
export BUILD_DIR=/data/src/winehua/build-gpu-followup-1936fa6-20261004
export WINE_ARCH=aarch64 NATIVE_ARCH=arm64-v8a GUEST_ARCH=aarch64 JOBS=16
if [ -e "$BUILD_DIR" ]; then
    printf 'Fresh build directory already exists; refusing to reseed it.\n' >&2
    exit 1
fi
mkdir "$BUILD_DIR"
# Share only dependency/runtime artifacts. Wine configure/objects/identity are new.
for item in sysroot-ext host-tools dxvk vkd3d-proton fex-ec fex-pe box64-pe guest_gfx guest_vulkan host_vulkan smoke-payload wine-mono; do
    if [ -e "build/$item" ]; then ln -s "/data/src/winehua/build/$item" "$BUILD_DIR/$item"; fi
done
printf 'Fresh Wine directory: %s\n' "$BUILD_DIR"
bash scripts/build_wine.sh
bash scripts/build_wine.sh --check-cached-identity
