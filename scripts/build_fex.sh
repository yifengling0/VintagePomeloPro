#!/bin/bash
# build_fex.sh — 构建 FEX 模拟器 DLL (arm64 原生 wine 转译 x86_64 / x86 应用)
#
#   libarm64ecfex.dll : x86_64 模拟 (必需) — arm64ec ABI, 由 Wine 的
#                       load_arm64ec_module() 在 ARM64EC/WoW64 层内加载 (HODLL64)
#   libwow64fex.dll   : i386 (32 位 x86) 模拟 — aarch64 ABI, 由 Wine 的
#                       get_cpu_dll_name() 在 WoW64 层内加载 (HODLL)
#
# 产物:
#   build/fex-ec/Bin/libarm64ecfex.dll  (assemble.sh 归位到 aarch64-windows/)
#   build/fex-pe/Bin/libwow64fex.dll    (assemble.sh 归位到 aarch64-windows/)
# 前置: LLVM_MINGW (llvm-mingw, 需 LLVM ≥ 18 支持 arm64ec) + thirdparty/fex
set -euo pipefail
SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
source "$SCRIPT_DIR/env.sh"

# 仅 arm64 原生 wine 需要 FEX (x86_64 模拟器同目标, 不需要转译)
[ "$WINE_ARCH" = "aarch64" ] || { log "FEX 仅 arm64 原生需要 (WINE_ARCH=$WINE_ARCH)，跳过"; exit 0; }

FEX_SRC="${FEX_SRC:-$ROOT/thirdparty/fex}"
OUT_DIR="$BUILD_DIR/fex-ec/Bin"

if [ ! -f "$FEX_SRC/CMakeLists.txt" ]; then
    # Proton migration keeps the product tree read-only.  Stage its pinned FEX
    # source in this worktree so downstream fixes are reproducible and never
    # mutate /data/prod.
    FEX_VENDOR_SRC="${FEX_VENDOR_SRC:-/data/prod/thirdparty/fex}"
    [ -f "$FEX_VENDOR_SRC/CMakeLists.txt" ] || \
        FEX_VENDOR_SRC="${PROD:-/home/liufeng/src/WineHua-arm64ec}/thirdparty/fex"
    test -f "$FEX_VENDOR_SRC/CMakeLists.txt" || \
        err "FEX 源码缺失: $ROOT/thirdparty/fex 和 $FEX_VENDOR_SRC"

    FEX_SRC="$BUILD_DIR/fex-src"
    if [ ! -f "$FEX_SRC/CMakeLists.txt" ]; then
        stage_tmp="$FEX_SRC.tmp.$$"
        rm -rf "$stage_tmp"
        mkdir -p "$stage_tmp"
        tar -C "$FEX_VENDOR_SRC" --exclude=.git --exclude=build --exclude=Build -cf - . | \
            tar -C "$stage_tmp" -xf -
        rm -rf "$FEX_SRC"
        mv "$stage_tmp" "$FEX_SRC"
        log "已复制只读产品 FEX 源码到: $FEX_SRC"
    fi
fi

test -f "$FEX_SRC/Data/CMake/toolchain_mingw.cmake" || err "FEX toolchain_mingw.cmake 缺失"
test -x "$LLVM_MINGW/bin/arm64ec-w64-mingw32-clang" || err "llvm-mingw 缺失 arm64ec 支持: $LLVM_MINGW (需 LLVM ≥ 18)"
test -x "$LLVM_MINGW/bin/aarch64-w64-mingw32-clang" || err "llvm-mingw 缺失 aarch64-w64-mingw32-clang: $LLVM_MINGW"

export PATH="$LLVM_MINGW/bin:$PATH"

# Strict PC24 multiplication is an isolated, compile-time WOW64 experiment.
# Disabled in production until no-diagnostics game acceptance demonstrates a gain.
case "${FEX_STRICT_MUL24:-0}" in
    0|1) ;;
    *) err "FEX_STRICT_MUL24 must be 0 or 1" ;;
esac

# 随构建走的 FEX 补丁 (子模块锁定 86ff33bbe 且指向 FEX-Emu/FEX 上游,
# 非 fork 不能推分支, 按 glib-format-security 先例 patch 化):
#   fex-missing-includes.patch   — 上游 08031a2767 "Add missing includes",
#     StringConv.h 等 3 文件缺 <cstdlib>/<stdarg.h>, 旧 llvm-mingw (20260616)
#     的 libc++ 靠传递 include 侥幸能编, 20260826 起不行.
#   fex-winapi-locale-stubs.patch — 20260826 libc++ 的 locale_win32.cpp.obj
#     引用 GetACP/GetLocaleInfoEx, 上游 master 在 WinAPI/Misc.cpp 以
#     UNIMPLEMENTED 桩解决, 回补到本树同名文件.
#   fex-wow64-lookup-cache-commit.patch — Wine-OHOS 不能保证把 native CPU
#     DLL 的 lookup-cache 访问异常回调给 FEX，WOW64 显式保持映射 committed.
#   fex-wow64-dynamic-l1-recommit.patch — 动态 L1 缓存缩容后也保持页面
#     committed；否则后续扩容会在 FindBlock 重新访问已 decommit 的页面。
#   fex-arm64ec-unaligned-diagnostics.patch — 绕过默认 SilentLog，有界记录
#     ARM64EC SIGBUS 的 opcode、JIT 归属和模拟结果。
#   fex-windows-unaligned-stderr.patch — 同样覆盖 Steam 32 位 CEF 实际使用的
#     WoW64 reset 路径，并直接写入 Wine stderr。
#   fex-wow64-exact-store.patch — 仅在 fex-pe 定义
#     FEX_WOW64_EXACT_STORE 时编译的 x87 exact-normal store 实验。
patched_source=0
# The ARM64EC lookup-cache patch deliberately changes the context of the
# earlier WOW64 patches. Recognize the complete result when rebuilding the
# already staged source; a fresh staged tree still applies every patch below.
if grep -Fq '#if defined(_WIN32)' "$FEX_SRC/FEXCore/Source/Interface/Core/LookupCache.cpp" &&
   grep -Fq 'VirtualDontNeed(FirstZeroL1Entry, ZeroMemorySize, true)' "$FEX_SRC/FEXCore/Source/Interface/Core/LookupCache.h" &&
   grep -Fq 'static std::atomic<uint32_t> UnalignedEventCount' "$FEX_SRC/Source/Windows/ARM64EC/Module.cpp" &&
   patch -d "$FEX_SRC" -p1 -R --dry-run -s < "$SCRIPT_DIR/patches/fex-windows-unaligned-stderr.patch" >/dev/null 2>&1 &&
   patch -d "$FEX_SRC" -p1 -R --dry-run -s < "$SCRIPT_DIR/patches/fex-missing-includes.patch" >/dev/null 2>&1 &&
   patch -d "$FEX_SRC" -p1 -R --dry-run -s < "$SCRIPT_DIR/patches/fex-winapi-locale-stubs.patch" >/dev/null 2>&1; then
    patched_source=1
fi
for PATCH in \
    "$SCRIPT_DIR/patches/fex-missing-includes.patch" \
    "$SCRIPT_DIR/patches/fex-winapi-locale-stubs.patch" \
    "$SCRIPT_DIR/patches/fex-wow64-lookup-cache-commit.patch" \
    "$SCRIPT_DIR/patches/fex-wow64-dynamic-l1-recommit.patch" \
    "$SCRIPT_DIR/patches/fex-arm64ec-lookup-cache-commit.patch" \
    "$SCRIPT_DIR/patches/fex-arm64ec-unaligned-diagnostics.patch" \
    "$SCRIPT_DIR/patches/fex-windows-unaligned-stderr.patch"; do
    if [ "$patched_source" = 1 ]; then
        log "已验证完整 FEX 补丁集: $(basename "$PATCH")"
        continue
    fi
    if patch -d "$FEX_SRC" -p1 -R --dry-run -s < "$PATCH" >/dev/null 2>&1; then
        continue
    fi
    patch -d "$FEX_SRC" -p1 --dry-run -s < "$PATCH" >/dev/null || \
        err "FEX patch 无法应用: $PATCH"
    patch -d "$FEX_SRC" -p1 -s < "$PATCH"
    log "已应用 patch: $(basename "$PATCH")"
done

PATCH="$SCRIPT_DIR/patches/fex-wow64-exact-store.patch"
if grep -Fq 'bool WindowsExactFloatStoreEnabled {}' "$FEX_SRC/FEXCore/Source/Interface/Context/Context.h" &&
   grep -Fq 'std::getenv("FEX_EXACTSTORE")' "$FEX_SRC/FEXCore/Source/Interface/Core/Core.cpp" &&
   grep -Fq 'FEX_WOW64_EXACT_STORE' "$FEX_SRC/FEXCore/Source/Interface/Core/Dispatcher/Dispatcher.cpp"; then
    log "已验证 patch: $(basename "$PATCH")"
elif ! patch -d "$FEX_SRC" -p1 -R --dry-run -s < "$PATCH" >/dev/null 2>&1; then
    patch -d "$FEX_SRC" -p1 --dry-run -s < "$PATCH" >/dev/null || \
        err "FEX WOW64 exact-store patch 无法应用: $PATCH"
    patch -d "$FEX_SRC" -p1 -s < "$PATCH"
    log "已应用 patch: $(basename "$PATCH")"
fi

# Counter instrumentation is deliberately a second overlay and build identity.
# It is never part of clean OFF/ON timing DLLs.
if [ "${FEX_EXACTSTORE_DIAGNOSTICS:-0}" = "1" ]; then
    PATCH="$SCRIPT_DIR/patches/fex-wow64-exact-store-diagnostics.patch"
    if grep -Fq 'bool WindowsExactFloatStoreStatsEnabled {}' "$FEX_SRC/FEXCore/Source/Interface/Context/Context.h" &&
       grep -Fq 'FEXExactStoreCounters[6]' "$FEX_SRC/FEXCore/Source/Interface/Core/Core.cpp" &&
       grep -Fq 'CountExactStore' "$FEX_SRC/FEXCore/Source/Interface/Core/Dispatcher/Dispatcher.cpp" &&
       [ "$(grep -Fc 'ReportExactStoreStats(false);' "$FEX_SRC/Source/Windows/WOW64/Module.cpp")" -eq 3 ] &&
       grep -Fq 'Reports.load(std::memory_order_relaxed) >= 600' "$FEX_SRC/Source/Windows/WOW64/Module.cpp" &&
       grep -Fq 'if (!Handle && !After) ReportExactStoreStats(true)' "$FEX_SRC/Source/Windows/WOW64/Module.cpp"; then
        log "已验证 patch: $(basename "$PATCH")"
    elif ! patch -d "$FEX_SRC" -p1 -R --dry-run -s < "$PATCH" >/dev/null 2>&1; then
        patch -d "$FEX_SRC" -p1 --dry-run -s < "$PATCH" >/dev/null || \
            err "FEX exact-store diagnostics patch 无法应用: $PATCH"
        patch -d "$FEX_SRC" -p1 -s < "$PATCH"
        log "已应用 patch: $(basename "$PATCH")"
    fi
fi

# Keep this separate from patched_source: an already staged older patch set
# must still acquire the synthetic-return fix.
callret_patch="$SCRIPT_DIR/patches/fex-wow64-synthetic-return.patch"
if ! patch -d "$FEX_SRC" -p1 -R --dry-run -s < "$callret_patch" >/dev/null 2>&1; then
    patch -d "$FEX_SRC" -p1 --dry-run -s < "$callret_patch" >/dev/null || \
        err "FEX WOW64 synthetic return patch 无法应用"
    patch -d "$FEX_SRC" -p1 -s < "$callret_patch"
    log "已应用 patch: $(basename "$callret_patch")"
fi

# POSIX read-only is zero. Preserve the explicit share and create modes
# when the Windows CRT forwards an ANSI path to its Unicode implementation.
crt_open_patch="$SCRIPT_DIR/patches/fex-windows-crt-file-open.patch"
if ! patch -d "$FEX_SRC" -p1 -R --dry-run -s < "$crt_open_patch" >/dev/null 2>&1; then
    patch -d "$FEX_SRC" -p1 --dry-run -s < "$crt_open_patch" >/dev/null || \
        err "FEX Windows CRT file open patch 无法应用"
    patch -d "$FEX_SRC" -p1 -s < "$crt_open_patch"
    log "已应用 patch: $(basename "$crt_open_patch")"
fi

# Diagnose Windows JIT output without depending on the game's DOS drive.
# Apply separately so an older, already staged patch set receives this fix.
jit_symbols_patch="$SCRIPT_DIR/patches/fex-windows-jit-symbols.patch"
if ! patch -d "$FEX_SRC" -p1 -R --dry-run -s < "$jit_symbols_patch" >/dev/null 2>&1; then
    patch -d "$FEX_SRC" -p1 --dry-run -s < "$jit_symbols_patch" >/dev/null || \
        err "FEX Windows JIT symbols patch 无法应用"
    patch -d "$FEX_SRC" -p1 -s < "$jit_symbols_patch"
    log "已应用 patch: $(basename "$jit_symbols_patch")"
fi

# Opt-in JIT cost accounting and dispatcher/helper symbol ranges.
jit_cost_patch="$SCRIPT_DIR/patches/fex-windows-jit-cost.patch"
if ! patch -d "$FEX_SRC" -p1 -R --dry-run -s < "$jit_cost_patch" >/dev/null 2>&1; then
    patch -d "$FEX_SRC" -p1 --dry-run -s < "$jit_cost_patch" >/dev/null || \
        err "FEX Windows JIT cost patch 无法应用"
    patch -d "$FEX_SRC" -p1 -s < "$jit_cost_patch"
    log "已应用 patch: $(basename "$jit_cost_patch")"
fi

# Checked Windows config/cache reads and native exceptions before FEX init.
file_loading_patch="$SCRIPT_DIR/patches/fex-windows-file-loading.patch"
if ! patch -d "$FEX_SRC" -p1 -R --dry-run -s < "$file_loading_patch" >/dev/null 2>&1; then
    patch -d "$FEX_SRC" -p1 --dry-run -s < "$file_loading_patch" >/dev/null || \
        err "FEX Windows file loading patch 无法应用"
    patch -d "$FEX_SRC" -p1 -s < "$file_loading_patch"
    log "已应用 patch: $(basename "$file_loading_patch")"
fi

if grep -Fq 'const bool deep = (depth >= 0x7000 && depth < 0x90000) ||' \
     "$FEX_SRC/Source/Windows/ARM64EC/Module.cpp"; then
    patch -d "$FEX_SRC" -p1 --dry-run -s < "$SCRIPT_DIR/patches/fex-steam-recursion-entry-limit.patch" >/dev/null || \
        err "FEX Steam recursion entry-limit patch 无法应用"
    patch -d "$FEX_SRC" -p1 -s < "$SCRIPT_DIR/patches/fex-steam-recursion-entry-limit.patch"
fi

# Steam 诊断补丁集（fex-steam-*.patch，10 个）从 v13–v21 的迭代快照导出，没有形成能从
# 干净树重放的序列：实测 6/10 打不上（hunk 冲突），剩余补丁之间也有脚本未表达的顺序依赖。
# 这些是 CEF 崩溃排查用的诊断代码，不进产品包。默认跳过；要复现当时的诊断树，先把
# FEX_SRC 手工准备成对应状态，再开 FEX_STEAM_DIAG_PATCHES=1。
if [ "${FEX_STEAM_DIAG_PATCHES:-0}" != "1" ]; then
    log "跳过 Steam 诊断补丁集 (FEX_STEAM_DIAG_PATCHES=0)"
elif grep -Fq 'SteamEntryTraceEdges.fetch_add(1, std::memory_order_relaxed) < 512;' \
     "$FEX_SRC/Source/Windows/ARM64EC/Module.cpp"; then
    log "已验证完整 FEX Steam 诊断补丁集"
else
if { grep -Fq 'extern "C" void WineHuaGuestBoundary' "$FEX_SRC/Source/Windows/ARM64EC/Module.cpp" &&
     grep -Fq 'WineHuaGuestBoundary' "$FEX_SRC/FEXCore/Source/Interface/Core/JIT/MiscOps.cpp" &&
     grep -Fq 'WineHuaGuestCall' "$FEX_SRC/FEXCore/Source/Interface/Core/JIT/BranchOps.cpp"; } ||
   patch -d "$FEX_SRC" -p1 -R --dry-run -s < "$SCRIPT_DIR/patches/fex-steam-boundary-trace.patch" >/dev/null 2>&1; then
    log "已应用 patch: fex-steam-boundary-trace.patch"
else
    patch -d "$FEX_SRC" -p1 --dry-run -s < "$SCRIPT_DIR/patches/fex-steam-boundary-trace.patch" >/dev/null || \
        err "FEX Steam boundary patch 无法应用"
    patch -d "$FEX_SRC" -p1 -s < "$SCRIPT_DIR/patches/fex-steam-boundary-trace.patch"
fi

if patch -d "$FEX_SRC" -p1 -R --dry-run -s < "$SCRIPT_DIR/patches/fex-arm64ec-threadterm-context.patch" >/dev/null 2>&1; then
    log "已应用 patch: fex-arm64ec-threadterm-context.patch"
else
    patch -d "$FEX_SRC" -p1 --dry-run -s < "$SCRIPT_DIR/patches/fex-arm64ec-threadterm-context.patch" >/dev/null || \
        err "FEX ARM64EC thread termination patch 无法应用"
    patch -d "$FEX_SRC" -p1 -s < "$SCRIPT_DIR/patches/fex-arm64ec-threadterm-context.patch"
fi

if patch -d "$FEX_SRC" -p1 -R --dry-run -s < "$SCRIPT_DIR/patches/fex-arm64ec-threadterm-get-context-right.patch" >/dev/null 2>&1; then
    log "已应用 patch: fex-arm64ec-threadterm-get-context-right.patch"
else
    patch -d "$FEX_SRC" -p1 --dry-run -s < "$SCRIPT_DIR/patches/fex-arm64ec-threadterm-get-context-right.patch" >/dev/null || \
        err "FEX ARM64EC thread context handle access patch 无法应用"
    patch -d "$FEX_SRC" -p1 -s < "$SCRIPT_DIR/patches/fex-arm64ec-threadterm-get-context-right.patch"
fi

if grep -Fq 'if (sequence >= 2048 && (sequence - 2048) % 16) return;' \
     "$FEX_SRC/Source/Windows/ARM64EC/Module.cpp" ||
   grep -Fq 'const bool deep = depth >= 0x7f4000 && depth < 0x7fe800;' \
     "$FEX_SRC/Source/Windows/ARM64EC/Module.cpp" ||
   grep -Fq 'if (sequence >= 1024 && (sequence - 1024) % 16) return;' \
     "$FEX_SRC/Source/Windows/ARM64EC/Module.cpp" ||
   patch -d "$FEX_SRC" -p1 -R --dry-run -s < "$SCRIPT_DIR/patches/fex-steam-deferred-call-trace.patch" >/dev/null 2>&1; then
    log "已应用 patch: fex-steam-deferred-call-trace.patch"
else
    patch -d "$FEX_SRC" -p1 --dry-run -s < "$SCRIPT_DIR/patches/fex-steam-deferred-call-trace.patch" >/dev/null || \
        err "FEX deferred Steam call trace patch 无法应用"
    patch -d "$FEX_SRC" -p1 -s < "$SCRIPT_DIR/patches/fex-steam-deferred-call-trace.patch"
fi

if grep -Fq 'const bool deep = startRsp > rsp && startRsp - rsp >= 0x3000 &&' \
     "$FEX_SRC/Source/Windows/ARM64EC/Module.cpp" ||
   grep -Fq 'const uint32_t deepEdge = inDeep ? SteamTraceDeepEdges.fetch_add' \
     "$FEX_SRC/Source/Windows/ARM64EC/Module.cpp" ||
   grep -Fq 'const bool deep = depth >= 0x7f4000 && depth < 0x7fe800;' \
     "$FEX_SRC/Source/Windows/ARM64EC/Module.cpp" ||
   patch -d "$FEX_SRC" -p1 -R --dry-run -s < "$SCRIPT_DIR/patches/fex-steam-deep-call-trace.patch" >/dev/null 2>&1; then
    log "已应用 patch: fex-steam-deep-call-trace.patch"
else
    patch -d "$FEX_SRC" -p1 --dry-run -s < "$SCRIPT_DIR/patches/fex-steam-deep-call-trace.patch" >/dev/null || \
        err "FEX deep Steam call trace patch 无法应用"
    patch -d "$FEX_SRC" -p1 -s < "$SCRIPT_DIR/patches/fex-steam-deep-call-trace.patch"
fi

if grep -Fq 'const bool deep = inDeep && deepEdge >= 600 && deepEdge < 2000;' \
     "$FEX_SRC/Source/Windows/ARM64EC/Module.cpp" ||
   grep -Fq 'const uint32_t deepEdge = inDeep ? SteamTraceDeepEdges.fetch_add' \
     "$FEX_SRC/Source/Windows/ARM64EC/Module.cpp" ||
   grep -Fq 'const bool deep = depth >= 0x7f4000 && depth < 0x7fe800;' \
     "$FEX_SRC/Source/Windows/ARM64EC/Module.cpp" ||
   patch -d "$FEX_SRC" -p1 -R --dry-run -s < "$SCRIPT_DIR/patches/fex-steam-recursion-window.patch" >/dev/null 2>&1; then
    log "已应用 patch: fex-steam-recursion-window.patch"
else
    patch -d "$FEX_SRC" -p1 --dry-run -s < "$SCRIPT_DIR/patches/fex-steam-recursion-window.patch" >/dev/null || \
        err "FEX Steam recursion window patch 无法应用"
    patch -d "$FEX_SRC" -p1 -s < "$SCRIPT_DIR/patches/fex-steam-recursion-window.patch"
fi

if grep -Fq 'const bool deep = inDeep && deepEdge >= 600 && deepEdge < 2000;' \
     "$FEX_SRC/Source/Windows/ARM64EC/Module.cpp" ||
   grep -Fq 'const bool deep = depth >= 0x7f4000 && depth < 0x7fe800;' \
     "$FEX_SRC/Source/Windows/ARM64EC/Module.cpp" ||
   patch -d "$FEX_SRC" -p1 -R --dry-run -s < "$SCRIPT_DIR/patches/fex-steam-recursion-transition.patch" >/dev/null 2>&1; then
    log "已应用 patch: fex-steam-recursion-transition.patch"
else
    patch -d "$FEX_SRC" -p1 --dry-run -s < "$SCRIPT_DIR/patches/fex-steam-recursion-transition.patch" >/dev/null || \
        err "FEX Steam recursion transition patch 无法应用"
    patch -d "$FEX_SRC" -p1 -s < "$SCRIPT_DIR/patches/fex-steam-recursion-transition.patch"
fi

if grep -Fq 'const bool deep = depth >= 0x7f4000 && depth < 0x7fe800;' \
     "$FEX_SRC/Source/Windows/ARM64EC/Module.cpp" ||
   patch -d "$FEX_SRC" -p1 -R --dry-run -s < "$SCRIPT_DIR/patches/fex-steam-recursion-tail.patch" >/dev/null 2>&1; then
    log "已应用 patch: fex-steam-recursion-tail.patch"
else
    patch -d "$FEX_SRC" -p1 --dry-run -s < "$SCRIPT_DIR/patches/fex-steam-recursion-tail.patch" >/dev/null || \
        err "FEX Steam recursion tail patch 无法应用"
    patch -d "$FEX_SRC" -p1 -s < "$SCRIPT_DIR/patches/fex-steam-recursion-tail.patch"
fi

if grep -Fq 'const bool deep = depth >= 0x7f4000 && depth < 0x7fe800;' \
     "$FEX_SRC/Source/Windows/ARM64EC/Module.cpp" ||
   patch -d "$FEX_SRC" -p1 -R --dry-run -s < "$SCRIPT_DIR/patches/fex-steam-recursion-contiguous.patch" >/dev/null 2>&1; then
    log "已应用 patch: fex-steam-recursion-contiguous.patch"
else
    patch -d "$FEX_SRC" -p1 --dry-run -s < "$SCRIPT_DIR/patches/fex-steam-recursion-contiguous.patch" >/dev/null || \
        err "FEX Steam contiguous recursion patch 无法应用"
    patch -d "$FEX_SRC" -p1 -s < "$SCRIPT_DIR/patches/fex-steam-recursion-contiguous.patch"
fi

if patch -d "$FEX_SRC" -p1 -R --dry-run -s < "$SCRIPT_DIR/patches/fex-steam-recursion-depth-window.patch" >/dev/null 2>&1; then
    log "已应用 patch: fex-steam-recursion-depth-window.patch"
else
    patch -d "$FEX_SRC" -p1 --dry-run -s < "$SCRIPT_DIR/patches/fex-steam-recursion-depth-window.patch" >/dev/null || \
        err "FEX Steam depth-window patch 无法应用"
    patch -d "$FEX_SRC" -p1 -s < "$SCRIPT_DIR/patches/fex-steam-recursion-depth-window.patch"
fi

if patch -d "$FEX_SRC" -p1 -R --dry-run -s < "$SCRIPT_DIR/patches/fex-steam-recursion-entry-window.patch" >/dev/null 2>&1; then
    log "已应用 patch: fex-steam-recursion-entry-window.patch"
else
    patch -d "$FEX_SRC" -p1 --dry-run -s < "$SCRIPT_DIR/patches/fex-steam-recursion-entry-window.patch" >/dev/null || \
        err "FEX Steam recursion entry-window patch 无法应用"
    patch -d "$FEX_SRC" -p1 -s < "$SCRIPT_DIR/patches/fex-steam-recursion-entry-window.patch"
fi

if patch -d "$FEX_SRC" -p1 -R --dry-run -s < "$SCRIPT_DIR/patches/fex-steam-recursion-entry-limit.patch" >/dev/null 2>&1; then
    log "已应用 patch: fex-steam-recursion-entry-limit.patch"
else
    patch -d "$FEX_SRC" -p1 --dry-run -s < "$SCRIPT_DIR/patches/fex-steam-recursion-entry-limit.patch" >/dev/null || \
        err "FEX Steam recursion entry-limit patch 无法应用"
    patch -d "$FEX_SRC" -p1 -s < "$SCRIPT_DIR/patches/fex-steam-recursion-entry-limit.patch"
fi
fi

# Source overlay is harmless in clean builds: code is compiled only in fex-pe
# when explicitly selected, and never in ARM64EC. Keep source replay idempotent.
mul24_patch="$SCRIPT_DIR/patches/fex-wow64-strict-mul24.patch"
if ! patch -d "$FEX_SRC" -p1 -R --dry-run -s < "$mul24_patch" >/dev/null 2>&1; then
    patch -d "$FEX_SRC" -p1 --dry-run -s < "$mul24_patch" >/dev/null || \
        err "FEX strict mul24 patch cannot be applied"
    patch -d "$FEX_SRC" -p1 -s < "$mul24_patch"
    log "Applied patch: $(basename "$mul24_patch")"
fi

prepare_mul24_cache() {
    local build="$1" expected="$2" flags cached=0
    [ -f "$build/CMakeCache.txt" ] || return 0
    flags="$(sed -n 's/^CMAKE_CXX_FLAGS:STRING=//p' "$build/CMakeCache.txt" | head -1)"
    [[ " $flags " == *' -DFEX_WOW64_STRICT_MUL24=1 '* ]] && cached=1
    if [ "$expected" != "$cached" ] || \
       { [ "$expected" = 0 ] && [[ "$flags" == *FEX_WOW64_STRICT_MUL24* ]]; }; then
        log "FEX strict mul24 selection changed; refreshing CMake cache"
        rm -rf "$build/CMakeFiles"
        rm -f "$build/CMakeCache.txt" "$build/Makefile" "$build/cmake_install.cmake"
    fi
}

prepare_build_dir() {
    local build="$1" cached_source
    [ -f "$build/CMakeCache.txt" ] || return 0
    cached_source="$(sed -n 's/^CMAKE_HOME_DIRECTORY:INTERNAL=//p' "$build/CMakeCache.txt" | head -1)"
    if [ -n "$cached_source" ] && [ "$cached_source" != "$FEX_SRC" ]; then
        log "FEX 源码位置变化，刷新 CMake 缓存: $cached_source -> $FEX_SRC"
        rm -rf "$build/CMakeFiles"
        rm -f "$build/CMakeCache.txt" "$build/Makefile" "$build/cmake_install.cmake"
    fi
    if [ -f "$build/CMakeCache.txt" ] && [ "${#fex_version_args[@]}" -gt 0 ]; then
        # Reconfigure explicit version metadata even when reusing object files.
        cmake -S "$FEX_SRC" -B "$build" "${fex_version_args[@]}"
    fi
}

prepare_exact_store_cache() {
    local build="$1" expected="$2" flags diagnostics_expected=0 diagnostics_cached=0
    [ -f "$build/CMakeCache.txt" ] || return 0
    flags="$(sed -n 's/^CMAKE_CXX_FLAGS:STRING=//p' "$build/CMakeCache.txt" | head -1)"
    [ "${FEX_EXACTSTORE_DIAGNOSTICS:-0}" = "1" ] && diagnostics_expected=1
    [[ "$flags" == *FEX_WOW64_EXACT_STORE_DIAGNOSTICS* ]] && diagnostics_cached=1
    if [ "$expected" = enabled ] && [[ " $flags " != *' -DFEX_WOW64_EXACT_STORE=1 '* ]]; then
        log "fex-pe exact-store 编译标识变化，刷新 CMake 缓存"
        rm -rf "$build/CMakeFiles"
        rm -f "$build/CMakeCache.txt" "$build/Makefile" "$build/cmake_install.cmake"
    elif [ "$expected" = disabled ] && [[ "$flags" == *FEX_WOW64_EXACT_STORE* ]]; then
        log "fex-ec 检测到 WOW64 exact-store 污染，刷新 CMake 缓存"
        rm -rf "$build/CMakeFiles"
        rm -f "$build/CMakeCache.txt" "$build/Makefile" "$build/cmake_install.cmake"
    elif [ "$expected" = enabled ] && [ "$diagnostics_expected" != "$diagnostics_cached" ]; then
        log "fex-pe exact-store diagnostics 编译标识变化，刷新 CMake 缓存"
        rm -rf "$build/CMakeFiles"
        rm -f "$build/CMakeCache.txt" "$build/Makefile" "$build/cmake_install.cmake"
    fi
}

# ---- libarm64ecfex.dll (x86_64 模拟, arm64ec ABI) ----
fex_version_args=()
# Isolated staged sources have no .git; avoid detecting the enclosing product
# repository as FEX's own version. Callers can supply their verified source pin.
if [ -n "${FEX_SOURCE_HASH:-}" ]; then
    [[ "$FEX_SOURCE_HASH" =~ ^[0-9a-f]{8,40}$ ]] || err "FEX_SOURCE_HASH 必须为已验证的源码 hash"
    fex_source_version="${FEX_SOURCE_VERSION:-ohos-${FEX_SOURCE_HASH:0:10}}"
    [ "${#fex_source_version}" -lt 31 ] || err "FEX_SOURCE_VERSION 太长，无法放入 CPUID 版本字段"
    fex_version_args+=("-DOVERRIDE_HASH=$FEX_SOURCE_HASH" "-DOVERRIDE_VERSION=$fex_source_version")
fi
build_fex_ec() {
    local build="$BUILD_DIR/fex-ec"
    mkdir -p "$build"
    prepare_build_dir "$build"
    prepare_exact_store_cache "$build" disabled
    prepare_mul24_cache "$build" 0
    cd "$build"
    if [ ! -f CMakeCache.txt ]; then
        # BUILD_TESTING=False: FEX 用 CTest 的 BUILD_TESTING (非 BUILD_TESTS)
        # 控制 unittests/, 开着会在 configure 阶段 enable_language(ASM_NASM)
        # 硬依赖 nasm — 我们只编 dll 目标, 显式关掉
        cmake -DCMAKE_BUILD_TYPE=RelWithDebInfo \
            -DCMAKE_TOOLCHAIN_FILE="$FEX_SRC/Data/CMake/toolchain_mingw.cmake" \
            -DENABLE_LTO=False \
            -DMINGW_TRIPLE=arm64ec-w64-mingw32 \
            -DBUILD_TESTING=False \
            "${fex_version_args[@]}" \
            "$FEX_SRC"
    fi
    require_cmake_not_debug CMakeCache.txt "fex-ec"
    require_cmake_flag_var CMakeCache.txt CMAKE_CXX_FLAGS_RELWITHDEBINFO "fex-ec CMAKE_CXX_FLAGS_RELWITHDEBINFO"
    require_ndebug "fex-ec CMAKE_CXX_FLAGS_RELWITHDEBINFO" \
        "$(sed -n 's/^CMAKE_CXX_FLAGS_RELWITHDEBINFO:STRING=//p' CMakeCache.txt | head -1)"
    make -j"$JOBS" arm64ecfex

    local dll="$OUT_DIR/libarm64ecfex.dll"
    test -f "$dll" || err "arm64ecfex 构建失败: $dll 不存在"
    # arm64ec 验证: file 显示 "x86-64" 是 magic 误识别, 以 llvm-readobj 为准
    local readobj="$LLVM_MINGW/bin/llvm-readobj"
    if "$readobj" --file-headers "$dll" 2>/dev/null | grep -q "COFF-ARM64EC"; then
        log "OK: libarm64ecfex.dll 为 arm64ec PE"
    else
        warn "架构异常: $("$readobj" --file-headers "$dll" 2>/dev/null | grep -m1 'Format:')"
    fi
    log "产物: $dll (assemble.sh 归位到 aarch64-windows/)"
}

# ---- libwow64fex.dll (i386 / 32 位 x86 模拟, aarch64 ABI) ----
# 与 arm64ecfex 使用不同 MINGW_TRIPLE (aarch64-w64-mingw32), 必须用独立 build
# 目录 (fex-pe), 避免 CMake 缓存与 arm64ec 配置互相覆盖。
build_fex_pe() {
    local build="$BUILD_DIR/fex-pe" exact_store_flags="-DFEX_WOW64_EXACT_STORE=1"
    if [ "${FEX_EXACTSTORE_DIAGNOSTICS:-0}" = "1" ]; then
        exact_store_flags="$exact_store_flags -DFEX_WOW64_EXACT_STORE_DIAGNOSTICS=1"
    fi
    if [ "${FEX_STRICT_MUL24:-0}" = 1 ]; then
        exact_store_flags="$exact_store_flags -DFEX_WOW64_STRICT_MUL24=1"
    fi
    mkdir -p "$build"
    prepare_build_dir "$build"
    prepare_exact_store_cache "$build" enabled
    prepare_mul24_cache "$build" "${FEX_STRICT_MUL24:-0}"
    cd "$build"
    if [ ! -f CMakeCache.txt ]; then
        cmake -DCMAKE_BUILD_TYPE=RelWithDebInfo \
            -DCMAKE_TOOLCHAIN_FILE="$FEX_SRC/Data/CMake/toolchain_mingw.cmake" \
            -DCMAKE_CXX_FLAGS="$exact_store_flags" \
            -DENABLE_LTO=False \
            -DMINGW_TRIPLE=aarch64-w64-mingw32 \
            -DBUILD_TESTING=False \
            "${fex_version_args[@]}" \
            "$FEX_SRC"
    fi
    require_cmake_not_debug CMakeCache.txt "fex-pe"
    require_cmake_flag_var CMakeCache.txt CMAKE_CXX_FLAGS_RELWITHDEBINFO "fex-pe CMAKE_CXX_FLAGS_RELWITHDEBINFO"
    require_ndebug "fex-pe CMAKE_CXX_FLAGS_RELWITHDEBINFO" \
        "$(sed -n 's/^CMAKE_CXX_FLAGS_RELWITHDEBINFO:STRING=//p' CMakeCache.txt | head -1)"
    grep -q '^CMAKE_CXX_FLAGS:STRING=.*FEX_WOW64_EXACT_STORE' CMakeCache.txt || \
        err "fex-pe 缓存缺少 FEX_WOW64_EXACT_STORE"
    if [ -f "$BUILD_DIR/fex-ec/CMakeCache.txt" ] && \
       grep -q '^CMAKE_CXX_FLAGS:STRING=.*FEX_WOW64_EXACT_STORE' "$BUILD_DIR/fex-ec/CMakeCache.txt"; then
        err "fex-ec 缓存被 FEX_WOW64_EXACT_STORE 污染"
    fi
    make -j"$JOBS" wow64fex

    local dll="$build/Bin/libwow64fex.dll"
    test -f "$dll" || err "wow64fex 构建失败: $dll 不存在"
    local readobj="$LLVM_MINGW/bin/llvm-readobj"
    if "$readobj" --file-headers "$dll" 2>/dev/null | grep -q "COFF-ARM64"; then
        log "OK: libwow64fex.dll 为 aarch64 PE"
    else
        warn "架构异常: $("$readobj" --file-headers "$dll" 2>/dev/null | grep -m1 'Format:')"
    fi
    log "产物: $dll (assemble.sh 归位到 aarch64-windows/)"
}

build_fex_ec
build_fex_pe
log "FEX 构建完成 (arm64ecfex + wow64fex)"
