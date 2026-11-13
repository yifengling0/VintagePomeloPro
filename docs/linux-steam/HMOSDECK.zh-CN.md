# HmosDeck：OHOS 上的 Steam 与游戏运行能力

HmosDeck 是当前原生 Linux ARM64 Steam 接入方向的能力名称。目标是在既有 OHOS 宿主中使用原版 Steam 登录、管理库和启动游戏，接续现有图形、输入与游戏后端；名称不意味着当前应用或 bundle 已改名。

## 客户端来源

使用 Valve 官方更新源 `https://client-update.steamstatic.com/` 的 Linux ARM64 包，包含 `bins_linuxarm64_linuxarm64`、`webkit_linuxarm64_linuxarm64`、SDL3 和客户端资源，入口是 `steamrtarm64/steam`。这是与 Steam Frame 相同的 ARM64 Linux 客户端方向，当前工作不是移植整套 SteamOS 系统，也不声称已经验证 Steam Frame 的全部设备功能。

原版客户端 ELF 的校验值保持不变；GNU libc、系统接口与显示适配在外部运行环境完成。Steam 本身由 ARM64 CPU 直接执行。Steam 与 GNU 文件从 Wine Z 盘对应目录导入，按 manifest 校验并独立更新，不进入 HAP。

## 运行分层

| 层 | 设计 | 当前证据 |
| --- | --- | --- |
| OHOS 宿主 | 既有应用、窗口、输入、生命周期与私有文件权限 | 原生 GNU 和 Wayland 测试通过 |
| Steam 客户端 | 官方 Linux ARM64 Steam、GNU libc、受限 Linux 接口兼容 | 用户已登录；平板实际显示商店与游戏库，设备点击完成页面往返；持久会话尚未验收 |
| 显示 | Steam X11 → GNU Xwayland → 既有 Wayland | X11 真实窗口和像素读回通过 |
| OpenGL | GNU Mesa → 既有 vtest/virglrenderer 接收端 | Maleoon 910 上 GLX 18 帧绘制、显示及回读通过 |
| 游戏 | 按游戏架构、操作系统与图形接口选择后端 | 尚未通过 Steam 实际启动游戏验收 |

原生 ARM64 Steam 与游戏的 CPU 架构要分别判断。ARM64 Linux 游戏可以使用原生执行路径；x86/x64 游戏仍需要 FEX 等 CPU 转译。Wine/Proton 处理 Windows 接口，DXVK 需要可用的 Vulkan 链路，不能由 OpenGL 测试通过推断已兼容。

## 验收顺序

1. 原版客户端：登录页可见、真实键鼠输入、稳定退出及重启，保护配置目录免受并发写入损坏。
2. Steam 基础功能：账号登录、库、下载、校验与独立资源更新，沿用真实 TLS 证书验证。
3. 一个可控的原生 ARM64 程序由 Steam 启动，验证环境、进程回收、输入和显示。
4. 按保留现有 OHOS Proton/Wine 的方案，先验证 GNU Steam 接口辅助进程及 Wine 侧认证桥，再由兼容工具交接到现有游戏启动接口，回归当前 DXVK／virgl／FEX 路径。GNU Vulkan 游戏为独立的后续能力，不作为 Windows 游戏接入前置条件。

2026-10-19，v29 / external-64 已通过平板屏幕实际显示及应用内点击验证：商店 → 游戏库 → 关闭好友窗口 → 返回商店。用户另行确认“主界面／游戏库已显示”。登录后的 SIGSEGV 由外置运行时补齐标准 Xlib locale 数据解决，原版 Valve ELF 未改。当前仍为最长 15 分钟的诊断会话；下载、游戏启动、完整输入、CEF GPU 加速及正常产品生命周期另行验收。

诊断模式禁用 CEF 沙箱或 GPU 的参数不代表正式配置。当前 X11 输出仍有读回与拷贝；零拷贝、完整 Steam 图形加速及游戏兼容性分别验收。具体轮次证据见 [阶段 5](CLIENT_PHASE_5.zh-CN.md) 和 [共享接收端](SHARED_MESA_RECEIVER.zh-CN.md)。

当前启动命令、USB 平板包装脚本、窗口候选与 GPU／分辨率边界见 [启动方式](LAUNCHER.zh-CN.md)。ARM64 启动尚未接入普通 Windows Steam 卡片，独立子窗口已验证显示／点击／边缘缩放及菜单退出，共同 Wine 桌面融合仍未通过。

后续按用户约束保留当前 OHOS Proton/Wine 与 DXVK/virgl 路径；GNU Wine 不作为此次方案。游戏下载到 Z:\Steam，兼容工具交接启动，独立 GNU Steam 接口进程连接官方客户端。DroidDeck 提供兼容工具与拥有状态探针的参考，但其 GNU Proton 不等于 OHOS Wine 已具备跨 libc 认证。详见 [游戏启动设计](GAME_LAUNCH_DESIGN.zh-CN.md) 与 [DroidDeck 源码对照](DROIDDECK_REVIEW.zh-CN.md)。

2026-10-19 接口验证补充：v31 / external-69、70 已通过独立 GNU helper 查询官方 Steam 登录状态，以及当前 OHOS Wine Windows x64 PE → GNU helper → 官方 steamclient 的只读诊断通信。尚未实现完整 SteamAPI_Init、回调／票据或由 Steam 启动 Windows 游戏；图形后端保持现有实现。见 [接口验证](STEAMCLIENT_PROBE.zh-CN.md)。
