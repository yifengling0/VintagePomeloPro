# Gamescope 免费 2× 插帧

实施分支：`feature/gamescope-framegen`，基线 `main@d8128446`。本轮不升级版本、合 main、推送或上架。

## 实现顺序与能力边界

插帧设置规划为「关闭」「Win-FG 2×」「鸿蒙超帧」。Win-FG 和鸿蒙官方超帧各自独立选择，旧设置 `x2` 对应 Win-FG。鸿蒙超帧按系统能力和游戏提供的帧数据开放。功能免费、离线。

当前可选择 Win-FG 2×。**Mini 的 PAL4/VirGL 已完成 30 FPS 档和调度修复后 60 FPS 上限档的两轮对照，每轮关开各 3×60 秒，完整画质和三端矩阵仍未验收**。具体包身份、调度修复及数据见 [评价记录](frame-generation-validation.zh-CN.md)。鸿蒙官方超帧目前处于能力探测阶段。`candidate-available` 表示设备报告支持且指定的 Vulkan 入口齐全，实际送显待验证。

官方接口（2026-11-19 核对）

- Graphics Accelerate Kit 提供 `HMS_FG_*`。SDK 26 的能力查询分别接受 `INTERPOLATION_GPU_VULKAN` 和 `INTERPOLATION_AI_VULKAN`。[能力查询](https://developer.huawei.com/consumer/cn/doc/harmonyos-references/frame__generation__base_8h/)
- GPU Vulkan 内插需要颜色、深度和相机矩阵，可由游戏提交预测帧，也可使用系统送显。[游戏送显](https://developer.huawei.com/consumer/cn/doc/HarmonyOS-Guides/graphics-accelerate-fg-interpolation-vulkan/) · [系统送显](https://developer.huawei.com/consumer/cn/doc/HarmonyOS-Guides/graphics-accelerate-fg-systempresent-vulkan/)
- AI Vulkan 内插提交颜色和相机矩阵，省略深度，使用 `FG_PRESENT_BY_SYSTEM`。[AI 超帧 Vulkan 指南](https://developer.huawei.com/consumer/cn/doc/HarmonyOS-Guides/graphics-accelerate-fg-ai-vulkan/)

## 鸿蒙超帧接入条件

1. 用 VPP 应用内命名空间独立查询 GPU 和 AI。显示库不可见、旧系统无查询、设备返回否定、入口缺失或候选可用。
2. 在实际处理游戏图像的 VkDevice 上创建上下文。普通显示路径可通过 Vulkan 交换链使用游戏送显，但同样依赖下面的逐帧输入。
3. VPP 当前显示桥只传颜色 NativeBuffer 和 fence。官方 GPU 路线需由游戏渲染端提供同帧深度、视图投影矩阵及其逆矩阵。AI 路线也需相机矩阵。
4. 系统送显需要 `GraphicsAccelerateKit_FusionAware=Vulkan` 元数据。待验证阶段接入，当前能力查询不修改现有游戏输出路径。
5. 验证系统生成画面与最终 NativeWindow、客户区裁切、FSR、弹窗、光标和 ArkUI 浮层的关系。保持现有 sRGB 输出，不能重复 gamma。
6. 明确历史图像使用期限、resize/旋转/后台重置和超时回退；不能长时间占住游戏 producer ring。
7. `HMS_FG_SetTargetFps_VK` 的 SDK 注释说明首次上架后生效。调试包结果必须区分能力查询、激活结果及真实系统送显，不能用 Dispatch 成功或重复显示代替生成帧计数。

先在设备报告支持时使用有真实深度和相机数据的原生 Vulkan 场景验证，再按游戏逐一验证渲染端能否提供上述数据。

## 通用实现与来源

固定 [Win-FG Native](https://github.com/The412Banner/win-fg) `fbc69ed6c5bf16b824d0a29bd8b171675101f9dd`，MIT 授权。`components/winfg-source.lock.json` 记录原始文件、适配补丁及校验值；`scripts/winfg_source.py` 从 GLSL 重编译并验证 SPIR-V。最终 HAP 校验 Win-FG 和 AMD FidelityFX 许可。未使用或分发 Lossless Scaling。

```text
游戏 NativeBuffer + fence
  → 客户区裁切、方向规范化（GPU）
  → 两张引擎持有的历史图像
  → model 3 / Balanced / 2× 光流与合成
  → 现有线性、最近邻、整数或 FSR 1.0 缩放
  → 弹窗和普通窗口合成
  → NativeWindow（光标与 ArkUI 浮层保持现有路径）
```

不引入 CPU 截屏、生产整帧读回或 CPU 全局运动求解。场景突变标志仅回传 4 字节，完成 GPU fence 后读取。真实与生成帧共用现有 SDR/sRGB 输出；保留用户选择的 VirGL、DXVK、Zink 与 Wine DPI。

## 调度与回退

- 用真实 GPU 源序号去重，显示 tick 或弹窗重绘不构成新游戏帧。统计分别发布真实源 FPS、完成的生成 FPS、有效呈现 FPS及可取得的 GPU 计算耗时；它们不是面板物理扫描计数。
- NativeVSync 提供额外显示节拍。预热后，仅源间隔至少约为显示周期 1.7 倍时尝试插一个中间帧；真实帧已接近屏幕上限时旁路。用户设置的 30/60 源帧上限仍生效，不自动降分辨率或限制真实帧。
- 至多一个待显示真实帧，生成帧后在后续节拍显示当前真实帧；新源覆盖过时待显示帧，不累积排队。插值需要新旧两帧，可能增加最多一个源帧间隔的显示延迟，不提高游戏逻辑与输入采样速度。
- 生成帧之后，先送出仍在期限内的真实端点，再接收下一份 Direct/GL/Venus 图像，避免新源抢先替换待显示真实帧。超过一个显示周期的旧期限、显示回调失效、后台或历史重置均放行新源；GL/Venus 的源限帧仍由 producer 管理，不重复限流。
- resize、裁切、方向、源窗口/后端变化、暂停恢复和会话重建清历史。检测到场景突变时输出当前真实画面，下一次完成检查重置历史，不计作有效生成帧。
- 缺少 Vulkan 格式/计算能力、刷新节拍、适用 GPU 游戏图层，或前次计算仍忙时使用真实帧。计算预算为 `max(显示周期, 源间隔/2) × 80%`，连续三次超预算后旁路；30→60 在 120 Hz 屏上使用约 16.7 ms 的输出间隔预算，不能误按 8.3 ms 判定。
- VSync tick 失去更新后，最多宽限 100 ms 或四个显示周期（取较大值），随后取消旧节拍等待并恢复真实帧路径；未来时钟值同样不用于插帧，回调恢复后可重新预热。不能让一次旧回调永久阻塞游戏呈现。
- 首轮 GPU 历史与计算预算限制为客户区面积不超过 `1920×1080`；更大分辨率显示预算不足并正常呈现真实帧。历史图像分配失败回退原合成路径。该限制不是设备能力上限。
- 资源与 descriptor/UBO 更新受 fence 保护；仅生命周期边界等待队列，不在正常插帧循环调用 DeviceWaitIdle。已提交到同一 GPU 队列的任务不能被 CPU 超时取消，预算旁路在完成结果可用后生效。
- Gamescope 的 `srgbView()` 实际为可写 STORAGE 的 UNORM 编码值视图，`linearView()` 为采样时解码的 SRGB 视图且移除了 STORAGE usage。Win-FG 输入及输出均使用前者；真实帧和生成帧再走同一色彩输出链。GPU 行为测试通过同一生产模板校验该契约。

## 免费设置与三端边界

全局 Game 模式设置、单游戏设置与运行侧栏新增 `frameGeneration: 'off' | 'x2'`。默认关闭，旧配置缺字段按关闭处理。运行侧栏可实时开关，其他缩放/限帧调整保留插帧值。开启后侧栏显示预热、运行及旁路原因，性能浮层显示三种 FPS 和 GPU ms。没有会员、账号或网络判断。

手机继续走同进程 embedded Gamescope，不新增 NativeChildProcess 依赖；平板和 PC 使用同一计算与呈现实现。这里只说明代码路径；M80 实际插帧、旋转/后台恢复和 PC 窗口场景尚未验收。

## 本次探测与验证

- `probeHarmonyFrameGeneration()` 在异步工作线程内动态加载系统库，不增加 API 23 的硬链接依赖，不启动 NCP、Wine 或 Gamescope。
- 调试签名诊断页分别显示 GPU 和 AI Vulkan 内插结果。
- 主机行为测试覆盖库缺失、旧系统缺少查询、设备否定、GPU 与 AI 各自支持、Vulkan 入口缺失，以及只有 AI 需要系统送显入口的情况。上下文操作的替身入口调用即中止，用于检查探测的副作用。
- Mini MLR-AL10，`7.0.0.111(SP16C00E111R1P2)` / API 26 的历史调试包查询中，`INTERPOLATION_AI_VULKAN` 返回 false。该包只查了 AI，GPU Vulkan 结果待真机复测。
- 覆盖升级安装成功、应用可启动、查询后正常返回 Game 游戏库；原有 17 项游戏条目仍存在，未清数据或变更图形设置。M80 当前未确认在线，PC 未测。
- 原能力检查包：633,928,457 字节；SHA-256 `5cf02b422709474489bc7468873554f52dfaa647386e91ce0530f5976f582291`，源码 `59d87b0c`。它仅是能力检查包，不能作为插帧功能或性能验收证据；当前交付包身份另见任务 `package.json`。

后续验收仍按固定后端/渲染与输出尺寸执行关闭/开启各 3×60 秒。目标有效场景呈现提升至少 70%，真实 FPS 降幅不超过 10%；关闭态下降超过 5% 必须定位。

## 通用后备主机验证与设备待验收项

主机已执行真实 Vulkan 计算图（lavapipe），覆盖常量色阶、运动物块中间位置、130 像素奇数块边界、场景突变、五轮引擎生命周期、descriptor 分配失败和独立实例。该测试发现并修复 model 3 flow 方向与 synth 约定不一致，以及最粗层 predictor 同 dispatch 读写竞态；不是仅测试配置或数学公式。

调度测试覆盖 30→60、60→120、重复源/显示 tick、高源帧率旁路、历史重置、缺 cadence 和显式源限帧。Game、Steam、剪贴板、Direct/显示节拍及配方行为回归通过。OHOS 原生/ArkTS 与最终包验证见任务交付记录，编译通过不能替代设备验收。

| 目标 | 设备检查 | 状态 |
| --- | --- | --- |
| Mini | PAL4/VirGL，800×600→2400×1600；静止存档关开各 3×60 秒及有界转镜头检查 | 已取得性能结果；逐生成帧画质仍未验收 |
| Mini 其他组合 | Heaven、DX11，DXVK/Zink，真正 60→120 与旧稳定包关闭态回归 | 未验收 |
| M80 | 同进程启动、触摸、旋转、后台恢复、退出重进 | 未验收 |
| PC | resize、最大化、隐藏恢复、焦点、多窗口 | 未验收 |
| 生命周期 | 至少五轮启停、资源持续增长检查 | 主机引擎五轮通过，设备未验收 |
| 性能 | 首轮 30 FPS 档，关/开预热后各 3×60 秒 | 26.18→57.20 FPS 有效呈现；不是游戏逻辑或物理扫描率 |
| 性能 | `99030044`，60 FPS 上限档，关/开预热后各 3×60 秒 | 36.93→75.54 FPS 有效呈现，+104.51%；源接收率约 38，未达到真正 60→120 |

游戏 HUD 重影、人物边缘和 PAL4 既有 Zink 白地面单独评价。主机 GPU 耗时不能作为鸿蒙设备性能结果。
