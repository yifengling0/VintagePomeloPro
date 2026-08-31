# 本轮设备验证记录

状态：进行中，不能据此宣布全套回归通过。

## 包与设备条件

- 产品基线 main：`2c0436360ad3f821afe3fd6ea29d76e89f3781e7`。
- 基线签名 HAP：1.3.3 ARM64，SHA-256 `2bcc61e7124480810bdb03e4ce98321984e79972c856dfad6da84ab4e52a0bf2`。RC 源码 `358e3147` 与 main 只差 CI/docs，已核对运行时代码等价。
- 初始候选源码：`d28d1a02c42f87f6cb7a37f130ad35fefb668205`；1.3.3 双 ABI，481284048 bytes；SHA-256 `8c3239aa2a4fcc73deec13a2ec5993ac3d5684dfc42fd7007049207c83129a61`。下方 1–13 项的候选设备证据来自此包。
- 会话修复候选源码：`beb007114776644a47244565a44a83b0b4afa9c9`；1.3.3 双 ABI，481283693 bytes；SHA-256 `bb6e65b9718d9819e543505d1ddbb8504c38ba34b9cda7f3e21fc09956186fec`。ZIP CRC、API23、关键双 ABI ELF 与 SDK 签名验证 PASS。121 个 Native 库和 wine-data.zip 均与 d28d1a02 字节相同；ArkTS modules.abc 已改变。缺失的中间依赖缓存从已验证 d28 包重新暂存，保留旧目录备份，再经根 Makefile 的 HAP 编译/签名；未跳过应用编译或包校验。记录 `root-fix-{runtime-reuse,package-audit}.json`、`root-fix-signature.log`、`root-session-fix-hap.log`。
- 最新 IME 修复源码：`edd6fc879a64efcf2f348b53cacbebc7dc22a1fb`；1.3.3 双 ABI，481302674 bytes；SHA-256 `4e8a710e6b41f8190d0e9c28b51bf7b9b4facca565e34656c699440698617d60`。第 18 项证据来自此包。ZIP CRC/API23/关键 ELF/SDK 签名均 PASS。与 beb 比较，仅 ARM64/x86_64 的 libentry.so 变化；其余 Native、modules.abc、wine-data.zip 逐字节相同。113 个已暂存依赖输入以及相关源码/pins 核对后，根 Makefile 复用 assemble 输出，实际重新编译两种 ABI 应用 Native 和 ArkTS，再打包签名。记录 `ime-lifecycle-{runtime-reuse,package-audit,device-identity}.json`、`ime-lifecycle-signature.log`、`ime-lifecycle-hap.log`。
- 真机 ARM64、系统 API26；包 target/min API23。构建支持 x86_64 不等于在 x86_64 设备上验证。Windows guest x86/x64 与 HAP ABI 也不是同一概念。
- 通过 `install -r` 保留用户数据；无卸载、无重置 prefix。运行锁保持亮屏。原屏幕超时 600000ms，测试完成后释放临时锁；原产品渲染模式 DXVK 2.6，自动化后需恢复。
- 截图、完整日志、安装与签名验证输出放在忽略目录 `.hvigor/outputs/sync-master-20260831/`。不提交设备标识、证书或用户资料。
- 注意采样脚本复制到本地忽略目录后，自动生成的 `sourceCommit` 会从外层原工作树取得 `2c043636`，它不是候选二进制的源码身份。原 JSON 保留；候选身份以实际安装 HAP 的 SHA-256、安装记录和上述 `d28d1a02` 构建记录为准，不把这个脚本字段用作候选来源证明。

## 已有证据

1. 基线与候选的主页/设置/库页面保持产品 UI；源代码保护范围另由 audit.py 校验。
2. 候选 Modern cube 从 frame 24211 持续推进，窗口最大化、F1 D3D9、F2 D3D11 可见，regress=0。五轮 maximize/restore 后返回启动器，再点运行卡片恢复，frame 37200 仍正常。这些是功能证据，不能当作严格性能 A/B。
3. 候选短探针 run `phase2-20260901-001243-01-core-reuse`：audio-x64/audio-x86 PASS；opengl-x86 PASS；opengl-x64 的程序结果 PASS（1297 frames、fallback=false）和四象限画面 PASS，但 NCP 日志出现 `serial=10 gl=0x505 drops=1`，严格 host-summary 为 FAIL。
4. 基线复测 run `phase2-20260901-001819-01-opengl-reuse`：opengl-x86 PASS；opengl-x64 同样完成 1297 frames、四象限正确、fallback=false，但出现 `serial=374 gl=0x505 drops=1`，严格 host-summary 为 FAIL。这仅证实基线也存在问题，不能证明候选所有失败频率/恢复时间都等价。
5. 第一组基线 GL 稳态测量 `baseline-a1` 出现 `serial=3755 gl=0x505 drops=1`，严格 runner 记 INCONCLUSIVE；原始时间序列保留，后续描述性分析不得改写为无告警通过。
6. 三组采样的 presenter FPS / 帧间隔 P95(us)：A1 124.2909 / 8182，B1 124.3806 / 8195；A2 124.3980 / 8189，B2 124.3968 / 8186；A3 124.4722 / 8189，B3-retry1 124.2486 / 8194。第一组两边各一条告警，严格 INCONCLUSIVE；其余完成的样本严格 MEASURED。第三组原 B3 是启动 readiness 失败，没有性能结果，重试不能抹除该失败。完整 TIMING 序列、温度/电源条件与解释在忽略目录的 `gl-comparison-descriptive.json`。这是 NCP presenter 间隔，不冒充屏幕刷新率；所有有效样本的差异小于 5%，不足以宣称统计等价。
7. 候选主进程 63093、同一 VirGL / observe-product-summary 会话中，GL 运行期间正常 Want 新开记事本成功，没有引擎 preparing/stop。临时 100 行 fixture 在 guest 内生成；滚轮从 row 1 到 row 4 再回 row 1，日志有匹配目标与 InjectAxis。Ctrl+O 打开文件框、浏览组合框 popup、Esc/取消、Alt+F 菜单、触摸拖动窗口均可见正确响应。启用宿主键盘后“同会话中文”真正出现在 Wine 记事本文本中；不是只看剪贴板候选条。编辑仅限测试 fixture，关闭选择不保存。截图前缀 `candidate-notepad-*`、`candidate-open-combobox-popup`、`candidate-warm-ime-committed`。
8. x64 连续会话从 00:52:43 出图到 01:03:50，超过十一分钟；同主进程与同 surface key，NCP blit frame 最终 77760、MAIN displayed 75000、末段 failed_swaps=0。五轮最大化/还原和五轮启动器/运行卡恢复都可见正确图像；中间还包含上述多窗口/IME 动作。严格日志门禁仍失败：NCP 累计 drops=719，0x505 告警保留在 `candidate-x64-soak-completed.log`，末段帧数继续增长且 drops 不再增加。不能把恢复成功写成“零丢帧通过”，也不能将有动作的这段拿来和静态性能样本直接比较。
9. 首次 x86 长跑主进程 6563 约 01:04 至 01:12:43，NCP frame 60000 后被图形策略重建中断，尚不足十分钟。同会话运行 War3 后，运行卡片恢复选择了 product-virgl，日志明确为 ready→switching“切换图形主机配置”，不是可计数的后台恢复。该组五轮缩放有图像；后台批次无效，截图必须逐张核对，不能凭自动点击脚本退出码当作 PASS。后续应使用不带 LAB 的产品路径重新计时，且每步确认页面/会话仍匹配再操作。
10. 候选 War3 1.24.4 OpenGL 已进入 Booty Bay 地图，框选五个单位、P 巡逻命令、单位实际移动、F10/结束游戏/退出确认有效。全屏右侧可见边缘拖影，部分菜单首次点按只高亮；随后原版主进程 19182 在相同 x86 GL 共存条件复现这两种表现及退出后 GL 边框裁切，证据为 `baseline-war3-*`。功能动作有证据，但不标游戏无缺陷通过。虚拟触摸板开关和单指相对光标移动可见，测试后已恢复 OFF；这不代替真实鼠标锁定/双指/手柄验收。
11. IME 原版对照：主进程 19182 在 LAB→产品策略切换、Wine 同进程重建后，中文候选提交未落入 notepad，英文 `baseline-ascii` 正常。force-stop 后主进程 23192 冷启动 notepad，使用相同输入方法，“原版冷启动中文”真正上屏。与候选重建失败/冷启动成功的表现相同；`baseline-ime-restart*`、`baseline-ime-cold*` 留证，源码生命周期问题见 OPEN_ISSUES I2。
12. 第二次 x86 长跑主进程 27821，使用无 LAB 产品路径从 01:25:30 左右出图。首次最大化/还原可见；01:26 Back 后，“图形测试”运行卡片未恢复，未发生 Wine 引擎重建，桌面 renderer 保持后台暂停，NCP 报 0x505。01:30 新开同产品策略 notepad 触发正常应用内前台提升，01:31:12 关闭空白 notepad 后原 GL 同 key 持续出图。这次诊断恢复不能冒充运行卡片恢复通过；也不能把后台暂停的四分钟算成连续前台出图十分钟。完整日志 `candidate-x86-product-app.log` 和 `candidate-x86-product-recovery.log` 保留。
13. 同次 x86 在恢复后的 01:31:12 至 01:41:53 连续前台运行超过十分钟，末段 `displayed=83280`、`failed_swaps=0`；同 key `123050813030425`、同进程，无引擎重建。五轮最大化/还原的截图逐张确认，后四轮在恢复之后完成。每 25 秒采样的 MAIN/GL-PERF 日志留在 `candidate-x86-product-monitor.log`，最终截图和日志为 `candidate-x86-product-completed.*`。该前台区间未再观察到 NCP 0x505 告警；之前后台阶段最后一条告警为 drops=5400，整次运行的严格零告警门禁仍不通过，也没有五轮卡片恢复成功。

14. 桌面根会话基线对照：主进程 41748、01:42–01:43 的 `baseline-root-card-*`，同产品 x86 GL 启动后 root 保留，运行页同时显示 Wine 桌面与图形测试。点击图形测试能返回桌面，但内置相对路径会另启动一份 smoke；这次控制用于确认“有 root 时不卡住”，不当作原 x86 会话的恢复。
15. `beb00711` 修复包冷启动主进程 54665：02:00:49.457 收到 desktop_root #1，49.556 才 READY，期间不再清理该 root。02:01–02:05 逐轮 Back→运行中→Wine 桌面，共五轮；每轮先确认卡片存在，再点桌面卡，截图逐张确认原 GL 图像恢复，帧数持续推进至 MAIN 22680，同 key `238636972900377`、同 PID，没有引擎重建或额外 smoke 进程。证据 `root-fix-{running,resume}-1..5`、`root-fix-runtime.log`、`root-fix-five-cycles.log`。这通过 I6 的定向恢复门禁；整段保留 10 行 0x505 告警，最后记录 drops=1080，不宣称严格零错误。
16. beb 的 Legacy DXVK 独立冷启动，主进程 55651，日志确认 `backend=dxvk_legacy route=product-vulkan`、product batching。D3D11 cube 出图并持续转动；最大化、还原、Back→运行中→Wine 桌面返回后仍出图，frame 27639、regress 0。第一次 F1 未取得窗口焦点，没有发生切换；点击窗口再 F1 后 D3D9 初始化失败，HRESULT 0x8876086a、白屏。F2 回 D3D11 恢复。原版主进程 63216 同样操作复现相同错误/白屏；这是失败的 Legacy D3D9 门禁，详见 OPEN_ISSUES I7。还原动作后立即截图曾截到过渡白帧，稍后恢复截图有正确图像；不把即时白帧删掉。
17. 最新 beb 候选重新覆盖安装（`root-fix-final-candidate-identity.json`）后，主进程 1524 使用产品 `dxvk_modern_2_6`。可见 D3D11、触摸窗口后 F1 的 D3D9、F2 的 D3D11 均实际出图，frame 2865→4716→6844，regress 0，未出现上述 init 错误；对应 `root-fix-modern-{ready,d3d9,d3d11-final}.jpeg` 与日志。包保留在手机，无卸载或 prefix 重置。

18. `edd6fc87` TextInput 生命周期修复，ARM64 主进程始终为 18390，未 force-stop：
    - 冷启动 02:35:26.858 注册；产品 notepad PID 19150 正文可见“移植冷启动中文”。
    - 02:42:02.124 Shutdown(resources=2)，03.160 重新注册；LAB x86 GL 实际出图。初次截图被系统电源菜单遮挡，退出菜单后才取可见画面，不把遮挡截图算绘制结果。
    - 02:43:07.054 Shutdown(resources=3)，07.496 重新注册；切回产品 notepad PID 25246，“引擎重启中文”实际写入 Wine。此路径在原版/先前候选失败，本次定向通过。
    - 键盘开启状态下 02:44:31.589 Shutdown(resources=3)，32.002 重新注册；READY/root #11 建立，但首个 notepad PID 26937 在出窗口前退出。**这是失败样本**，有 mmap/dlopen SIGSEGV/异常恢复日志，详见 I4。一次独立的同引擎新启动 PID 29451 于 02:47:22 成功，没有第四次重建；“再次重建中文”上屏。Back→运行中→Wine 桌面保留原 notepad 正文，再次提交“恢复后中文”可见。不能用该成功重试声称首个启动成功，也不把无窗口启动失败记为中文 commit 丢失。
    - 设置恢复 DXVK 2.6 后，产品维护动作于 02:51:39.102 执行第四次 Shutdown、39.627 注册，47.145 READY；桌面任务栏重新出图。测试输入仅涉及未命名测试记事本，无用户文档写入、无 prefix 重置。键盘/触摸板 OFF，常亮 override 保持，外接马达仍未检出、Hub seq=0。
    - 图像：`ime-lifecycle-cold-chinese-result`、`restart-chinese-result`、`third-chinese-result`、`text-running`、`text-resume`、`resumed-chinese-result`（后续前缀同为 ime-lifecycle）；`ime-lifecycle-restored-launcher.jpeg` 实际为恢复后的设置页，已核对 DXVK 2.6 蓝色选中。日志 `ime-lifecycle-device-final.log`、`ime-lifecycle-final-restored.log`、`ime-lifecycle-registration-timeline.log`、`ime-lifecycle-armed-current.log`、`ime-lifecycle-armed-wine-stderr.log`、`ime-lifecycle-same-engine-retry1.log`；最小启动摘要 `startup-i4-minimal.json`。启动异常不能据现有证据归因于 I2 修复或与旧 B3 认定同根因。

19. `dc287c9f` 后台零拷贝消费修复（本轮不是重新执行 T1–T7）：
    - 旧 edd 主进程 44689：03:09:55.690 Back 隐藏后 18 ms，3-buffer 队列全部占用；NativeWindow RequestBuffer 返回 40601000，EGL blit 报 0x505。到 03:14:47.898 恢复前，日志共 5693 次 RequestBuffer NO_BUFFER、48 行限频 blit-drop 告警，末条 drops=5640。同 key 恢复后出图、生产端失败停止。`gl-i1-before-events.log`、`gl-i1-before-resume.log` 和 before/resume 图像保留。
    - 新候选 ARM64 主进程 53844，正常产品 x86 GL（无 LAB）：03:21:49.818–03:22:57.251 后台消费 2000 帧（29.66 Hz），显示 loop 固定 4673；03:23:53.124–03:24:41.034 最大化后的后台消费 1422 帧（29.68 Hz），显示 loop 固定 11352。两个区间生产/消费 NO_BUFFER 和 blit-drop 均为 0；恢复均显示原 GL、同 key 234835926843417，原 8329 μs display period 被还原。图像 `gl-background-{ready,resume-1,maximized,resume-2}` 逐张核对。
    - 同一运行在最大化重配（03:23:28）仍出现队列耗尽及 0x505，之后出图；前台也有消费端空队列警告。不能把两个后台区间的 0 错误扩展为全程 0 错误。主进程 top 最后短样本前台 70.5%、后台 25.0%，只证明本轮未以后台满速换取消费，不是新旧版耗电/性能等价试验。
    - 第三次 GL 隐藏后切产品 Modern 策略：03:25:52.919 旧 key 释放完成、52.961 renderer Shutdown OK，53.233 TextInput 重新注册，03:26:01.067 READY。未 force-stop 主进程；后台等待没有阻塞本次引擎退出。随后 Modern D3D11 出图，F1 D3D9 出图；03:27:09.986–03:28:33.020 D3D9 后台消费 2499 帧（30.10 Hz）、显示 loop 固定 8066，区间错误为 0，恢复后 frame 9787，F2 回 D3D11 frame 13036、图像有效。D3D9 切换阶段另有 blit-drop，不能归入通过的后台区间，也不凭 profile 名称断言其 transport；D3D11 Venus 独立检查另记后续行。
    - 接着在同进程第二次重建启动 x64 GL：03:30:04.013 进入“启动桌面”，05.695 有 shell 窗口 #7 元数据，但 WL-STAT 始终 `toplevels=1 surfaces=0 renderers=0`，180 秒仍未出桌面；GL 程序没有进入本次回归。截图 `gl-background-x64-{ready,state,not-ready,deadline}`（ready 是文件名，不代表就绪）和 `gl-background-x64-failed-start-full.log`、`gl-background-x64-start-stderr.log` 保留。此为 I4 新失败样本；没有无限重试、没有将旧渲染器正常 Shutdown 当新桌面启动成功。
    - 构建：根 Makefile 的完整 host/model/GLES/HUD/navigation/CI 测试 PASS；113 个 staged 依赖和 wine-data.zip 先逐字节核对才复用，双 ABI 应用 Native 重编并签名。HAP `VintagePomeloPro-sync-dc287c9f-dual.hap`，481314373 bytes，SHA-256 `f4d01a4e1bd9fd92f48135af71397c4d0e5ea009859029da415a41add3143b65`。ZIP CRC、API23、bundle/version、关键 ELF ABI 检查通过；与 edd 相比仅两份 libentry.so 变化，其余 119 个 Native 条目、ArkTS 和嵌套 Wine 数据相同。`gl-background-{host,hap,install}.log`、`runtime-reuse.json`、`package-audit.json`、`device-identity.json` 留在忽略证据目录（后四类前缀均 gl-background）。新 Native 提交按两个精确路径登记，T7 的 181 次机械搬移检查不变。
    - 独立冷启动 Modern D3D11 控制主进程 62830：日志明确 Venus Direct capability/target ready，key=279868658941977。03:35:05.785–03:36:27.735 隐藏 81.95 秒、消费 2265 帧（27.64 Hz），显示 loop 固定 9738；该区间生产/消费 NO_BUFFER、blit-drop 为 0。恢复截图立方体实际绘制，frame 10904，同 key。整个控制样本仍有前台 13 次 RequestBuffer NO_BUFFER 和 2 次消费端空队列，不能把后台结果泛化为全程无错误。`gl-background-venus-{ready,hidden,resume}` 图像与 `gl-background-venus-complete.log` 留证。
    - 收尾：03:37 正常关闭 cube，Wine 桌面/任务栏可见；03:39 设置截图 `gl-background-setting-scan-5.jpeg` 明确 DXVK 2.6（实验）为蓝色选中、Box64 默认档选中，无需再修改；scan-4 仍为 Wine 手柄模式、seq=0、无外接马达。触摸板/键盘 OFF，最新包留在手机，常亮 override 保留，无卸载、prefix 重置或保存用户文档。SDK `verify-app` 独立校验成功（`gl-background-signature.log`），并非仅凭构建签名输出判断。
    - 最小 I1 摘要 `gl-i1-minimal.json` 约 6 KB，原始日志 `gl-background-combined.log` 包含第一主进程的 GL/Modern 阶段，Venus 控制另存，保留全部错误；新启动摘要 `startup-i4-second-recreation.json` 约 4 KB。本轮定向结论是后台消费/恢复路径改善；I1 缩放/空通知与 I4 启动仍开放，不声明完整 GL 或整机验收通过。

20. **I4 退出状态与 Wine 清理信号（2026-09-01 03:51–04:28）**：最终代码 `c5e00c9ac841eb00d262b2a80ff596f90551a7e8`，保留已完成的移植/机械重构与产品 UI；本轮没有更新任何 guest pin。
    - 原 dc 主进程 62830：从 Modern 切 VirGL、准备启动记事本时，Explorer launcher 13143 / client 13151 只有 shell #4 元数据，至少 294 秒无首帧。记事本的 WINEDEBUG Want 只在 ensureReady 后生效，无法追到这个 Explorer。线程为 sleeping、WCHAN 屏蔽；processdump 权限被拒，未绕过。最小记录 `startup-i4-third-recreation.json`。
    - 临时诊断只在 Explorer env 增加有限 WINEDEBUG / WAYLAND_DEBUG，没有改 UI、重试或时限。双 ABI 构建/包审计/签名通过，HAP SHA-256 `2ac1acd3e0b6ce21c0f03186d21b519cdd854737e9dd788fcef0b6943bef7d1f`；只用于诊断，不是产品候选。PID 17335 冷启的首个 Explorer 17929 在 dlopen 出错，内建第二次尝试后记事本可见；一次 VirGL 重建也可见。第二次 Modern 重建时 wineboot worker 20058 明确 exit=1，却被登记成 unknown 后误判初始化完成，Explorer 20112 卡在无 toplevel。`startup-i4-failed-boot-minimal.json` 保留关键证据；WINEDEBUG trace 有输出，未建立 WAYLAND_DEBUG wire trace。临时补丁已经撤掉，未提交到产品代码。
    - `56345d67` 修复 waitpid 状态丢失、未知通知覆盖、重复等待返回值、引擎标记覆盖及历史失败污染新尝试；新增 `make test-process-lifecycle`。真实 fork/waitpid + 生产注册表 31 checks、全套 host/model/GLES/HUD/navigation/CI 通过；旧源码在退出码保存断言失败。不过它误把 SIGKILL 编为 137 参与失败判定，实机第一次重建被错误拒绝，因此这个中间 HAP **不接受**，错误截图/日志 `startup-exit-first-rebuild*` 和 `startup-exit-signal-false-failure.json` 保留。
    - 563 的 x64 GL 是独立有限证据：PID 28043 / guest 28849，key=123905511522329，前台有旋转立方体；后台 04:14:20.192–04:16:35.268 共 135.076 秒消费 4018 帧（29.75 Hz），显示循环固定 9135，区间内生产/消费队列错误为零；桌面卡恢复后同 key 出图。初次 attach 有两次 RequestBuffer NO_BUFFER，不能记全程零告警。240 秒 smoke 在约 235 秒被明确切换引擎中断，不能计长时通过，也不是最终 c5 的性能资格。见 `startup-exit-x64-gl-summary.json`。
    - `c5e00c9a` 纠正信号含义：Wine 的正常退出清理可最终发 SIGKILL，因此保留 source=signal/exitCode=-1，只有 WIFEXITED 才提供已知退出码。修正后的 31 checks 和完整主机门禁通过，日志 `startup-exit-signal-all-host.log`。两次正式构建均经既有 winehua-dev 的根 Makefile；复用前校验 113 个 staged dependency ELF 与 wine-data.zip。最终 HAP `VintagePomeloPro-sync-c5e00c9a-dual.hap`，481343036 bytes，SHA-256 `4fba7852a92273b15b77b976a6e0fefc2dd57c7264aa6a75b684a8bd995af8ee`。API 23/双 ABI/ZIP CRC/ELF 校验通过，SDK verify-app 在 04:21:20 独立通过。相对 dc 只有两份 libentry.so 变化，其余 119 Native 条目、ArkTS 与嵌套运行时字节不变。
    - c5 实机主进程 36343：Modern 冷启 READY 04:22:17.982，记事本 PID 37114 窗口可见；第一次切 VirGL READY 04:23:28.189，PID 38464 窗口可见。该次 wineboot 38278/38362 的 signal 清理不再误判失败。没有改 Box64 档位、重试或 prefix。第二次切 Modern 在 04:24:16.642 进入 Explorer 阶段，launcher 39271 / client 39283、shell #7 元数据出现，但留证到 209 秒仍是 toplevels=1、mapped surfaces=0、renderers=0，180 秒门禁失败；**保留失败，不算三轮通过**。原始日志 `startup-exit-signal-complete.log`、stderr 与 deadline 截图保留。
    - 范围限制：c5 的明确 exit=1 拒绝分支有真实 host 进程测试，本设备序列没有重新产生该明确退出码，不以 563 的错误 137 代替验证。冷启动备用 reaper 仍覆盖 NAPI handler 并丢状态，现有 handler 异步信号安全也未修；metadata-only 首帧挂起、void Main 丢返回码和 guest loader 问题仍开放。Wine 子模块没有变化，Box64 offset 无符号信息时没有猜偏移或改子模块。

21. **I4 手机进程统一回收（2026-09-01 04:35–04:52）**：运行时 `252176de96800577c7c87914a7d2fd418f0c9f35`，本轮五次首帧与会话动作定向通过，全量 T8 仍未完成。
    - 修复范围为 `proc/wine_process.{cpp,h}`、`phone_adapter/phone_process.cpp`、`bridge/napi_init.cpp`。移除互相覆盖的两种 SIGCHLD 安装；信号中只写非阻塞 self-pipe，普通线程按现有注册表只 waitpid 本模块创建的 fork child，不夺走其他模块的状态。手机 fork 后立即登记、握手前便可保存早退；broker/app 标签保留同一生命周期，新 fork 显式重置复用的 PID。ReaderThread 不再 waitpid/阻塞等待进程，只读日志并独占关闭 fd；停止时用有限 poll 唤醒。公共 UI/NAPI JSON、重试和时限没有改动。
    - 132 checks 使用真实 SIGCHLD/fork/waitpid 与生产注册表，覆盖持有注册表 mutex 时收到信号、可重入注册表的后台通知、退出早于登记、24 个连续短进程、其他模块的 wait 所有权、迟到标签、PID 生命周期、实际 fd 号码复用以及停止空闲 reader。旧源码仅加 API 适配后，在早期退出发布处失败（`reaper-before-test.log`），完整 host/model/GLES/HUD/navigation/CI 门禁 PASS（`reaper-all-host.log`）。SDK 调用边界仍为 stub，不把该测试泛化为所有 NAPI 生命周期竞争或实际 NCP 硬件证明。
    - 构建仅经既有 winehua-dev 容器根 Makefile，先核验 113 个依赖 ELF 与 wine-data.zip 再复用；API23 双 ABI、ZIP CRC、ELF、SDK verify-app 独立校验通过。HAP `VintagePomeloPro-sync-252176de-dual.hap`，481350349 bytes，SHA-256 `057ce2f392ad0b37882ef674e43b44b9a907a43d0ca7a419026eb2f09a883654`。相对 dc 仅两份 libentry.so 改变，其他 119 Native 条目、ArkTS 和 guest payload 字节一致。产物审计/签名/安装见 `reaper-package-audit.json`、`reaper-signature.log`、`reaper-install.log`；24 个 gitlinks 与产品 UI 保护检查不变。
    - 同一主进程 52298：冷 Modern 在 04:45:51.445 READY（notepad 53221）；第一次 VirGL 04:46:36.535（54109）；第二次 Modern 04:47:03.127（54724）；第三次 VirGL 04:47:58.953（55660）；第四次 Modern 04:48:28.083（56448）。五次均有独立截图目视确认记事本，Explorer 都是 attempt=1/3，没有以重试覆盖失败。所有记录到的退出发布均为后台 TID 52800，包括首次冷启动；明确 code=0 保留 waitpid 来源，Wine 正常 SIGKILL 清理仍是 signal/-1。
    - 第四次重建后输入 `reaper-resume`，键盘剪贴板候选提交“回归测试”真正上屏；04:49:51.751 从桌面 root #13 返回应用库，04:50:12.332 点击 Wine 桌面卡恢复同一 root/主进程，文本保留，再提交“继续”上屏。`reaper-ime-committed.jpeg`、`reaper-card-{hidden,resume}`、`reaper-ime-after-resume.jpeg` 留证。这是记事本/SHM 会话测试，不能把 key=0、后台 frames=0 当成 GL 消费或性能结果。
    - 正常点记事本关闭，再明确选择“不保存”自己的未命名测试文本；04:51:12.114 notepad 56448 退出（signal/-1），桌面和任务栏仍可见。随后关闭测试 app 进程并不带测试 Want 重开应用库，主进程 58852；最终截图 `reaper-final-launcher.jpeg` 已检查，常亮 override 保留、无卸载或 prefix 重置。
    - 最小摘要 `reaper-device-summary.json` 为 4.3 KB，原始父进程日志/stderr 均在忽略目录。该短序列未再出现 c5 的第二次重建 metadata-only 卡住，但不能证明所有历史零 toplevel/loader 故障同因或都已修复；本包没有实机复现明确非零 wineboot 退出，拒绝该退出的分支仍以真实 host 进程测试为证。I1 严格 GL 告警、实体手柄、缺少游戏/设备资产等门禁继续保留。

22. `42e9330a` 几何缩放候选与 252 冷启动反证（2026-09-01 04:59–05:14）：
    - 安装新包前先用已安装 252 冷启动产品路径 x64 wined3d GL。main 60091 于 04:59:19.330 进入“启动桌面”，Explorer attempt=1/3 已起，但只有 metadata toplevel；`surfaces=0 renderers=0` 一直持续到 05:03:44，197 秒时 GL guest 尚未启动，判 FAIL。TID 64612 同期正常发布 signal 与 waitpid(code=0) 退出，证明回收工作但不等于首帧根因解决。失败后才 force-stop；无重试计成功，最小证据 `reaper-cold-first-frame-failure.json`。
    - 新提交只改 `graphics/virgl_surface_presenter.cpp`：geometry-only resize 调 NativeWindow `SET_BUFFER_GEOMETRY` 并保留 EGL context/window surface、consumer queue；display/source-context/direct-transport 变化仍完整 reset。全套 host 门禁 PASS；双 ABI API23 构建、ZIP/ELF/包差异和 SDK signature PASS。HAP 481349410 bytes，SHA256 `fa79d3bb7e74ed81861bbc0c0f76eb9b6c53909fa2fa309ef27faca1f537cf3c`；相对 252 只有两个 ABI 的 `libvirgl_child.so` 改变，119 个其他 Native、ArkTS 与 wine-data 不变。
    - 安装后 main 4378 冷 x64 GL 于 05:05:22.258 desktop READY，guest 5138 于 05:05:23.321 启动，producer TID 4718、consumer EGL TID 5103、surface key `22067541966873`。五轮最大化 1408x593/还原 960x540 共十次都记录 `retained_egl=1`，key 不变；十张截图逐张目视为真实旋转方块/棋盘且尺寸正确。两次返回库再点 root 卡恢复，隐藏区间消费 1179/1648 帧，返回后同 key、同 PID 动态 GL 可见；截图 `gl-resize-{max,restore}-1..5.jpeg`、`gl-resize-card-resume{,-2}.jpeg`。
    - 严格门禁仍 FAIL。完整区间 10 次 geometry change、20 次 producer `NativeWindowRequestBuffer 40601000`、一次前台 consumer `UpdateSurfaceImage 40601000`，没有 cache-miss 行；错误后均恢复继续出图。这说明保留 EGL 消除了本样本的重建/cache 失配，但没有解决 producer/consumer 瞬时队列耗尽。证据 `gl-resize-complete.log`、`gl-resize-device-summary.json`；不能写成 I1 通过或以可见恢复覆盖错误。
    - 本轮没有改 UI、ArkTS、协议、超时/50 ms 退避、GLES Direct 默认、guest 或 pins。手机继续保留常亮；当前包/数据/prefix 未卸载或重置。实体手柄、I4 首帧、I1 零错误和缺失资产门禁继续开放。

音频 PASS 指 guest 提交、host 消费及非零 RMS；没有人耳确认时不声称可听性、音色或延迟通过。当前没有实体手柄输入/马达证据。

## 验收剩余项（不要重跑已完成的移植）

- GL 的 x86/x64 时长、各五轮缩放、x64 五轮恢复、beb x86 五轮恢复以及三组交替采样已有上述证据。严格零告警仍失败（I1），第三组首次冷启动失败未定根因（I4）；不能用成功重试或描述性 FPS 覆盖这些失败。
- Modern D3D9/D3D11 与 Legacy D3D11 有可见绘制证据；Legacy D3D9 两版均失败（I7）。不要把空循环 frame 增长当渲染成功。
- 菜单/popup/滚动/拖动、多窗口、冷启动与同会话 IME、War3 地图动作已有证据；I2 的注册/中文路径已由 edd 定向修复并完成第 18 项真机验证，原失败证据保留。新一次启动无窗口退出另记 I4；War3 拖边/首次触摸（I5）仍未修复。
- 实体手柄方向、断连释放、重连、游戏震动仍未覆盖。当前没有检测到外接马达，手机振动与模型测试不替代这个门禁；真实鼠标锁定/双指触摸板也没有完整动作证据。
- RA2/PAL2/Heaven 等指定游戏在当前设备目录未找到，不以 War3 或 smoke 替代；x86_64 Harmony 硬件未提供。构建与 guest x86 测试不等于另一设备 ABI 实测。
- 受影响主机门禁已在最新 42e9330a 全套重跑通过，日志 `gl-resize-host.log`；其中保留 252 的 132 项真实进程检查及 edd 的真实 Wayland 生命周期 91 checks。双 ABI 包/签名、来源/保护范围与逐提交 Native 修复审计 PASS。当前仍不是全项无缺陷的发行验收。

设备当前安装 42e9330a 包，main 4378 的 x64 GL 有界测试仍在运行且最近一次卡片恢复画面已目视确认；常亮 override=2147483647ms，原 timeout=600000ms 留存。本轮未改 Box64、手柄、触摸板、用户 prefix 或 UI。结束测试时只正常退出自带 smoke/回到应用库，不卸载、不清数据；应用库收尾不算额外 Wine 启动通过。

最终验收应逐项更新本文件和 STATUS，并保留失败和限制，不抹去先前严格检查结果。下一轮最小范围见 NEXT_TASK.md。
