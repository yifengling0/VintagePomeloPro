# Game 模式 Windows Steam 与正版游戏评价

## 范围与身份

本轮只在 Mini MLR-AL10 / Maleoon 910 操作，保留 Steam 登录、Wine prefix、游戏和存档。
用户明确将 Steam -118 归为网络问题，本轮不追查商店、网络或游戏服务器连接提示。
不改版本、不合 main、不推送、不上架，分支仍为 `feature/gamescope-port`。

渲染修复源码为 `b3b30da29518ccf0f3f5b245134161232d4dcb11`；
启动等待修复为 `818201af53feb8b7a1bd68782376a8ccafda5288`。
后者只改变 ArkTS 启动等待和主机行为测试，所有原生库、Wine/FEX/DXVK/Mesa 载荷与前者逐成员相同。

本轮沿用 Mini 的 DXVK 1.10 Legacy 宿主配置和 FEX，显示为 Wayland + Game/Gamescope。
Steam CEF 命令实际包含 `-cef-disable-gpu`，因此不能称为 ANGLE 硬件 D3D11 已验收。
前序 `ea87bd10` 的 CEF D3D9 隔离继续保留，DXVK 1.10 D3D11/DXGI 未被替换。

## 已修复的问题

### 菜单或小窗口覆盖成整屏黑色

旧层设置 `blackBorder=true`，不透明层采用 `ALPHA_BLENDING_MODE_NONE`。
shader 对窗口外采黑色，NONE 又替换整个输出，导致任务栏、菜单或小窗口擦除背后的 Steam 库。

`b3b30da2` 将合成层设为预乘透明混合，矩形外透明；不透明 SHM 使用 XRGB，原生不透明
图像采样强制 alpha=ONE，避免游戏 framebuffer 的 alpha=0 使真实游戏变透明。
Gamescope 补丁为 `0025-allow-opaque-native-sampling-with-transparent-bounds.patch`。
真实 SPIR-V 双层 GPU 回读正例通过，旧配置的负例能触发并被拒绝。

Mini 上打开 Steam 菜单，菜单外游戏库保持完整；回 VPP 库再进入 Steam 也正常。
Banana 的 GPU 客户区无标题栏，未观察到原整屏黑色回归。

### 慢启动恰好越过 45 秒期限

正常退出 Steam 后重新打开，日志记录 `00:35:47.319` 创建 steam.exe，
`00:36:32.375` 才创建 CEF 实际窗口，间隔约 45.06 秒。
VPP 原来的 45 秒等待先到期，界面显示“未显示客户端窗口”，窗口实际稍后已存在；
再次点启动即可打开完整本地库。这是已定位的本地等待边界，不能归因于 -118。

`818201af` 将等待上限改为 90 秒，继续保持按真实进程、会话和受管理客户端窗口判定。
Windows 会话停止仍即时中断，主动导航仍优先；超时不杀 Steam，不重置数据。
行为测试覆盖 60 秒后才出现 CEF 窗口时自动呈现、完全无窗口时有界退出，以及原有
bootstrap 换 PID、CEF 独立会话、回收 PID、错误传播和导航隔离。

## b3b30da2 真机流程

| 动作 | 观察结果 |
| --- | --- |
| 打开 Steam 本地库 | 完整中文库、保留登录；此前冷启动曾停留加载画面，需要重新打开库 |
| 打开菜单 / 返回 VPP 库再进 Steam | 背景完整，无原整屏黑色回归 |
| 从 Steam 内 Banana 详情点击“开始游戏” | 实际 Banana.exe 运行；客户区放大，无标题栏 |
| 游戏输入 | 点击计数 1→2；ESC 打开中文游戏菜单 |
| 游戏菜单退出 | 自动回 Steam Banana 详情，之后恢复“开始游戏” |
| VPP Banana 卡片第二次派发 | 再次创建实际 Banana.exe，点击计数 2→3，ESC 菜单可用 |
| 第二次游戏内退出 | 自动回完整 Steam 库，无黑屏 |
| Steam 菜单最底部“退出” | 主客户端和 CEF 实际退出，自动回 VPP Game 游戏库 |

首轮 Banana.exe 为 `pid=51239`，启动 `00:27:14.887`，退出 `00:29:22.964`；
第二轮为 `pid=52242`，启动 `00:30:04.120`，退出 `00:33:34.448`。
退出均由游戏菜单触发；NCP 记录 `exit=-1 source=ipc-death` 不等于已证明游戏崩溃。
Steam 的正常菜单退出触发 `00:34:57.184` 主客户端完成，CEF 在约 3 秒后完成。

游戏时原生日志显示 `native=1 filter=3 fsrPasses=1 cpuGameCopies=0`。
HUD 瞬时约 59–60 FPS，只是短时功能观察，不是同条件性能收益或长时稳定性结论。
第二轮“退出”第一次点击未生效，第二次成功；不将此记录为输入可靠性全面通过。

## 818201af 最终包回归

覆盖安装且保留数据后，从 Game 页一次点击“启动 Steam”自动到达完整中文本地库，
无需再次点击。steam.exe `00:43:35.996` 创建，真实 CEF 窗口 `00:44:13.791` 出现，
间隔约 37.80 秒；本次未触及旧 45 秒期限，超过 45 秒的等待修复由前述 60 秒行为测试覆盖。

从 Steam 首页 Banana 的真实绿色启动按钮派发，`00:45:14.258` 创建 Banana.exe
`pid=58225`，`00:45:22.744` 自动关联窗口。真实中文 GPU 画面完整、无标题栏，点击计数
3→4，ESC 菜单正常；游戏内退出后回到 Steam Banana 详情并恢复“开始游戏”。
本轮菜单退出也用了第二次点击，触控首击偶发未生效继续列作待改善项。
游戏仍显示 `native=1 filter=3 fsrPasses=1 cpuGameCopies=0`，没有新增 CPU 游戏帧复制。
Banana 在 `00:48:31.695` 完成；随后通过 Steam 菜单正常退出，CEF 在 `00:49:34.003`
完成，steam.exe 在 `00:49:34.921` 以 exit=0 完成，自动回到 VPP Game 的 Steam 入口页。
收尾关闭临时帧诊断，手动切至 Game 游戏库留给用户测试，未强杀会话或注销账号。

## 包与主机验证

最终调试包固定位置为 `workspace_temp/gamescope-port/VPP-2.0.4-gamescope-debug-signed.hap`。
源码 `818201af`，2.0.4 / 2000040、ARM64、API23、debug 签名，633911162 bytes；
SHA-256 `4dfa8e74c544fd19844848abc2200791a2d40e15f32ce546b79e85804cfc3d42`。
剪贴板 Profile、签名、包成员和 application-build 身份核验通过。
回退仍引用 `6f22d375` 的已接受包，SHA-256
`ea7313bc5ed55052cebe24a0f50e308817d80595887716ecc4e67d2eee671290`。

本轮复用的验证包括 Gamescope 配方、game presentation、窗口边界 GPU 回读、色彩/FSR、
OHOS core/native 和 Windows Steam 路由；等待修复之后重新运行 `make test-game-mode-models`
及 ArkTS/HAP/签名核验通过。最终包相对 b3b30da2 仅 ABC 和 application-build 变化。
原 `HostWindowTasks` GNU Steam 旧测试接口失败仍不计作通过，详见旧侧栏报告。

## 验收边界

Banana 是本机已安装的 Steam 库内正版游戏，本轮确实通过 Steam 派发，非直接运行 EXE。
不据此承诺所有游戏、反作弊或 DRM 兼容，也不把本地游玩作为网络功能通过。
Steam 大屏、ANGLE 硬件 CEF、手机、多外设、长时运行、异常重启恢复和完整图形矩阵未验收。
历史 PAL4、剪贴板及颜色结果按各自包身份保留，不转记为本包全项通过。
`renderQualified` 仍为 false，直到完整矩阵完成。

日志与截图留在固定任务目录，含账号的截图不公开、不入 Git。
`steam-flow-device.json` 记录逐包设备结果，`steam-flow-delivery.json` 和
`steam-timeout-delivery.json` 记录最终成员身份及复用边界。
