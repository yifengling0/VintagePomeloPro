#!/bin/bash
#
# build.sh — Wine for HarmonyOS 构建入口
#
# 用法:
#   ./build.sh {command} [device_ip] [arch]
#
# arch: arm64 (默认) | x86_64
#
# 命令:
#   full       全量构建 (含依赖)
#   deps       模拟层交叉编译依赖 (Wine用, $WINE_ARCH-linux-ohos)
#   native     Native compositor 依赖 (按架构)
#   wine       构建 Wine
#   fex        构建 FEX 模拟器 DLL (方案③ arm64 原生转译 x64/x86 应用)
#   box64      构建 Box64 box64.so (方案② box64+wine, arm64 设备 + x86_64 wine)
#   box64-wow64 构建 wowbox64.dll (方案③ HODLL 默认引擎, 转译 32 位 x86 应用)
#   assemble   组装布局 (按架构)
#   hap        构建 HAP + 签名 (按架构)
#   deploy     推送到设备并安装
#   quick      assemble → hap → deploy
#
set -euo pipefail

ROOT="$(cd "$(dirname "$0")" && pwd)"
SCRIPTS="$ROOT/scripts"

DEFAULT_IP="192.168.1.4:38879"

# ── 参数解析 ──
cmd="${1:-}"
device_ip="${DEFAULT_IP}"
arch="arm64"

case $# in
    0) ;;
    1) cmd="$1" ;;
    *)
        cmd="$1"
        shift
        device_ip="${DEFAULT_IP}"
        arch="arm64"
        for arg in "$@"; do
            if [[ "$arg" == *":"* ]] || [[ "$arg" =~ ^[0-9]+\.[0-9]+\.[0-9]+\.[0-9]+$ ]]; then
                device_ip="$arg"
            elif [ "$arg" = "arm64" ] || [ "$arg" = "x86_64" ]; then
                arch="$arg"
            fi
        done
        ;;
esac

# ── 验证 arch ──
case "$arch" in
    arm64) NATIVE_ARCH="arm64-v8a" ;;
    x86_64) NATIVE_ARCH="x86_64" ;;
    *) echo "错误: arch 必须是 arm64 | x86_64 (NATIVE_ARCH=all 不再支持)"; exit 1 ;;
esac

export NATIVE_ARCH

# 让 WINE_ARCH / TARGET 等可用 (方案② = WINE_ARCH=x86_64 可经环境传入;
# 默认由 NATIVE_ARCH 推导: arm64→aarch64 方案③, x86_64→x86_64 方案①)
source "$SCRIPTS/env.sh"

# 与 Makefile 对齐: guest 栈架构 = wine 架构 (方案② x86_64, 方案③ aarch64);
# guest gfx/vulkan 默认构建 (Makefile:22-23 BUILD_GUEST_GFX/BUILD_GUEST_VULKAN ?= 1)
export GUEST_ARCH="${GUEST_ARCH:-$WINE_ARCH}"
export BUILD_GUEST_GFX="${BUILD_GUEST_GFX:-1}"
export BUILD_GUEST_VULKAN="${BUILD_GUEST_VULKAN:-1}"

# ── 工具函数 ──
log()  { echo -e "\033[32m[BUILD]\033[0m $*"; }
warn() { echo -e "\033[33m[WARN]\033[0m $*"; }

run_native() {
    local a="${1:-arm64-v8a}"
    NATIVE_ARCH="$a" bash "$SCRIPTS/build_native.sh"
}

run_assemble() {
    local a="${1:-arm64-v8a}"
    NATIVE_ARCH="$a" bash "$SCRIPTS/assemble.sh"
}

run_deps() {
    bash "$SCRIPTS/build_deps.sh"
}

run_wine() {
    bash "$SCRIPTS/build_wine.sh"
}

run_fex() {
    bash "$SCRIPTS/build_fex.sh"
}

run_box64() {
    bash "$SCRIPTS/build_box64.sh"
}

run_box64_wow64() {
    bash "$SCRIPTS/build_box64_wow64.sh"
}

run_hap() {
    local a="${1:-arm64-v8a}"
    NATIVE_ARCH="$a" bash "$SCRIPTS/package.sh" hap
}

run_deploy() {
    bash "$SCRIPTS/package.sh" deploy "$device_ip"
}

# ── 架构迭代 (仅单架构; NATIVE_ARCH=all 不再支持) ──
for_each_arch() {
    local fn="$1"
    $fn "$NATIVE_ARCH"
}

# assemble 单步完成 (assemble 会清除 staging 目录)
for_each_arch_assemble() {
    NATIVE_ARCH="$NATIVE_ARCH" bash "$SCRIPTS/assemble.sh"
}

# ── 命令处理 ──
case "$cmd" in
    deps)
        run_deps
        ;;
    native)
        for_each_arch run_native
        ;;
    wine)
        run_wine
        ;;
    fex)
        run_fex
        ;;
    box64)
        run_box64
        ;;
    box64-wow64)
        run_box64_wow64
        ;;
    assemble)
        # assemble 布局
        for_each_arch_assemble
        ;;
    hap)
        run_hap "$NATIVE_ARCH"
        ;;
    deploy)
        run_deploy
        ;;
    quick|full)
        run_deps
        run_wine
        # 方案③ (arm64 原生) → FEX DLL + wowbox64.dll (HODLL 默认引擎, 缺则 32 位应用失败);
        # 方案② (box64+wine) → box64.so
        if [ "$WINE_ARCH" = "aarch64" ]; then
            run_fex
            run_box64_wow64
        fi
        [ "$WINE_ARCH" = "x86_64" ] && [ "$NATIVE_ARCH" = "arm64-v8a" ] && run_box64 || true
        for_each_arch run_native
        for_each_arch_assemble
        NATIVE_ARCH="$NATIVE_ARCH" bash "$SCRIPTS/package.sh" hap
        [ "$cmd" = "quick" ] && run_deploy || true
        ;;
    pad)
        # Pad 构建 (fork-only, 无 execve)
        run_deps
        run_wine
        # 方案③ (arm64 原生) → FEX DLL + wowbox64.dll; 方案② (box64+wine) → box64.so
        if [ "$WINE_ARCH" = "aarch64" ]; then
            run_fex
            run_box64_wow64
        fi
        [ "$WINE_ARCH" = "x86_64" ] && [ "$NATIVE_ARCH" = "arm64-v8a" ] && run_box64 || true
        for_each_arch run_native
        for_each_arch_assemble
        NATIVE_ARCH="$NATIVE_ARCH" bash "$SCRIPTS/package.sh" hap
        log "Pad HAP 构建完成"
        ;;
    pad-hap)
        # Pad 仅 HAP (只改 ArkTS/napi_init.cpp 时用，跳过 Wine 重编译)
        for_each_arch run_native
        for_each_arch_assemble
        NATIVE_ARCH="$NATIVE_ARCH" bash "$SCRIPTS/package.sh" hap
        log "Pad HAP 构建完成"
        ;;
    pad-deploy)
        bash "$SCRIPTS/package.sh" deploy "$device_ip"
        ;;
    *)
        echo "用法: $0 {full|deps|native|wine|fex|box64|box64-wow64|assemble|hap|deploy|quick|pad|pad-deploy} [device_ip] [arch]"
        echo ""
        echo "  arch: arm64 (默认) | x86_64"
        echo "  方案② (box64+wine): WINE_ARCH=x86_64 $0 full <ip> arm64"
        echo ""
        echo "  通用命令:"
        echo "    full       全量构建 (首次使用)"
        echo "    deps       模拟层交叉编译依赖"
        echo "    native     Native compositor 依赖"
        echo "    wine       构建 Wine"
        echo "    fex        构建 FEX (方案③ arm64 原生转译 x86_64)"
        echo "    box64      构建 Box64 box64.so (方案② box64+wine)"
        echo "    box64-wow64 构建 wowbox64.dll (方案③ 32 位应用 HODLL 默认引擎)"
        echo "    assemble   组装布局"
        echo "    hap        构建 HAP + 签名"
        echo "    deploy     推送到设备并安装"
        echo "    quick      快速: assemble → hap → deploy"
        echo ""
        echo "  Pad 快捷命令:"
        echo "    pad <arch>        构建 Pad HAP (arm64|x86_64)"
        echo "    pad-hap <arch>    仅 Pad HAP (只改 ArkTS/native 时用)"
        echo "    pad-deploy <ip>   推送并安装"
        exit 1
        ;;
esac
