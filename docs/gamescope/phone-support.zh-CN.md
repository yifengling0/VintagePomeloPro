# 手机 Game 模式与呈现适配边界

用户要求手机具备类似的 Game 功能；手机不依赖系统 NativeChildProcess（NCP）。
这是本轮范围约束，不把手机归入可忽略的设备。本文区分实现路线与已完成能力。

## 共用产品层

手机、Mini 和 PC 共用 Game 游戏库、Steam 入口、游戏覆盖、性能设置和快捷面板。
进入 Game 页面不以 `GamescopeRuntimeStatus.loaded` 或系统 NCP 是否存在为条件；
这些状态仅用于选择已验收的呈现实现。保持已有 Wine/FEX、后端选择、配置和存档。

设置按会话启动时解析，游戏卡片覆盖优先于当前模式。现有会话冲突时要求正常退出，
不热迁移窗口。单个能力（FSR、限帧等）只有实际接入并验证后才对用户标为可用。

## 平台实现

| 平台 | 进程和最终呈现 | 当前状态 |
| --- | --- | --- |
| 支持 NCP 的平板/PC | 独立 OHOS Gamescope core，通过 OHIPC 交接 NativeWindow | Mini 已启动 Xwayland/系统 Vulkan；像素与生命周期继续验收，GPU 游戏桥未接通 |
| 手机 | 复用现有 Venus/VirGL/Zink 与 App 内原生合成器，承载类似的 Game 呈现功能 | 适配路线，尚未实现 FSR、Game 限帧或完整 UI；不宣称已可用 |

手机已有早期 clean fork server 可启动纯运行时子进程；没有服务或服务已死亡时不得
回退到系统 NCP，也不在 ArkUI/GPU 已运行后临时 fork。该进程能力不等同于图形能力。
`StartOutputChild` 在手机上明确拒绝平板专用的 NativeWindow IPC bootstrap。
原生查询另有 `standaloneOutputSupported`，手机恒为 false；不会因为 clean fork
或动态库可加载就启用独立输出，诊断页也在启动前显示相应状态。

历史 Mate80 的 NativeBuffer 读入及反向 Vulkan allocation 都在 fork 子进程的
HDI/SAMGR 服务获取中等待，零帧。见
[手机 Direct 记录](../architecture/mate80-direct-reverse-export-20260929.md)。
因此不以 surface ID、原始指针或普通 socket 序列化替代系统窗口对象交接，也不将
历史失败协议直接开放为手机呈现路径。未来若系统提供可用入口，可另行验收真正的
手机 Gamescope 子进程，且无需更换共用产品层。

## 手机后续接入点与验收

1. 在统一会话设置中解析呈现选项，注入现有 `EglRenderer` / `DesktopCompositor`。
   继续由现有 NativeImage 消费和最终输出持有图像；不新增完整桌面的重复合成。
2. 线性、最近邻、整数缩放接到最终显示矩形；输入共用
   `ComputeFrameDisplayRect` / `GetInputLetterbox` 的几何定义，覆盖黑边、旋转和触摸。
3. FSR 1.0 在同进程 GPU pass 中实现 EASU/RCAS，沿用 HSL 锐化转换。必要的中间纹理
   在 GPU 内保留，不用整帧 CPU 读回/重新上传来伪装游戏共享图像。
4. 限帧同时核对生产者和呈现者。仅减少宿主上屏次数不代表游戏 FPS 已限制；
   不能在持有渲染锁或未释放消费图像时睡眠。反馈/背压必须有实际帧时间证据。
5. 保留已有输入、IME、授权剪贴板和多手柄通道，不新增第二路投递。

手机单独验证正常初始化、游戏画面、旋转、后台恢复、五轮退出重入；以相同后端、
实际游戏/输出尺寸比较普通模式和 Game 模式。Mini 的任何通过结果不能代替手机验收。
本轮没有手机真机结果，完整 Game UI 和上述呈现接入仍待完成。
