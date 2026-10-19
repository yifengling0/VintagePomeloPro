# Linux ARM64 Steam 的当前启动方式

当前为 v37 外置 GNU 研究入口，默认全屏，最长运行 4 小时。普通 Steam 卡片仍使用已有 Windows 客户端流程，不能用来启动本轮 Linux ARM64 Steam。HAP 只包含宿主适配；运行时、原版 Steam ELF 与客户端资源位于 Z:\SteamLinuxARM64。实际原生入口为外置 release 内的 steamrtarm64/steam，由配对 GNU loader 执行。

## 已验证的桌面启动

已安装候选且外置资源就绪时，通过 HDC 显式选择设备，向现有 EntryAbility 发送：

```text
hdc -t <设备> shell aa start -b com.vintage.pomelopro -a EntryAbility --ps winehua.mode smoke --ps winehua.suite linux-steam-client --ps winehua.run_id <本轮唯一标识>
```

该命令启动宿主专用 GNU 诊断，不通过 Wine 执行 Steam ELF。热启动可能把宿主游戏库带到前台，此时点击普通 Wine 桌面卡片恢复已有桌面，不重复启动 Steam。当前已有用户登录及商店／游戏库显示、点击切换证据。

## 可复用启动脚本

scripts/launch_linux_steam_client.py 默认选择全屏（fullscreen），保留现有呈现、共同桌面候选和独立子窗口诊断配置；默认关闭 CEF GPU 加速，沿用已经验证的 virpipe 接收端，禁用自动桌面观察、截图、XTEST 和信号追踪。它不会安装包、结束进程、清理数据或发送输入。

```text
python scripts/launch_linux_steam_client.py --presentation fullscreen --dry-run
python scripts/launch_linux_steam_client.py --hdc <hdc路径> --target <设备> --artifact-dir <记录目录> --presentation fullscreen
```

本机提供 artifacts/steam-linux-arm64-client-20261019/Start-SteamARM64.ps1 包装器，使用已记录的 USB 平板，不自动选择手机。Windows PowerShell 可执行：

```powershell
& 'F:\VintagePomelo-Workspace\artifacts\steam-linux-arm64-client-20261019\Start-SteamARM64.ps1'
& 'F:\VintagePomelo-Workspace\artifacts\steam-linux-arm64-client-20261019\Start-SteamARM64.ps1' -Presentation Fullscreen -DryRun
& 'F:\VintagePomelo-Workspace\artifacts\steam-linux-arm64-client-20261019\Start-SteamARM64.ps1' -Presentation Fullscreen
```

Existing 保留当前宿主呈现策略；Fullscreen 在冷启动时选择独立 fusion/subwindow 承载，并通过 Xwayland 的 `-fullscreen` 请求 Wayland 全屏，复用现有宿主的 fullscreen 处理；不把普通 X11 桌面窗口嵌入 Wine 桌面。各配置统一使用 1280×800 X11 桌面，容纳设置／安装对话框的最小尺寸；全屏不运行 X11 窗口位置约束，不干预 Steam 自己显示／停放的窗口。`desktopSession=bounds` 仅保留为显式窗口诊断配置，不默认启用。Windowed 在冷启动时选择现有 fusion/subwindow 承载，目标是将整个 X11 桌面显示在一个可交互的宿主子窗口中，仍为 rootful Xwayland，不要求先做 rootless/XWM。

external-65 在 USB 平板验证了独立子窗口：初始物理范围 [464,48][2512,1328]，1024×640 内容放大显示，页面点击有效；边缘拖拽后范围 [678,48][2528,1166]。通过 Steam 菜单退出，客户端与服务器均退出 0，无信号或超时，NCP 回收通过。外层标题栏移动和宿主关闭按钮尚未验收。

Desktop 候选使用 virtual 承载和 1024×640 X11 桌面，目标是在已有 Wine 桌面内显示，不创建 fusion 子窗口。external-66 创建出 Wine Notepad 窗口，但 Steam 页面内容变黑、点击未见更新，两者正常共存尚未通过，根因未确定。已主动停止该候选，external-67 恢复 1280×800 虚拟桌面；该轮商店页显示 Error Code -118，不据此判定账号或兼容接口故障。按用户要求默认改为 Fullscreen；不将 Desktop 配置或同一接收端当成 Wine 窗口融合已完成。

完整库目录、认证与游戏启动设计见 [游戏启动设计](GAME_LAUNCH_DESIGN.zh-CN.md)，DroidDeck 的可参考机制见 [源码对照](DROIDDECK_REVIEW.zh-CN.md)。

每轮先备份已有 client-launch.conf，配置或系统启动确认失败时尝试恢复原配置。成功发出 Want 仅记录 launch_requested_not_startup_verified，不等于 Steam 实际出画面。重复 GNU 会话、活动宿主下切换呈现模式均在任何设备写入之前拒绝，配置覆盖不持久化桌面模式设置。

## GPU、大屏和清晰度

当前 CEF 实际带 --disable-gpu、--disable-gpu-compositing。独立 OpenGL 预检识别 Maleoon 910，证明共享接收端可绘制，不代表当前网页硬件加速。GPU 加速可能改善滚动、动画、视频及高分辨率下的 CPU 开销；是否获益还取决于 CEF 选择的实际后端与输出拷贝，需单独比较。

GPU 开关不会自动增加界面清晰度或显示面积。大屏清晰度应独立调整 X11 桌面像素尺寸、界面 DPI／缩放和宿主窗口客户区，避免仅把低分辨率桌面放大；提高分辨率也会增加现有共享内存输出的开销。窗口化无需以启用 GPU 为前提。

本轮脚本验证涵盖活动宿主／GNU 会话阻止写入、冷启动参数、现有呈现策略保留、失败启动配置恢复及备份失败阻止修改。主机测试与真实平板只读预检均通过；独立子窗口已取得上述实机证据；共同桌面正常共存、外层窗口控制和 Steam 游戏启动仍待实现／验收。

## v37 全屏实机验证（2026-10-19）

USB 平板 external-78 已验证原版 ARM64 Linux Steam 在独立承载全屏显示，FusionWM 记录 2560×1600 fullscreen、系统栏隐藏；商店画面约59 FPS，库导航点击和《新仙剑奇侠传》详情页切换有效，绿色“开始游戏”按钮可见。该轮已通过菜单正常退出，保留登录资料。使用原有登录数据，没有启动 Windows Steam。

下载退出修复已在 external-76 通过下载阶段，独立库 AppID 695630 的安装状态变为4；全屏实测再次显示已安装的游戏详情。Steam 页面提示云状态未同步，未替用户选择覆盖云／本地存档。external-79 已修复 SDK 启动包装层，取得真实 Wine 启动请求及游戏窗口；用户确认能启动。完整 Steam API 认证仍未验收，最小化后缺少恢复入口，见统一窗口方案。

该全屏仅控制原生 Steam 的 Xwayland/Wayland 承载，保留当前 OHOS Wine/FEX/DXVK 后端，不宣称 Wine 桌面逐窗口融合已完成。

## external-79 游戏启动与任务管理（2026-10-19）

缺失的 SDK `steam-launch-wrapper` 已通过 GNU 精确 exec 别名接到 HmosDeck，核验目标后执行官方 reaper。真实进程主机测试覆盖 PID、参数、文件描述符、信号与过滤环境后的内部启动凭据继承。平板原版 Linux Steam 已发出 NewPAL 启动请求，宿主记录 Wine 进程创建并显示游戏窗口，用户确认能启动；完整 SteamAPI/DRM 不据此宣称通过。

用户反馈最小化后找不到窗口。后续在宿主统一任务/窗口管理，先保留全屏并补恢复入口，再处理共同桌面。具体接入点、状态规则和验收见 [统一窗口与任务方案](DESKTOP_INTEGRATION.zh-CN.md)。云存档目录映射仍未修复。
