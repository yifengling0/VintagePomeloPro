# 本次测试发现的限制与定向后续任务

这是问题记录，不把已有缺陷或测试工具限制隐去，也不默认扩大本次上游移植范围。

## I1：OpenGL 0x505 间歇性丢帧

候选与冻结基线的 x64 短探针均出图并完成四象限校验，但严格日志门禁因 `gl=0x505` 的 blit-drop 告警失败；首组稳定采样两边各一条相同告警。三组采样和长跑已在 DEVICE_RESULTS 中补齐：后两组完成的样本无该告警，原 B3 readiness 失败单独保留；x64 长跑与 x86 后台阶段仍有告警。beb 的五轮 x86 卡片恢复成功，但整段有 10 行告警、末次 drops=1080。不能因基线复现就判定频率/恢复完全一致，也不把改善 root 卡片的 ArkTS 修复当 GL 错误修复。

2026-09-01 新证据把后台持续失败定位到队列耗尽：edd 的 PID 44689 在 03:09:55.690 隐藏 tl=1；18 ms 后同生产线程报告 `Released=0 Requested=0 Flushed=2 Acquired=1`、NativeWindow `RequestBuffer ret:40601000` 和 `gl=0x505`。主 renderer 的 loop=10091 一直暂停，生产端按既有 50 ms 退避重试，最后告警 drops=5640。03:14:47.898 恢复同 key 后重新出图，生产端错误停止。证据 `gl-i1-before-events.log`、`gl-i1-before-resume.log` 及截图；这是 NO_BUFFER 路径的实证，不把所有 0x505 都推断为显存不足。

`dc287c9f` 只改 EglRenderer 两个文件：隐藏时用帧通知唤醒同一 EGL 线程执行 UpdateSurfaceImage，不绘制/交换隐藏 XComponent；现有 SetFramePeriod 接口限制后台约 30 Hz，恢复时显式还原原 VSync 周期。没有混用 NativeImage 手动 Acquire/Release，没有改 WHIP、guest pins、默认关闭的 GLES Direct、producer 错误检查或退避。帧、暂停与关闭状态按同一个 CV mutex 发布，避免检查/等待间丢通知。

真机 PID 53844 的两次后台区间分别消费 2000/1422 帧，display loop 保持不动；同 key `234835926843417` 返回后实际恢复 GL。后台生产端没有 NO_BUFFER/0x505，后台停止引擎也完成资源释放和 Shutdown，再注册并启动 Modern D3D11。已有 host 全套、双 ABI Native/HAP、来源/包审计通过。详见 DEVICE_RESULTS 19；这只关闭后台持续耗尽的定向路径。

**I1 仍未全项通过**：同一候选在 03:23:28 最大化时，EGL window 重配后出现 `Flushed=3 Acquired=0`、RequestBuffer NO_BUFFER 和 `gl=0x505`；随后恢复出图。前台消费端也有 UpdateSurfaceImage NO_BUFFER（信号合并/通知时序尚未定论）。下一步 Terra 先读 `gl-i1-minimal.json`，只追缩放的队列/旧 buffer 生命周期与残留通知；需要时再看单个时间窗和 `egl_renderer.cpp`、`virgl_surface_presenter.cpp`。不能把队列耗尽当成成功、清掉错误检查、删除日志门禁或用成功重试覆盖失败。若需修改 guest，另开子模块任务，本轮 pins 保持不动。

## I2：同进程 Wine 引擎重建后的 IME 注册

实测候选：先通过 `observe-product-summary` 启动 GL，再用无 LAB 参数的正常 notepad Want；产品策略切换按既有设计重启了 Wine 引擎。新桌面/记事本能运行，英文按键输入有效，但中文 commit 未落入 Wine。完全 force-stop 应用再冷启动 notepad，启用宿主键盘后中文“冷启动中文”成功上屏。

源码证据：`input/text_input.cpp` 的 `Register` 在 `global_` 非空时直接返回；本单例没有对应 Unregister/Shutdown。`compositor/wayland_server.cpp::Stop` 销毁 display，但没有清理这个单例保存的 global、event source、pipe 和排队状态。日志显示第二轮没有新的 TextInput object/register/armed 处理，冷启动则恢复。此段实现与 main `2c043636` 一致（仅搬移 include 路径）。

基线设备对照已补齐：主进程 19182 的 LAB GL/War3 会话，01:21:40 经无 LAB notepad Want 发生图形策略切换和引擎重建；notepad 新窗口可用，点击剪贴板候选“原版重启中文”后正文仍空，随后 `baseline-ascii` 正常上屏。force-stop 后主进程 23192 冷启动 notepad，“原版冷启动中文”真正上屏。`baseline-ime-restart.log`、`baseline-ime-restart-chinese-result.jpeg`、`baseline-ime-restart-ascii-result.jpeg`、`baseline-ime-cold-chinese-result.jpeg` 保留。因此重启后中文失效已经有基线运行证据，不能宣称本轮整体 IME 门禁无缺陷通过。

修复 `edd6fc87` 已实现显式 display 生命周期：先停止并 join Wayland dispatch，再清理旧 TextInput resource/global、event source、FD、focus、armed 和 pending op；最后销毁 display 客户及 display。Register 分阶段创建、失败回滚，Shutdown 可重复调用。NAPI 的目标选择与入队使用一致的 state→queue 锁顺序，旧 resource 销毁时删除其排队输入；资源析构在锁外，防止回调重入死锁。

验证：真实固定版本 Wayland/协议/资源回调的主机测试 91 项通过，旧源码二次 global 注册探针按预期失败；全部 host/model/GLES/HUD/导航/CI 门禁通过。双 ABI 应用 Native/HAP 重建、包与 SDK 签名校验 PASS，未改 UI/ArkTS/guest 载荷。源审计分别证明 f05cb825 的 181 次机械搬移与 native-fixes.json 中逐提交、逐路径登记的后续语义修复，没有取消搬移断言。

手机主进程 18390 冷启动中文通过；产品 notepad→LAB GL→产品 notepad 连续两次引擎重建，日志均有 Shutdown/重新 Register，“引擎重启中文”真正上屏。第三次在键盘开启时重建也重新注册，但第一次 notepad 在出窗口前退出；一次同引擎新启动后，“再次重建中文”和桌面卡恢复后的“恢复后中文”均上屏。**I2 注册/提交路径定向通过；第三次首次程序启动失败保留在 I4，不算无缺陷重启成功。** 证据和边界见 DEVICE_RESULTS 第 18 项。不要重做已经完成的生命周期修复，也不据此宣称所有 IME/实体键盘场景通过。

## I3：测试方法与硬件限制

- UI 自动化的英文 inputText 是按键注入；中文 inputText 会先放入宿主剪贴板，需点输入法候选条才能验证 commit。不能把剪贴板候选可见当作 Wine 上屏。
- 用新的 Want 改变 LAB/profile 会触发既有引擎重启，不能把这样被中断的 GL 会话算成十分钟长跑。第二个测试程序若需要共存，使用同一 backend 和相同 LAB 参数，并核对进程/窗口未重建。
- 目前实体手柄/马达未被检测到；硬件方向、断连释放及震动未实测。host 模型测试或手机振动不替代实体手柄验收。
- x86_64 HAP 已编译并做包检查，但没有 x86_64 Harmony 设备运行证据。

## I4：引擎启动、首帧与退出状态

当前运行时 `252176de` 已修复回收线程/早期登记，主进程 52298 的冷 Modern→VirGL→Modern→VirGL→Modern 五次首帧全部可见，每次 Explorer attempt=1/3；中文提交、root #13 卡片恢复和恢复后继续中文也通过。首次冷启即由 TID 52800 记录退出，已不再被备用 handler 覆盖。见 4.3 KB `reaper-device-summary.json` 与 DEVICE_RESULTS 21。**I4 的历史 metadata-only/零 toplevel/guest fault 仍待更广验证，不能由一次短序列断言根因相同或全部解决。**

历史 `c5e00c9a` 只修复明确退出状态的保存和当前 wineboot 尝试的失败判定。临时诊断包（dc + 未提交的 `startup-i4-diagnostic.patch`，已撤掉）抓到 PID 20058 的 wineboot.exe 以 code=1 退出，注册表却保存为 unknown，随后仍启动 Explorer 20112 并一直等待。最小证据 `startup-i4-failed-boot-minimal.json`。`56345d67` 增加真实 fork/waitpid 的 31 项测试及失败检查，但第一次实机重建暴露 SIGKILL→137 的误判；这个中间 HAP 不接受。成功的旧控制样本中 wineboot 18901/19009 同样被 SIGKILL 清理，Wine `server/process.c` 的退出清理定时器也说明不能从这个信号推导 Windows 错误码。`c5e00c9a` 保持 signal 来源、exitCode=-1，只对明确非零 WIFEXITED 状态判失败。不要恢复那个信号映射，也不要把 hilog 的 CRASH 标签当成根因。

纠正后的主机全套、双 ABI 编译、包校验和 SDK 签名通过。手机主进程 36343：Modern 冷启和第一次切 VirGL 均显示记事本；第一次重建明确记录 wineboot signal/exit=-1 并正常 READY。第二次切回 Modern 再现 shell #7 只有元数据、0 mapped surfaces/renderers，180 秒仍未就绪，因此不是全启动 PASS。此时 guest Explorer PID 39283，launcher 39271；有些其他进程的 code=0 已正确保存为 waitpid。新的成功/失败最小记录为 `startup-exit-signal-device-summary.json`，完整动作见 DEVICE_RESULTS 20。明确 exit=1 的拒绝分支目前由真实 host 进程测试覆盖，这个 c5 手机序列没有重新产生同样的 exit=1，不能以先前被否决的 137 样本冒充其设备验证。

已完成的回收包不要重做：`252176de` 删除 NAPI 重装和手机吞状态的 handler，信号路径只写 nonblocking self-pipe；普通线程只 waitpid 本模块登记的 fork child，其他模块保留自己的 wait 状态。握手前登记、迟到 broker/app label、PID 新生命周期、EOF 与退出分离、实际 fd 号码复用/停止空闲 reader 均有真实进程检查。132 checks 与全套主机门禁通过，旧源码加 API 适配探针在早期退出发布处失败。测试会在 gProcMutex 持有时发送真实 SIGCHLD，并检查通知线程可重入注册表；SDK NAPI 仍为 stub，不声称实际 NCP 或整个 NAPI callback 生命周期都无竞争。手机 void Main 丢返回码和 guest loader 错误仍是独立缺口；后续遇到失败按 PID/时间单独归因，不新增第二套进程注册表或修改超时/重试。

原 dc 主进程 62830 又复现一次 metadata-only 卡住，至少 294 秒未 ready，记录 `startup-i4-third-recreation.json`。HDC 的 processdump 被设备拒绝，未提权或绕过；线程只能看到 sleeping/WCHAN 屏蔽。旧 `startup-i4-second-recreation.json` 的共享 stderr 文件没有对应 62170 的 PID 标记，不能用其中旧进程的异常推导 62170 的故障。WL-STAT 的 surfaces 是已映射的 toplevel surfaces，不是协议 wl_surface 对象总数。

新增 dc287c9f 证据在 `startup-i4-second-recreation.json`（约 4 KB）：PID 53844 第二次同进程引擎重建从 Modern 切 wined3d/x64 smoke，旧 renderer 已 Shutdown，新 shell #7 元数据出现，但 surfaces=0/renderers=0，180 秒仍未 ready。没有执行到新 renderer 的后台等待；这排除了将其直接当作该线程正在等待，但不能排除此前共享状态影响。原 stderr/hilog/截图完整保留，不能与下述 B3/edd 样本未经验证就认定同根因。失败收证后 force-stop 恢复设备，另开 Modern D3D11 控制成功；未重试此 x64 GL 样本，也未计它通过。

`candidate-b3` 在 180 秒 readiness 门禁失败，451 秒截图仍为“启动桌面”。主进程与 Explorer 存活，Wayland 统计始终为零 surface/toplevel；Wine stderr 记录 Explorer guest 在 dlopen 的 SIGSEGV 及 Wine 异常处理恢复信息。现有证据不能认定异常是直接根因，也不能认定这是本次合并引入或基线已有。

已保留 `candidate-b3-failed.json`、startup/stderr 日志和现场截图；未重置 prefix、未更改参数。一次单独编号的 force-stop 冷启动 `candidate-b3-retry1` 正常创建桌面并完成测量。后者不覆盖首次失败，不能用它计算“全部启动成功”。产品启动代码保留原有十分钟 root watchdog，性能工具的 180 秒 readiness 是更短的测试门禁。

定向排查：先比较同包重复冷启动与基线的失败率、Explorer/loader 与 wineserver 生命周期、安装替换后的进程清退；保留 HAP 身份和失败样本。不要扩大重试次数、删 watchdog、跳过桌面根要求或重置用户数据来获得通过。

新增独立样本（不能直接判定与 B3 同根因）：edd6fc87 主进程 18390 在 02:44:31 第三次同进程引擎重建后，root #11 与 taskbar #12 已提交，TextInput 也重新注册；02:44:40 启动 notepad PID 26937，43.729 创建其 text-input resource，46.739 资源销毁，46.742 broker 报退出 code=0，未创建 notepad toplevel，界面只见蓝色桌面。stderr 同段有 Explorer PID 26864 的 mmap 与 notepad PID 26937 的 dlopen SIGSEGV/异常恢复；原始完整日志留在 `ime-lifecycle-armed-wine-stderr.log`，不能只凭该信号就断言根因。一次独立编号的同引擎重试 PID 29451 于 02:47:22 成功，未 force-stop/重建，原失败保留。调查入口为 `ime-lifecycle-armed-current.log`、`ime-lifecycle-same-engine-retry1.log`；需对照实际 loader 返回值、exit 状态传播与新旧进程清理，勿将“没有新窗口”误判为 IME commit 丢失。

已完成的有界排查：`proc/wine_child.cpp` 记录 Box64 的返回值但没有从 void 入口传播；`phone_adapter/phone_process.cpp::StartChildMain` 在入口返回后统一 `_exit(0)`。`proc/wine_process.cpp::sigchld_handler` 记录 wait status 后调用不带状态的 `RemoveProcess(pid)`，因此 registry 留下 -1/unknown。这解释诊断信息为何不足，不证明本次一定经过某个失败 return，也未修复启动根因。成功重试同样记录 libbsd/libvulkan 缺库告警，不能据此认定缺库为区别。旧 B3 的 PID 58408 与新样本报告相同 Native PC `0x5be06d5524`，但没有证实加载基址或具体函数；读取当前 guest `/proc/.../maps` 被系统拒绝，未绕过访问控制。

最小接手材料已经提炼为忽略目录 `startup-i4-minimal.json`（约 7 KB，21 条 parent 事件、9 条 TextInput 事件、2 条 guest signal）；原始失败日志未删除。下一步若需要诊断构建，应只增加实际 loader/guest 退出信息与有限的模块/SEH 日志，预先固定少量失败/成功对照动作。不要通过改变 prefix、Box64 配置、UI、子模块 pins 或丢弃失败结果来制造成功样本。

## I5：War3 共存全屏的右侧拖边与首次触摸高亮

候选在 x86 GL 窗口共存时启动 War3 1.24.4 OpenGL，全屏右侧延伸出拖边，退出后 GL 标题栏右端裁切，重新最大化/还原后恢复。部分菜单第一次 `uitest click` 只高亮，第二次才进入。候选仍能进入 Booty Bay、框选五个单位、发出巡逻命令并观察单位移动，F10/结束/退出有效。

冻结基线在相同 LAB、backend、x86 GL 共存与游戏参数下复现同样的右侧拖边、退出后 GL 边框裁切，以及“单人模式”第一次触摸高亮、第二次进入；证据为 `baseline-war3-ready`、`single-tap`、`second-tap`、`single-menu`、`exit` 的截图和 `baseline-war3-graphics.log`。这支持“已存在的表现”，不证明所有游戏几何与输入均正确。真实物理鼠标和无共存场景未以此替代验收。

后续分开排查 GPU/SHM 组合画面的边界与触摸按下/移动/释放次序；保留前景/根窗口几何和输入日志，不用裁剪截图或取消全屏来掩盖问题。

## I6：产品路径的诊断 x86 程序运行卡片未恢复

候选主进程 27821 以无 LAB Want 启动 `C:/smoke/x86/winehua_graphics_smoke.exe`。root #1 已建立并出图，随后 Back→运行中→“图形测试”卡片停留在启动器，没有引擎重建。01:25:24.969 日志先出现 `reconcile removes session=desktop pid=0 state=running`，01:25:24.997 才进入 READY。Native explorer 登记为 `@engine/explorer`，ArkTS `getWineSession('desktop')` 不能靠这个键补回已丢的 root 会话；内置图形测试的相对 executable 与诊断 x86 绝对路径也不是同一精确路径，点击走新启动等待 root 的路径。

01:30 同 backend、无 LAB 的 notepad Want 能通过新窗口 auto-promote 恢复桌面，原 GL session/key 未重建。这个诊断恢复不是卡片恢复通过。外部直接启动未导出的 DesktopAbility 被系统拒绝，未改 exported、权限或其他访问控制。

基线控制：主进程 41748、相同产品 x86 GL Want，01:42–01:43 的 `baseline-root-card-*` 样本保留了 Wine 桌面与图形测试两个卡片，未出现 root 的提前删除；点击图形测试虽按内置相对路径另启一个程序，但能返回桌面。因此不能把候选的卡住直接归为基线已有缺陷。

修复提交 `beb00711` 只调整 AppSessionService 的 reconcile：PREPARING 时查询 Native 当前 desktop root；仅当它是正 ID 且与现有 pid=0 会话的 toplevelId 精确匹配时保留。停止、切换、错误、root 消失/更换、查询失败及已退出游戏进程仍被清理。没有全局跳过 reconcile、修改 UI/exported/协议或导入另一套 process registry。

新增测试运行实际转译后的服务源码，按手机日志的 root callback→reconcile→READY 顺序执行；旧实现失败、新实现通过，并覆盖失效 root 与退出进程。`make test-model` 通过。新 HAP 的全部 121 个 Native 库和嵌套 Wine 数据与 d28d1a02 逐字节相同，只有 ArkTS 内容变化；包/签名校验通过。手机循环恢复结果以 DEVICE_RESULTS 第 15 项为准，不能删除旧失败样本。

## I7：Legacy DXVK 的 D3D9 初始化失败（原版已复现）

候选 beb00711、主进程 55651，以产品 `dxvk_legacy` 和 x64 switch cube 的 `--d3d11` 冷启动，D3D11 出图；先触摸窗口取得焦点，再按 F1，标题出现 `init D3D9 failed: HRESULT 0x8876086a`，窗口白屏。标题 frame 仍快速增长，因此仅凭 frame/“regress 0”不能判定真实绘制通过。F2 切回 D3D11 后恢复正常图像；最大化和经桌面卡返回也可用。

冻结原版同参数、主进程 63216，在可见 D3D11 后触摸窗口并按 F1，复现相同 HRESULT 和白屏。证据 `root-fix-legacy-d3d9-failure.log`、`root-fix-legacy-d3d9-focused.jpeg`、`baseline-legacy-d3d9.log/.jpeg`。这是两版均失败的 Legacy/D3D9 覆盖，不能写成 Legacy 全面通过，也不说明新包的 Modern 结果。

后续定向检查 cube 的 D3D9 CreateDevice 参数与 Legacy 的设备能力/格式/交换链创建日志；对照 Modern。先分辨探针兼容性、驱动能力与 Wine/DXVK 行为，再决定是否需要独立子模块修复。保持当前 pins，不通过关闭默认 batching、忽略 HRESULT 或只计空循环帧来获取 PASS。
