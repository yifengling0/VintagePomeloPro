# 后续向 WineHua master 回推的功能包

本文件是准备与拆分方案，不代表已经推送。回推时从核验过的 WineHua master 新建干净分支；2026-09-01 刷新值是 `61cb4c6400a1c9a759fe2c511e6ef782d8586edb`，执行时如果 master 再前进，先更新差异分析。禁止把私有产品历史、页面、品牌、签名、设备或机器信息推给上游。

## P1：全屏几何和输入正确性

- 主要来源：`7d5e7874c37bff5cdd781d90dbc750cb6a7a4de4`；最终对齐参考本轮 T2–T6 的产品适配，不直接 cherry-pick 旧路径整提交。
- 目标：临时子窗口尺寸不能改变父窗口全屏映射；渲染、点击、relative pointer 使用一致几何；partial GPU layer 不覆盖 CPU 周边；direct/composed 切换和 damage 回放不残留旧像素。
- 最小阅读范围：`compositor/frame` 的 planner/blitter/composers、`compositor/input/input_resolver.*`、`compositor/input/input_state_tracker.*`、geometry、graphics renderer 对 PresentedFrame 的消费，以及对应 state/geometry/input 测试。
- 拆分原则：几何选择、输入空间失效、damage/cache 三个可独立解释的提交；若接口存在依赖，先提交接口与测试再提交消费者。
- 验收：host 几何/输入/合成器测试；窗口与全屏的鼠标落点；War3 启动临时尺寸；菜单、子窗口和五轮 resize/background。主机测试不能替代设备落点验证。
- 建议 Terra 实施语义适配，Luna 核对文件范围及收集固定检查输出。

## P2：GL 故障退避与资源生命周期

- 主要来源：`8de093d35d58dda25e55b9290c938204a6c39778`；涉及 `graphics/present_pacing.h`、`virgl_surface_presenter.*`，并核对现有 target generation/fence/device-release 合约。
- 目标：连续呈现失败/失焦时有上限地重试，停止无效忙转；恢复前台仍继续出图。保留成功路径 pacing 和失败 deadline。
- 不把现有 GLES Direct 实验作为前置条件。若共享 policy/helper 来自 `9ca71b84`，提取所需接口，保持默认后端不变。
- 验收：失败序列单元测试、后台 CPU/重试速率、十分钟出图、反复 NativeWindow 重建与恢复；错误码存在与否不能单独代表回归，需检查持续失败及恢复。
- 该包与 P1 尽量独立；涉及共同 presenter 的部分顺序合入，避免两个任务同时改其生命周期。

## P3：DXVK batching 和性能证据

- 产品默认策略来源包含 `12de3b9bdbd441531b6799caaa39f149c5ddbdcc`，但这个根仓提交主要是默认值/工具，不能代替 DXVK 子模块实现。
- 产品/上游冻结 pin：Legacy 为 `f3436e1796e0e4bab9406460380460a81e1aac69` / `5058927a741132fd643d20800f23e62dda432760`；Modern 为 `ff2d6a2c3d26a3c3098f8a490e4e4adc5aa4704b` / `977a3d782e657a14ee28b9553b6e5cf29c4bd88c`。
- 先在各自子模块比较 patch-id 和具体函数，确认哪些优化上游已有、哪些仍私有；不能把两个 pin 之间的全部历史一并回推。
- 按 Legacy / Modern 分成两个子模块 PR，分别证明 flush 边界、顺序、readback 与映射写入的正确性。子模块提交被上游接收后，根仓才改 gitlink 和单一 Native 默认参数。
- 产品 batching 始终默认开启；验证使用现有产品条件，不恢复已废弃 batch-off 全局实验。记录画面、帧推进、三组交替基线/候选样本和温度；超过 5% 的 FPS/P95 差异先调查。
- FPS、真正帧间隔 P95、renderer 工作耗时 P95 是不同指标，报告不得互换。没有 frame timeline 就明确说缺少帧间隔 P95。

## P4：Controller Hub 与 Wine 输入/震动

- 根仓来源：`87b23e566b52776f924468df8ba38b738c42f947`、`2ad6ac240a294775cd498264ee6f94d892e839e8`、`d2629a9ccfdb8c425e519dd305eddecab1cdb7a5`。最后一个含版本升级，必须排除 AppScope。前两者含私有设置页/资源，不整体 cherry-pick。
- 先回推 Wine guest：controller 引入对应 `69c92023f7431c09c6aaf0a5c39ea1d019c56752..efa3e5e04be8d28e4c16f50c017358384dd41294`，震动后为 `3fc36c426830211751248ae3f5e7485a2295c323`。上游冻结 Wine pin 为 `6569cd8dbd940d9b6b9d89db8f0751bbda57d770`。必须重新核对祖先/等价补丁，仅提取 WHGP、winebus/XUSB 和反馈链所需功能。
- 再回推 host：`input/controller`、`input/game_controller_bridge.*`、NAPI 声明和注册、Wine launch/env 所需连接；最后是 `GamepadManager`、`InputDispatcher`、`InputRouter` 的逻辑适配。
- UI 接入由上游自己的设置与 overlay 承载，不复制产品页面。必要权限描述按上游资源规范补齐；不能为了不改 UI 而省略震动权限或用户入口。
- WHGP 两端结构/版本、消息长度和生命周期一起审查；本轮 inbound pins 不变，未来 outbound 的协议变化必须单独版本化。
- Y 轴只在约定层翻转一次；Hub hat/analog 与 WASD/鼠标映射可共存；释放覆盖断连、失焦、后台、会话退出。震动只路由外接马达，不把手机振动当作手柄成功。
- 验收：host 合并/释放测试，Wine joy.cpl 和 XInput 可见性；实体左右摇杆/扳机/十字键/按钮；虚拟+实体并存；断连后全零；重新连接；游戏触发左右马达。记录手柄型号/连接方式，不记录设备唯一标识。
- 建议三个有明确依赖的 PR：Wine guest → Native Hub → ArkTS 接入；Luna 可核对协议字段和导出列表，Terra 处理状态合并及释放语义。

## P5：暂不作为稳定优化回推

`9ca71b846d0e9302a161f8db8e9513e4258bbaff` 的 GLES Direct target、`85f6f46c` busy-query IO 原型，以及 HUD/CPU/JIT 诊断应单独列为实验或工具。GLES Direct 未取得设备资格，不能在 P1/P2 顺手开启；busy-query 默认关闭。产品 HUD 页面不进上游修复 PR，仅在需要时提供可复用的 Native 观测接口。

## 每个回推包的提交前清单

1. 写清上游目标 SHA、所依赖的 guest 提交、精确允许文件、来源 SHA 和排除项。
2. 用路径映射定位当前实现，再做语义移植。每个包只有一个职责，不包含版本发布、品牌、签名或整套产品架构。
3. 构建/测试日志保存在忽略目录；提交可复现命令、结果摘要和真实限制。已知基线缺陷与新增回归分开记录。
4. 扫描 staged diff 的 AppScope、resources、signing、私有 URL、机器路径及设备数据；gitlink 只引用已经可公开取得的目标提交。
5. 先生成可审阅本地提交/补丁和 PR 描述，再按用户当时的发布授权推送。不要直接推 master，不强推，不发布这个产品分支的历史。
