# DroidDeck 源码对照：Steam、Proton 与认证边界

2026-10-19 静态源码核对。对象为 [Droid-Deck/DroidDeck](https://github.com/Droid-Deck/DroidDeck)，固定提交 `ebc7f89fe32801f57cdb18fae4b1375b6540ee5d`；未安装或运行 DroidDeck。不能把其文档中的设备结果记作 OHOS 验收。

## 源码确认的机制

| 问题 | DroidDeck 实现 | 对当前工程的意义 |
|---|---|---|
| Linux 环境 | `LinuxRuntime.java` 提供 app-private linuxfs 与 proot；`SessionService.kt` 通过 LinuxRuntime.command 启动会话并设置目录绑定 | Steam 和 GNU Proton 处在匹配的 GNU/Linux 用户态，并非 Android/Bionic Wine 直接加载 GNU 库 |
| 原版客户端 | `droiddeck-session` 选择 `steamrtarm64/steam`；游戏模式使用 gamescope；桌面模式可继承桌面 Xwayland | 可以参考会话与窗口管理，不能把 gamescope 当作 OHOS 的先决条件 |
| Windows 游戏入口 | `steam-compatibility` 注册 `compatibilitytools.d`，声明 windows → linux，`toolmanifest.vdf` 使用 `%verb%`、`use_sessions=1` | 应通过 Steam 的兼容工具契约接管启动，不扫描 exe 并绕开原客户端启动流程 |
| Proton 启动 | 工具包装器找到官方 ARM64 Proton depot，保留 verb/argv，调用其 `proton`；辅助 `droiddeck-proton-run` 使用 `waitforexitandrun` 和 `STEAM_COMPAT_*` | 可借鉴参数、目录、兼容工具选择与退出语义；该实现没有解决当前 OHOS Wine 的跨 libc 边界 |
| 游戏库 | `GameStorage.kt`、`droiddeck-steam-library` 管理真实库路径和 libraryfolders.vdf；`SecondaryLibrary.kt` 将 compatdata/shadercache 等按需分开映射 | 大文件库与依赖 Unix 文件语义的运行数据应分开管理 |
| 反向查询原版 Steam | `droiddeck-steam-games:122–147` 在独立 GNU 进程用 ctypes 加载 ARM64 `steamclient.so`，取得 SteamClient020、真实 pipe/global user、SteamUser021 和 SteamApps008，查询登录及 BIsSubscribedApp，最后释放 user/pipe | 可直接借鉴“GNU 辅助进程连接已登录客户端”的验证步骤；它只是拥有状态查询，不是 Windows Steam API 远程调用的完整实现 |

已读的上述启动与查询源码中没有发现“将完整 Steam API 转发给 Android/Bionic Wine”的实现。该结论有范围：本轮按固定源码路径核对，未对仓库所有二进制资源作逆向，也未独立验收其 DRM/游戏兼容性。

## DXVK、GNU Vulkan 与 GPU 驱动

同时核对 [Droid-Deck/Drivers](https://github.com/Droid-Deck/Drivers) 固定提交 `6c3f8592535d14e2f1dbe845d0baf7c594b9938f`。下面是静态源码确认的架构，不代表本轮运行过 DroidDeck 游戏。

DXVK 的 D3D 实现主要是由 Wine 加载的 Windows PE DLL，须匹配游戏／Wine 的机器架构和调用约定；不能把它当作 GNU `.so`，也不需要为了“原生 OHOS”将这些 DLL 直接改成 OHOS ELF。GNU／Bionic／OHOS 的 ABI 区别主要落在 Wine 的 Unix 侧、Vulkan loader、ICD 及其依赖。

DroidDeck 的 GNU Proton 图形链路可概括为：

```text
游戏 D3D → DXVK（Windows DLL）→ Wine Vulkan 适配
        → GNU Vulkan loader → glibc Turnip ICD
        → KGSL 内核接口 → Adreno GPU
```

关键证据：

- Drivers `README.md:26–48`：一个包分别提供 `android/libvulkan_freedreno.so`（Bionic）与 `linux/libvulkan_freedreno.so`（glibc），二者并非同一个 ABI 的库。
- Drivers `scripts/build_linux.sh:15–46`：使用 `aarch64-linux-gnu-gcc/g++` 与 Linux sysroot，配置 `vulkan-drivers=freedreno`、`freedreno-kmds=msm,kgsl`、`platforms=wayland,x11`，生成 GNU ARM64 Turnip。
- Drivers `scripts/build_android.sh:14–46`：另用 Android NDK、`system=android`、`platforms=android`、`freedreno-kmds=kgsl` 编译 Android 版本。
- DroidDeck `LinuxRuntime.java:93–100` 优先选择 freedreno ICD；`SessionService.kt:567` 设置 `VK_ICD_FILENAMES`。`LinuxRuntime.java:240–278` 将真实 `/dev/kgsl-3d0` 暴露给 GNU 会话。
- Drivers `patches/scripts/linux_kgsl_wsi.py:10–33`：补齐 KGSL 的设备标识与 dma-buf feedback，并按真实时间戳能力限制扩展，说明内核访问之外还需要窗口呈现与能力适配。
- DroidDeck `DriverBundle.kt:54–82` 分别安装 Android 与 Linux 两份驱动。

因此，GNU Wine 并不必然增加一层 GPU 模拟或把 Vulkan 请求转发到 Android 厂商驱动。此方案可以由 glibc Turnip 访问同一设备内核接口；Android 原生 Bionic Vulkan 驱动不是该 GNU 链路的必经点。rootfs 的存在本身也不意味着像素复制或 CPU 指令转译。实际性能仍受驱动、同步、窗口呈现与运行后端影响。

这依赖 Adreno／Turnip／KGSL 支持。不能把此驱动直接用于华为 Maleoon，也不能因同为 ARM64 就将 OHOS 的 Vulkan `.so` 直接 dlopen 到 glibc 进程；ABI、依赖、窗口对象和同步契约仍需匹配。

当前 HmosDeck 继续使用现有 OHOS Proton/Wine 图形后端：DXVK 经已有 Wine Vulkan 适配连接 OHOS 原生 Vulkan／驱动，保留现有 Direct／Venus 等已支持配置；OpenGL 游戏继续使用现有 virgl 路径。GNU 侧承载 Steam 与认证辅助进程，Steam API IPC 不承载 Vulkan 调用、纹理或鸿蒙图形对象。若未来支持 GNU Vulkan 游戏，另立 loader／ICD／WSI 及同步的实现与验收任务，不将其当作当前 Steam 接入的前置条件。

## Proton 上游的实际桥接点

另核对 ValveSoftware/Proton 的 `proton_10.0` 分支固定提交 `e91ca2be0df2cef4c230cbbc0b86604d73a0bbf6`：

- `lsteamclient/steamclient_main.c:83`：Windows 侧通过 `WINE_UNIX_CALL(code,args)` 进入 Unix 适配。
- `lsteamclient/unixlib.cpp:734–798`：ARM64 使用 `~/.steam/sdkarm64/steamclient.so`，通过 dlopen 加载，取得 CreateInterface、Steam_BGetCallback、Steam_GetAPICallResult 等官方导出。
- `lsteamclient/unixlib.h` 包含字符串、输入／输出指针、缓冲区、回调与 WoW64 参数；`gen_wrapper.py` 生成接口与 ABI 转换，并有手工处理方法。

上游 Unix-call 参数默认在同一进程地址空间，不能把整块参数 struct 原样发送到另一个进程。当前 OHOS Wine 不能直接 dlopen GNU `steamclient.so`。复用上游 Windows 接口与转换思路后，仍需新增有类型的跨进程封送、对象生命周期和回调通道。

## 采用方式

按用户约束保留现有 OHOS Proton/Wine、DXVK、virgl 与鸿蒙对象传递。GNU 侧仅承载原版 Steam、匹配的 steamclient.so 和 Steam 接口辅助进程；不把 GNU Wine 作为默认方案，不让图形对象跨这条 Steam RPC 通道。

第一步应复现独立只读探针：连接真实登录会话，查询用户可用性及指定 AppID 的拥有状态，并实际释放对象。随后验证当前 Wine → 本地 RPC → GNU steamclient 的最小请求／响应，再分项扩展 SteamAPI_Init、真实票据与异步回调。拥有状态为 true 不能直接当作某个 DRM 已通过。

完整下载、启动、认证与窗口方案见 [游戏启动设计](GAME_LAUNCH_DESIGN.zh-CN.md)。

## 固定源码入口

- [DroidDeck 双 ABI Turnip 驱动说明](https://github.com/Droid-Deck/Drivers/blob/6c3f8592535d14e2f1dbe845d0baf7c594b9938f/README.md#L26)
- [GNU Turnip 编译与 KGSL／Wayland／X11 配置](https://github.com/Droid-Deck/Drivers/blob/6c3f8592535d14e2f1dbe845d0baf7c594b9938f/scripts/build_linux.sh#L15)
- [GNU 会话选择 Vulkan ICD](https://github.com/Droid-Deck/DroidDeck/blob/ebc7f89fe32801f57cdb18fae4b1375b6540ee5d/app/src/main/java/com/droiddeck/launcher/session/SessionService.kt#L567)

- [DroidDeck 的兼容工具注册与 Proton 包装器](https://github.com/Droid-Deck/DroidDeck/blob/ebc7f89fe32801f57cdb18fae4b1375b6540ee5d/tools/linuxfs/overlay/usr/local/bin/steam-compatibility#L33)
- [DroidDeck 的官方 Steam 接口查询](https://github.com/Droid-Deck/DroidDeck/blob/ebc7f89fe32801f57cdb18fae4b1375b6540ee5d/tools/linuxfs/overlay/usr/local/bin/droiddeck-steam-games#L122)
- [DroidDeck 的外置库映射](https://github.com/Droid-Deck/DroidDeck/blob/ebc7f89fe32801f57cdb18fae4b1375b6540ee5d/app/src/main/java/com/droiddeck/launcher/session/SecondaryLibrary.kt#L56)
- [DroidDeck 桌面 Steam 的验证范围](https://github.com/Droid-Deck/DroidDeck/blob/ebc7f89fe32801f57cdb18fae4b1375b6540ee5d/docs/testing/steam-in-kde.md)
- [Proton ARM64 steamclient 装载](https://github.com/ValveSoftware/Proton/blob/e91ca2be0df2cef4c230cbbc0b86604d73a0bbf6/lsteamclient/unixlib.cpp#L734)

下载的公开文本源码、来源和 SHA-256 分别保存在本机 artifacts/steam-linux-arm64-client-20261019/droiddeck-research/sources.json、droiddeck-research/driver-sources.json 与 proton-design-references/sources.json。源码只用于本轮调查，未将 DroidDeck 代码合入宿主。

当前工程已于 2026-10-19 在平板完成上述只读探针，并用当前 Wine 下的 Windows x64 PE 程序验证有版本和 nonce 的本机诊断通信。loggedOn=true、真实接口查询和 user／pipe 释放均有证据；这不是完整 lsteamclient 或游戏认证实现。见 [首轮接口验证](STEAMCLIENT_PROBE.zh-CN.md)。
