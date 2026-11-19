# Mini 插帧修复与性能浮层复测

`c4e5c529` 保留应用启动参数的顺序和重复值，并以当前设置替换本地游戏的旧参数快照。`878dafc5` 为预算不足增加退避重试，从可见 GPU 客户区选择插帧源，并在光流不可信处保留当前真实像素。`ef538c95` 将常驻 HUD 缩成呈现帧率和插帧标记，侧栏继续显示真实帧、生成帧、呈现帧和插帧 GPU 耗时。

主机验证覆盖启动参数、调度退避、GPU 图层选择、Gamescope 配方和实际 Vulkan shader 执行。普通 8 px 位移仍生成接近中间位置的画面。快速低对比 24 px 位移测试中，旧轮廓像素从 198 或 210 降到 0，结果回退到当前真实帧位置，局部运动的平滑度因此受限。

| Mini 测试 | 真实 FPS | 生成 FPS | 呈现 FPS | 观察 |
| --- | ---: | ---: | ---: | --- |
| PAL4 VirGL 线性，关闭，左右转镜头 | 35.6 | 0 | 35.6 | 青鸾峰存档 2 |
| PAL4 VirGL 线性，开启，左右转镜头 | 33.2 | 31.7 | 64.9 | 抽样截图未再见此前明显的屋檐、人物双轮廓 |
| PAL4 DXVK9 FSR，开启，左右转镜头 | 33.9 | 32.0 | 66.0 | 同一会话曾从预算旁路约 1.0 秒后自行恢复，期间未操作插帧设置 |
| Heaven DX11 / DXVK 1.10 FSR，开启，自动巡游 | 12.4 | 12.1 | 24.5 | 800×600、Low、AA 和 VSync 关闭，巡游阶段 GPU 游戏层可用 |

各行是约 10～14 秒的短测，设备输出 2400×1600。不同缩放方式和场景的结果不用于后端排名。PAL4 的截图未标注单张生成帧身份，运动画质需要更多场景覆盖。Heaven 直接执行 `Heaven.exe`，实际 argv 中 `-video_app direct3d11` 只出现一次，`-video_multisample 0` 和 `-video_vsync 0` 都保留。启动阶段仍会等待 GPU 游戏层，进入巡游后 `fgLayer=0 layers=1 gpuLayers=1` 且计数持续增加。约 12 FPS 的真实渲染率仍限制游戏响应。

PAL4 Zink 读档后可正常插帧，静止画面曾显示约 29 真实帧、28 生成帧、56 呈现 FPS。转镜头采样混入读档间断，剔除这组均值。此次未捕获 Zink 的预算旁路恢复，既有天空黑块属于尚待排查的渲染项。手机、PC、完整性能与运动画质矩阵仍待验收。

Gamescope 在 Linux 上通过 MangoApp 提供浮层。Gamescope 的统计通道使用 SysV 消息队列，HSL 曾在鸿蒙沙箱中复现 `msgget` 触发 `SIGSYS`。VPP 的简洁浮层与侧栏共用现有原生统计，完整移植 MangoApp 需替换统计传输并适配其图形显示层。Mini 上已确认简洁浮层显示 `63 FPS · 插帧`，侧栏实时刷新三项帧率和 GPU 耗时。

2.0.6 ARM64 调试签名包由 `ef538c95` 构建，634081927 字节，SHA-256 `640a068820d6977eebc699278e7a9d812fa019f5c27c28da13e23f5cd07b2d2e`，已保留数据覆盖安装。相比 `878dafc5` 调试包，仅 ArkTS 字节码和源码身份发生变化。上一份 `878dafc5` 签名包作为回退。两游戏设置已恢复并复核，Mini 正常退出游戏、关闭临时诊断并返回 Game 游戏库。

包、截图、原始计数与退出记录位于 `F:/VintagePomelo-Workspace/workspace_temp/gamescope-port/`。主要证据是 `framegen-package-audit.json`、`device/repair-restoration.json`、`device/repair-hud-compact.jpeg`、`device/repair-hud-sidebar.jpeg`、`device/repair-pal4-dxvk9-loaded-counters.json`、`device/repair-heaven-launch.log` 与 `device/smoke-repair-*.json`。
