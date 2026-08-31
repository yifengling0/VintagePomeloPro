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
    - 图像：`ime-lifecycle-cold-chinese-result`、`restart-chinese-result`、`third-chinese-result`、`text-running`、`text-resume`、`resumed-chinese-result`（后续前缀同为 ime-lifecycle）；日志 `ime-lifecycle-device-final.log`、`ime-lifecycle-armed-current.log`、`ime-lifecycle-armed-wine-stderr.log`、`ime-lifecycle-same-engine-retry1.log`。启动异常不能据现有证据归因于 I2 修复或与旧 B3 认定同根因。

音频 PASS 指 guest 提交、host 消费及非零 RMS；没有人耳确认时不声称可听性、音色或延迟通过。当前没有实体手柄输入/马达证据。

## 验收剩余项（不要重跑已完成的移植）

- GL 的 x86/x64 时长、各五轮缩放、x64 五轮恢复、beb x86 五轮恢复以及三组交替采样已有上述证据。严格零告警仍失败（I1），第三组首次冷启动失败未定根因（I4）；不能用成功重试或描述性 FPS 覆盖这些失败。
- Modern D3D9/D3D11 与 Legacy D3D11 有可见绘制证据；Legacy D3D9 两版均失败（I7）。不要把空循环 frame 增长当渲染成功。
- 菜单/popup/滚动/拖动、多窗口、冷启动与同会话 IME、War3 地图动作已有证据；I2 的注册/中文路径已由 edd 定向修复并完成第 18 项真机验证，原失败证据保留。新一次启动无窗口退出另记 I4；War3 拖边/首次触摸（I5）仍未修复。
- 实体手柄方向、断连释放、重连、游戏震动仍未覆盖。当前没有检测到外接马达，手机振动与模型测试不替代这个门禁；真实鼠标锁定/双指触摸板也没有完整动作证据。
- RA2/PAL2/Heaven 等指定游戏在当前设备目录未找到，不以 War3 或 smoke 替代；x86_64 Harmony 硬件未提供。构建与 guest x86 测试不等于另一设备 ABI 实测。
- 受影响主机门禁已在 edd 全套重跑通过，日志 `ime-lifecycle-all-host.log`；新增真实 Wayland 生命周期 91 checks，旧源码二次注册探针按预期失败（`ime-lifecycle-before.log`）。来源/保护范围与逐提交 Native 修复审计 PASS。当前仍不是全项无缺陷的发行验收。

设备收尾检查：02:14 已关闭 cube；02:17 设置页明确选中 DXVK 2.6，Box64 默认档未变，Wine 手柄模式未变（seq=0、无外接马达）；触摸板/键盘 OFF。手机保留最新 beb 包和测试期间亮屏锁，尚未将设备门禁标完成，不清用户 prefix。测试 fixture 只在 Wine 临时目录生成且关闭未保存改动。

最终验收应逐项更新本文件和 STATUS，并保留失败和限制，不抹去先前严格检查结果。下一轮最小范围见 NEXT_TASK.md。
