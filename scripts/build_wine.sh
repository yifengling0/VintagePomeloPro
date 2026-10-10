#!/bin/bash
# build_wine.sh — Wine 交叉编译
set -euo pipefail
SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
case "${1:-}" in
    --check-identity|--check-cached-identity) export WINE_IDENTITY_CHECK_ONLY=1 ;;
esac
source "$SCRIPT_DIR/env.sh"

# Fresh targeted Unix builds keep the same overlay/identity/configure checks.
# No Wine objects or generated Makefiles are copied from another build.
WINE_UNIX_MODULES=""
WINE_PE_MODULES=""
if [ "${1:-}" = --unix-modules ]; then
    shift
    for module in "$@"; do
        case "$module" in
            ntdll|win32u|opengl32|winevulkan|winewayland.drv|wineserver) ;;
            *) echo "unsupported Unix module: $module" >&2; exit 1 ;;
        esac
        WINE_UNIX_MODULES="${WINE_UNIX_MODULES:+$WINE_UNIX_MODULES }$module"
    done
    [ -n "$WINE_UNIX_MODULES" ] || { echo "--unix-modules needs module names" >&2; exit 1; }
fi

# Fresh targeted PE builds use the same source, overlay and compiler identity.
if [ "${1:-}" = --pe-modules ]; then
    shift
    for module in "$@"; do
        case "$module" in
            wined3d|d3d9) ;;
            *) echo "unsupported PE module: $module" >&2; exit 1 ;;
        esac
        WINE_PE_MODULES="${WINE_PE_MODULES:+$WINE_PE_MODULES }$module"
    done
    [ -n "$WINE_PE_MODULES" ] || { echo "--pe-modules needs module names" >&2; exit 1; }
fi

wine_build_identity() {
    local mode="$1"
    shift
    python3 "$SCRIPT_DIR/wine_build_identity.py" "$mode" "$@" \
        --source "$WINE_SRC" --build "$BUILD_DIR" --repo "$SCRIPT_DIR/.." \
        --config "WINE_ARCH=$WINE_ARCH" --config "NATIVE_ARCH=$NATIVE_ARCH" \
        --config "GUEST_ARCH=$GUEST_ARCH" --config "HOST_TRIPLE=$HOST_TRIPLE" \
        --config "HOST_OS=$HOST_OS" --config "TARGET=$TARGET" \
        --config "WINE_UNIX_MODULES=$WINE_UNIX_MODULES" --config "WINE_PE_MODULES=$WINE_PE_MODULES" \
        --config "SYSROOT=$SYSROOT" --config "LLVM_MINGW=$LLVM_MINGW" \
        --config "CFLAGS=${CFLAGS:-}" --config "CXXFLAGS=${CXXFLAGS:-}" \
        --config "LDFLAGS=${LDFLAGS:-}" --config "CROSSCFLAGS=${CROSSCFLAGS:-}" \
        --config "CC=${CC:-}" --config "CXX=${CXX:-}" --config "AR=${AR:-}" \
        --config "PKG_CONFIG_BIN=$PKG_CONFIG_BIN" --tool "$CLANG"
}
# Fail before applying overlays or generating files in any source tree. A
# foreign/stale cache is never repaired by resetting source or deleting cache.
if [ "${1:-}" = --check-cached-identity ]; then
    wine_build_identity check --require-manifest
    exit 0
fi
wine_build_identity check
if [ "${1:-}" = --check-identity ]; then exit 0; fi

ensure_wine_patch() {
    local patch_file="$1" description="$2"
    # Docker mounts this managed worktree without the external Git metadata
    # referenced by its .git file.  patch checks the source tree directly.
    # GNU patch may silently undo -R when it recognizes an unpatched source.
    # Force the requested direction so that detection cannot report a clean
    # checkout as "already applied".
    if patch -d "$WINE_SRC" -p1 --batch --force --dry-run -R < "$patch_file" >/dev/null 2>&1; then
        log "$description already applied"
    else
        # Later Direct overlays change code introduced by 0009/0056. Prove
        # the stack on a copy, reversing only overlays actually present there.
        # Never reverse/reset the caller's possibly dirty source tree.
        local early_file="${patch_file%/*}/0056-wayland-direct-wsi-before-show.patch"
        local drawable_file="${patch_file%/*}/0057-wayland-direct-drawable-identity.patch"
        if [[ "${patch_file##*/}" = 0009-direct-ohos-wsi-and-resize-smoke.patch ||
              "${patch_file##*/}" = 0056-wayland-direct-wsi-before-show.patch ]]; then
            if (
                verify_tree=$(mktemp -d -t wine-overlay-proof-XXXXXXXX)
                trap 'rm -rf -- "$verify_tree"' EXIT
                while IFS= read -r relative; do
                    mkdir -p "$verify_tree/$(dirname "$relative")"
                    cp "$WINE_SRC/$relative" "$verify_tree/$relative"
                done < <(awk '$1 == "+++" && $2 ~ /^b\// { print substr($2, 3) }' "$patch_file" "$early_file" "$drawable_file" | sort -u)
                if patch -d "$verify_tree" -p1 --batch --force --dry-run -R < "$drawable_file" >/dev/null 2>&1; then
                    patch -d "$verify_tree" -p1 --batch --force -R < "$drawable_file" >/dev/null
                fi
                if [ "${patch_file##*/}" != 0056-wayland-direct-wsi-before-show.patch ] &&
                   patch -d "$verify_tree" -p1 --batch --force --dry-run -R < "$early_file" >/dev/null 2>&1; then
                    patch -d "$verify_tree" -p1 --batch --force -R < "$early_file" >/dev/null
                fi
                patch -d "$verify_tree" -p1 --batch --force --dry-run -R < "$patch_file" >/dev/null
            ); then
                log "$description already applied below Direct drawable overlays"
                return
            fi
        fi
        patch -d "$WINE_SRC" -p1 --batch --force --dry-run < "$patch_file" >/dev/null
        patch -d "$WINE_SRC" -p1 --batch --force < "$patch_file" >/dev/null
        log "$description applied"
    fi
}

reject_quarantined_wine_patch() {
    local patch_file="$1" description="$2"
    # Removing an ensure_wine_patch registration cannot undo a patch that was
    # already written into a persistent WINE_SRC.  Refuse that sticky state
    # instead of resetting or modifying a possibly user-owned dirty tree.
    if patch -d "$WINE_SRC" -p1 --batch --force --dry-run -R < "$patch_file" >/dev/null 2>&1; then
        printf 'error: quarantined Wine patch is already applied: %s\n' "$description" >&2
        printf 'use a clean isolated wine-valve checkout and replay the normal patch chain\n' >&2
        return 1
    fi
    if ! patch -d "$WINE_SRC" -p1 --batch --force --dry-run < "$patch_file" >/dev/null 2>&1; then
        printf 'error: cannot prove quarantined Wine patch absent: %s\n' "$description" >&2
        printf 'use a clean isolated wine-valve checkout; this script will not reset the tree\n' >&2
        return 1
    fi
    log "$description quarantined (not applied)"
}

# Refuse persistent sources contaminated by quarantined experiments before any
# normal ensure_wine_patch call can write to that source tree.
reject_quarantined_wine_patch "$SCRIPT_DIR/../patches/wine/0013b-wow64-exception-dispatch-frame-guard.patch" \
    "Wow64 exception dispatch frame guard"
reject_quarantined_wine_patch "$SCRIPT_DIR/../patches/wine/0013c-ntdll-wow32-stack-top-pad.patch" \
    "Wow32 stack top pad experiment"

# Keep prefix font registration repair reproducible after refreshing Wine.
ensure_wine_patch "$SCRIPT_DIR/../patches/wine/0001-win32u-repair-external-font-registration.patch" \
    "External-font registration repair"

# Fault diagnostics must not dereference a saved FEX SP in a guard page.
ensure_wine_patch "$SCRIPT_DIR/../patches/wine/0002-ntdll-ohos-signal-safe-stack-read.patch" \
    "Signal-safe diagnostic stack reader"

# The App owns a virtual shell desktop across application launches. Native
# child startup must not race Wine's idle close timer.
ensure_wine_patch "$SCRIPT_DIR/../patches/wine/0006-app-managed-desktop-lifetime.patch" \
    "App-managed desktop lifetime"
ensure_wine_patch "$SCRIPT_DIR/../patches/wine/0007-client-only-window-state.patch" \
    "Client-only window state refresh"
ensure_wine_patch "$SCRIPT_DIR/../patches/wine/0008-win32u-surface-region-lock-order.patch" \
    "Surface region update without USER lock inversion"
ensure_wine_patch "$SCRIPT_DIR/../patches/wine/0009-direct-ohos-wsi-and-resize-smoke.patch" \
    "Direct OHOS WSI and running-window resize smoke"
# Owner-attached pure decoration floats (WeCom shadow windows) must take the
# owner subsurface path instead of independent xdg_toplevels. Master carries
# the same change in thirdparty/wine f6492f8.
ensure_wine_patch "$SCRIPT_DIR/../patches/wine/0010-winewayland-decoration-owner-floating.patch" \
    "Owner-attached decoration float not managed"
ensure_wine_patch "$SCRIPT_DIR/../patches/wine/0011-wineboot-durable-prefix-completion.patch" \
    "Durable wineboot prefix completion handshake"
ensure_wine_patch "$SCRIPT_DIR/../patches/wine/0012-wineserver-locked-registry-migration.patch" \
    "Prefix migration under wineserver session lock"
# RPGXP 取证诊断组默认关闭，仅在 WINEHUA_RPGXP_DIAGNOSTICS=1 时启用。
ensure_wine_patch "$SCRIPT_DIR/../patches/wine/0013a-ntdll-ohos-rpgxp-diagnostics.patch" \
    "RPGXP forensic diagnostics"
# 旧 CJK 游戏文字二值化遮罩兼容: WINEHUA_FONT_AA=bitmap 让游戏进程拿到双电平字形,
# 修梦幻群侠传/XYQ 系"乱码" (从 main 分支 ad7bdd7f092 迁移; 应用侧已注入该变量)。
ensure_wine_patch "$SCRIPT_DIR/../patches/wine/0014-win32u-font-aa-override.patch" \
    "Per-process GDI font antialiasing override"
# OHOS noexec workaround used exact-size pread for exec sections;
# SteamStub-style unpadded final sections overrun EOF by a sector and
# failed image load with c000007b (PAL4 launch.exe). EOF now zero-fills
# like mmap does.
ensure_wine_patch "$SCRIPT_DIR/../patches/wine/0015-ohos-exec-section-eof-pread.patch" \
    "Exec-section EOF-tolerant read"
# Use each image's PE flag instead of forcing all PE32 processes into LAA.
ensure_wine_patch "$SCRIPT_DIR/../patches/wine/0016-ntdll-honor-pe-large-address-aware-default.patch" \
    "PE large-address-aware default"
# Keep the first stability fixes reproducible on the selected Wine source.
ensure_wine_patch "$SCRIPT_DIR/../patches/wine/0017-wayland-configure-window-lifetime.patch" \
    "Wayland configure window lifetime"
ensure_wine_patch "$SCRIPT_DIR/../patches/wine/0018-private-vulkan-present-validation.patch" \
    "Private Vulkan presentation validation"
ensure_wine_patch "$SCRIPT_DIR/../patches/wine/0019-ntdll-broker-log-metadata.patch" \
    "Broker argument metadata logging"
ensure_wine_patch "$SCRIPT_DIR/../patches/wine/0020-broker-v2-startup-contract.patch" \
    "Broker v2 complete startup contract"
# Persistent Wayland metadata need not be marshalled for every SHM frame.
ensure_wine_patch "$SCRIPT_DIR/../patches/wine/0021-wayland-shm-state-cache.patch" \
    "SHM geometry and viewport last-sent state cache"
# A signal-only batch avoids reusing one pending command buffer across images.
ensure_wine_patch "$SCRIPT_DIR/../patches/wine/0022-private-vulkan-signal-only-acquire.patch" \
    "Private Vulkan signal-only acquire"
# Font/locale behaviors from main, adapted to Valve without overriding explicit
# charsets or non-OHOS defaults. Keep 0014 as the sole GDI bitmap/gray override.
ensure_wine_patch "$SCRIPT_DIR/../patches/wine/0023-ohos-default-grayscale-font-aa.patch" \
    "OHOS grayscale font default"
ensure_wine_patch "$SCRIPT_DIR/../patches/wine/0024-ohos-scan-prefix-fonts.patch" \
    "OHOS imported prefix font discovery"
ensure_wine_patch "$SCRIPT_DIR/../patches/wine/0025-ohos-locale-font-fallback.patch" \
    "OHOS locale-aware default font fallback"
ensure_wine_patch "$SCRIPT_DIR/../patches/wine/0026-ohos-musl-selected-locale.patch" \
    "OHOS musl selected locale"
ensure_wine_patch "$SCRIPT_DIR/../patches/wine/0027-arm64ec-seh-exit-thunk.patch" \
    "ARM64EC implicit SEH handler exit thunk"
ensure_wine_patch "$SCRIPT_DIR/../patches/wine/0028-ohos-child-winedebug-environment.patch" \
    "OHOS per-child Windows debug environment"
ensure_wine_patch "$SCRIPT_DIR/../patches/wine/0029-wayland-minimize-restore-handshake.patch" \
    "Wayland minimize and explicit restore handshake"
ensure_wine_patch "$SCRIPT_DIR/../patches/wine/0030-ohos-transient-activation-return.patch" \
    "OHOS transient fullscreen activation return and explicit activation restore"
ensure_wine_patch "$SCRIPT_DIR/../patches/wine/0031-wayland-client-only-remap.patch" \
    "Wayland client-only window role recreation on remap"
ensure_wine_patch "$SCRIPT_DIR/../patches/wine/0032-wayland-readback-content-diagnostics.patch" \
    "Wayland bounded readback content and framebuffer diagnostics"
ensure_wine_patch "$SCRIPT_DIR/../patches/wine/0033-ohos-fshack-context-capability.patch" \
    "OHOS fullscreen framebuffer context capability gate"
ensure_wine_patch "$SCRIPT_DIR/../patches/wine/0034-opengl-zero-texture-not-reserved.patch" \
    "OpenGL default/detached texture zero is not an internal texture"
ensure_wine_patch "$SCRIPT_DIR/../patches/wine/0035-opengl-wow64-explicit-buffer-flush.patch" \
    "WOW64 shadow-buffer publication before explicit GL flush"
ensure_wine_patch "$SCRIPT_DIR/../patches/wine/0036-wined3d-pixel-format-id-lookup.patch" \
    "WineD3D bounded WGL pixel-format ID lookup"
ensure_wine_patch "$SCRIPT_DIR/../patches/wine/0037-wine-opengl-performance-summary.patch" \
    "Opt-in bounded Unix OpenGL API wall-time summaries"
ensure_wine_patch "$SCRIPT_DIR/../patches/wine/0038-wined3d-discard-map-invalidation.patch" \
    "Propagate nonpersistent D3D discard to OpenGL shadow maps"
ensure_wine_patch "$SCRIPT_DIR/../patches/wine/0039-wined3d-opt-in-dynamic-buffer-sysmem.patch" \
    "Opt-in WOW64 nonpersistent dynamic buffer sysmem staging"
ensure_wine_patch "$SCRIPT_DIR/../patches/wine/0040-ntdll-owned-virgl-shared-low-map.patch" \
    "Opt-in Wine-owned VirGL shared low-address buffer mappings"
ensure_wine_patch "$SCRIPT_DIR/../patches/wine/0041-winebus-ohos-poll-progress.patch" \
    "OHOS gamepad socket polling always makes progress"
ensure_wine_patch "$SCRIPT_DIR/../patches/wine/0042-opengl-opt-in-wgl-lock-wait.patch" \
    "Opt-in WGL mutex acquisition timing"
ensure_wine_patch "$SCRIPT_DIR/../patches/wine/0043-winebus-canonical-whgp-v2.patch" \
    "Canonical WHGP v2 controller axes from main"
ensure_wine_patch "$SCRIPT_DIR/../patches/wine/0044-wayland-input-thread-desktop.patch" \
    "Wayland input thread follows the focused window desktop"
ensure_wine_patch "$SCRIPT_DIR/../patches/wine/0045-ntdll-arm64-invalid-leaf-unwind.patch" \
    "ARM64 metadata-free unwind rejects non-progressing adjusted return PCs"
ensure_wine_patch "$SCRIPT_DIR/../patches/wine/0046-ntdll-ohos-process-exit.patch" \
    "OHOS Wine process exit avoids the appspawn host CRT interposer"
ensure_wine_patch "$SCRIPT_DIR/../patches/wine/0047-win32u-wow64-vulkan-map-safety.patch" \
    "WOW64 Vulkan mapped-span safety and explicit copy visibility"
ensure_wine_patch "$SCRIPT_DIR/../patches/wine/0048-ntdll-opt-in-server-request-timing.patch" \
    "Opt-in per-thread server request timing without verbose request tracing"
ensure_wine_patch "$SCRIPT_DIR/../patches/wine/0049-wined3d-require-srgb-write-control-for-shared-storage.patch" \
    "Use separate linear and sRGB textures when framebuffer write control is missing"
ensure_wine_patch "$SCRIPT_DIR/../patches/wine/0050-wayland-explicit-gpu-front-present.patch" \
    "Explicit GPU front-resource publication with exact surface identity"
ensure_wine_patch "$SCRIPT_DIR/../patches/wine/0051-opengl-preserve-pbuffer-front-storage.patch" \
    "Storage-preserving pbuffer front presentation"
ensure_wine_patch "$SCRIPT_DIR/../patches/wine/0052-ntdll-default-virgl-shared-low-map.patch" \
    "Default WOW64 Wine-owned VirGL shared mapping with explicit opt-out"
ensure_wine_patch "$SCRIPT_DIR/../patches/wine/0053-wined3d-restore-legacy-alpha-test-state.patch" \
    "Restore compatibility-context alpha-test enable and disable state"
ensure_wine_patch "$SCRIPT_DIR/../patches/wine/0054-ntdll-broker-preserve-resolved-image-path.patch" \
    "Preserve resolved child image path for PE address-space selection"
ensure_wine_patch "$SCRIPT_DIR/../patches/wine/0055-layered-child-screen-coordinates-and-root.patch" \
    "Layered child screen coordinates and Wayland ancestry"
ensure_wine_patch "$SCRIPT_DIR/../patches/wine/0056-wayland-direct-wsi-before-show.patch" \
    "Prepare Direct WSI for windows created before ShowWindow"
ensure_wine_patch "$SCRIPT_DIR/../patches/wine/0057-wayland-direct-drawable-identity.patch" \
    "Independent Direct producers for exact desktop drawables"
# Wine 编译标志 (Unix .so + wineserver)
WINE_CFLAGS="-g -O2 -D__MUSL__ -D_GNU_SOURCE -D__ANDROID__ -D__OHOS__ -DWINE_UNIX_LIB \
    -D_NTSYSTEM_ -D__WINESRC__ -DFAR= -D_ACRTIMP= -DWINBASEAPI= -DZ_SOLO \
    -fPIC -fasynchronous-unwind-tables \
    -I$SYSROOT_EXT_INC/libdrm"

build_native_tools() {
    log "--- Native 构建 (winegcc 等 host 工具) ---"
    mkdir -p "$BUILD_DIR/wine-native"
    cd "$BUILD_DIR/wine-native"
    if [ ! -f "Makefile" ]; then
        # 用 cache variables 骗过 configure, 避免 host 安装 wayland/xkbcommon/freetype/GL
        # 这些库仅供 winewayland.drv/winex11.drv 等 DLL 编译使用
        # 但我们只编译 tools/ 下的纯 host 工具, 不编任何 DLL, 不需要实际头文件/库
        export ac_cv_header_wayland_client_h=yes
        export ac_cv_lib_wayland_client_wl_display_connect=yes
        export ac_cv_header_xkbcommon_xkbcommon_h=yes
        export ac_cv_lib_xkbcommon_xkb_context_new=yes
        export ac_cv_header_xkbcommon_xkbregistry_h=yes
        export ac_cv_lib_xkbregistry_rxkb_context_new=yes
        export ac_cv_header_ft2build_h=yes
        export ac_cv_lib_soname_freetype="libfreetype.so.6"
        if [ "$HOST_OS" = "Darwin" ] || [ "$HOST_OS" = "HarmonyOS" ]; then
            export FREETYPE_CFLAGS="$("$PKG_CONFIG_BIN" --cflags freetype2)"
            export FREETYPE_LIBS="$("$PKG_CONFIG_BIN" --libs freetype2)"
            "$CONFIGURE_BIN" --srcdir="$WINE_SRC" --enable-archs=x86_64 --disable-tests \
                --without-x --without-alsa --without-opengl --without-vulkan
        else
            export FREETYPE_CFLAGS="-I/usr/include/freetype2"
            export FREETYPE_LIBS="-lfreetype"
            "$CONFIGURE_BIN" --srcdir="$WINE_SRC" --enable-win64 --disable-tests \
                --without-x --without-alsa --without-opengl --without-vulkan
        fi
    fi
    # 只编译 OHOS 交叉构建实际需要的 host 工具 (~44 .o 文件)
    # 不编 DLL (PE/fake-module 和 Unix .so), 砍掉 ~90% 编译时间
    # 也不需要在 host 上安装 wayland/xkbcommon/freetype/GL dev 包
    # 只编译 OHOS 交叉构建实际需要的 host 工具
    # winegcc/winebuild/wrc/widl: 交叉编译 PE DLL
    # wine: 加载器 (locale.nls 等数据文件)
    # makedep/make_xftmpl/wmc: Makefile 依赖/资源生成
    # sfnt2fon: 字体 .fon 生成 (唯一需要 host freetype 的工具)
    make -j$JOBS \
        tools/winegcc/winegcc \
        tools/winebuild/winebuild \
        tools/wrc/wrc \
        tools/widl/widl \
        tools/wine/wine \
        tools/makedep \
        tools/make_xftmpl \
        tools/wmc/wmc \
        tools/sfnt2fon/sfnt2fon

    # 确保 wrc 能加载 locale.nls (翻译资源编译需要)。build_native_tools
    # 只编 host 工具, 不跑生成 nls 数据的 make 规则, 故手动 symlink
    # 源码 nls 到 wine-native/nls/ (wrc 硬编码从 ../nls 找)
    mkdir -p "$BUILD_DIR/wine-native/nls"
    for nlsf in "$WINE_SRC"/nls/*.nls; do
        ln -sf "$nlsf" "$BUILD_DIR/wine-native/nls/$(basename "$nlsf")"
    done
}

build_ohos_unix() {
    log "--- OHOS 交叉编译 (Unix .so, WINE_ARCH=$WINE_ARCH) ---"

    # 构建目录按 WINE_ARCH 隔离: 同工作树切换架构 (方案① x86_64 ↔ 方案③ arm64 原生)
    # 时不复用跨架构 configure 缓存。方案①/② (WINE_ARCH=x86_64) 共享 wine-ohos-x86_64。
    local wine_build_dir="$BUILD_DIR/wine-ohos-$WINE_ARCH"
    mkdir -p "$wine_build_dir"
    cd "$wine_build_dir"

    # llvm-mingw 的 clang 必须在 PATH 里，**且与是否重新 configure 无关**：
    # winebuild 在 PE 侧靠 `clang -print-prog-name=...` 找 ar/ranlib
    # （tools/tools.h 的 find_clang_tool）。这一句原来只写在"需要重新 configure"
    # 的分支里 → 重跑时 Makefile 已存在、跳过 configure，PATH 里就没有 llvm-mingw，
    # winebuild 只能裸名 spawn "clang" → ENOENT，报出：
    #   error: winebuild : No such file or directory
    # （该文案是 tools/winebuild/utils.c 里 fatal_perror("winebuild") 的固定字符串，
    #  与实际缺失的程序名无关，排查时容易误判。）
    if [ "$WINE_ARCH" = "aarch64" ]; then
        export PATH="$LLVM_MINGW/bin:$PATH"
    fi

    # 检查是否需要重新 configure。只查 wine 真正生成到 config.h 的 SONAME 宏
    # (freetype/vulkan/gnutls 用 WINE_CHECK_SONAME); wayland/gstreamer 不生成
    # SONAME 宏 (config.h.in 无条目), 旧检查 SONAME_LIBWAYLAND_CLIENT /
    # SONAME_LIBGSTREAMER_1_0 永远为真 → 每次都重配, 已移除。
    # 外加 host 校验 (config.status 的 --host), 防止切换架构后复用旧 host 缓存。
    # W1 追加: configure.ac（或我们生成的 configure）比 config.status 新时也要重配,
    # 否则改了 configure.ac（例如排除 amd_ags_x64 模块）不会生效。
    if [ ! -f "Makefile" ] || [ "$WINE_SRC/configure.ac" -nt config.status ] \
       || [ "$BUILD_DIR/configure" -nt config.status ] \
       || ! grep -q '#define SONAME_LIBFREETYPE' include/config.h 2>/dev/null \
       || ! grep -q '#define SONAME_LIBVULKAN "libvulkan.so.1"' include/config.h 2>/dev/null \
       || ! grep -q '#define SONAME_LIBGNUTLS' include/config.h 2>/dev/null \
       || ! grep -q '^#define HAVE_LIBWAYLAND_EGL 1$' include/config.h 2>/dev/null \
       || ! grep -q -- "--host=$HOST_TRIPLE" config.status 2>/dev/null; then
        export FREETYPE_CFLAGS="-I$SYSROOT_EXT_INC/freetype2"
        export FREETYPE_LIBS="-L$SYSROOT_EXT_LIB -lfreetype"
        export ac_cv_header_ft2build_h=yes
        export ac_cv_lib_soname_freetype="libfreetype.so.6"
        # Wayland 交叉编译缓存
        export ac_cv_header_wayland_client_h=yes
        export ac_cv_lib_wayland_client_wl_display_connect=yes
        export ac_cv_lib_soname_wayland_client="libwayland-client.so.0"
        export ac_cv_header_xkbcommon_xkbcommon_h=yes
        export ac_cv_lib_xkbcommon_xkb_context_new=yes
        export ac_cv_lib_soname_xkbcommon="libxkbcommon.so.0"
        export ac_cv_header_xkbcommon_xkbregistry_h=yes
        export ac_cv_lib_soname_xkbregistry="libxkbregistry.so.0"
        # The x86_64 Guest Vulkan Loader is assembled separately from the OHOS
        # sysroot. Wine only dlopens it at runtime, so provide the canonical
        # soname explicitly instead of linking the cross build against it.
        export ac_cv_lib_soname_vulkan="libvulkan.so.1"
        # GnuTLS 交叉编译缓存 (schannel TLS 后端, 由 build_gnutls.sh 编入 sysroot-ext)
        export GNUTLS_CFLAGS="-I$SYSROOT_EXT_INC"
        export GNUTLS_LIBS="-L$SYSROOT_EXT_LIB -lgnutls"
        export ac_cv_header_gnutls_gnutls_h=yes
        export ac_cv_lib_soname_gnutls="libgnutls.so.30"
        # GStreamer 交叉编译缓存 (winegstreamer 后端, 由 build_gstreamer.sh 编入 sysroot-ext)
        # configure 探测 gstreamer-1.0/video/audio/tag 4 个 .pc (PKG_CONFIG_PATH 已含)。
        # 注意: 不设 GSTREAMER_CFLAGS/LIBS env — autoconf 惯例 env 优先于 pkg-config
        # 探测, 设了会覆盖 4 包合并的完整链接列表 (winegstreamer 链接缺 glib 符号)
        export ac_cv_header_gst_gst_h=yes
        export ac_cv_lib_gstreamer_1_0_gst_pad_new=yes
        export ac_cv_lib_soname_gstreamer_1_0="libgstreamer-1.0.so.0"
        export WAYLAND_CLIENT_CFLAGS="-I$SYSROOT_EXT_INC"
        export WAYLAND_CLIENT_LIBS="-L$SYSROOT_EXT_LIB -lwayland-client"
        # Required on every build host; missing pkg-config metadata must not
        # silently compile winewayland.drv without its OpenGL implementation.
        export WAYLAND_EGL_CFLAGS="-I$SYSROOT_EXT_INC"
        export WAYLAND_EGL_LIBS="-L$SYSROOT_EXT_LIB -lwayland-egl"
        export XKBCOMMON_CFLAGS="-I$SYSROOT_EXT_INC"
        export XKBCOMMON_LIBS="-L$SYSROOT_EXT_LIB -lxkbcommon"
        export XKBREGISTRY_CFLAGS="-I$SYSROOT_EXT_INC"
        export XKBREGISTRY_LIBS="-L$SYSROOT_EXT_LIB -lxkbregistry"
        local pkg_config=/usr/bin/pkg-config
        if [ "$HOST_OS" = "Darwin" ] || [ "$HOST_OS" = "HarmonyOS" ]; then
            local guest_gfx_prefix="$BUILD_DIR/guest_gfx_install/x86_64"
            export EGL_CFLAGS="-I$guest_gfx_prefix/include"
            export EGL_LIBS="-L$guest_gfx_prefix/lib -lEGL"
            export ac_cv_lib_soname_EGL="libEGL.so.1"
            pkg_config="$PKG_CONFIG_BIN"
        fi
        if [ "$HOST_OS" = "HarmonyOS" ]; then
            ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"
            GUEST_GFX_ROOT="${WINEHUA_GUEST_GFX_INSTALL_ROOT:-$ROOT/build/guest_gfx_install/x86_64}"
            # 必须含 -g -O2: Wine configure 用 ${CROSSCFLAGS:-"-g -O2"} 作为
            # 各 PE 架构 CFLAGS 的整串默认值。只写 -I 会把 ARM64EC/aarch64/i386
            # PE 全部编成无 -O*（clang 默认 O0）。
            export CROSSCFLAGS="-g -O2 -I$GUEST_GFX_ROOT/include"

            MINGW_CC="$LLVM_MINGW/bin/clang"
        else
            MINGW_CC="gcc"
        fi

        if [ "$WINE_ARCH" = "aarch64" ]; then
            # arm64 原生 wine: aarch64 Unix 层 + arm64ec/aarch64/i386 PE (FEX 转译 x64 应用)
            export PATH="$LLVM_MINGW/bin:$PATH"
            CC="$CLANG --target=$TARGET --sysroot=$SYSROOT" \
            CXX="$OHOS_SDK/native/llvm/bin/clang++ --target=$TARGET --sysroot=$SYSROOT" \
            CFLAGS="${WINE_CFLAGS:-} -I$SYSROOT_EXT_INC -I$SYSROOT_EXT_INC/freetype2" \
            LDFLAGS="-fuse-ld=lld --sysroot=$SYSROOT --target=$TARGET -L$SYSROOT_EXT_LIB" \
            PKG_CONFIG="$pkg_config" \
            PKG_CONFIG_PATH="$SYSROOT_EXT_PC" \
            "$CONFIGURE_BIN" --srcdir="$WINE_SRC" \
                --host="$HOST_TRIPLE" \
                --enable-archs=arm64ec,aarch64,i386 \
                --prefix=/opt/winehua \
                --libdir='${prefix}' \
                --with-wine-tools="$BUILD_DIR/wine-native" \
                --with-mingw="$LLVM_MINGW/bin/clang" \
                --disable-tests \
                --without-x --without-alsa \
                --with-opengl --with-vulkan
        else
            CC="$CLANG --target=$TARGET --sysroot=$SYSROOT" \
            CFLAGS="${WINE_CFLAGS:-} -I$SYSROOT_EXT_INC -I$SYSROOT_EXT_INC/freetype2" \
            LDFLAGS="-fuse-ld=lld --sysroot=$SYSROOT --target=$TARGET -L$SYSROOT_EXT_LIB" \
            PKG_CONFIG="$pkg_config" \
            PKG_CONFIG_PATH="$SYSROOT_EXT_PC" \
            "$CONFIGURE_BIN" --srcdir="$WINE_SRC" \
                --host="$HOST_TRIPLE" \
                --enable-archs=i386,x86_64 \
                --prefix=/opt/winehua \
                --libdir='${prefix}' \
                --with-wine-tools="$BUILD_DIR/wine-native" \
                --with-mingw="$MINGW_CC" \
                --disable-tests \
                --without-x --without-alsa \
                --with-opengl --with-vulkan
        fi
    fi

    python3 "$SCRIPT_DIR/wine_graphics_capabilities.py" --build "$wine_build_dir"

    # arm64 用 llvm-mingw clang: aarch64-windows target 的默认 include 路径不含
    # generic-w64-mingw32 的 GL/gl.h (x86_64 用 GNU mingw gcc 自带 GL 头)。
    # winehua_graphics_smoke 需要 <GL/gl.h> → 从 llvm-mingw 复制到 build 树
    # include/GL/ (PE 编译命令含 -Iinclude)。GL/gl.h 仅依赖 windows.h/stddef.h,
    # 与 wine 的 -Iinclude/-Iinclude/msvcrt 兼容。
    if [ "$WINE_ARCH" = "aarch64" ]; then
        mkdir -p include/GL
        cp -f "$LLVM_MINGW/generic-w64-mingw32/include/GL/gl.h" include/GL/gl.h
        # PE 交叉编译不走上面的 WINE_CFLAGS（那是 Unix .so）。Wine 默认
        # ${arch}_CFLAGS=-g -O2；HarmonyOS CROSSCFLAGS 曾只写 -I 会冲掉 -O2。
        require_makefile_opt_var Makefile aarch64_CFLAGS
        require_makefile_opt_var Makefile arm64ec_CFLAGS
        require_makefile_opt_var Makefile i386_CFLAGS
    fi

    local targets=()
    for module in $WINE_UNIX_MODULES; do
        [ "$module" = wineserver ] || targets+=("dlls/$module/${module%.drv}.so")
    done
    local pe_archs="i386 x86_64"
    if [ "$WINE_ARCH" = aarch64 ]; then pe_archs="i386 aarch64"; fi
    for module in $WINE_PE_MODULES; do
        for arch in $pe_archs; do targets+=("dlls/$module/$arch-windows/$module.dll"); done
    done
    # A server-only build still configures and validates the same Wine source,
    # but must not turn an empty target list into a full default make.
    if [ -n "$WINE_UNIX_MODULES" ] && [ ${#targets[@]} -eq 0 ]; then return; fi
    make -j$JOBS "${targets[@]}" \
        CC="$CLANG --target=$TARGET --sysroot=$SYSROOT" \
        CXX="$OHOS_SDK/native/llvm/bin/clang++ --target=$TARGET --sysroot=$SYSROOT" \
        CFLAGS="$WINE_CFLAGS -I$SYSROOT_EXT_INC -I$SYSROOT_EXT_INC/freetype2" \
        LDFLAGS="-fuse-ld=lld --sysroot=$SYSROOT --target=$TARGET -L$SYSROOT_EXT_LIB"

    if [ -n "$WINE_PE_MODULES" ]; then
        for module in $WINE_PE_MODULES; do
            for arch in $pe_archs; do test -s "dlls/$module/$arch-windows/$module.dll" || return 1; done
        done
        return
    fi

    if [ -n "$WINE_UNIX_MODULES" ]; then
        for module in $WINE_UNIX_MODULES; do
            [ "$module" = wineserver ] && continue
            test -s "dlls/$module/${module%.drv}.so" || return 1
        done
        return
    fi

    python3 "$SCRIPT_DIR/wine_graphics_capabilities.py" --build "$wine_build_dir" \
        --driver "$wine_build_dir/dlls/winewayland.drv/winewayland.so" \
        --readelf "$OHOS_SDK/native/llvm/bin/llvm-readelf"

    # 验证关键 .so 已成功链接（make -k 可能静默跳过链接失败）
    for pair in "winewayland.drv/winewayland.so" "wineohos.drv/wineohos.so" \
                "win32u/win32u.so" "ntdll/ntdll.so"; do
        if [ ! -f "dlls/$pair" ]; then
            warn "关键 .so 缺失: dlls/$pair (链接可能失败，检查 sysroot-ext)"
        fi
    done
}

build_wineserver() {
    log "--- 编译 wineserver (含 OHOS 修复) ---"
    # 目录按 WINE_ARCH 隔离 (方案①/② 共用 wine_server-x86_64, 产物形式不同 → 见 pie_mode)
    local out="$BUILD_DIR/wine_server-$WINE_ARCH"
    # 数据文件在应用 sandbox 内
    local bindir="$WINE_DEVICE_ROOT/bin"
    local datadir="$WINE_DEVICE_ROOT/share"
    local wine_include="-I$WINE_SRC/include -I$WINE_SRC/include/wine -I$WINE_SRC/server -I$BUILD_DIR/wine-ohos-$WINE_ARCH/include"
    # 目标架构 = WINE_ARCH 的 TARGET (wineserver 与 wine 同架构)。
    # 产物形式: box64+wine 方案 (arm64 设备 + x86_64 wine) → x86_64 PIE 可执行 (box64 转译);
    # 其余 (方案① x86_64 原生 / 方案③ arm64 原生) → native libwineserver.so (dlopen)。
    local srv_target="$TARGET"
    local pie_mode=0
    if [ "$WINE_ARCH" = "x86_64" ] && [ "$NATIVE_ARCH" = "arm64-v8a" ]; then
        pie_mode=1
    fi
    # Use the Unix Wine release flags here as well. The standalone server
    # compile used to omit -O2 entirely, despite serving thousands of requests
    # per second. Keep assertions and synchronization semantics unchanged.
    # Wine's generated EXTRACFLAGS disable strict aliasing: its object/list
    # casts depend on that contract. The standalone compile bypasses Makefile.
    local srv_cflags="--target=$srv_target --sysroot=$SYSROOT $WINE_CFLAGS -fno-strict-aliasing \
        -DBINDIR=\"$bindir\" -DDATADIR=\"$datadir\" \
        $wine_include"
    require_optimization_flags "Wine server CFLAGS" $srv_cflags

    # Source mtimes alone do not invalidate objects after a compiler/flags
    # change. Old caches without this record also need a full server rebuild.
    local flags_fingerprint
    flags_fingerprint="$( { printf '%s\n' "$srv_cflags" "$pie_mode"; "$CLANG" --version; } | sha256sum | cut -d' ' -f1 )"
    local flags_file="$out/.compile-flags.sha256"

    mkdir -p "$out"
    local need_rebuild=0
    local target_binary="$out/libwineserver.so"
    [ "$pie_mode" = "1" ] && target_binary="$out/wineserver"
    if [ ! -f "$target_binary" ] || [ "$(cat "$flags_file" 2>/dev/null || true)" != "$flags_fingerprint" ]; then
        need_rebuild=1
    else
        for f in $WINE_SRC/server/*.c; do
            [ "$f" -nt "$target_binary" ] && { need_rebuild=1; break; }
        done
    fi
    if [ $need_rebuild -eq 0 ]; then
        # Publish the selected cache even when another build left a library
        # at the destination. Existence alone does not establish its identity.
        if [ "$pie_mode" = "0" ] && [ -f "$out/libwineserver.so" ]; then
            mkdir -p "$NATIVE_LIBS"
            local publish_file
            publish_file="$(mktemp "$NATIVE_LIBS/.wineserver-publish.XXXXXX")"
            cp -p "$out/libwineserver.so" "$publish_file"
            mv -f "$publish_file" "$NATIVE_LIBS/libwineserver.so"
        fi
        return
    fi
    for f in $WINE_SRC/server/*.c; do
        $CLANG $srv_cflags -c -o "$out/$(basename "$f" .c).o" "$f"
    done

    # musl_compat.c 已在 WINE_SRC/server/ 中, 遍历编译时已打包

    if [ "$pie_mode" = "1" ]; then
        # box64+wine: x86_64 PIE 可执行, box64 转译加载
        log "  wineserver → x86_64 PIE ELF (box64 转译, arm64 设备)"
        $CLANG --target=$srv_target --sysroot=$SYSROOT -fuse-ld=lld -pie \
            -o "$out/wineserver" "$out"/*.o -lm
        log "  → $out/wineserver"
    else
        # 原生: 编译为共享库 (dlopen 加载), 目标 = wine 架构
        log "  wineserver → libwineserver.so ($srv_target)"
        $CLANG --target=$srv_target --sysroot=$SYSROOT -fuse-ld=lld \
            -shared -Wl,-soname,libwineserver.so \
            -o "$out/libwineserver.so" "$out"/*.o -lm
        mkdir -p "$NATIVE_LIBS"
        # Replace the directory entry, not a potentially hard-linked old
        # artifact in another checkout or build cache.
        local publish_file
        publish_file="$(mktemp "$NATIVE_LIBS/.wineserver-publish.XXXXXX")"
        cp -p "$out/libwineserver.so" "$publish_file"
        mv -f "$publish_file" "$NATIVE_LIBS/libwineserver.so"
        log "  → $NATIVE_LIBS/libwineserver.so"
    fi
    printf '%s\n' "$flags_fingerprint" > "$flags_file"
}

# ---- main ----
log "=== 构建 Wine ==="

# 检查 gettext 工具 (msgfmt)。缺失时 wine configure 会禁用 po 翻译,
# 产物 PE 资源只有英文 (中文/多语言 UI 依赖 msgfmt 编翻译语言块)
if ! command -v msgfmt >/dev/null 2>&1; then
    log "ERROR: msgfmt (gettext) 未安装, wine 翻译资源不会编译"
    log "  请安装: apt-get install -y gettext  或  brew install gettext"
    exit 1
fi

# 从 configure.ac 重新生成 configure 到构建目录 (不污染源码树)
# 我们的 configure.ac 新增了 wineohos.drv 等模块的 WINE_CONFIG_MAKEFILE
CONFIGURE_BIN="$BUILD_DIR/configure"
if [ ! -x "$CONFIGURE_BIN" ] || [ "$WINE_SRC/configure.ac" -nt "$CONFIGURE_BIN" ]; then
    log "--- 重新生成 configure (autoconf) ---"
    (cd "$BUILD_DIR" && autoconf -I "$WINE_SRC" -o "$CONFIGURE_BIN" "$WINE_SRC/configure.ac")
    chmod +x "$CONFIGURE_BIN"
fi

# out-of-tree 构建时，Wine 生成的 Makefile 里 config.status 依赖 $(srcdir)/configure。
# winehua fork 的源码树里带着 configure，所以旧流程没暴露这个问题；
# Valve 的 proton_11.0 树不带（上游 autogen.sh 会在树内生成），于是 make 会报：
#   make: *** No rule to make target '<srcdir>/configure', needed by 'config.status'.  Stop.
if [ ! -f "$WINE_SRC/configure" ]; then
    log "--- 源码树缺少 configure，从构建目录补齐 (config.status 依赖 \$(srcdir)/configure) ---"
    cp -f "$CONFIGURE_BIN" "$WINE_SRC/configure"
    chmod +x "$WINE_SRC/configure"
fi

# ── 上游 autogen.sh 的生成步骤（换上游/Valve 源码树时必需）──────────────
# Wine 的 autogen.sh 实际做四件事：
#   tools/make_requests / tools/make_specfiles
#   dlls/winevulkan/make_vulkan -x vk.xml -X video.xml / autoreconf -ifv
# winehua 的 fork 把产物（include/config.h.in、dlls/ntdll/ntsyscalls.h、
# include/wine/vulkan.h、winevulkan 的 thunks 等）**提交进了仓库**，
# 所以以前只跑 autoconf 就够用；换成 Valve 的 proton_11.0 树后会依次撞上：
#   config.status: error: cannot find input file: 'include/config.h.in'
#   error: open wine/vulkan.h : No such file or directory
#   dlls/ntdll/signal_arm.c:35: error: ntsyscalls.h: No such file or directory
# 这里按 autogen.sh 的等价步骤补齐（幂等：产物已在则整块跳过）。
if [ ! -f "$WINE_SRC/dlls/ntdll/ntsyscalls.h" ] \
   || [ ! -f "$WINE_SRC/include/config.h.in" ] \
   || [ ! -f "$WINE_SRC/include/wine/vulkan.h" ] \
   || [ ! -f "$WINE_SRC/dlls/vulkan-1/vulkan-1.spec" ] \
   || [ "$WINE_SRC/dlls/winevulkan/make_vulkan" -nt "$WINE_SRC/dlls/winevulkan/vulkan_thunks.c" ]; then
    log "--- 源码树缺少上游生成物 → 运行 autogen.sh 等价的生成步骤 ---"
    (
        cd "$WINE_SRC" &&
        tools/make_requests &&
        tools/make_specfiles &&
        ( cd dlls/winevulkan && ./make_vulkan -x vk.xml -X video.xml ) &&
        autoheader
    )
    log "--- 生成完成: ntsyscalls.h / include/wine/vulkan.h / include/config.h.in ---"
fi

# Record the exact effective source after overlays/generated inputs, before make.
wine_build_identity record
build_native_tools
build_ohos_unix
case " $WINE_UNIX_MODULES $WINE_PE_MODULES " in
    "   "|*" wineserver "*) build_wineserver ;;
esac

log "Wine 构建完成"
