#!/usr/bin/env bash
# Shared prerequisite repair for local images and hosted CI.
set -euo pipefail
packages=()
if [ ! -f /usr/share/aclocal/ltdl.m4 ]; then
  packages+=(libltdl-dev)
fi
if ! command -v glslangValidator >/dev/null 2>&1; then
  packages+=(glslang-tools)
fi
if ! command -v x86_64-w64-mingw32-widl >/dev/null 2>&1; then
  packages+=(mingw-w64-tools)
fi
# vkd3d-proton 补丁系列用 patch 打到隔离副本; 精简镜像可能没带
if ! command -v patch >/dev/null 2>&1; then
  packages+=(patch)
fi
if ! command -v Xvfb >/dev/null 2>&1; then
  packages+=(xvfb)
fi
if ! command -v wayland-scanner >/dev/null 2>&1 || \
   [ ! -f /usr/include/wayland-server.h ] || [ ! -f /usr/include/xcb/xcbext.h ] || \
   [ ! -f /usr/include/X11/Xlib.h ]; then
  packages+=(libwayland-dev libxcb1-dev libx11-dev)
fi
if [ ! -f /usr/include/X11/extensions/XInput2.h ]; then
  packages+=(libxi-dev)
fi
# Real wlroots buffer/addon lifecycle regression uses native headers.
if ! pkg-config --exists pixman-1 libdrm; then
  packages+=(libpixman-1-dev libdrm-dev)
fi
if [ ! -f /usr/share/wayland-protocols/stable/xdg-shell/xdg-shell.xml ]; then
  packages+=(wayland-protocols)
fi
if ! command -v msgfmt >/dev/null 2>&1; then
  packages+=(gettext)
fi
if ! command -v curl >/dev/null 2>&1; then
  packages+=(curl)
fi
# Old buildenv images predate the native ARM64 Wine path. Keep the
# runtime fallback aligned with ci/Dockerfile.buildenv.
if ! command -v aarch64-linux-gnu-gcc >/dev/null 2>&1 || \
   [ ! -f /usr/aarch64-linux-gnu/lib/Scrt1.o ]; then
  packages+=(gcc-aarch64-linux-gnu libc6-dev-arm64-cross)
fi
if ! command -v makeinfo >/dev/null 2>&1; then
  packages+=(texinfo)
fi
if ! command -v wget >/dev/null 2>&1; then
  packages+=(wget)
fi
if ! command -v xz >/dev/null 2>&1; then
  packages+=(xz-utils)
fi
# Match the hosted preflight runner's cache compression format.
if ! command -v zstd >/dev/null 2>&1; then
  packages+=(zstd)
fi
for spirv_tool in spirv-as spirv-dis spirv-val spirv-opt; do
  if ! command -v "$spirv_tool" >/dev/null 2>&1; then
    packages+=(spirv-tools)
    break
  fi
done
if [ ! -f /usr/include/alsa/pcm_external.h ]; then
  packages+=(libasound2-dev)
fi
if [ ! -f /usr/include/libudev.h ]; then
  packages+=(libudev-dev)
fi
if ! command -v xkbcomp >/dev/null 2>&1; then
  packages+=(x11-xkb-utils)
fi
if [ "${#packages[@]}" -gt 0 ]; then
  echo "Installing missing CI packages: ${packages[*]}"
  apt-get update
  apt-get install -y --no-install-recommends "${packages[@]}"
else
  echo "CI build dependencies already present"
fi
