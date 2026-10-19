# Linux ARM64 Steam 原生接入进度

整体能力方向称为 **HmosDeck**，客户端来源、运行分层与验收顺序见 [HmosDeck](HMOSDECK.zh-CN.md)。

当前路线为原生 AArch64 GNU 执行，不使用 QEMU 或指令解释器。原版 Steam 与 GNU 资源已从 Z 盘原生加载，已修复 clone3、POSIX 共享内存和当前线程 robust-list 查询阻塞。原版脚本已通过独立 GNU shell 与 taskset 执行到 Chromium 初始化。GNU Mesa 已共用 Wine 的图形接收端，实际 Maleoon 910 渲染与 18 帧显示/回读验证通过；Steam 完整可交互界面仍未完成。

| 阶段 | 状态 | 报告 |
| --- | --- | --- |
| OHOS Wayland 宿主协议、SHM、键盘 | 平板通过 | [阶段 1](HOST_PHASE_1.zh-CN.md) |
| 原生 GNU main、TLS、线程、动态加载、退出 | 平板通过 | [阶段 2](NATIVE_PHASE_2.zh-CN.md) |
| GNU Wayland 建窗、SHM、双向 FD、可见画面 | 平板三轮通过；实际按键待测 | [阶段 3](WAYLAND_PHASE_3.zh-CN.md) |
| GNU Xwayland / X11 | 平板五轮通过，实际显示；输入、GPU 待测 | [阶段 4](XWAYLAND_PHASE_4.zh-CN.md) |
| 原版 Steam / CEF | 进行中；Z 盘加载、原版子进程与共享 Mesa GLX 通过，后续启动兼容处理中 | [阶段 5](CLIENT_PHASE_5.zh-CN.md) |
| GNU Vulkan / Proton 与现有游戏后端互通 | 待实现 | 不从显示通过外推 |

阶段报告按当时证据记录；后续结果以编号较新的报告为准。全部入口默认关闭，
当前没有正式 Linux Steam 产品入口。

共用既有 Wine 图形服务的接入与实机边界见 [SHARED_MESA_RECEIVER.zh-CN.md](SHARED_MESA_RECEIVER.zh-CN.md)。
