# WineHua PR #78 通用修复移植

2026-10-31，在 `main_proton_steamarm64` 的 `00b8f589` 上按语义吸收上游 PR #78（head `7155e6d5`），不整包覆盖 UI 或运行库。

## 已吸收的修复

- EGL 配置、Context 和 make-current 失败检查；NativeWindow 所有权统一；析构和显式 Shutdown 均可安全回收。Vulkan compositor 在 NativeWindow 之前销毁。
- Surface ID 随创建、缩放和销毁传递，过滤迟到的旧回调。无效 Surface 不替换健康的旧实例；失效 renderer 可由恢复/尺寸回调重建。
- EGL/Vulkan 共用请求序号记录重画，只有成功上屏才确认；静态画面恢复和失败重试不会丢失请求。
- 桌面根窗口尚未提交背景时，已具备真实 CPU 像素的子窗口可以显示；不伪造 Guest 首帧。初始 configure 在锁外发送。
- VirGL/Venus 整体设备销毁复用 buffer/memory 解绑，先解除 shadow 关联再释放驱动对象。SCM_RIGHTS 发送成功后释放发送端 FD，接收端所有权不变。
- 原 OHOS Wine 的 DirectDraw 裁剪保留原矩形对应关系，避免越界和缩放偏移；IME 保留扫描码、扩展键和 key-up 信息。
- 隐藏窗口拒绝新的 key-down，仍允许 key-up 清除状态。原有虚拟键引用计数继续使用。

GL divisor 别名、字体和虚拟键中已有的同义修复不重复添加。未引入 PR 的产品页面、游戏/机型特判、全局图形策略、CPU 像素回读、Zink/cache 实验、全局 waitpid(-1) 或缩短首次初始化总超时。

## 构建与交付

Wine 新补丁为 `0070`、`0071`，VirGL 新补丁为 `0009`、`0010`；均登记到正式构建脚本及 graphics-stack lock。VirGL 独立构建的输入键补入 server 源码和新补丁，并编译 `server/render_context.c`。已有补丁的漏记指纹同时补齐，没有升级 DXVK/Mesa 子模块。

设备候选只在现有 Wine 数据包中逐成员替换本次编译的两种架构 ddraw/imm32，不以旧有效源码生成的整套 Wine 覆盖后续修复。可重建输入是已提交的补丁、登记顺序和源码指纹；本地 build/cache 不作为 CI 隐含依赖。

GNU Proton 的 Wine 未套用这两个 OHOS Wine 补丁。共享宿主 renderer/VirGL 修复与两套 Guest Wine 的补丁交付分别核对。

## 已完成验证及限制

- renderer lifecycle：708 项检查，含 ASan；桌面无背景首帧、锁外 configure 行为测试通过。
- VirGL 销毁：3 项真实函数测试及 ASan/UBSan 负例；FD：2 项测试，含 2048 次延迟 SCM_RIGHTS 接收。
- IME：30 项真实消息检查。
- 图形合同：RGB 40 项、producer-context 204 项、MF 27 项、scene/input 20 项；GStreamer 原生插件、字体兼容回归通过。
- Release HAP 编译及包内图形合同通过；Steam payload 审计确认 `windowsSteamRequired=false`。

上述主机检查不等于全部游戏、声音、控制器及设备矩阵验收。当前真机游戏记录另见 [原生 Linux 启动补充](../linux-steam/NATIVE_LINUX_FEX.zh-CN.md)。没有设备素材的 OP、全部 DXVK 性能和 Edge/手机完整回归不宣称通过。
