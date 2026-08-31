# 下一轮最小入口

当前代码：`codex/sync-master-20260831`。产品 main `2c043636` 与 WineHua master `61cb4c64` 已刷新核对；`d256317e..61cb4c64` 共 75 项全部有处置。原产品 main、其他工作树和所有子模块 pins 均未移动。T1–T7 与新增 master 增量已完成，**不要重新移植 75 项或重搬 181 个 Native 文件**。

用户已接受现有设备结果并要求停止测试。当前任务是先创建产品 `main` PR，再从干净的 `61cb4c64` 上游分支启动 UPSTREAM_PACKETS 的反向贡献；不再安装候选或继续手机回归。PR 必须保留 OPEN_ISSUES/DEVICE_RESULTS 中的实际限制。

## 当前优先任务：产品 main PR 与干净上游回推

当前源码含 `9bce0b9f` 的 EGL queue pacing 候选（完整 host 与双 ABI HAP 构建通过、未安装）以及 `eb780253` 对 master `ff865647` 的 VKD3D semaphore-feedback 适配（focused host PASS、未新建 HAP）。用户明确停止后续测试，因此两项都必须在 PR 中标明设备未复核；手机仍安装 `42e9330a`。先让 source audit PASS、推送产品分支并创建 base=`main` 的 PR。随后从 `61cb4c64` 新建干净上游分支，先做不依赖子模块的 P1/P2；P3/P4 必须按子模块先行，产品 UI/品牌/签名/设备记录不能进入上游。

I1/I4、Legacy D3D9 和缺失资产的既有证据继续保留为已知限制，不在创建 PR 前继续追测。实体手柄按用户要求延期，不作为这次 main PR 阻塞条件，也不能改写成已通过。

这项完成不能自动关闭全部历史 metadata-only/零 toplevel 故障、手机 void Main 丢返回码或 guest loader 错误。若再复现 I4，先对照本次五步成功摘要，再按 PID/时间读对应原日志；需要理解退出码时才读 `startup-i4-failed-boot-minimal.json` 与 `startup-exit-signal-false-failure.json`。`56345d67` 是被真机否决的中间包，禁止作为接受包使用。Wine 的 SIGKILL 清理不等于 Windows 失败，不能恢复 128+signal 映射。Box64 的 AGENTS 禁止 AI 代写其贡献；当前不改源码/pins、prefix、超时、重试或 UI 来制造通过。

## 其他未完成项与分工（按需读取）

1. **实体手柄验收（I3）**：手机当前仍无外接马达、Hub seq=0。用户连接后，按 UI 明确识别设备；核对左右摇杆 Y、方向/扳机/按钮、按住时断连释放、重连、实际游戏的 XInput/DirectInput 及外接马达震动。Luna 可按固定步骤记录；失败时交 Terra 做一个最小定位任务。模拟事件、手机振动不计硬件通过。
2. **启动失败（I4）**：Terra 先读 `startup-i4-minimal.json`（约 7 KB）、`startup-i4-second-recreation.json`（约 4 KB）和 OPEN_ISSUES I4。手机入口会吞掉返回状态，但启动根因未定；旧 B3 是零 toplevel，edd 是 root 存在/首个 notepad 不出窗口，新 dc287c9f 第二次重建则有 shell #7 元数据、0 surfaces/renderers、180 秒未 ready。需要时才按 PID/时间读取原日志。只诊断 loader/guest 退出与有限模块/SEH 事件；不凭 code=0 或共有缺库告警判断成功/同根因。不 reset prefix、无限重试或缩减门禁。
3. **GL 告警（I1）**：先读 `gl-i1-minimal.json` 和 I1 新段。dc287c9f 已修复后台停止消费引起的持续队列耗尽，下一步只追缩放重配期间的 RequestBuffer NO_BUFFER/0x505，以及前台消费端残留通知；不要重做后台修复或把全项写成通过。三个交替样本属于旧候选，不能冒充新修复的性能比较，也禁止筛选“漂亮结果”。
4. **已有兼容缺陷（I5/I7）**：War3 共存边缘/首触摸、Legacy D3D9 初始化失败，原版对照已完成。每次仅处理一项；需要 guest/子模块变化时另开干净来源的独立功能任务，当前 pins 不动。不得把 UI 改版、绕过错误检查或空循环 FPS 当修复。
5. **缺少测试资产**：RA2/PAL2/Heaven、真实鼠标和 x86_64 Harmony 设备未提供。补齐后再跑相应动作；不声称由 War3、guest x86 或主机模型替代。

I6 桌面根误清理已由 `beb00711` 修复，实际服务模型测试与五轮同 PID/key 的手机卡片恢复已通过。无需重做这项；若后续修改会话逻辑，保留该回归测试。

I2 生命周期修复 `edd6fc87` 已完成：真实 Wayland 91 checks，冷启动/重建后中文与卡片恢复后继续输入有真机证据。第三次重建的首个 notepad 启动失败单列 I4。不要把该失败重新当成已定位的 global 未重新注册，也不要泛化为所有 IME 场景通过。

## 安全与成本约束

- 保留产品 UI、资源、身份、浮窗、控制层、会话/NAPI 合约、GL 策略与 controller 约定。允许改动范围必须由所选问题的调用链决定。
- Native/build 只能走既有 winehua-dev 容器中的根 Makefile 和 ext4 源码。原 Windows/其他 WSL 工作树不修改；不要把整个分支 merge 到 WineHua master。
- 文档/日志改动只需 diff 与 source audit。ArkTS 修改需模型测试及 HAP 编译；Native 语义修改需受影响 host 测试、双 ABI 编译/包校验与相应实机动作。
- 当前最新 HAP 为 `VintagePomeloPro-sync-42e9330a-dual.hap`，SHA-256 `fa79d3bb7e74ed81861bbc0c0f76eb9b6c53909fa2fa309ef27faca1f537cf3c`。实际产物、截图与日志在忽略目录 `.hvigor/outputs/sync-master-20260831/`；来源身份以安装记录和哈希为准。
- 本机设备证据实际位于原 Windows 工作树 `D:/temp/VintagePomeloPro/.hvigor/outputs/sync-master-20260831/`，不是新 ext4 clone 的同名目录；原工作树只允许使用这个忽略输出目录，不改原代码。
- 42 包相对 252 只有两种 ABI 的 `libvirgl_child.so` 变化；其余 119 个 Native 条目、ArkTS/wine-data.zip 字节一致。Hvigor 曾移除根 build 中间目录；复用依赖必须校验实际输入，不盲信缓存。Native 后续修复另提交、另验收，并在 native-fixes.json 精确登记，不改 T7 机械检查点。
- hilog/stderr 先按进程和主题过滤，排除 `__env`、`entryParams` 再输出；不输出签名资料、设备 ID。用 JSON 解析提取 UI 文本，避免把单行完整 layout JSON 灌进上下文。
- 每个任务输出：一个结论、变更范围、命令/exit code、证据路径、未覆盖项、提交 SHA。长日志不进 Git；失败结果不删除。每轮交付可编译的小提交后再换模型。

## 后续回推

产品验收和 main 接受后，按 UPSTREAM_PACKETS 的 P1–P5 从干净上游分支提取：几何正确性、GL 稳定性、DXVK 优化、guest→Native→ArkTS 手柄链、默认关闭的实验。子模块贡献先于 gitlink。此文件不是向公开 master 推送私有历史的授权。
