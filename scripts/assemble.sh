#!/bin/bash
# assemble.sh — 组装 HAP 打包布局
set -euo pipefail
SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
source "$SCRIPT_DIR/env.sh"

# ============================================================
# 文件分流到 libs/ + rawfile/
# ============================================================
assemble_pad() {
    log "=== 组装布局 ($NATIVE_ARCH) ==="

    local wine_data="$STAGING_DIR/wine-data"
    local guest_arch="${GUEST_ARCH:-x86_64}"
    local smoke_suite_version="phase2-vulkan-dxvk-v10-vkd3d-product-media"
    rm -rf "$STAGING_DIR"
    rm -rf "$wine_data"
    mkdir -p "$wine_data/bin/x86_64-windows"
    mkdir -p "$wine_data/bin/x86_64-unix"
    mkdir -p "$wine_data/share/wine/nls"
    mkdir -p "$wine_data/share/wine/fonts"
    mkdir -p "$wine_data/share/wine/winmd"
    mkdir -p "$wine_data/share/wine/mono"
    mkdir -p "$wine_data/share/wine/gecko"
    mkdir -p "$wine_data/share/X11"

    # SoundFont (MIDI 音色库)
    local soundfont="$WINEHUA/entry/src/main/resources/rawfile/winehua-gm.sf2"
    if [ -f "$soundfont" ]; then
        mkdir -p "$wine_data/audio"
        cp "$soundfont" "$wine_data/audio/winehua-gm.sf2"
        log "    winehua-gm.sf2 → rawfile audio/"
    else
        warn "winehua-gm.sf2 not found; MIDI output will be unavailable"
    fi

    # -- 1. 原生 .so → libs/$NATIVE_ARCH/ (由各 build 脚本完成) --
    mkdir -p "$NATIVE_LIBS"

    if [ "$NATIVE_ARCH" = "x86_64" ]; then
        # x86_64 Pad: Wine .so 是原生架构, 直接放 libs/
        log "  → Wine .so → libs/x86_64/"

        # 所有 Wine Unix .so → libs/x86_64/ (系统 linker 通过文件名搜索)
        for so in "$BUILD_DIR/wine-ohos/dlls/"*/*.so; do
            cp "$so" "$NATIVE_LIBS/"
        done
        log "    Wine .so: $(ls "$BUILD_DIR/wine-ohos/dlls/"*/*.so 2>/dev/null | wc -l) files"

        # 交叉编译依赖 → libs/x86_64/
        # (系统 linker 自动搜索此路径, 无需 x86_64-unix 子目录)
        _pick_lib_pad() {
            local name="$1" soname="$2" linker="${3:-}"
            local dest="$NATIVE_LIBS"
            if [ -f "$SYSROOT_EXT_LIB/$soname" ]; then
                cp "$SYSROOT_EXT_LIB/$soname" "$dest/$soname"
            elif [ -f "$SYSROOT/usr/lib/x86_64-linux-ohos/$name" ]; then
                cp "$SYSROOT/usr/lib/x86_64-linux-ohos/$name" "$dest/$soname"
            else
                warn "$soname 未找到"
                return 0
            fi
            if [ -n "$linker" ] && [ ! -f "$dest/$linker" ]; then
                cp "$dest/$soname" "$dest/$linker"
            fi
        }
        _pick_lib_pad "libfreetype.so.6.20.2"       "libfreetype.so.6"   "libfreetype.so"
        _pick_lib_pad "libz.so"                      "libz.so"
        _pick_lib_pad "libwayland-client.so.0.22.0"  "libwayland-client.so.0"
        _pick_lib_pad "libwayland-egl.so.1.22.0"     "libwayland-egl.so.1"
        _pick_lib_pad "libxkbcommon.so.0.0.0"        "libxkbcommon.so.0"
        _pick_lib_pad "libxkbregistry.so.0.0.0"      "libxkbregistry.so.0"
        _pick_lib_pad "libxml2.so.2.12.0"            "libxml2.so.2"
        _pick_lib_pad "libffi.so.8.1.4"              "libffi.so.8"
        # GnuTLS 链 (schannel TLS 后端)
        _pick_lib_pad "libgnutls.so.30.37.1"         "libgnutls.so.30"   "libgnutls.so"
        _pick_lib_pad "libnettle.so.8.11"            "libnettle.so.8"
        _pick_lib_pad "libhogweed.so.6.11"           "libhogweed.so.6"
        _pick_lib_pad "libgmp.so.10.4.1"             "libgmp.so.10"
        _pick_lib_pad "libtasn1.so.6.6.4"            "libtasn1.so.6"
        _pick_lib_pad "libunistring.so.5.2.0"        "libunistring.so.5"
        _pick_lib_pad "libm.so"                      "libm.so"
        # GStreamer 链 (winegstreamer 后端)
        for so in libglib-2.0.so.0 libgobject-2.0.so.0 libgmodule-2.0.so.0 libgio-2.0.so.0 \
                  libgthread-2.0.so.0 libpcre2-8.so.0 libintl.so.8 libintl.so libm.so \
                  libgstreamer-1.0.so.0 libgstbase-1.0.so.0 libgstcontroller-1.0.so.0 \
                  libgstnet-1.0.so.0 libgstvideo-1.0.so.0 libgstaudio-1.0.so.0 \
                  libgsttag-1.0.so.0 libgstpbutils-1.0.so.0 libgstallocators-1.0.so.0 \
                  libgstapp-1.0.so.0 libgstfft-1.0.so.0 libgstriff-1.0.so.0 \
                  libgstrtp-1.0.so.0 libgstrtsp-1.0.so.0 libgstsdp-1.0.so.0 \
                  libgstcodecparsers-1.0.so.0 libgstmpegts-1.0.so.0; do
            _pick_lib_pad "$so" "$so"
        done
        log "    交叉编译依赖 → libs/x86_64/"

        # libc.so → libs/x86_64/
        cp "$SYSROOT/usr/lib/x86_64-linux-ohos/libc.so" "$NATIVE_LIBS/"

        # libfreetype 已由 _pick_lib_pad 放入 libs/x86_64/，系统 linker 可直接找到

        # libwineserver.so (Pad fork+dlopen 入口)
        if [ -f "$BUILD_DIR/wine_server/libwineserver.so" ]; then
            cp "$BUILD_DIR/wine_server/libwineserver.so" "$NATIVE_LIBS/"
            log "    libwineserver.so → libs/x86_64/"
        else
            warn "libwineserver.so 未找到！请先执行: bash scripts/build_wine.sh"
        fi
    elif [ "$NATIVE_ARCH" = "arm64-v8a" ]; then
        # arm64 Pad: Wine .so 是 x86_64, 不放 libs/, 放 rawfile zip
        # box64.so 由 build_box64.sh 放入 NATIVE_LIBS
        log "  → Wine x86_64 .so → rawfile zip"

        # ARM64 原生库 → libs/arm64-v8a/ (Box64 dlopen bridge libraries)
        # Box64 模拟 x86_64 时需要加载 ARM64 原生的 freetype/xkbcommon 等,
        # 系统 linker 搜索 libs/arm64-v8a/
        local aarch64_lib="$SYSROOT_EXT/usr/lib/$NATIVE_TARGET"
        _pick_arm64_native() {
            local soname="$1" linker="${2:-}"
            if [ -f "$aarch64_lib/$soname" ]; then
                cp "$aarch64_lib/$soname" "$NATIVE_LIBS/$soname"
            else
                warn "ARM64 原生库 $soname 未找到, 跳过"
                return 0
            fi
            if [ -n "$linker" ] && [ ! -f "$NATIVE_LIBS/$linker" ]; then
                cp "$aarch64_lib/$soname" "$NATIVE_LIBS/$linker"  # HAP 不支持 symlink, 实体复制
            fi
        }
        # Box64 native bridge libs: soname 文件 + linker 名拷贝
        _pick_arm64_native "libfreetype.so.6"   "libfreetype.so"
        _pick_arm64_native "libxkbcommon.so.0"   "libxkbcommon.so"
        _pick_arm64_native "libxkbregistry.so.0" "libxkbregistry.so"
        _pick_arm64_native "libxml2.so.2"        "libxml2.so"
        _pick_arm64_native "libwayland-client.so.0" "libwayland-client.so"
        _pick_arm64_native "libwayland-server.so.0" "libwayland-server.so"
        _pick_arm64_native "libffi.so.8"         "libffi.so"

        # box64.so → libs/arm64-v8a/ (ARM64 原生翻译器)
        if [ -f "$BUILD_DIR/box64_build/box64.so" ]; then
            cp "$BUILD_DIR/box64_build/box64.so" "$NATIVE_LIBS/"
            log "    box64.so → libs/arm64-v8a/"
        else
            warn "box64.so 未找到！请先执行: bash scripts/build_box64.sh"
        fi

        # ntdll.so → rawfile
        cp "$BUILD_DIR/wine-ohos/dlls/ntdll/ntdll.so" "$wine_data/bin/"

        # x86_64-unix/ .so → rawfile
        for so in "$BUILD_DIR/wine-ohos/dlls/"*/*.so; do
            [ "$(basename "$so")" = "ntdll.so" ] && continue
            cp "$so" "$wine_data/bin/x86_64-unix/"
        done

        # 交叉编译依赖 → rawfile
        _pick_lib_pad_rf() {
            local name="$1" soname="$2" linker="${3:-}"
            local dest="$wine_data/bin/x86_64-unix"
            if [ -f "$SYSROOT_EXT_LIB/$soname" ]; then
                cp "$SYSROOT_EXT_LIB/$soname" "$dest/$soname"
            elif [ -f "$SYSROOT/usr/lib/x86_64-linux-ohos/$name" ]; then
                cp "$SYSROOT/usr/lib/x86_64-linux-ohos/$name" "$dest/$soname"
            else
                warn "$soname 未找到"
                return 0
            fi
            if [ -n "$linker" ] && [ ! -f "$dest/$linker" ]; then
                cp "$dest/$soname" "$dest/$linker"
            fi
        }
        _pick_lib_pad_rf "libfreetype.so.6.20.2"       "libfreetype.so.6"   "libfreetype.so"
        _pick_lib_pad_rf "libz.so"                      "libz.so"
        _pick_lib_pad_rf "libwayland-client.so.0.22.0"  "libwayland-client.so.0"
        _pick_lib_pad_rf "libwayland-egl.so.1.22.0"     "libwayland-egl.so.1"    "libwayland-egl.so"
        _pick_lib_pad_rf "libxkbcommon.so.0.0.0"        "libxkbcommon.so.0"
        _pick_lib_pad_rf "libxkbregistry.so.0.0.0"      "libxkbregistry.so.0"
        _pick_lib_pad_rf "libxml2.so.2.12.0"            "libxml2.so.2"
        _pick_lib_pad_rf "libffi.so.8.1.4"              "libffi.so.8"
        # GnuTLS 链 (schannel TLS 后端, x86_64 guest) → rawfile
        _pick_lib_pad_rf "libgnutls.so.30.37.1"         "libgnutls.so.30"   "libgnutls.so"
        _pick_lib_pad_rf "libnettle.so.8.11"            "libnettle.so.8"
        _pick_lib_pad_rf "libhogweed.so.6.11"           "libhogweed.so.6"
        _pick_lib_pad_rf "libgmp.so.10.4.1"             "libgmp.so.10"
        _pick_lib_pad_rf "libtasn1.so.6.6.4"            "libtasn1.so.6"
        _pick_lib_pad_rf "libunistring.so.5.2.0"        "libunistring.so.5"
        # libm.so: 补 OHOS 缺失的 frexpl/ldexpl (glib long double 数学)
        # 系统 libm.so 是空壳, 必须用我们的版本 (含 math 符号需 libc 兜底)
        _pick_lib_pad_rf "libm.so"                      "libm.so"
        # GStreamer 链 (winegstreamer 后端: glib + gstreamer core + gst-libs)
        for so in libglib-2.0.so.0 libgobject-2.0.so.0 libgmodule-2.0.so.0 libgio-2.0.so.0 \
                  libgthread-2.0.so.0 libpcre2-8.so.0 libintl.so.8 libintl.so libm.so \
                  libgstreamer-1.0.so.0 libgstbase-1.0.so.0 libgstcontroller-1.0.so.0 \
                  libgstnet-1.0.so.0 libgstvideo-1.0.so.0 libgstaudio-1.0.so.0 \
                  libgsttag-1.0.so.0 libgstpbutils-1.0.so.0 libgstallocators-1.0.so.0 \
                  libgstapp-1.0.so.0 libgstfft-1.0.so.0 libgstriff-1.0.so.0 \
                  libgstrtp-1.0.so.0 libgstrtsp-1.0.so.0 libgstsdp-1.0.so.0 \
                  libgstcodecparsers-1.0.so.0 libgstmpegts-1.0.so.0; do
            # box64 按 SONAME 解析依赖时可能查找无版本名 (libgstvideo-1.0.so),
            # 与 gnutls 链一致补上无版本软链, 否则 winegstreamer dlopen 报
            # "Error loading shared library libgstvideo-1.0.so: No such file"
            local unversioned="${so%.so.0}"
            if [ "$unversioned" != "$so" ] && [[ "$so" == *.so.0 ]]; then
                _pick_lib_pad_rf "$so" "$so" "$unversioned.so"
            else
                _pick_lib_pad_rf "$so" "$so"
            fi
        done
        # FFmpeg 解码库 (gst-libav 依赖) → rawfile
        for so in libavcodec.so.60 libavformat.so.60 libavutil.so.58 \
                  libswscale.so.7 libswresample.so.4 libavfilter.so.9; do
            _pick_lib_pad_rf "$so" "$so"
        done
        # GStreamer 插件 (gst-plugins-base/good + gst-libav) → rawfile
        local gst_plugin_dir="$SYSROOT_EXT_LIB/gstreamer-1.0"
        if [ -d "$gst_plugin_dir" ]; then
            mkdir -p "$wine_data/bin/x86_64-unix/gstreamer-1.0"
            for pso in "$gst_plugin_dir"/*.so; do
                [ -f "$pso" ] || continue
                cp "$pso" "$wine_data/bin/x86_64-unix/gstreamer-1.0/"
            done
            log "    GStreamer 插件 ($(ls "$gst_plugin_dir"/*.so 2>/dev/null | wc -l) 个) → rawfile gstreamer-1.0/"
        else
            warn "gstreamer-1.0 插件目录缺失: $gst_plugin_dir"
        fi

        # libgnutls → bin/ (box64 按名 dlopen 搜索路径: .)
        cp "$wine_data/bin/x86_64-unix/libfreetype.so.6" "$wine_data/bin/"
        cp "$wine_data/bin/x86_64-unix/libfreetype.so" "$wine_data/bin/"

        # libc.so → bin/ (当前目录) + x86_64-unix/ (BOX64_LD_LIBRARY_PATH)
        cp "$SYSROOT/usr/lib/x86_64-linux-ohos/libc.so" "$wine_data/bin/"
        cp "$SYSROOT/usr/lib/x86_64-linux-ohos/libc.so" "$wine_data/bin/x86_64-unix/"

        # wine + wineserver (x86_64 ELF, 由 box64 加载)
        cp "$BUILD_DIR/wine-ohos/loader/wine" "$wine_data/bin/"
        if [ -f "$BUILD_DIR/wine_server/wineserver" ]; then
            cp "$BUILD_DIR/wine_server/wineserver" "$wine_data/bin/"
        elif [ -f "$BUILD_DIR/wine-ohos/server/wineserver" ]; then
            cp "$BUILD_DIR/wine-ohos/server/wineserver" "$wine_data/bin/"
        fi
    fi

    # -- 2. PE DLL + 数据文件 → rawfile (两种架构共用) --
    # x86_64-windows/ — 复制所有运行时 PE 文件
    # 注意: .cpl 打包 (含 appwiz.cpl), 但依赖 wine-mono msi 同包就位:
    # wineboot 初始化时 mscoree.dll 触发 appwiz.cpl install_mono →
    # install_addon 在 WINEDATADIR/mono/ 找到 msi → 静默安装不弹框;
    # 若 msi 缺失 (BUILD_WINE_MONO=0) 则弹 DialogBoxW 模态框, OHOS
    # 无头环境无人响应 → wineboot 永久阻塞. 故 cpl 与 mono msi 必须同包.
    for ext in dll drv exe sys acm ax ocx tlb cpl; do
        for f in "$BUILD_DIR/wine-ohos/dlls/"*/x86_64-windows/*.$ext; do
            [ -f "$f" ] && cp "$f" "$wine_data/bin/x86_64-windows/"
        done
    done
    log "  x86_64-windows → $(ls "$wine_data/bin/x86_64-windows" | wc -l) files"

    # strip PE 调试符号 (DWARF .debug_*, 缩减 ~50%)
    log "  stripping debug symbols..."
    if command -v x86_64-w64-mingw32-strip &>/dev/null; then
        for f in "$wine_data/bin/x86_64-windows/"*.dll "$wine_data/bin/x86_64-windows/"*.drv "$wine_data/bin/x86_64-windows/"*.exe "$wine_data/bin/x86_64-windows/"*.sys; do
            [ -f "$f" ] && x86_64-w64-mingw32-strip "$f" 2>/dev/null
        done
        log "  64-bit PE stripped"
    else
        warn "  x86_64-w64-mingw32-strip not found, skipping strip"
    fi
    # i386-windows/ (32-bit PE DLL for WoW64)
    # 主构建 --enable-archs=i386,x86_64 已产出全部 32-bit PE, 直接取自 wine-ohos,
    # 无需独立的 i686-mingw32 构建.
    # 注意: wineboot/rpcss/services/conhost 等服务程序只有 x86_64 版,
    # WoW64 下它们由 Wine 以 64 位进程拉起, 属上游 WoW64 的正常行为.
    mkdir -p "$wine_data/bin/i386-windows"
    for ext in dll drv exe sys acm ax ocx tlb cpl; do
        for f in "$BUILD_DIR/wine-ohos/dlls/"*/i386-windows/*.$ext; do
            [ -f "$f" ] && cp "$f" "$wine_data/bin/i386-windows/"
        done
    done
    log "  i386-windows → $(ls "$wine_data/bin/i386-windows" | wc -l) files (ALL)"

    # 32-bit exe stubs, 放在 bin/i386-windows/.
    # Wine 通过 WINEARCH 或 exe header 判断 32/64, 自动加载对应 DLL.
    for exe in "$BUILD_DIR/wine-ohos/programs/"*/i386-windows/*.exe; do
        [ -f "$exe" ] && cp "$exe" "$wine_data/bin/i386-windows/"
    done
    log "  i386 exe stubs → $(ls "$wine_data/bin/i386-windows"/*.exe 2>/dev/null | wc -l) files"

    # 32-bit PE strip (必须在 copy 之后)
    if command -v i686-w64-mingw32-strip &>/dev/null; then
        for f in "$wine_data/bin/i386-windows/"*.dll "$wine_data/bin/i386-windows/"*.drv "$wine_data/bin/i386-windows/"*.exe "$wine_data/bin/i386-windows/"*.sys; do
            [ -f "$f" ] && i686-w64-mingw32-strip "$f" 2>/dev/null
        done
        log "  32-bit PE stripped"
    else
        warn "  i686-w64-mingw32-strip not found, skipping strip"
    fi

    # *.exe stubs → rawfile
    for exe in "$BUILD_DIR/wine-ohos/programs/"*/x86_64-windows/*.exe; do
        cp "$exe" "$wine_data/bin/"
    done
    # Built-in smoke tests are launched by name.  Keep their PE stubs in the
    # architecture directory as well as bin/ so WINEDLLDIR0 can resolve them.
    for smoke in winehua_graphics_smoke winehua_audio_smoke; do
        smoke_exe="$BUILD_DIR/wine-ohos/programs/$smoke/x86_64-windows/$smoke.exe"
        if [ -f "$smoke_exe" ]; then
            cp "$smoke_exe" "$wine_data/bin/x86_64-windows/"
            log "  $smoke.exe → x86_64-windows/"
        fi
    done
    # GStreamer 解码流程探针 (调试用): mingw 编译的自包含 PE, 生成测试 WAV
    # 并用 DirectShow 播放以验证 winegstreamer→GStreamer 链路是否工作。
    if [ -f "$WINEHUA/smoke/gst_probe.exe" ]; then
        cp "$WINEHUA/smoke/gst_probe.exe" "$wine_data/bin/x86_64-windows/"
        log "  gst_probe.exe → x86_64-windows/ (GStreamer 解码探针)"
    fi
    # The built-in audio smoke must be self-contained and use a format accepted
    # by wineohos.drv.  Wine's idw_testsound.wav is IMA ADPCM, while the native
    # bridge accepts PCM/float input, so generate a deterministic PCM16 stereo
    # fixture instead of depending on user media or an external codec tool.
    python3 - "$wine_data/bin/Alarm01.wav" <<'PY'
import math
import struct
import sys
import wave

output = sys.argv[1]
rate = 48000
seconds = 2
amplitude = 7200
with wave.open(output, 'wb') as wav:
    wav.setnchannels(2)
    wav.setsampwidth(2)
    wav.setframerate(rate)
    frames = bytearray()
    for index in range(rate * seconds):
        value = int(amplitude * (math.sin(2.0 * math.pi * 523.25 * index / rate) +
                                 0.45 * math.sin(2.0 * math.pi * 659.25 * index / rate)))
        sample = max(-32768, min(32767, value))
        frames.extend(struct.pack('<hh', sample, sample))
    wav.writeframes(frames)
PY
    log "  generated PCM16 stereo → bin/Alarm01.wav (audio smoke fixture)"

    # Versioned, App-managed C:\smoke payload.  Keep it separate from Wine's
    # DLL search directories so a prefix refresh can update tests without
    # touching user files or relying on Explorer.
    local smoke_dir="$wine_data/smoke"
    mkdir -p "$smoke_dir/x64" "$smoke_dir/x86" "$smoke_dir/assets"
    local cube_source="$WINEHUA/smoke/winehua_d3d_switch_cube.c"
    x86_64-w64-mingw32-gcc -O2 -s -mwindows -o \
        "$smoke_dir/x64/winehua_d3d_switch_cube.exe" "$cube_source" \
        -ld3d9 -ld3d11 -ldxgi -ld3dcompiler -luuid -lshell32 -luser32 -lgdi32 -lm
    i686-w64-mingw32-gcc -O2 -s -mwindows -o \
        "$smoke_dir/x86/winehua_d3d_switch_cube.exe" "$cube_source" \
        -ld3d9 -ld3d11 -ldxgi -ld3dcompiler -luuid -lshell32 -luser32 -lgdi32 -lm
    # Keep WineHua master's capability probes in the managed payload. They
    # exercise the same Wine PE Vulkan import libraries as real DXVK games.
    local vulkan_import_x64="$BUILD_DIR/wine-ohos/dlls/vulkan-1/x86_64-windows/libvulkan-1.a"
    local vulkan_import_x86="$BUILD_DIR/wine-ohos/dlls/vulkan-1/i386-windows/libvulkan-1.a"
    [ -s "$vulkan_import_x64" ] || err "Wine x64 Vulkan import library missing: $vulkan_import_x64"
    [ -s "$vulkan_import_x86" ] || err "Wine x86 Vulkan import library missing: $vulkan_import_x86"
    local diagnostics_source="$WINEHUA/smoke/winehua_gpu_diagnostics.c"
    x86_64-w64-mingw32-gcc -O2 -s -Wall -Wextra -Werror -mwindows -I"$DXVK_SRC/include" -o \
        "$smoke_dir/x64/winehua_gpu_diagnostics.exe" "$diagnostics_source" \
        "$vulkan_import_x64" \
        -ld3d11 -ldxgi -lversion -luuid -lshell32 -luser32 -lgdi32
    i686-w64-mingw32-gcc -O2 -s -Wall -Wextra -Werror -mwindows -I"$DXVK_SRC/include" -o \
        "$smoke_dir/x86/winehua_gpu_diagnostics.exe" "$diagnostics_source" \
        "$vulkan_import_x86" \
        -ld3d11 -ldxgi -lversion -luuid -lshell32 -luser32 -lgdi32
    local d3d8_source="$WINEHUA/smoke/winehua_d3d8_smoke.c"
    x86_64-w64-mingw32-gcc -O2 -s -mwindows -o \
        "$smoke_dir/x64/winehua_d3d8_smoke.exe" "$d3d8_source" \
        -luser32 -lgdi32
    i686-w64-mingw32-gcc -O2 -s -mwindows -o \
        "$smoke_dir/x86/winehua_d3d8_smoke.exe" "$d3d8_source" \
        -luser32 -lgdi32
    local dxvk26_requirements_source="$WINEHUA/smoke/winehua_dxvk26_requirements.c"
    x86_64-w64-mingw32-gcc -O2 -s -Wall -Wextra -Werror -I"$DXVK_SRC/include" -o \
        "$smoke_dir/x64/winehua_dxvk26_requirements.exe" "$dxvk26_requirements_source" \
        "$vulkan_import_x64" -luser32 -lcomctl32 -lgdi32
    i686-w64-mingw32-gcc -O2 -s -Wall -Wextra -Werror -I"$DXVK_SRC/include" -o \
        "$smoke_dir/x86/winehua_dxvk26_requirements.exe" "$dxvk26_requirements_source" \
        "$vulkan_import_x86" -luser32 -lcomctl32 -lgdi32
    # Deterministic DirectShow/GStreamer probe.  Build both PE widths so media
    # regressions can be separated from a game's own playback state machine.
    local media_source="$WINEHUA/smoke/winehua_media_smoke.cpp"
    # Keep the diagnostic self-contained.  The Wine runtime intentionally does
    # not ship MinGW's libgcc/libstdc++ DLLs, and a dynamically linked probe
    # would fail before wmain() with c0000135 instead of testing DirectShow.
    x86_64-w64-mingw32-g++ -O2 -s -municode -static-libgcc -static-libstdc++ -o \
        "$smoke_dir/x64/winehua_media_smoke.exe" "$media_source" \
        -lstrmiids -lole32 -luuid -luser32
    i686-w64-mingw32-g++ -O2 -s -municode -static-libgcc -static-libstdc++ -o \
        "$smoke_dir/x86/winehua_media_smoke.exe" "$media_source" \
        -lstrmiids -lole32 -luuid -luser32
    local win32_driver_source="$WINEHUA/smoke/winehua_win32_driver.c"
    x86_64-w64-mingw32-gcc -O2 -s -municode -mwindows -o \
        "$smoke_dir/x64/winehua_win32_driver.exe" "$win32_driver_source" \
        -lshell32 -luser32
    i686-w64-mingw32-gcc -O2 -s -municode -mwindows -o \
        "$smoke_dir/x86/winehua_win32_driver.exe" "$win32_driver_source" \
        -lshell32 -luser32
    local net_source="$WINEHUA/smoke/winehua_network_probe.c"
    local net_wininet_source="$WINEHUA/smoke/winehua_network_wininet.c"
    x86_64-w64-mingw32-gcc -O2 -s -mwindows -o \
        "$smoke_dir/x64/winehua_network_probe.exe" "$net_source" "$net_wininet_source" \
        -lwinhttp -lwininet -lws2_32 -luser32 -lgdi32
    cp "$smoke_dir/x64/winehua_network_probe.exe" "$wine_data/bin/x86_64-windows/"
    log "  winehua_network_probe.exe → smoke/x64 + bin/x86_64-windows"
    local guest_shader_root="$BUILD_DIR/guest_vulkan/$guest_arch/share/winehua"
    local smoke_shader
    for smoke_shader in venus_storage_write venus_storage_read venus_image_fetch venus_combined_sample venus_separated_sample; do
        [ -f "$guest_shader_root/$smoke_shader.spv" ] || err "Wine Vulkan sampled-image shader missing: $guest_shader_root/$smoke_shader.spv"
        cp "$guest_shader_root/$smoke_shader.spv" "$smoke_dir/assets/$smoke_shader.spv"
    done
    local dxvk_root="$DXVK_BUILD_ROOT"
    mkdir -p "$wine_data/dxvk/legacy/x64" "$wine_data/dxvk/legacy/x86"
    local dxvk_arch dxvk_dll
    for dxvk_arch in x64 x86; do
        for dxvk_dll in d3d9.dll d3d10core.dll d3d10.dll d3d10_1.dll d3d11.dll dxgi.dll; do
            [ -f "$dxvk_root/$dxvk_arch/bin/$dxvk_dll" ] || \
                err "DXVK Legacy $dxvk_arch $dxvk_dll missing: $dxvk_root/$dxvk_arch/bin/$dxvk_dll"
            cp "$dxvk_root/$dxvk_arch/bin/$dxvk_dll" \
                "$wine_data/dxvk/legacy/$dxvk_arch/$dxvk_dll"
        done
    done
    # DXVK Modern 2.6 (dxvk_modern_2_6 后端): 独立版本化目录, 与 legacy 并列。
    local dxvk_modern_root="$DXVK_MODERN_BUILD_ROOT"
    mkdir -p "$wine_data/dxvk/modern-2.6/x64" "$wine_data/dxvk/modern-2.6/x86"
    for dxvk_arch in x64 x86; do
        for dxvk_dll in d3d11.dll dxgi.dll; do
            [ -f "$dxvk_modern_root/$dxvk_arch/bin/$dxvk_dll" ] || \
                err "DXVK Modern $dxvk_arch $dxvk_dll missing: $dxvk_modern_root/$dxvk_arch/bin/$dxvk_dll"
            cp "$dxvk_modern_root/$dxvk_arch/bin/$dxvk_dll" \
                "$wine_data/dxvk/modern-2.6/$dxvk_arch/$dxvk_dll"
        done
    done
    # VKD3D-Proton (D3D12): limited-500K profile and its common smoke assets.
    local vkd3d_root="$VKD3D_PROTON_BUILD_ROOT/limited-500k"
    [ -f "$vkd3d_root/x64/d3d12.dll" ] || err "VKD3D-Proton x64 d3d12.dll missing: $vkd3d_root/x64/d3d12.dll"
    [ -f "$vkd3d_root/x64/winehua-d3d12-smoke.exe" ] || \
        err "VKD3D-Proton x64 graphics smoke missing: $vkd3d_root/x64/winehua-d3d12-smoke.exe"
    [ -f "$vkd3d_root/manifest.json" ] || err "VKD3D-Proton manifest missing: $vkd3d_root/manifest.json"
    local vkd3d_demo_triangle="$vkd3d_root/x64/triangle.exe"
    local vkd3d_demo_gears="$vkd3d_root/x64/gears.exe"
    if [ ! -f "$vkd3d_demo_triangle" ] || [ ! -f "$vkd3d_demo_gears" ]; then
        local vkd3d_upstream_demos="$WINEHUA/.temp/vkd3d-upstream-demos-20260806-payload"
        vkd3d_demo_triangle="$vkd3d_upstream_demos/triangle.exe"
        vkd3d_demo_gears="$vkd3d_upstream_demos/gears.exe"
    fi
    [ -f "$vkd3d_demo_triangle" ] || err "VKD3D-Proton triangle demo missing: $vkd3d_demo_triangle"
    [ -f "$vkd3d_demo_gears" ] || err "VKD3D-Proton gears demo missing: $vkd3d_demo_gears"
    cp "$vkd3d_demo_triangle" "$smoke_dir/x64/triangle.exe"
    cp "$vkd3d_demo_gears" "$smoke_dir/x64/gears.exe"
    mkdir -p "$wine_data/vkd3d/limited-500k/x64"
    cp "$vkd3d_root/x64/d3d12.dll" "$wine_data/vkd3d/limited-500k/x64/d3d12.dll"
    cp "$vkd3d_root/manifest.json" "$wine_data/vkd3d/manifest.json"
    cp "$vkd3d_root/x64/winehua-d3d12-smoke.exe" \
        "$smoke_dir/x64/winehua_d3d12_smoke.exe"
    # The DXVK binaries are runtime-owned overlays.  Do not place them next
    # to the smoke executables: that would make the test layout look like a
    # game distribution and would force real games to carry WineHua-specific
    # DLLs.  SpawnWineProgram exposes this versioned directory through
    # WINEDLLPATH only for a selected dxvk_* backend.
    local smoke_program
    for smoke_program in winehua_audio_smoke winehua_graphics_smoke winehua_vulkan_smoke winehua_d3d11_smoke; do
        local smoke64="$BUILD_DIR/wine-ohos/programs/$smoke_program/x86_64-windows/$smoke_program.exe"
        local smoke32="$BUILD_DIR/wine-i386-pe/programs/$smoke_program/i386-windows/$smoke_program.exe"
        if [ ! -f "$smoke32" ]; then
            smoke32="$BUILD_DIR/wine-ohos/programs/$smoke_program/i386-windows/$smoke_program.exe"
        fi
        [ -f "$smoke64" ] || err "managed smoke x64 artifact missing: $smoke64"
        [ -f "$smoke32" ] || err "managed smoke x86 artifact missing: $smoke32"
        cp "$smoke64" "$smoke_dir/x64/$smoke_program.exe"
        cp "$smoke32" "$smoke_dir/x86/$smoke_program.exe"
    done
    local audio64_sha graphics64_sha vulkan64_sha d3d1164_sha d3d864_sha cube64_sha media64_sha diagnostics64_sha driver64_sha requirements64_sha
    local audio32_sha graphics32_sha vulkan32_sha d3d1132_sha d3d832_sha cube32_sha media32_sha diagnostics32_sha driver32_sha requirements32_sha
    local storage_write_sha storage_read_sha image_fetch_sha combined_sample_sha separated_sample_sha
    local vkd3d64_d3d12_sha vkd3d64_smoke_sha vkd3d_triangle_sha vkd3d_gears_sha
    audio64_sha="$(sha256sum "$smoke_dir/x64/winehua_audio_smoke.exe" | awk '{print $1}')"
    graphics64_sha="$(sha256sum "$smoke_dir/x64/winehua_graphics_smoke.exe" | awk '{print $1}')"
    vulkan64_sha="$(sha256sum "$smoke_dir/x64/winehua_vulkan_smoke.exe" | awk '{print $1}')"
    d3d1164_sha="$(sha256sum "$smoke_dir/x64/winehua_d3d11_smoke.exe" | awk '{print $1}')"
    d3d864_sha="$(sha256sum "$smoke_dir/x64/winehua_d3d8_smoke.exe" | awk '{print $1}')"
    cube64_sha="$(sha256sum "$smoke_dir/x64/winehua_d3d_switch_cube.exe" | awk '{print $1}')"
    media64_sha="$(sha256sum "$smoke_dir/x64/winehua_media_smoke.exe" | awk '{print $1}')"
    diagnostics64_sha="$(sha256sum "$smoke_dir/x64/winehua_gpu_diagnostics.exe" | awk '{print $1}')"
    driver64_sha="$(sha256sum "$smoke_dir/x64/winehua_win32_driver.exe" | awk '{print $1}')"
    requirements64_sha="$(sha256sum "$smoke_dir/x64/winehua_dxvk26_requirements.exe" | awk '{print $1}')"
    audio32_sha="$(sha256sum "$smoke_dir/x86/winehua_audio_smoke.exe" | awk '{print $1}')"
    graphics32_sha="$(sha256sum "$smoke_dir/x86/winehua_graphics_smoke.exe" | awk '{print $1}')"
    vulkan32_sha="$(sha256sum "$smoke_dir/x86/winehua_vulkan_smoke.exe" | awk '{print $1}')"
    d3d1132_sha="$(sha256sum "$smoke_dir/x86/winehua_d3d11_smoke.exe" | awk '{print $1}')"
    d3d832_sha="$(sha256sum "$smoke_dir/x86/winehua_d3d8_smoke.exe" | awk '{print $1}')"
    cube32_sha="$(sha256sum "$smoke_dir/x86/winehua_d3d_switch_cube.exe" | awk '{print $1}')"
    media32_sha="$(sha256sum "$smoke_dir/x86/winehua_media_smoke.exe" | awk '{print $1}')"
    diagnostics32_sha="$(sha256sum "$smoke_dir/x86/winehua_gpu_diagnostics.exe" | awk '{print $1}')"
    driver32_sha="$(sha256sum "$smoke_dir/x86/winehua_win32_driver.exe" | awk '{print $1}')"
    requirements32_sha="$(sha256sum "$smoke_dir/x86/winehua_dxvk26_requirements.exe" | awk '{print $1}')"
    vkd3d64_d3d12_sha="$(sha256sum "$wine_data/vkd3d/limited-500k/x64/d3d12.dll" | awk '{print $1}')"
    vkd3d64_smoke_sha="$(sha256sum "$smoke_dir/x64/winehua_d3d12_smoke.exe" | awk '{print $1}')"
    vkd3d_triangle_sha="$(sha256sum "$smoke_dir/x64/triangle.exe" | awk '{print $1}')"
    vkd3d_gears_sha="$(sha256sum "$smoke_dir/x64/gears.exe" | awk '{print $1}')"
    storage_write_sha="$(sha256sum "$smoke_dir/assets/venus_storage_write.spv" | awk '{print $1}')"
    storage_read_sha="$(sha256sum "$smoke_dir/assets/venus_storage_read.spv" | awk '{print $1}')"
    image_fetch_sha="$(sha256sum "$smoke_dir/assets/venus_image_fetch.spv" | awk '{print $1}')"
    combined_sample_sha="$(sha256sum "$smoke_dir/assets/venus_combined_sample.spv" | awk '{print $1}')"
    separated_sample_sha="$(sha256sum "$smoke_dir/assets/venus_separated_sample.spv" | awk '{print $1}')"
    local dxvk_commit dxvk_modern_commit mesa_commit virglrenderer_commit
    local guest_venus_icd_sha host_virglrenderer_sha venus_runtime_id
    dxvk_commit="$(git -c safe.directory="$DXVK_SRC" -C "$DXVK_SRC" rev-parse HEAD 2>/dev/null || echo unknown)"
    dxvk_modern_commit="$(git -c safe.directory="$DXVK_MODERN_SRC" -C "$DXVK_MODERN_SRC" rev-parse HEAD 2>/dev/null || echo unknown)"
    mesa_commit="$(git -c safe.directory="$ROOT/thirdparty/mesa" -C "$ROOT/thirdparty/mesa" rev-parse HEAD 2>/dev/null || echo unknown)"
    virglrenderer_commit="$(git -c safe.directory="$ROOT/thirdparty/virglrenderer" -C "$ROOT/thirdparty/virglrenderer" rev-parse HEAD 2>/dev/null || echo unknown)"
    guest_venus_icd_sha="$(sha256sum "$BUILD_DIR/guest_vulkan/$guest_arch/lib/libvulkan_virtio.so" | awk '{print $1}')"
    host_virglrenderer_sha="$(sha256sum "$ROOT/entry/libs/$NATIVE_ARCH/libvirglrenderer.so.1" | awk '{print $1}')"
    venus_runtime_id="venus-${guest_venus_icd_sha:0:12}-${host_virglrenderer_sha:0:12}"
    local dxvk64_d3d9_sha dxvk64_d3d10core_sha dxvk64_d3d10_sha dxvk64_d3d10_1_sha
    local dxvk64_d3d11_sha dxvk64_dxgi_sha
    local dxvk32_d3d9_sha dxvk32_d3d10core_sha dxvk32_d3d10_sha dxvk32_d3d10_1_sha
    local dxvk32_d3d11_sha dxvk32_dxgi_sha
    local dxvkmodern64_d3d11_sha dxvkmodern64_dxgi_sha dxvkmodern32_d3d11_sha dxvkmodern32_dxgi_sha
    dxvk64_d3d9_sha="$(sha256sum "$wine_data/dxvk/legacy/x64/d3d9.dll" | awk '{print $1}')"
    dxvk64_d3d10core_sha="$(sha256sum "$wine_data/dxvk/legacy/x64/d3d10core.dll" | awk '{print $1}')"
    dxvk64_d3d10_sha="$(sha256sum "$wine_data/dxvk/legacy/x64/d3d10.dll" | awk '{print $1}')"
    dxvk64_d3d10_1_sha="$(sha256sum "$wine_data/dxvk/legacy/x64/d3d10_1.dll" | awk '{print $1}')"
    dxvk64_d3d11_sha="$(sha256sum "$wine_data/dxvk/legacy/x64/d3d11.dll" | awk '{print $1}')"
    dxvk64_dxgi_sha="$(sha256sum "$wine_data/dxvk/legacy/x64/dxgi.dll" | awk '{print $1}')"
    dxvk32_d3d9_sha="$(sha256sum "$wine_data/dxvk/legacy/x86/d3d9.dll" | awk '{print $1}')"
    dxvk32_d3d10core_sha="$(sha256sum "$wine_data/dxvk/legacy/x86/d3d10core.dll" | awk '{print $1}')"
    dxvk32_d3d10_sha="$(sha256sum "$wine_data/dxvk/legacy/x86/d3d10.dll" | awk '{print $1}')"
    dxvk32_d3d10_1_sha="$(sha256sum "$wine_data/dxvk/legacy/x86/d3d10_1.dll" | awk '{print $1}')"
    dxvk32_d3d11_sha="$(sha256sum "$wine_data/dxvk/legacy/x86/d3d11.dll" | awk '{print $1}')"
    dxvk32_dxgi_sha="$(sha256sum "$wine_data/dxvk/legacy/x86/dxgi.dll" | awk '{print $1}')"
    dxvkmodern64_d3d11_sha="$(sha256sum "$wine_data/dxvk/modern-2.6/x64/d3d11.dll" | awk '{print $1}')"
    dxvkmodern64_dxgi_sha="$(sha256sum "$wine_data/dxvk/modern-2.6/x64/dxgi.dll" | awk '{print $1}')"
    dxvkmodern32_d3d11_sha="$(sha256sum "$wine_data/dxvk/modern-2.6/x86/d3d11.dll" | awk '{print $1}')"
    dxvkmodern32_dxgi_sha="$(sha256sum "$wine_data/dxvk/modern-2.6/x86/dxgi.dll" | awk '{print $1}')"
    cat > "$wine_data/dxvk/manifest.json" <<EOF
{
  "schemaVersion": 2,
  "backend": "dxvk",
  "defaultProfile": "legacy",
  "runtimeRoot": "dxvk",
  "venusRuntime": {
    "id": "$venus_runtime_id",
    "guestMesaCommit": "$mesa_commit",
    "guestIcdSha256": "$guest_venus_icd_sha",
    "hostVirglrendererCommit": "$virglrenderer_commit",
    "hostVirglrendererSha256": "$host_virglrenderer_sha",
    "transportCapabilities": {
      "remoteMemoryShadow": true,
      "multiRing": false,
      "fenceFeedback": false,
      "queryFeedback": false,
      "semaphoreFeedback": true,
      "modernRequiresSynchronousTimelineQueries": true
    }
  },
  "runtimes": {
    "legacy": {
      "version": "1.10.3",
      "commit": "$dxvk_commit",
      "state": "stable",
      "requiredCapabilities": {"vulkanApi": "1.1", "bcFormats": false, "descriptorIndexing": false},
      "x64": {"d3d9.dll": "$dxvk64_d3d9_sha", "d3d10core.dll": "$dxvk64_d3d10core_sha", "d3d10.dll": "$dxvk64_d3d10_sha", "d3d10_1.dll": "$dxvk64_d3d10_1_sha", "d3d11.dll": "$dxvk64_d3d11_sha", "dxgi.dll": "$dxvk64_dxgi_sha"},
      "x86": {"d3d9.dll": "$dxvk32_d3d9_sha", "d3d10core.dll": "$dxvk32_d3d10core_sha", "d3d10.dll": "$dxvk32_d3d10_sha", "d3d10_1.dll": "$dxvk32_d3d10_1_sha", "d3d11.dll": "$dxvk32_d3d11_sha", "dxgi.dll": "$dxvk32_dxgi_sha"}
    },
    "modern-2.6": {
      "version": "2.6.2",
      "commit": "$dxvk_modern_commit",
      "state": "adapted-game-validated-capability-gated",
      "requiredCapabilities": {"vulkanApi": "1.3", "robustness2": true, "dynamicRendering": true, "maintenance4": true},
      "x64": {"d3d11.dll": "$dxvkmodern64_d3d11_sha", "dxgi.dll": "$dxvkmodern64_dxgi_sha"},
      "x86": {"d3d11.dll": "$dxvkmodern32_d3d11_sha", "dxgi.dll": "$dxvkmodern32_dxgi_sha"}
    }
  }
}
EOF
    cat > "$smoke_dir/manifest.json" <<EOF
{
  "schemaVersion": 1,
  "suiteVersion": "$smoke_suite_version",
  "enabledSuites": ["core", "audio", "opengl", "wine-vulkan", "d3d8", "d3d9", "dxvk", "dxvk-modern-baseline", "gpu-diagnostics", "dxvk26-requirements", "d3d12"],
  "managedRoot": "C:\\\\smoke",
  "files": {
    "x64/winehua_audio_smoke.exe": "$audio64_sha",
    "x64/winehua_graphics_smoke.exe": "$graphics64_sha",
    "x64/winehua_vulkan_smoke.exe": "$vulkan64_sha",
    "x64/winehua_d3d11_smoke.exe": "$d3d1164_sha",
    "x64/winehua_d3d8_smoke.exe": "$d3d864_sha",
    "x64/winehua_d3d_switch_cube.exe": "$cube64_sha",
    "x64/winehua_media_smoke.exe": "$media64_sha",
    "x64/winehua_gpu_diagnostics.exe": "$diagnostics64_sha",
    "x64/winehua_win32_driver.exe": "$driver64_sha",
    "x64/winehua_dxvk26_requirements.exe": "$requirements64_sha",
    "x64/winehua_d3d12_smoke.exe": "$vkd3d64_smoke_sha",
    "x64/triangle.exe": "$vkd3d_triangle_sha",
    "x64/gears.exe": "$vkd3d_gears_sha",
    "x86/winehua_audio_smoke.exe": "$audio32_sha",
    "x86/winehua_graphics_smoke.exe": "$graphics32_sha",
    "x86/winehua_vulkan_smoke.exe": "$vulkan32_sha",
    "x86/winehua_d3d11_smoke.exe": "$d3d1132_sha",
    "x86/winehua_d3d8_smoke.exe": "$d3d832_sha",
    "x86/winehua_d3d_switch_cube.exe": "$cube32_sha",
    "x86/winehua_media_smoke.exe": "$media32_sha",
    "x86/winehua_gpu_diagnostics.exe": "$diagnostics32_sha",
    "x86/winehua_win32_driver.exe": "$driver32_sha",
    "x86/winehua_dxvk26_requirements.exe": "$requirements32_sha",
    "assets/venus_storage_write.spv": "$storage_write_sha",
    "assets/venus_storage_read.spv": "$storage_read_sha",
    "assets/venus_image_fetch.spv": "$image_fetch_sha",
    "assets/venus_combined_sample.spv": "$combined_sample_sha",
    "assets/venus_separated_sample.spv": "$separated_sample_sha"
  }
}
EOF
    # Suite 编排定义: companion of manifest.json, consumed by SmokeRunner.ets.
    # 每 suite: tests[] → testId/exe(相对 C:\smoke 根: x64/… 或 x86/…,
    # 载荷打包成 smoke/{x64,x86}, 播种到 C:\smoke 后即根级子目录; 无
    # smoke/ 前缀 — runner 拼 C:\smoke\ + exe)/env(测试专属
    # 诊断键)/d3dBackend(回归固定后端)/mode(present|offscreen)/seconds/timeoutMs
    # (-1=取请求 longSeconds)。产品语义 env (DXVK 稳定化 overlay/perf profile)
    # 由 native BuildSessionEnv 收口, 不在此重复; argv 协议由 runner 生成。
    cat > "$smoke_dir/suites.json" <<SMOKE_SUITES_EOF
{
  "schemaVersion": 1,
  "suiteVersion": "$smoke_suite_version",
  "suites": {
    "core": {
      "tests": [
        {"testId": "opengl-x64", "exe": "x64/winehua_graphics_smoke.exe", "env": {}, "d3dBackend": "wined3d", "seconds": 8, "timeoutMs": 60000},
        {"testId": "opengl-x86", "exe": "x86/winehua_graphics_smoke.exe", "env": {}, "d3dBackend": "wined3d", "seconds": 8, "timeoutMs": 60000}
      ]
    },
    "opengl": {
      "tests": [
        {"testId": "opengl-x64", "exe": "x64/winehua_graphics_smoke.exe", "env": {}, "d3dBackend": "wined3d", "seconds": 8, "timeoutMs": 60000},
        {"testId": "opengl-x86", "exe": "x86/winehua_graphics_smoke.exe", "env": {}, "d3dBackend": "wined3d", "seconds": 8, "timeoutMs": 60000}
      ]
    },
    "audio": {
      "tests": [
        {"testId": "audio-x64", "exe": "x64/winehua_audio_smoke.exe", "env": {}, "d3dBackend": "wined3d", "seconds": 3, "timeoutMs": 45000},
        {"testId": "audio-x86", "exe": "x86/winehua_audio_smoke.exe", "env": {}, "d3dBackend": "wined3d", "seconds": 3, "timeoutMs": 45000}
      ]
    },
    "d3d8": {
      "tests": [
        {"testId": "d3d8-capability-x86", "exe": "x86/winehua_d3d8_smoke.exe", "env": {}, "d3dBackend": "wined3d", "seconds": 5, "timeoutMs": 180000},
        {"testId": "d3d8-capability-x64", "exe": "x64/winehua_d3d8_smoke.exe", "env": {}, "d3dBackend": "wined3d", "seconds": 5, "timeoutMs": 180000}
      ]
    },
    "d3d9": {
      "tests": [
        {"testId": "d3d9-cube-x86", "exe": "x86/winehua_d3d_switch_cube.exe", "env": {}, "d3dBackend": "wined3d", "extraArgs": ["--d3d9"], "seconds": 8, "timeoutMs": 180000},
        {"testId": "d3d9-cube-x64", "exe": "x64/winehua_d3d_switch_cube.exe", "env": {}, "d3dBackend": "wined3d", "extraArgs": ["--d3d9"], "seconds": 8, "timeoutMs": 180000}
      ]
    },
    "wine-vulkan": {
      "tests": [
        {"testId": "wine-vulkan-offscreen-x64", "exe": "x64/winehua_vulkan_smoke.exe", "env": {"WINEHUA_SMOKE_ASSETS": "C:/smoke/assets", "WINEHUA_VULKAN_RUNTIME": "1"}, "d3dBackend": "wined3d", "mode": "offscreen", "seconds": 0, "timeoutMs": 90000},
        {"testId": "wine-vulkan-offscreen-x86", "exe": "x86/winehua_vulkan_smoke.exe", "env": {"WINEHUA_SMOKE_ASSETS": "C:/smoke/assets", "WINEHUA_VULKAN_RUNTIME": "1"}, "d3dBackend": "wined3d", "mode": "offscreen", "seconds": 0, "timeoutMs": 90000},
        {"testId": "wine-vulkan-sampled-only-x64", "exe": "x64/winehua_vulkan_smoke.exe", "env": {"WINEHUA_SMOKE_ASSETS": "C:/smoke/assets", "WINEHUA_VULKAN_RUNTIME": "1", "WINEHUA_VULKAN_SAMPLED_ONLY": "1"}, "d3dBackend": "wined3d", "mode": "offscreen", "seconds": 0, "timeoutMs": 90000},
        {"testId": "wine-vulkan-sampled-only-x86", "exe": "x86/winehua_vulkan_smoke.exe", "env": {"WINEHUA_SMOKE_ASSETS": "C:/smoke/assets", "WINEHUA_VULKAN_RUNTIME": "1", "WINEHUA_VULKAN_SAMPLED_ONLY": "1"}, "d3dBackend": "wined3d", "mode": "offscreen", "seconds": 0, "timeoutMs": 90000}
      ]
    },
    "wine-vulkan-present": {
      "tests": [
        {"testId": "wine-vulkan-present-x64", "exe": "x64/winehua_vulkan_smoke.exe", "env": {"WINEHUA_SMOKE_ASSETS": "C:/smoke/assets", "WINEHUA_VULKAN_RUNTIME": "1"}, "d3dBackend": "wined3d", "seconds": 5, "timeoutMs": 180000},
        {"testId": "wine-vulkan-present-x86", "exe": "x86/winehua_vulkan_smoke.exe", "env": {"WINEHUA_SMOKE_ASSETS": "C:/smoke/assets", "WINEHUA_VULKAN_RUNTIME": "1"}, "d3dBackend": "wined3d", "seconds": 5, "timeoutMs": 180000}
      ]
    },
    "dxvk": {
      "tests": [
        {"testId": "dxvk-legacy-x86", "exe": "x86/winehua_d3d11_smoke.exe", "env": {"WINEDEBUG": "+loaddll,+module"}, "d3dBackend": "dxvk_legacy", "seconds": 5, "timeoutMs": 180000},
        {"testId": "dxvk-legacy-x64", "exe": "x64/winehua_d3d11_smoke.exe", "env": {"WINEDEBUG": "+loaddll,+module"}, "d3dBackend": "dxvk_legacy", "seconds": 5, "timeoutMs": 180000},
        {"testId": "dxvk-cube-x64", "exe": "x64/winehua_d3d_switch_cube.exe", "env": {"WINEDEBUG": "+loaddll,+module"}, "d3dBackend": "dxvk_legacy", "seconds": 8, "timeoutMs": 180000}
      ]
    },
    "dxvk-dynamic": {
      "tests": [
        {"testId": "dxvk-dynamic-cb-x86", "exe": "x86/winehua_d3d11_smoke.exe", "env": {"WINEDEBUG": "+loaddll,+module"}, "d3dBackend": "dxvk_legacy", "seconds": 5, "timeoutMs": 180000},
        {"testId": "dxvk-dynamic-cb-x64", "exe": "x64/winehua_d3d11_smoke.exe", "env": {"WINEDEBUG": "+loaddll,+module"}, "d3dBackend": "dxvk_legacy", "seconds": 5, "timeoutMs": 180000}
      ]
    },
    "dxvk-long": {
      "tests": [
        {"testId": "dxvk-long-x64", "exe": "x64/winehua_d3d11_smoke.exe", "env": {"WINEDEBUG": "+loaddll,+module"}, "d3dBackend": "dxvk_legacy", "seconds": -1, "timeoutMs": -1}
      ]
    },
    "dxvk-modern-baseline": {
      "tests": [
        {"testId": "dxvk-modern-baseline-x86", "exe": "x86/winehua_d3d11_smoke.exe", "env": {"WINEDEBUG": "+loaddll,+module", "DXVK_WINEHUA_TRACE_SAMPLED": "0", "DXVK_WINEHUA_TRACE_FLOW": "0", "DXVK_WINEHUA_TRACE_API": "0"}, "d3dBackend": "dxvk_modern_2_6", "seconds": 5, "timeoutMs": 180000},
        {"testId": "dxvk-modern-baseline-x64", "exe": "x64/winehua_d3d11_smoke.exe", "env": {"WINEDEBUG": "+loaddll,+module", "DXVK_WINEHUA_TRACE_SAMPLED": "0", "DXVK_WINEHUA_TRACE_FLOW": "0", "DXVK_WINEHUA_TRACE_API": "0"}, "d3dBackend": "dxvk_modern_2_6", "seconds": 5, "timeoutMs": 180000},
        {"testId": "dxvk-modern-cube-x64", "exe": "x64/winehua_d3d_switch_cube.exe", "env": {"WINEDEBUG": "+loaddll,+module", "DXVK_WINEHUA_TRACE_SAMPLED": "0", "DXVK_WINEHUA_TRACE_FLOW": "0", "DXVK_WINEHUA_TRACE_API": "0"}, "d3dBackend": "dxvk_modern_2_6", "seconds": 8, "timeoutMs": 180000}
      ]
    },
    "dxvk-modern-long": {
      "tests": [
        {"testId": "dxvk-modern-long-x64", "exe": "x64/winehua_d3d11_smoke.exe", "env": {"WINEDEBUG": "+loaddll,+module", "DXVK_WINEHUA_TRACE_SAMPLED": "0", "DXVK_WINEHUA_TRACE_FLOW": "0", "DXVK_WINEHUA_TRACE_API": "0"}, "d3dBackend": "dxvk_modern_2_6", "seconds": -1, "timeoutMs": -1}
      ]
    },
    "gpu-diagnostics": {
      "tests": [
        {"testId": "gpu-diagnostics-x86", "exe": "x86/winehua_gpu_diagnostics.exe", "env": {}, "d3dBackend": "dxvk_legacy", "seconds": 0, "timeoutMs": 90000},
        {"testId": "gpu-diagnostics-x64", "exe": "x64/winehua_gpu_diagnostics.exe", "env": {}, "d3dBackend": "dxvk_legacy", "seconds": 0, "timeoutMs": 90000}
      ]
    },
    "dxvk26-requirements": {
      "tests": [
        {"testId": "dxvk26-requirements-x86", "exe": "x86/winehua_dxvk26_requirements.exe", "env": {}, "d3dBackend": "wined3d", "seconds": 0, "timeoutMs": 90000},
        {"testId": "dxvk26-requirements-x64", "exe": "x64/winehua_dxvk26_requirements.exe", "env": {}, "d3dBackend": "wined3d", "seconds": 0, "timeoutMs": 90000}
      ]
    },
    "d3d12": {
      "tests": [
        {"testId": "d3d12-1000f", "exe": "x64/winehua_d3d12_smoke.exe", "env": {}, "d3dBackend": "vkd3d_limited_500k", "argvMode": "raw",
         "argv": ["--frames", "1000", "--result", "C:/smoke/results/<run-id>/<test-id>.json",
                  "--checkpoint", "C:/smoke/results/<run-id>/<test-id>.ckpt"],
         "seconds": 0, "timeoutMs": 180000}
      ]
    },
    "all": {
      "tests": [
        {"testId": "audio-x64", "exe": "x64/winehua_audio_smoke.exe", "env": {}, "d3dBackend": "wined3d", "seconds": 3, "timeoutMs": 45000},
        {"testId": "audio-x86", "exe": "x86/winehua_audio_smoke.exe", "env": {}, "d3dBackend": "wined3d", "seconds": 3, "timeoutMs": 45000},
        {"testId": "opengl-x64", "exe": "x64/winehua_graphics_smoke.exe", "env": {}, "d3dBackend": "wined3d", "seconds": 8, "timeoutMs": 60000},
        {"testId": "opengl-x86", "exe": "x86/winehua_graphics_smoke.exe", "env": {}, "d3dBackend": "wined3d", "seconds": 8, "timeoutMs": 60000},
        {"testId": "d3d8-capability-x86", "exe": "x86/winehua_d3d8_smoke.exe", "env": {}, "d3dBackend": "wined3d", "seconds": 5, "timeoutMs": 180000},
        {"testId": "d3d8-capability-x64", "exe": "x64/winehua_d3d8_smoke.exe", "env": {}, "d3dBackend": "wined3d", "seconds": 5, "timeoutMs": 180000},
        {"testId": "d3d9-cube-x86", "exe": "x86/winehua_d3d_switch_cube.exe", "env": {}, "d3dBackend": "wined3d", "extraArgs": ["--d3d9"], "seconds": 8, "timeoutMs": 180000},
        {"testId": "d3d9-cube-x64", "exe": "x64/winehua_d3d_switch_cube.exe", "env": {}, "d3dBackend": "wined3d", "extraArgs": ["--d3d9"], "seconds": 8, "timeoutMs": 180000},
        {"testId": "wine-vulkan-offscreen-x64", "exe": "x64/winehua_vulkan_smoke.exe", "env": {"WINEHUA_SMOKE_ASSETS": "C:/smoke/assets", "WINEHUA_VULKAN_RUNTIME": "1"}, "d3dBackend": "wined3d", "mode": "offscreen", "seconds": 0, "timeoutMs": 90000},
        {"testId": "wine-vulkan-offscreen-x86", "exe": "x86/winehua_vulkan_smoke.exe", "env": {"WINEHUA_SMOKE_ASSETS": "C:/smoke/assets", "WINEHUA_VULKAN_RUNTIME": "1"}, "d3dBackend": "wined3d", "mode": "offscreen", "seconds": 0, "timeoutMs": 90000},
        {"testId": "wine-vulkan-present-x64", "exe": "x64/winehua_vulkan_smoke.exe", "env": {"WINEHUA_SMOKE_ASSETS": "C:/smoke/assets", "WINEHUA_VULKAN_RUNTIME": "1"}, "d3dBackend": "wined3d", "seconds": 5, "timeoutMs": 180000},
        {"testId": "wine-vulkan-present-x86", "exe": "x86/winehua_vulkan_smoke.exe", "env": {"WINEHUA_SMOKE_ASSETS": "C:/smoke/assets", "WINEHUA_VULKAN_RUNTIME": "1"}, "d3dBackend": "wined3d", "seconds": 5, "timeoutMs": 180000},
        {"testId": "dxvk-legacy-x86", "exe": "x86/winehua_d3d11_smoke.exe", "env": {"WINEDEBUG": "+loaddll,+module"}, "d3dBackend": "dxvk_legacy", "seconds": 5, "timeoutMs": 180000},
        {"testId": "dxvk-legacy-x64", "exe": "x64/winehua_d3d11_smoke.exe", "env": {"WINEDEBUG": "+loaddll,+module"}, "d3dBackend": "dxvk_legacy", "seconds": 5, "timeoutMs": 180000},
        {"testId": "dxvk-cube-x64", "exe": "x64/winehua_d3d_switch_cube.exe", "env": {"WINEDEBUG": "+loaddll,+module"}, "d3dBackend": "dxvk_legacy", "seconds": 8, "timeoutMs": 180000}
      ]
    },
    "long": {
      "tests": [
        {"testId": "audio-x64", "exe": "x64/winehua_audio_smoke.exe", "env": {}, "d3dBackend": "wined3d", "seconds": 3, "timeoutMs": 45000},
        {"testId": "audio-x86", "exe": "x86/winehua_audio_smoke.exe", "env": {}, "d3dBackend": "wined3d", "seconds": 3, "timeoutMs": 45000},
        {"testId": "opengl-x64", "exe": "x64/winehua_graphics_smoke.exe", "env": {}, "d3dBackend": "wined3d", "seconds": 3600, "timeoutMs": 3660000},
        {"testId": "opengl-x86", "exe": "x86/winehua_graphics_smoke.exe", "env": {}, "d3dBackend": "wined3d", "seconds": 3600, "timeoutMs": 3660000},
        {"testId": "dxvk-long-x64", "exe": "x64/winehua_d3d11_smoke.exe", "env": {"WINEDEBUG": "+loaddll,+module"}, "d3dBackend": "dxvk_legacy", "seconds": -1, "timeoutMs": -1}
      ]
    }
  }
}
SMOKE_SUITES_EOF
    log "  smoke suite definitions → smoke/suites.json ($smoke_suite_version)"
    log "  VKD3D-Proton 2.6 limited-500K (default mixed D3D12 profile) → vkd3d/limited-500k/x64 (sha256=$vkd3d64_d3d12_sha)"
    log "  product media/network smoke assets retained"

    # fonts
    cp "$WINE_SRC/fonts/"*.ttf "$wine_data/share/wine/fonts/"
    # NLS
    cp "$BUILD_DIR/wine-ohos/nls/"*.nls "$wine_data/share/wine/nls/"
    # winmd
    cp "$BUILD_DIR/wine-ohos/include/"*.winmd "$wine_data/share/wine/winmd/"
    # Wine Mono (.NET 运行时)
    if ls "$BUILD_DIR/wine-ohos/share/wine/mono/"*.msi >/dev/null 2>&1; then
        cp "$BUILD_DIR/wine-ohos/share/wine/mono/"*.msi "$wine_data/share/wine/mono/"
        log "    wine-mono.msi → rawfile share/wine/mono/"
    elif ls "$wine_data/bin/x86_64-windows/"appwiz.cpl >/dev/null 2>&1; then
        err "appwiz.cpl packaged but wine-mono MSI missing; rebuild with BUILD_WINE_MONO=1"
    fi
    # Wine Gecko (IE HTML 渲染引擎): 缺它 IE 打开网页报
    # "Could not find Wine Gecko. HTML rendering will be disabled."
    # 与 wine-mono 同源策略: 由构建环境预置 MSI, 首次 wineboot 自动注册。
    if ls "$BUILD_DIR/wine-ohos/share/wine/gecko/"*.msi >/dev/null 2>&1; then
        cp "$BUILD_DIR/wine-ohos/share/wine/gecko/"*.msi "$wine_data/share/wine/gecko/"
        log "    wine-gecko.msi → rawfile share/wine/gecko/"
    else
        log "warn: wine-gecko MSI 缺失, IE HTML 渲染将不可用"
    fi
    # wine.inf (含 OHOS font substitutes)
    cp "$BUILD_DIR/wine-ohos/loader/wine.inf" "$wine_data/share/wine/"
    sed_i '/^\[MCI\]$/i\
;; OHOS font substitutes\
HKLM,%FontSubStr%,"System",,"HarmonyOS Sans SC"\
HKLM,%FontSubStr%,"MS Sans Serif",,"HarmonyOS Sans SC"\
HKLM,%FontSubStr%,"MS Shell Dlg",,"HarmonyOS Sans SC"\
HKLM,%FontSubStr%,"MS Shell Dlg 2",,"HarmonyOS Sans SC"\
HKLM,%FontSubStr%,"Arial",,"HarmonyOS Sans SC"\
HKLM,%FontSubStr%,"Arial Black",,"HarmonyOS Sans SC"\
HKLM,%FontSubStr%,"Calibri",,"HarmonyOS Sans SC"\
HKLM,%FontSubStr%,"Cambria",,"Noto Serif"\
HKLM,%FontSubStr%,"Candara",,"HarmonyOS Sans SC"\
HKLM,%FontSubStr%,"Comic Sans MS",,"HarmonyOS Sans SC"\
HKLM,%FontSubStr%,"Constantia",,"Noto Serif"\
HKLM,%FontSubStr%,"Corbel",,"HarmonyOS Sans SC"\
HKLM,%FontSubStr%,"Impact",,"HarmonyOS Sans SC"\
HKLM,%FontSubStr%,"Palatino Linotype",,"Noto Serif"\
HKLM,%FontSubStr%,"Segoe UI",,"HarmonyOS Sans SC"\
HKLM,%FontSubStr%,"Tahoma",,"HarmonyOS Sans SC"\
HKLM,%FontSubStr%,"Trebuchet MS",,"HarmonyOS Sans SC"\
HKLM,%FontSubStr%,"Verdana",,"HarmonyOS Sans SC"\
;; Latin: 衬线 (serif)\
HKLM,%FontSubStr%,"Georgia",,"Noto Serif"\
HKLM,%FontSubStr%,"Times New Roman",,"Noto Serif"\
;; CJK: 简体中文\
HKLM,%FontSubStr%,"Microsoft JhengHei",,"HarmonyOS Sans TC"\
HKLM,%FontSubStr%,"Microsoft JhengHei UI",,"HarmonyOS Sans TC"\
HKLM,%FontSubStr%,"Microsoft YaHei",,"HarmonyOS Sans SC"\
HKLM,%FontSubStr%,"Microsoft YaHei UI",,"HarmonyOS Sans SC"\
;; CJK: 宋体/楷体 (serif)\
HKLM,%FontSubStr%,"SimSun",,"Noto Serif CJK SC"\
HKLM,%FontSubStr%,"NSimSun",,"Noto Serif CJK SC"\
HKLM,%FontSubStr%,"SimHei",,"HarmonyOS Sans SC"\
HKLM,%FontSubStr%,"FangSong",,"Noto Serif CJK SC"\
HKLM,%FontSubStr%,"KaiTi",,"Noto Serif CJK SC"\
HKLM,%FontSubStr%,"YouYuan",,"HarmonyOS Sans SC"\
HKLM,%FontSubStr%,"LiSu",,"HarmonyOS Sans SC"\
HKLM,%FontSubStr%,"DengXian",,"HarmonyOS Sans SC"\
HKLM,%FontSubStr%,"STSong",,"Noto Serif CJK SC"\
HKLM,%FontSubStr%,"STKaiti",,"Noto Serif CJK SC"\
HKLM,%FontSubStr%,"STFangsong",,"Noto Serif CJK SC"\
HKLM,%FontSubStr%,"STHeiti",,"HarmonyOS Sans SC"\
HKLM,%FontSubStr%,"STXihei",,"HarmonyOS Sans SC"\
HKLM,%FontSubStr%,"STLiti",,"HarmonyOS Sans SC"\
HKLM,%FontSubStr%,"STXingkai",,"HarmonyOS Sans SC"\
HKLM,%FontSubStr%,"STXinwei",,"HarmonyOS Sans SC"\
HKLM,%FontSubStr%,"STHupo",,"HarmonyOS Sans SC"\
HKLM,%FontSubStr%,"STCaiyun",,"HarmonyOS Sans SC"\
HKLM,%FontSubStr%,"STZhongSong",,"Noto Serif CJK SC"\
HKLM,%FontSubStr%,"STBaoli",,"Noto Serif CJK SC"\
HKLM,%FontSubStr%,"FZShuTi",,"HarmonyOS Sans SC"\
HKLM,%FontSubStr%,"FZYaoti",,"HarmonyOS Sans SC"\
;; CJK: 繁体中文\
HKLM,%FontSubStr%,"MingLiU",,"HarmonyOS Sans TC"\
HKLM,%FontSubStr%,"PMingLiU",,"HarmonyOS Sans TC"\
HKLM,%FontSubStr%,"DFKai-SB",,"Noto Serif CJK TC"\
HKLM,%FontSubStr%,"Consolas",,"Noto Sans Mono"\
HKLM,%FontSubStr%,"Courier",,"Noto Sans Mono"\
HKLM,%FontSubStr%,"Courier New",,"Noto Sans Mono"\
HKLM,%FontSubStr%,"Fixedsys",,"Noto Sans Mono"\
HKLM,%FontSubStr%,"Lucida Console",,"Noto Sans Mono"' "$wine_data/share/wine/wine.inf"
    # XKB
    if [ -d "$SYSROOT_EXT_SHARE/X11/xkb" ]; then
        cp -r "$SYSROOT_EXT_SHARE/X11/xkb" "$wine_data/share/X11/"
    fi

    # guest GPU 库 (Mesa/VirGL, 供 GraphicsBroker 注入到 Wine LD_LIBRARY_PATH)
    if [ -d "$BUILD_DIR/guest_gfx/$guest_arch/lib" ]; then
        mkdir -p "$wine_data/bin/guest_gfx"
        cp -a "$BUILD_DIR/guest_gfx/$guest_arch/"* "$wine_data/bin/guest_gfx/"
        log "  guest_gfx ($guest_arch): $(ls "$wine_data/bin/guest_gfx/lib"/*.so* 2>/dev/null | wc -l) .so files"

        # On an x86_64 HarmonyOS device the platform linker rejects dlopen()
        # from the writable app sandbox, even when the extracted files are
        # executable. Put the Mesa receiver in the HAP native-lib namespace as
        # well. The ARM package keeps the x86_64 receiver only in wine-data,
        # because Box64 rather than the platform linker loads those files.
        if [ "$NATIVE_ARCH" = "x86_64" ]; then
            local guest_lib="$BUILD_DIR/guest_gfx/$guest_arch/lib"
            local guest_name
            # Never expose guest EGL/GLES/DRM under their standard names in
            # the HAP. Those names can override the system libraries used by
            # libentry/libvirglrenderer and crash the emulator host renderer.
            rm -f "$NATIVE_LIBS"/libEGL.so "$NATIVE_LIBS"/libEGL.so.1 \
                  "$NATIVE_LIBS"/libGLESv1_CM.so "$NATIVE_LIBS"/libGLESv1_CM.so.1 \
                  "$NATIVE_LIBS"/libGLESv2.so "$NATIVE_LIBS"/libGLESv2.so.2 \
                  "$NATIVE_LIBS"/libdrm.so "$NATIVE_LIBS"/libdrm.so.2 \
                  "$NATIVE_LIBS"/libwinehua_guest_EGL.so
            [ -f "$guest_lib/libEGL.so" ] || err "missing x86 bundled guest gfx library: libEGL.so"
            [ -f "$guest_lib/libgallium-25.0.1.so" ] || err "missing x86 bundled guest gfx library: libgallium-25.0.1.so"
            cp -a "$guest_lib/libEGL.so" "$NATIVE_LIBS/libwinehua_guest_EGL.so"
            cp -a "$guest_lib/libgallium-25.0.1.so" "$NATIVE_LIBS/libgallium-25.0.1.so"
            # Mesa selects swrast as its DRI frontend and GALLIUM_DRIVER=virpipe
            # as the actual renderer. Keep all public filenames expected by
            # Mesa even though these generated entrypoints are identical.
            for guest_name in kms_swrast_dri.so swrast_dri.so virtio_gpu_dri.so; do
                [ -f "$guest_lib/dri/$guest_name" ] || err "missing x86 bundled DRI driver: $guest_name"
                cp -a "$guest_lib/dri/$guest_name" "$NATIVE_LIBS/$guest_name"
            done
            log "  guest_gfx (x86_64): uniquely named EGL + Mesa DRI mirrored into HAP native libs"
        fi
    else
        if [ "${BUILD_GUEST_GFX:-0}" = "1" ]; then
            err "BUILD_GUEST_GFX=1 but build/guest_gfx/$guest_arch/lib is missing"
        fi
        log "  guest_gfx: SKIP (build/guest_gfx/$guest_arch/lib not found)"
    fi

    # Guest Linux Vulkan runtime is intentionally outside C:\\smoke: it is an
    # x86_64 OHOS ELF/Loader/ICD stack launched through Box64 for the B1 gate.
    if [ -f "$BUILD_DIR/guest_vulkan/$guest_arch/manifest.json" ]; then
        mkdir -p "$wine_data/bin/guest_vulkan"
        cp -a "$BUILD_DIR/guest_vulkan/$guest_arch/"* "$wine_data/bin/guest_vulkan/"
        log "  guest_vulkan ($guest_arch): Loader + Venus ICD + offscreen smoke"
    elif [ "${BUILD_GUEST_VULKAN:-0}" = "1" ]; then
        err "BUILD_GUEST_VULKAN=1 but build/guest_vulkan/$guest_arch/manifest.json is missing"
    else
        log "  guest_vulkan: SKIP"
    fi

    # Native offscreen replay runs in the App/NCP security domain and links the
    # system Host Vulkan loader. Captured resources remain in guest_vulkan so
    # there is one authoritative exact-replay input set for the Host/Venus A/B.
    local host_vulkan_root="$BUILD_DIR/host_vulkan/$NATIVE_ARCH"
    [ -f "$host_vulkan_root/manifest.json" ] || \
        err "Host Vulkan replay manifest missing: $host_vulkan_root/manifest.json"
    [ -f "$host_vulkan_root/bin/heaven_exact_host_replay" ] || \
        err "Host Vulkan replay marker missing: $host_vulkan_root/bin/heaven_exact_host_replay"
    [ -f "$host_vulkan_root/lib/libwinehua_host_heaven_replay.so" ] || \
        err "Host Vulkan replay module missing: $host_vulkan_root/lib/libwinehua_host_heaven_replay.so"
    mkdir -p "$wine_data/bin/host_vulkan"
    cp -a "$host_vulkan_root/"* "$wine_data/bin/host_vulkan/"
    log "  host_vulkan ($NATIVE_ARCH): native exact replay"

    # -- 3. 打包 zip → rawfile (不带 wine-data/ 前缀) --
    local rawfile_dir="$WINEHUA/entry/src/main/resources/rawfile"
    mkdir -p "$rawfile_dir"
    local zip_name="wine-data.zip"
    cd "$wine_data"
    # Runtime identity must describe extracted file content, not ZIP containe
    # metadata. zip records mtimes, so hashing the archive made two identical
    # builds look different and forced a full 466 MB extract plus wineboot
    # prefix update on every install.
    local runtime_content_sha
    runtime_content_sha="$(find . -type f -print0 | LC_ALL=C sort -z | xargs -0 sha256sum | sha256sum | awk '{print $1}')"
    [ -n "$runtime_content_sha" ] || err "failed to compute semantic wine runtime content hash"
    rm -f "$STAGING_DIR/$zip_name"
    zip -r "$STAGING_DIR/$zip_name" . -x '*.git*'
    cp "$STAGING_DIR/$zip_name" "$rawfile_dir/"
    local payload_sha
    payload_sha="$(sha256sum "$rawfile_dir/$zip_name" | awk '{print $1}')"
    cat > "$rawfile_dir/wine-runtime-manifest.json" <<EOF
{
  "schemaVersion": 1,
  "payload": "wine-data.zip",
  "payloadSha256": "$payload_sha",
  "smokeSuiteVersion": "$smoke_suite_version"
}
EOF
    # 自动生成运行时版本标记：解压后的语义内容哈希 + 影响 runtime/prefix 的
    # 子模块 git SHA。payloadSha256 仍校验实际 ZIP；设备端版本比较则不受 ZIP
    # 时间戳影响，避免内容未变也重复解压。该文件是纯构建产物，不要手动提交。
    local runtime_version
    runtime_version="content=${runtime_content_sha:0:16}"
    # 容器内以 root 跑 git 会因 dubious ownership 拒绝仓库；先幂等豁免。
    # 优先从 superproject 读 gitlink SHA（不依赖子模块自己的 .git，该 .git
    # 文件指向宿主绝对路径，容器内不可解析）。
    git config --global --add safe.directory "$WINEHUA" 2>/dev/null || true
    for m in wine box64 mesa virglrenderer gstreamer gnutls glib pcre2 dxvk; do
        local m_sha
        m_sha="$(git -C "$WINEHUA" ls-tree HEAD "thirdparty/$m" 2>/dev/null | awk '{print substr($3,1,10)}')"
        if [ -z "$m_sha" ]; then
            m_sha="$(git -C "$WINEHUA/thirdparty/$m" rev-parse --short HEAD 2>/dev/null || true)"
        fi
        [ -n "$m_sha" ] || m_sha="none"
        runtime_version="${runtime_version};${m}=${m_sha}"
    done
    printf '%s\n' "$runtime_version" > "$rawfile_dir/wine_runtime.version"
    log "  wine_runtime.version → rawfile/ ($runtime_version)"
    log "  $zip_name → rawfile/ ($(du -h "$rawfile_dir/$zip_name" | cut -f1))"

    log "Pad 布局组装完成 ($NATIVE_ARCH)"
    echo ""
    echo "  libs/$NATIVE_ARCH/"
    ls -la "$NATIVE_LIBS/" 2>/dev/null || echo "    (empty)"
    echo "  rawfile/$zip_name"
}

log "=== 组装布局 ($NATIVE_ARCH) ==="

# 统一使用 rawfile zip 布局
assemble_pad
