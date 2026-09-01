# main → master 功能同步交接

## 从这里继续

本轮以产品 main `2c0436360ad3f821afe3fd6ea29d76e89f3781e7` 建立 `codex/sync-master-20260831`。原冻结上游范围 `d256317e..74f2bfe1` 的 69 项已完成；创建 PR 前又刷新并处理 `74f2bfe1..61cb4c64` 六项，因此 ledger 当前覆盖 `d256317e02c83ed81172938c31152ded16393a32..61cb4c6400a1c9a759fe2c511e6ef782d8586edb` 共 75 个提交。它们是**选择性功能移植**，不是普通 Git merge；不要补一个 `ours` merge 来伪造已合并历史。

T1–T7 的代码移植和目录重构已经实现。继续工作先读 [STATUS.md](STATUS.md)，再读本文件对应任务，不要重新执行已经完成的补丁或搬目录脚本。全部来源、处置理由和检查点在 [commits.json](commits.json)，181 个 Native 路径映射在 [path-map.json](path-map.json)。总约束和验收范围见 [PLAN.md](PLAN.md)。

当前已安装包来自 `522f9f1d` smoke/common 架构收敛及其自动化修复链，SHA-256 为 `d58dc7d678231906935e0229c36379e19c1a1f914f462493e4b4c797da8c175a`；替换安装保留数据和 prefix。最终状态必须同时记录源码 SHA、HAP SHA-256、安装结果和设备范围，不能只记录版本号 1.3.3。最新状态/未通过项以 STATUS、NEXT_TASK 和 DEVICE_RESULTS 23 为准。本次仅有短 x64/x86 `core/reuse` PASS；DEVICE_RESULTS 22 的五轮缩放和 905 秒历史证据仍有效，但 20 次 producer NO_BUFFER、35 次 consumer empty-update 与一次 197 秒 metadata-only 启动仍分别保留在 I1/I4。

## 不得改变的产品行为

- 保留旧柚 Pro 的页面、卡片、浮窗、PC 沉浸、手机横竖屏、HUD、虚拟按键布局、品牌、包名、版本、权限和用户数据；不导入上游页面或资源来解决 Native 冲突。
- `WineEngineService` / `AppSessionService` 保持产品会话体系。NAPI 的 launchClient 参数、返回值和对外事件 JSON 保持兼容。typed event bus 仅收敛内部事件。
- 产品 Native graphics policy 是默认参数的唯一来源；两代 DXVK batching 默认开启。不能重新引入上游旧的 ArkTS perfProfile 决策。
- 全屏坐标使用父窗口内容几何；子 surface 的 viewport 偏移仍有效。显示空间、缓冲区像素空间、输入空间不能混用。
- 保留相对鼠标在焦点、几何、epoch 变化后的基线失效；旧 surface 的排队事件必须检查存活。
- 保留 GL 失败超时和退避、SHM 新鲜度、NativeWindow 所有权、generation/fence 回收与 Vulkan device release。GLES Direct 仍未取得产品资格，默认关闭；busy-query 实验仍默认关闭。
- 保留手柄物理 Y 轴约定、虚拟键盘与 Hub 并存、焦点/断连释放、仅外部马达震动。没有实体手柄时不能把硬件验收记为通过。
- 本轮所有 gitlink 和 `.gitmodules` 必须与产品起点一致。不能因 upstream master pin 不同而切换 Wine、DXVK 或 Mesa 子模块。

## 已完成任务包

### T1：产品服务与默认策略，c078c158

范围：`AppCatalogRules`、`AppSessionService`、`ManagedSmokePayloadService`、`WineEngineService` 的必要引用，以及 Native defaults 命名。

决策：只在运行中卡片过滤基础设施程序，不删除 Native 进程登记。最初移除未使用的 SmokeRunner；在 `522f9f1d` 按上游三层架构恢复为无 UI 权限的薄数据解释器，由 WineEngineService 管理独立 smoke 前缀和产品会话恢复。保留产品正在使用的 LAB API、页面和会话所有权。桌面浮窗状态在产品中仍然有效，不照抄上游 Pad 修复。文件复制使用现有同步递归实现，不导入过渡 URI workaround。

检查：`make test test-model`、ARM64 `check-native`。若后续改服务，另跑 HAP 编译和冷/热启动，不能只靠模型测试。

### T2：规划与像素合成，622387cf

范围：`compositor/frame/frame_planner.*`、`frame_blitter.*`、合成 helpers。读 ledger 中 T2 的 12 个来源提交即可。

决策：锁内构造计划/快照，像素工作在锁外；保留父窗口全屏、局部 GPU 子层、live viewport、direct→composed 缓存失效和 damage 内保留像素回放。

检查：compositor state / clipping / scaled blit 测试及 Native 编译。并行修改 planner 和 blitter 前先固定两者的数据接口。

### T3：帧与输入描述，eea65aba

来源：`df5fd270`、`37b83ad5`、`567697d3` 必须整体理解，不能单独搬第一个提交。

范围：PresentedFrame、display composers、InputTarget。直接 buffer 的输入仍是 root 坐标。产品额外修复 WindowFrameComposer 锁，并通过互斥快照发布 input fit，避免渲染线程与输入线程竞争。

检查：buffer/input 空间分离、window composer 独立加锁、窗口与全屏输入回归。

### T4：ZC 与呈现目标，ae1974c7

范围：ZcBridge、共同 layer 列表/z-order、PresentTarget、DirectPassPolicy、共用 shader/clock。

决策：共享 ZC 发布映射加锁，适应产品 PC 多 renderer；保留产品 GL/Vulkan 区分、Vulkan protocol-only surface、父窗口全屏 fit、故障退避、隔离与资源回收。渲染能力只决定能否直出，不能覆盖产品资格门禁。

检查：fresh-SHM fallback、target 能力拒绝、z-order、GLES default-off、窗口反复缩放/后台恢复。

### T5：输入职责拆分，fe372d2b

范围：`compositor/input/input_space_mapper.*`、同目录的 `input_state_tracker.*`、`input_queue.*`、`input_injector.*`、PointerExtras 注入的 baseline sink。

决策：采用上游 `cee5b1a8` scroll/popup 修复；排队 relative motion 保留 surface 并验证存活。InputTarget 的内容尺寸不能因上游没有使用就删除，它是产品 geometry guard 的依赖。IME focus/reset 保留。

检查：90 个输入状态检查；真机触摸/触摸板、滚轮、弹出菜单、焦点切换、输入法与软硬键盘应分别留证。

### T6：SHM、窗口提交、事件与依赖注入，7494e63f

范围：ShmFrameSource、CommittedSurface、ToplevelManager、PopupManager、ToplevelEventBus、DesktopSessionState 和 frame serial。

决策：产品 toplevel 仍使用未缩放的紧密 SHM 副本；root resize 输出尺寸和 session reset 维持产品实现。Created JSON 保留 sessionId/clientPid，Raise 事件保留。不能导入上游另一套 process registry 或 `desktop-ready` 副作用。通过明确引用注入 compositor/input resolver，避免用全局 server 恢复耦合。

检查：popup 像素/窗口尺寸分离、重复 commit、unmap 后帧与输入清理、拖动窗口回放、事件 JSON 合约。

### T7：Native 路径对齐，f05cb825

范围：181 个文件搬移；CMake、Makefile、生成协议头、host doubles 和 scripts 引用同时更新。只允许机械路径/包含关系变化，不夹带新的语义修复。

决策：`compositor/{frame,toplevel,input}`、`protocols`、`wine`、`proc`、`graphics`、`input`、`audio`、`bridge`、`common` 对齐；私有 controller 放 `input/controller`。blit 的 `-O2` 保留。

检查：搬移前后实现体比对已经通过 181/181；全套 host、模型、HUD/导航、GLES 和 11 个 CI release 检查通过。生成的 Wayland 头现在落在 protocols，不能再保留旧 include 目录中的重复副本。

### T8：发行包与实机验收，继续项

接手优先读 STATUS 的 `Current handoff`，再读 DEVICE_RESULTS 和 OPEN_ISSUES 中本次要处理的一个条目。后两份包含失败样本、原版对照与现场恢复，不能用下面的构建结论覆盖它们。T1–T7 不再重复移植。

`d28d1a02` 修复了新 x86_64 libffi 构建从错误工作目录执行 autogen 的问题。双 ABI 签名 HAP 已产出，ZIP CRC、关键 ELF 架构、API23、包名版本与 SDK 签名验证通过。真机后续结果以 STATUS/设备报告为准；不能因这些构建检查通过就宣布全部回归完成。

主机自动化现支持显式 HDC 路径和显式双 ABI 许可；原先 ARM64-only 默认仍保留。双 ABI 许可会额外验证 x86_64 必需库及 ELF 架构，不跳过包校验。UNC 脚本受本机执行策略限制时，可以复制审阅过的脚本到本地忽略目录运行，不修改全局执行策略。

当前候选 `edd6fc87` 保留 `beb00711` 的 desktop root 修复，并补齐 TextInput display 生命周期。真实 Wayland 的 91 项检查、旧源码失败对照、全套 host 与双 ABI 包检查通过；手机冷启动、同进程引擎重建中文、桌面卡恢复后追加中文均有正文截图。键盘开启时重建的首次 notepad 无窗口退出另记 I4，重试成功不能覆盖它。源码搬移审计固定检查 T6→f05cb825 的 181 个文件，之后 Native 修改必须在 native-fixes.json 按完整提交与精确路径登记；不能为了允许后续修复而删除机械检查。其余失败/缺口以 DEVICE_RESULTS、OPEN_ISSUES 为准。

## Terra / Luna 的低上下文工作方式

这是按工作性质分工的建议，不是模型价格或质量保证。不要为减少 token 跳过验收，也不承诺固定节省比例。

1. 每个任务先只读本文件约束、STATUS、本任务在 ledger 中的记录、涉及的函数和当前 diff。用符号搜索定位后再读函数，不一次加载所有提交或 181 个文件。
2. Terra 处理 planner/input/ZC/lifetime/锁等语义适配，以及真实失败的根因分析。一次只修一个可描述的行为；先说明旧/新数据的生产者、消费者和坐标空间。
3. Luna 适合路径引用、来源核对、固定命令验收、日志归档和按既定格式更新 ledger。遇到锁顺序、协议、回收或 UI/会话分歧，输出最小反例，交回语义任务，不自行扩大范围。
4. 每轮先做廉价确定性检查，再做受影响测试，只有涉及产品二进制才重建 HAP。不要每个文档修改都重编 Wine/Mesa；不要只编 Native 就跳过 ArkTS 变化的 HAP 检查。
5. 失败报告保留命令、exit code、前后约 20 行日志、期望/实际差异；完整日志和截图留忽略目录，聊天只带路径和结论。连续两次定向尝试仍失败时先整理证据与假设，不循环灌入全部输出。
6. 一个可编译/可核验批次一个提交；新一轮按 SHA 和文件继续，不依赖长聊天记忆。记录实际消耗，避免大量并行任务重复读取同一合成器上下文。

可直接使用的任务提示：

```text
在 codex/sync-master-20260831 工作。先核对 HEAD/status，不动其他工作区。
只读 docs/sync/20260831/HANDOFF.md、STATUS.md 和 commits.json 中 <任务号>。
目标：<一个明确行为/验收项>。
允许文件：<路径>；不能改：UI/资源/品牌/会话合约/子模块 pins。
输入来源：<源 SHA>；已有检查点：<当前 SHA>。
先定位函数及调用者，再实施；保留现有测试断言。
输出：变更理由、检查命令与结果、剩余限制、提交 SHA；详细日志留忽略目录。
不要重复执行已完成的移植，不 push，不声称未实测的硬件通过。
```

## 后续同步规则

本分支覆盖范围的下一段从 `61cb4c64` 之后开始。重新 fetch 时记录新的不可变 SHA；新的增量继续分类为 adapted / covered_by_product / reference_only / not_applicable / superseded，并写理由。

功能回推 master 从干净的上游分支提取，方案见 [UPSTREAM_PACKETS.md](UPSTREAM_PACKETS.md)。不要把此产品分支整体 merge 到 WineHua。
