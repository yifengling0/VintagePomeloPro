# 下一轮最小入口

当前代码：`codex/sync-master-20260831`，运行时提交 `dc287c9f`；后续文档提交不改变二进制。原产品 main、其他工作树、所有子模块 pins 均未移动；没有 push。T1–T7 已完成，**不要重新移植 69 项或重搬 181 个 Native 文件**。

先执行 `git status --short`、`git log -1 --oneline`，读 STATUS 的 Current handoff。只选下面一个未完成项，再读 OPEN_ISSUES 中对应条目及 DEVICE_RESULTS 的对应证据。不要一次载入全部 Git 历史、完整 hilog 或所有截图。

## 顺序与分工

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
- 当前最新 HAP 为 `VintagePomeloPro-sync-dc287c9f-dual.hap`，SHA-256 `f4d01a4e1bd9fd92f48135af71397c4d0e5ea009859029da415a41add3143b65`。实际产物、截图与日志在忽略目录 `.hvigor/outputs/sync-master-20260831/`；来源身份以安装记录和哈希为准。
- 本机设备证据实际位于原 Windows 工作树 `D:/temp/VintagePomeloPro/.hvigor/outputs/sync-master-20260831/`，不是新 ext4 clone 的同名目录；原工作树只允许使用这个忽略输出目录，不改原代码。
- 新包相对 edd 只有两种 ABI 的 libentry.so 变化；其余 119 个 Native 条目、ArkTS/wine-data.zip 字节一致。Hvigor 曾移除根 build 中间目录；复用依赖必须校验实际输入，不盲信缓存。Native 后续修复另提交、另验收，并在 native-fixes.json 精确登记，不改 T7 机械检查点。
- hilog/stderr 先按进程和主题过滤，排除 `__env`、`entryParams` 再输出；不输出签名资料、设备 ID。用 JSON 解析提取 UI 文本，避免把单行完整 layout JSON 灌进上下文。
- 每个任务输出：一个结论、变更范围、命令/exit code、证据路径、未覆盖项、提交 SHA。长日志不进 Git；失败结果不删除。每轮交付可编译的小提交后再换模型。

## 后续回推

产品验收和 main 接受后，按 UPSTREAM_PACKETS 的 P1–P5 从干净上游分支提取：几何正确性、GL 稳定性、DXVK 优化、guest→Native→ArkTS 手柄链、默认关闭的实验。子模块贡献先于 gitlink。此文件不是向公开 master 推送私有历史的授权。
