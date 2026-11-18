# Gamescope 插帧：鸿蒙 AI 优先验证

实施分支：`feature/gamescope-framegen`，基线 `main@d8128446`。本轮不升级版本、合 main、推送或上架。

## 实现顺序与能力边界

根据用户追加要求，先验证鸿蒙 Graphics Accelerate Kit 的 **AI Vulkan 内插**。满足设备能力、应用身份和 Gamescope 系统送显接入条件后，将其作为首选；Win-FG Native 保留为设备不支持时的通用后备。功能免费、离线，不接入会员。三端使用能力检查，不能根据手机或 PC 身份直接判定不支持。

截至本次源码变更，仅完成应用内异步能力探测与诊断展示；**没有激活超帧、改变游戏节拍或提供已生效的插帧开关**。`candidate-available` 只证明系统查询及必要 Vulkan 入口可用，不代表生成画面、呈现增益或三端验收通过。

官方依据（2026-11-18 核对）：

- [AI 超帧 Vulkan 指南](https://developer.huawei.com/consumer/cn/doc/HarmonyOS-Guides/graphics-accelerate-fg-ai-vulkan/)
- 本地 SDK 26 的 `graphics_game_sdk/frame_generation_base.h`、`frame_generation_vk.h`。
- AI 查询为 `HMS_FG_IsFrameGenerationSupported(INTERPOLATION_AI_VULKAN)`，SDK 26 新增。设备须满足 NPU 等要求；Vulkan 可用不代表 AI 超帧可用。
- 增强内插模式不传深度触发 AI 算法。普通 GPU 超帧需要深度/相机输入，不能直接用 Gamescope 最终图像替代。
- AI 路线依赖 `FG_PRESENT_BY_SYSTEM`，不沿用通用后备的自行交替 present 调度，避免重复补帧。

## 接入前必须解决的事项

1. 用 VPP 应用内命名空间查询，区分库不可见、旧系统无查询、设备明确不支持和候选可用。shell 执行权限错误不能判定硬件不支持。
2. 在 Gamescope 自己的 VkDevice 上创建上下文；跨设备只共享 NativeBuffer 与 fence，不直接传 VkImage。
3. 官方 Dispatch 仍列相机矩阵；当前合成器没有游戏相机数据。必须确认 AI 路线对此的实际约束，不伪造矩阵后宣称兼容。
4. 系统送显需要 `GraphicsAccelerateKit_FusionAware=Vulkan` 元数据。待验证阶段接入，当前能力查询不修改现有游戏输出路径。
5. 验证系统生成画面与最终 NativeWindow、客户区裁切、FSR、弹窗、光标和 ArkUI 浮层的关系。保持现有 sRGB 输出，不能重复 gamma。
6. 明确历史图像使用期限、resize/旋转/后台重置和超时回退；不能长时间占住游戏 producer ring。
7. `HMS_FG_SetTargetFps_VK` 的 SDK 注释说明首次上架后生效。调试包结果必须区分能力查询、激活结果及真实系统送显，不能用 Dispatch 成功或重复显示代替生成帧计数。

全局/单游戏 `off | x2`、安全实时切换、实际刷新率旁路，以及真实/生成/呈现 FPS 和开销统计仍属于后续实施范围。不能将本次诊断页面当成完整功能交付。

## 本次探测与验证

- `probeHarmonyFrameGeneration()` 在异步工作线程内动态加载系统库，不增加 API 23 的硬链接依赖，不启动 NCP、Wine 或 Gamescope。
- 查询仅由已有调试签名诊断页触发。原有普通模式、图形后端和版本号保持原行为。
- 主机行为测试覆盖：库缺失、旧系统缺少查询、设备否定、查询为真但 Vulkan 入口缺失，以及完整候选。所有测试入口的上下文操作均设为 abort，确保探测不会启动算法。
- Mini MLR-AL10，`7.0.0.111(SP16C00E111R1P2)` / API 26：调试签名 VPP 应用内查询为 `device-unsupported`，即 `HMS_FG_IsFrameGenerationSupported(INTERPOLATION_AI_VULKAN)` 返回 false。系统库与查询符号已解析，未进入上下文创建或渲染阶段。结果来自实际诊断页面，HDC 无权读取应用内结果文件；不能把当前结果外推为所有 Mini 系统永久不支持。
- 覆盖升级安装成功、应用可启动、查询后正常返回 Game 游戏库；原有 17 项游戏条目仍存在，未清数据或变更图形设置。M80 当前未确认在线，PC 未测。
- 当前测试包：`workspace_temp/gamescope-port/VPP-2.0.5-gamescope-debug-signed.hap`，633,928,457 字节；SHA-256 `5cf02b422709474489bc7468873554f52dfaa647386e91ce0530f5976f582291`。源码为 `59d87b0c`，通过原生、ArkTS 编译及签名检查。它仅是能力检查包，不能作为插帧功能或性能验收包。
- 保留先验证鸿蒙方案的顺序；Mini 当前环境需要通用后备。不在能力明确否定时继续强行激活 AI 上下文，也不将普通 GPU 超帧误作免深度替代。

后续验收仍按固定后端/渲染与输出尺寸执行关闭/开启各 3×60 秒。目标有效场景呈现提升至少 70%，真实 FPS 降幅不超过 10%；关闭态下降超过 5% 必须定位。
