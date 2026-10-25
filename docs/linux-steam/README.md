# Linux ARM64 Steam 原生接入进度

整体能力方向称为 **HmosDeck**，客户端来源、运行分层与验收顺序见 [HmosDeck](HMOSDECK.zh-CN.md)。

当前路线为原生 AArch64 GNU 执行，不使用 QEMU 或指令解释器。原版 Steam 与 GNU 资源已从 Z 盘原生加载，已修复 clone3、POSIX 共享内存和当前线程 robust-list 查询阻塞。原版脚本通过独立 GNU shell 与 taskset 启动官方 ARM64 Steam／CEF；平板已验证登录、游戏库、下载与仙剑主菜单，窗口通过 Wine HWND 接入现有桌面。GNU Mesa 共用 Wine 的图形接收端，GLX 显示／回读已验证；CEF GPU 默认关闭。手机已完成 GNU Proton、GPU、音频与中文的代表游戏验证；长时间稳定性及不同设备仍按各自证据验收。

| 阶段 | 状态 | 报告 |
| --- | --- | --- |
| OHOS Wayland 宿主协议、SHM、键盘 | 平板通过 | [阶段 1](HOST_PHASE_1.zh-CN.md) |
| 原生 GNU main、TLS、线程、动态加载、退出 | 平板通过 | [阶段 2](NATIVE_PHASE_2.zh-CN.md) |
| GNU Wayland 建窗、SHM、双向 FD、可见画面 | 平板三轮通过；实际按键待测 | [阶段 3](WAYLAND_PHASE_3.zh-CN.md) |
| GNU Xwayland / X11 | 平板五轮通过，实际显示；输入、GPU 待测 | [阶段 4](XWAYLAND_PHASE_4.zh-CN.md) |
| 原版 Steam / CEF | 平板已进入真实客户端与游戏；窗口／输入继续回归，手机适配待继续 | [阶段 5（历史）](CLIENT_PHASE_5.zh-CN.md)、[桌面接入](DESKTOP_INTEGRATION.zh-CN.md) |
| GNU ARM64 Proton / FEX | 手机 ARM64/x64/x86、共享 GPU、音频及代表游戏通过阶段验证，1.5.2 自动交付 | [独立实验](GNU_PROTON_EXPERIMENT.zh-CN.md) |

阶段报告按当时证据记录。当前 Linux 分支已增加经典／Deck 产品入口及组件管理页；GNU 支持库随包，Valve 客户端支持官网下载与配套本地包导入，已安装时可更新／修复，并可仅重置组件而保留游戏库。平板已通过系统文件选择器完成整包本地首装，详见[组件安装与维护验证](COMPONENT_DOWNLOAD.zh-CN.md)。任意版本跨 ABI 独立在线升级、全量在线首装尚未完成验收。

共用既有 Wine 图形服务的接入与实机边界见 [SHARED_MESA_RECEIVER.zh-CN.md](SHARED_MESA_RECEIVER.zh-CN.md)。

## 当前开发资料

- [GNU Proton 自动交付与升级恢复](GNU_PROTON_DELIVERY.zh-CN.md)。

- [GNU ARM64 Proton 独立实验](GNU_PROTON_EXPERIMENT.zh-CN.md)：手机 ARM64/x64 命令、GDI 窗口、隔离适配及 GPU/游戏验收边界。
- [源码整合范围与构建入口](SOURCE_INTEGRATION.zh-CN.md)：主工程、自有窗口宿主、GNU 组件、Wine 补丁与子模块的提交边界。
- [Release 稳定性修复与验收边界](RELEASE_STABILITY.zh-CN.md)。
- [Wine 首次初始化、取消重试与桌面黑屏修复](WINE_STARTUP_RELIABILITY.zh-CN.md)。
- [Linux Steam 第三方依赖与外置交付清单](THIRD_PARTY_RUNTIME.zh-CN.md)，以及可重建的 [ELF 依赖锁](runtime-dependencies.lock.json)。
- [DroidDeck 上游机制与 HmosDeck 主 UI 移植设计](DROIDDECK_UI_RESEARCH.zh-CN.md)：原生 ArkUI 主屏、Linux 游戏库、单一会话控制及现有 OHOS Wine 后端。
- [平板双指滚动回归](TABLET_INPUT_20261020.zh-CN.md)。

- [平板启动尺寸、大屏退出位置与最大化回归](TABLET_WINDOW_20261020.zh-CN.md)。

- [退出大屏后游戏库黑屏与 CEF 页面恢复](STEAM_DESKTOP_PAGE_RECOVERY.zh-CN.md)。

- [经典／Deck 双界面、一键安装和版本更新设计](DECK_PRODUCT_DESIGN.zh-CN.md)，附 [交互原型](deck-product-prototype.html)。

- [Deck 石墨主题、封面动画与平板往返验收](DECK_UI_TABLET_20261020.zh-CN.md)。
