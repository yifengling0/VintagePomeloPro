# Wine for HarmonyOS — 构建指南

> 最后更新: 2026-07-13

## 环境

构建环境搭建详见 [BUILD_ENV.md](./BUILD_ENV.md)。

快速检查：

```bash
# 必备工具
make cmake ninja meson bison flex autoconf libtoolize gcc-mingw-w64-x86-64 java
# OHOS SDK: /apps/harmony/sdk/default/openharmony/
# wayland-scanner: /usr/local/bin/wayland-scanner (需预装)
```

---

## 平台

| 平台 | Makefile 命令 | `NATIVE_ARCH` | Wine | Box64 | 打包 |
|------|-------------|---------------|------|-------|------|
| arm64 | `make NATIVE_ARCH=arm64-v8a` | `arm64-v8a` | x86_64 ELF → rawfile zip | box64.so (dlopen) | rawfile zip |
| x86_64 | `make NATIVE_ARCH=x86_64` | `x86_64` | Wine .so → libs/ | 无 | rawfile zip |
| 双架构 HAP | `make NATIVE_ARCH=all` | 双架构 | 同上 | arm64 含 Box64 | rawfile zip |

默认值：`NATIVE_ARCH=x86_64`。

> Wine 和 wineserver 通过 NCP（`OH_Ability_StartNativeChildProcess`）创建子进程。arm64 下 Box64 编译为 box64.so 由 NCP 子进程 dlopen 加载。

---

## Makefile 构建（推荐）

### 完整构建

```bash
# 默认: x86_64
make

# arm64
make NATIVE_ARCH=arm64-v8a

# 双架构 HAP
make NATIVE_ARCH=all
```

### 单阶段构建

```bash
make deps                          # 交叉编译依赖 → build/sysroot-ext/
make wine                          # Wine + wineserver
make box64                         # Box64 ARM64 翻译器 (仅 arm64)
make native                        # 各架构原生 compositor 依赖
make assemble                      # 组装布局
make hap                           # HAP 打包 + 签名
```

`entry/src/main/resources/rawfile/wine-data.zip`、`wine_runtime.version` 和
`entry/libs/` 是 Git 忽略的构建产物。切换提交或更新 Wine 子模块后，先运行
`make NATIVE_ARCH=arm64-v8a hap` 重新组装；直接运行 `hvigorw assembleHap`
只会打包当前目录中的文件。Hvigor 现在会核对运行时版本中的子模块提交和
`wine-data.zip` 的 SHA-256，不匹配时停止打包。CI 还会检查 HAP 内嵌运行时
与源码清单是否一致。

### 增量构建

```bash
# 只改 ArkTS / ets 文件
make NATIVE_ARCH=arm64-v8a hap

# 改了 entry/src/main/cpp/ 下的 C++ 代码
make NATIVE_ARCH=arm64-v8a hap

# 改了 Wine C 源码 (thirdparty/wine/)
make NATIVE_ARCH=arm64-v8a wine && make NATIVE_ARCH=arm64-v8a hap

# 改了 native compositor 依赖 (thirdparty/virglrenderer, libepoxy, wayland 等)
make NATIVE_ARCH=arm64-v8a native && make NATIVE_ARCH=arm64-v8a hap

# 完全清理
make clean
```

### Stamp 文件

构建状态记录在 `build/.stamps/` 下：

```
build/.stamps/
├── deps
├── wine-arm64-v8a
├── box64-arm64-v8a
├── arm64-v8a/
│   ├── native
│   └── assemble
└── x86_64/
    ├── native
    └── assemble
```

删除对应 stamp 文件即可强制重跑该阶段。

---

## 环境变量

构建时关键变量（由 `scripts/env.sh` 自动设置）：

| 变量 | 默认 | 说明 |
|------|------|------|
| `NATIVE_ARCH` | 随架构 | `arm64-v8a` 或 `x86_64` |
| `OHOS_SDK` | `/apps/harmony/sdk/default/openharmony` | HarmonyOS SDK 路径 |
| `BUILD_GUEST_GFX` | `0` | 设为 `1` 构建 guest Mesa (VirGL) |
| `BUILD_WINE_MONO` | `0` | 设为 `1` 下载 Wine Mono (.NET 运行时) |

运行时变量（由 `wine_env.cpp` / `wine_child.cpp` 设置）：

| 变量 | 作用 |
|------|------|
| `BOX64_LD_LIBRARY_PATH` | Box64 搜索 x86_64 .so 的路径 |
| `GALLIUM_DRIVER` | Mesa Gallium 驱动 (`virpipe` 启用 VirGL) |
| `LIBGL_DRIVERS_PATH` | Mesa DRI 驱动路径 |
| `VTEST_SOCKET_NAME` | VirGL socket 路径 |
| `WINEDEBUG` | Wine 调试频道 (`-all` 关闭) |
| `XKB_CONFIG_ROOT` | XKB 键盘布局数据路径 |
| `WINE_MONO` | 设为 `never` 禁用 .NET 运行时 |

---

## 产物说明

### Pad (rawfile zip + libs/)

```
entry/
├── libs/arm64-v8a/                    # ARM64 原生 .so
│   ├── box64.so                       # Box64, in-process dlopen
│   ├── libwine_child.so               # NCP 子进程入口
│   ├── libentry.so                    # NAPI 桥接
│   ├── libffi.so, libwayland-*.so     # compositor 依赖
│   ├── libepoxy.so, libvirglrenderer.so
│   └── virgl_test_server              # VirGL host server
├── libs/x86_64/                       # x86_64 原生 .so
│   └── ... (Wine x86_64 .so 直接放 libs)
└── resources/rawfile/
    └── wine-data.zip                  # 运行时解压
        ├── bin/
        │   ├── wine, wineserver       # x86_64 ELF
        │   ├── ntdll.so, *.exe
        │   ├── x86_64-windows/        # PE DLL
        │   ├── x86_64-unix/           # Unix .so
        │   └── guest_gfx/lib/         # Guest Mesa (VirGL)
        └── share/
            ├── wine/ (nls/, fonts/, wine.inf, mono/)
            └── X11/xkb/
```

---

## 相关文档

- [BUILD_ENV.md](./BUILD_ENV.md) — 从零搭建构建环境
- [CURRENT_STATUS.md](./CURRENT_STATUS.md) — 当前功能状态
- [ARCHITECTURE.md](./ARCHITECTURE.md) — 项目架构概览
- [.claude/rules/build-and-log.md](../.claude/rules/build-and-log.md) — 构建命令与调试日志
- [.claude/rules/submodule-workflow.md](../.claude/rules/submodule-workflow.md) — Submodule 管理方案
