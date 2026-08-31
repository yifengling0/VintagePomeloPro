# 下一轮最小入口

当前代码：`codex/sync-master-20260831`，运行时提交 `42e9330a`；后续文档提交不改变二进制。原产品 main、其他工作树、所有子模块 pins 均未移动；没有 push。T1–T7 已完成，**不要重新移植 69 项或重搬 181 个 Native 文件**。

先执行 `git status --short`、`git log -1 --oneline`，读 STATUS 的 Current handoff。只选下面一个未完成项，再读 OPEN_ISSUES 中对应条目及 DEVICE_RESULTS 的对应证据。不要一次载入全部 Git 历史、完整 hilog 或所有截图。

## 当前优先任务：I1 前台 GL 告警；I4 保留观察

先读 `gl-resize-device-summary.json` 和 OPEN_ISSUES I1；需要旧对照时才读 `gl-i1-minimal.json`。`42e9330a` 已让几何缩放保留 EGL context/window surface 与 NativeImage consumer queue，五轮十次尺寸变化保持同一 key 且全部可见；不要撤回为每次 Destroy/recreate。完整 905 秒中 consumer 成功帧 98,280、通知 101,525、empty-update 35，说明 queue producer 持续领先 120 Hz consumer；producer 的 20 次 NO_BUFFER 都在最后一次缩放前。下一步 Terra 只验证 EGL queue transport 去掉 0.5 ms dispatch lead 后的通知/成功帧差与两类 NO_BUFFER；Vulkan/Direct 的 lead 不动。`dc287c9f` 的后台消费也不要重做；禁止混用 AcquireNativeWindowBuffer 与 UpdateSurfaceImage、增加无界阻塞、清错误统计或改 UI/pins 来取得通过。Luna 用同一 x64 GL 五轮缩放与 900 秒有界运行复核实际画面、PID、key、最终 counters 和错误区间；启动失败单独保留，不能以成功重试覆盖。

I4 回收包已经由 `252176de` 完成：SIGCHLD 只通知、普通线程只回收本模块登记的 fork child、握手前登记、晚到的 label 不复活退出进程、fd 由 reader 单独关闭。132 项真实信号/进程检查和完整主机门禁通过，旧源码在早期退出发布处失败。主进程 52298 的五步成功序列仍有效，但安装 42 前的 252 主进程 60091 又在冷 x64 GL 启动中保持 metadata-only/0 surfaces/0 renderers 197 秒，GL guest 根本未启动；回收线程仍在发布真实退出。先读小文件 `reaper-cold-first-frame-failure.json`，不要再次实现统一回收器，也不要把 c5 的“备用 handler 仍覆盖”描述当作当前代码或声称 I4 已解决。

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
