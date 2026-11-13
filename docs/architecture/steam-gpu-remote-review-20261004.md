# Steam GPU 远端修复复核与调试报告

日期：2026-10-04（Asia/Shanghai）。分支：feature/main_proton。

仓库证据入口：[README](evidence/steam-gpu-remote-review-20261004/README.md)。本提交收录完整报告、脱敏关键时序、帧统计、构建/回归记录和可移植复现脚本；完整设备日志、截图、安装包及材料 ZIP 保存在本地采集目录。

评审基线：960bf939835e53494118a6be592bf68bd5f638de。
复核版本：2add84d3298924279eed5c38ccde3a61663855c2。
新增实现提交：036fda4f；新增基线固定提交：2add84d3。

## 结论

四项修复的目标路径均已通过生产代码回归测试，旧提交的三项宿主缺陷也按预期复现。Wine 的 acquire 修复已重新编译进入 HAP，签名验证和覆盖安装成功。

仍存在一个应优先处理的窗口归属问题：同进程唯一候选规则不校验 producer 与 owner 尺寸，1×1 producer 仍可绑定 101×398 窗口。本报告用完整生产 ResolvePresentBinding 函数复现了这一点。删除跨进程 surfaceId 猜测解决了 presenter 路由中的一类误投，但尚不能把整个 owner 解析链路称为严格身份绑定。

真机状态：解锁后新包已成功启动，完成约 10 分钟的被动采集。Steam 主面、菜单相关辅助表面、游戏和 Wallpaper UI 均有真实 GPU 消费记录，最多同时 3 个 consumer。CEF 浏览器存活 572.642 秒，最后由应用 killAll 清理；本会话没有 CEF SIGSEGV 退出记录。

但整体验收尚未通过：用户确认《千恋万花》被 Steam 遮住、无法显示后手动关闭。第二次启动时同样存在游戏持续消费 GPU 帧、窗口登记可见且全屏，但中心输入探针一直命中 Steam 的现象。另有 GameOverlayUI 反复 SIGSEGV 退出，需单独调查。四项目标回归通过不能替代这些真机问题的修复。

建议继续按“窗口归属与合成正确性 → 生命周期稳定性 → 准确统计 → 性能 A/B”的顺序推进，保持 1200×800。

## 四项修复逐项复核

| 项目 | 实现与验证 | 判断及边界 |
|---|---|---|
| 纯 GPU 子面、父窗口尚无 SHM | SnapshotGpuDesktopScene 验证真实 child→parent 协议边，恢复无 SHM 父窗口的既有层序位置；无父层时只允许满足既有 TopAnchored 规则的真实子面。生产函数测试覆盖无帧父窗口、协议身份错误、隐藏、最小化和 fullscreen。旧版本 scene 断言失败，新版通过。 | 目标缺陷已修复。完整 Wayland place_above/place_below 和多级 popup 尚需专项真机覆盖。 |
| producer 接管提前退役 | pending 转换从几何查询移入 NoteLayerConsumed；NativeImage 成功更新后核验 producer、owner、claim 与 bindingGeneration，再退役旧 producer。保留 retired 墓碑阻止旧 producer 被后备规则复活。旧版本 binding 断言失败，新版通过。 | 查询不会再被当成第一帧消费；测试还覆盖断连、错误代次、owner 丢失和代次重用。真机没有 BIND-TAKEOVER/BIND-RETIRE 记录，本轮未覆盖实际接管时序。 |
| 跨进程同 surfaceId 路由 | PresentVenus 只查完整 (clientPid, surfaceId)，缺目标立即返回 -EAGAIN；不再遍历其它 PID 的同号目标。绑定代次由单调递增 token 校验。旧版本 venus 断言失败，新版通过。 | 已解决 presenter 的同号碰撞；未消除 ZcBridge 的同进程唯一候选和跨进程几何 bootstrap。新 token 属于 binding 代次，producerGeneration/windowGeneration 占位字段仍为 0。 |
| acquire 共用 command buffer | 新增 0022 补丁，移除 acquire_pool/acquire_command，改为 commandBufferCount=0 的 signal-only submit，保留 semaphore/fence 信号。生产 acquire 测试覆盖连续三个 image、semaphore-only/fence-only/both、无额外分配、timeout 和 QueueSubmit 失败回滚。旧路径被相应断言拒绝，新路径通过。 | 修复方式是消除共用 command buffer，而不是给它增加完成状态等待。真机确认了 Venus/Vulkan 呈现及三 image 宿主 swapchain；没有 Wine acquire 事件或 fence 完成明细，不能据此宣称 acquire 并发/长时压力验证通过。 |

相关位置：

- entry/src/main/cpp/compositor/toplevel/desktop_compositor.cpp:348，protocolChild 与 frameless parent 可见性。
- entry/src/main/cpp/compositor/frame/zc_bridge.cpp:554，NoteLayerConsumed；:859，bindingGeneration 分配。
- entry/src/main/cpp/graphics/egl_renderer.cpp:146、:383、:398，代次核验与消费通知。
- entry/src/main/cpp/graphics/virgl_surface_presenter.cpp:663，完整 presenter key。
- patches/wine/0022-private-vulkan-signal-only-acquire.patch；应用后的 Wine vulkan.c:2927。

## 剩余问题

### [P1，真机待修] 游戏仍被 Steam 遮住，GPU 消费正常不等于上屏正常

用户原话：“游戏启动到后台了显示不出来了，所以我手动关了”“被steam遮住了”。13:56:45 的首个 SenrenBanka.exe 退出应按用户手动关闭处理，不作为 GPU 开启后自行闪退的证据；退出过程中记录的 signal=11 仍须保留。

第二次游戏进程 37100 的证据更完整：14:00:48.286 收到 toplevel #10 的 fullscreen 请求；14:00:49.506 attach producer (37100,58)，源和显示几何均 1200×800；14:00:49.619 消费第一帧，14:01:41.419 达到第 1200 帧，failures=0。14:00:52.577–14:01:39.147 的 11 次窗口快照中，游戏 visible=1；同期登记 fullscreen=1，但中心输入探针的 inputTarget 全为 Steam #4。实际点击日志也落在 #4。

因此本轮可以确认“游戏进程和消费链路仍活着，但显示/输入窗口切换有问题”。不能仅凭父窗口的 binding=none/contentSource=SHM 推断游戏 GPU 绑定丢失：该字段描述父 surface=27，真正的 GPU producer 是它的 child surface=58。

优先检查的位置：desktop_compositor.cpp:642 的 PickFullscreenLayerLocked，:688 的 ShouldSkipFullscreenCascade，:482 的 GPU owner 过滤；wayland_server.cpp:213 的用户 raise/全屏优先级更新；toplevel_manager.h:178 的初始优先级。渲染和输入均依赖同一全屏选取规则，需同时核对全部候选是否入 z-order、各 FsPriority、minimized/background，以及最终场景层。

现有日志没有完整的全屏候选优先级和实际绘制层列表，因此尚未锁定唯一代码根因，也未证明是 036fda4f 新引入。下一步应补“窗口可见→入候选→被选中→GPU 层进入场景→实际绘制”的诊断，围绕 Steam 与游戏两窗口复现和修复；仅强制所有 GPU 层置顶会破坏菜单及其它窗口遮挡语义。

### [P1] 同进程唯一候选仍可把辅助 producer 绑定到无关窗口

位置：entry/src/main/cpp/compositor/frame/zc_bridge.cpp:731–751；另一后备入口在 :356。

ResolvePresentBinding 的 L1 分支只要求同 PID、可见且未被占用的候选数量等于 1。considerWindow 已取出尺寸，但 L1 没有使用尺寸验证，也没有真实 producer→owner 协议关系。即使传入 1×1，仍会选择 101×398 的唯一窗口并返回带有新 generation 的有效绑定。代次校验能阻止旧绑定被错用，不能纠正首次选错的 owner。

证据：weak_owner_repro.py 逐字提取当前完整 ResolvePresentBinding 函数，只有平台资源为替身。输入为同 PID 的唯一可见 101×398 owner、独立 1×1 role-less producer；结果成功绑定 owner，输出：

    REPRODUCED: production ResolvePresentBinding accepts 1x1 producer for a 101x398 same-process sole owner.

这证明仍有错误归属入口；本轮未据此声称看到了放大伪影或测得了 FPS 损失。上一版真机的辅助表面误绑定可作为历史线索，但不能混入新会话。

建议：有真实角色或明确 owner 身份时优先采用；弱候选必须与合法显示几何一致，无法证明归属时保留未绑定状态。不要仅以“同 PID 且只剩一个窗口”建立显示绑定。两处入口应一起收紧，并补辅助表面、主窗口/菜单并存和 owner 重建回归。

### [P2，真机] Overlay/helper 仍有反复异常退出

仅统计本次宿主 30320 会话，GameOverlayUI.exe 共 13 条 signal/exit=11 完成记录；首轮游戏启动期间，13:53:50–13:53:59 有 6 个不同 PID 约每 1–2 秒退出，第二轮启动再次出现同类循环。最终 14:01:45 的 exit=9 属于 killAll，不混入 SIGSEGV 计数。

wallpaperui.exe 有 7 条 exit=11、1 条 exit=0；其中多条集中在 13:58:23–13:58:25 的窗口关闭/辅助进程清理阶段，用户没有确认 Wallpaper 自行闪退，不能将 7 条都解释为独立运行中崩溃。游戏手动关闭也有 exit=11，说明应把“发生异常信号”和“用户看到自行闪退”分开记录。另有 shanhelvtan.exe、UnityCrashHandler64 等退出记录，其退出意图未确认。

本轮没有能定位上述进程的直接异常栈。Wine stderr 中“OHOS sigchain claimed SIGSEGV/SIGBUS/SIGILL/SIGTRAP”是信号处理器注册日志，不是崩溃栈。建议用单独会话围绕 Overlay 自动重启补栈；当前用户测试过程没有关闭 Overlay 或更改其设置。

### [P2] GL-PERF 尚未计入混合场景的实际 SHM 上传

位置：entry/src/main/cpp/graphics/egl_renderer.cpp:674、:678、:1347。

DrawZeroCopyScene 中逐层 glTexImage2D/glTexSubImage2D 没有累加到 uploadUs/upload_bytes；perf.Add 仍沿用 cpuFrame 与整屏 px.size() 口径。混合场景切换时可能遗漏真实上传或统计上一份整屏缓冲大小。因此本轮不用 upload 字段评价总传输成本。

建议：按实际上传调用累加字节与耗时，区分静态纹理复用与重新上传，再用无 profiler、相同交互的场景比较帧率与长尾停顿。

### [P2] Wine 构建缓存未核验源码路径，可能成功返回却漏编补丁

首次调用 build_wine.sh 时，0022 已写入当前 source，但缓存 Makefile 的 srcdir 仍为 /tmp/proton-tablet-src/thirdparty/wine-valve。旧目录的依赖检查没有看到本次修改，脚本返回成功，vulkan.o/win32u.so 仍为 10 月 3 日产物。该结果未用于安装。

处理：备份生成的 Makefile/config.status，重新配置当前 /data/src/winehua/thirdparty/wine-valve，并重新执行 Wine 构建。vulkan.o 与 win32u.so 的新修改时间分别为 2026-10-04 13:13:23.430 和 13:13:23.923（+0800）。源码与重编 object 均不再包含 acquire_command/acquire_pool 字段；HAP 中 Win32u 与新 stripped 输出逐字相同。

此外首次 assemble 继承了 GUEST_ARCH=x86_64，因缺少对应 guest Vulkan 产物失败。实际存在且前版使用的是 aarch64 guest Vulkan；显式设置 WINE_ARCH=aarch64、NATIVE_ARCH=arm64-v8a、GUEST_ARCH=aarch64 后组装通过。未伪造 x86_64 清单或用不同架构的库补位。

建议 build_wine.sh 后续在复用缓存前校验 srcdir 与 WINE_SRC 的实际路径；源目录不一致时重新配置，或明确拒绝。当前评审保留远端源码不变，缓存修复只发生在构建产物中。

## 真机采集与性能判断

本次运行版本就是本报告的 HAP；宿主 PID=30320、图形子进程=30865、CEF=31385。约 13:51:43 启动，13:52:12.949 创建 CEF，14:01:45 走应用 killAll/DestroyAllToplevels/ResetSessionState 清理。采集末尾平板已回桌面，未再启动、注入输入或干扰用户测试。

用户使用手柄并自行切换/启动程序，全部采集为只读观察。Steam 分辨率保持 1200×800；原 1.bat 保留。此次使用 GPU 开启的直接启动参数（移除 -cef-disable-gpu，增加 -bigpicture），不是原禁用 GPU 的 1.bat；同时启用 frameDiagnostics 和 trace-present-image，故不用于严格性能 A/B。

| 时间/材料 | 观察 | 可得出的结论 |
|---|---|---|
| resumed-loading / resumed-loaded | Steam GPU 主面 (31385,27) 于 13:52:39.546 首次消费；辅助面 (31385,64) 为 311×78 | Steam GPU 内容可启动，真实消费已发生 |
| resumed-game，截图约 13:55:05 | 《千恋万花》实际游戏场景、角色和文字可见 | 至少一次成功上屏；不能否定随后用户报告的遮挡问题 |
| resumed-followup，约 13:58 | Wallpaper UI 叠在 Steam 大屏之上，缩略图、工具栏可见；状态栏显示 Steam is unavailable | 多窗口可绘制；IPC/Steam 可用性提示仍存在，不能声称 Wallpaper 全功能正常 |
| resumed-observe 的 14:00:48–14:01:41 日志 | 第二次游戏持续消费，但窗口输入探针命中 Steam；出现 233×801 Steam 辅助面 | 复现线索指向窗口切换/层级过滤，菜单相关 surface 有消费但完整显示未验收 |

日志确认 `[VENUS-PRESENT][NCP] swapchain ready`（images=3、transfer=copy、present_mode=fifo）与 virgl-host 的 `WineHuaPresentImage: layer=host event=present`，包括真实 1200×800 帧。本次不能简单称为只走 OpenGL。不同程序也出现 VIRGL-ZC 消费；该名称和 VENUS-ORDER 名称本身不足以判定每个 producer 的后端，需结合具体 callback/key。

本会话共 9 条 attach、9 条第一帧消费记录，日志最大同时 consumer=3。宿主 290 个 FRAME-LOOP 窗口中 failed=0，169 个 GL-PERF 样本中 failed_swaps=0；明确匹配生产日志格式后，没有发现 NativeImage update failed 或 consumer re-attach 记录。**这些计数只覆盖消费/宿主交换阶段。**

上游 Venus callback 在 14:00:59.593 的最后一条累计统计为 count=12203、ok=11903、fail=300、result=-11；共有 25 条 target missing 警告，涉及 24 个不同 key。日志有采样，25 不是全部失败次数，300 也不是会话最终总数。初始化 target 尚未 attach 时返回 EAGAIN 是预期行为；Steam 辅助面后续重复出现缺目标，仍需要与具体菜单显示对齐。严格路由返回 EAGAIN 的安全行为已验证，不能为了减少这个计数恢复跨进程同号猜测。

两段性能数据应分开理解：

- 首帧后约 538.716 秒混合场景的宿主交换加权均值为 37.52 FPS，单窗口范围 0–64.5；含加载、静态画面、Wallpaper、游戏启动和关闭，不能作为 Steam 导航性能均值。
- 第二次游戏被遮挡附近，20 个约 2 秒窗口（结束时间 14:01:02.571–14:01:40.585，共 40.015 秒）的宿主交换均值为 **18.393 FPS**，范围 15.00–44.49，多数在 15–19。2400 次循环里 1664 次没有新帧而跳过，宿主工作占 6.801%，帧节拍等待占 92.843%；work 最大 9.296 ms，已记录的最长交换间隔为 720.205 ms，vsync error/timeout=0，记录到的锁等待最大为 0。

该低速段说明宿主渲染线程仍按约 60 Hz 检查，但大多数轮次没有可提交的新内容；不支持把这一段主要归因于分辨率或宿主每帧绘制超预算。工作/等待的统计范围只是宿主线程，不包含 Wine/FEX/CEF CPU、Venus 队列和 GPU 等待；被遮挡游戏也可触发消费和宿主交换，交换速率不等于用户看到的 Steam 唯一内容帧率。接下来应先修窗口可见性，再在无遮挡的同场景中量出 producer→consumer→实际可见绘制的间隔，判断是 CEF 产帧不足、FEX 调度还是呈现队列等待。

覆盖边界：没有实际 producer 接管日志；没有 Wine acquire/fence 完成事件；没有完整 no-SHM sibling place_above/place_below、深层 popup 或数小时长会话；截图序列不能证明全程无缺图/黑块。`/proc/31385/maps` 权限拒绝已保存，没有声称已读取其库映射。

## 后续处理顺序

1. 修 Steam/游戏前台切换，增加完整候选和实际绘制层诊断；保证显示、输入和窗口切换使用一致的可见结果。
2. 收紧两处弱 owner 选择，补辅助 surface、几何变化和 owner 重建用例，保留无法证明归属时的 EAGAIN。
3. 单独定位 GameOverlay 异常退出，区分运行中崩溃与关闭清理；不要混入性能采集。
4. 修复上传计数与构建缓存源码路径检查，再做相同 GPU 参数、相同菜单、同样 1200×800、无诊断 trace 的 A/B。此时再考虑遮挡 producer 降频、CEF/FEX 调度及呈现队列等待的性能优化。

## 验证记录

- make test 全部通过，退出码 0；包含新的 steam GPU contracts、frame-loop、CEF switches，以及已有 gamepad、binding lifecycle、multi-consumer、Wine 稳定性测试。
- steam_gpu_contract_test.py --baseline 使用固定旧提交 960bf939，venus/binding/scene 三组指定断言均按预期失败。不是把随机非零退出当成复现。
- weak_owner_repro.py 完整生产函数复现剩余弱绑定问题；平台替身不代表真实 GPU 或设备帧率。
- git diff --check 通过。
- 当前源码 Wine 构建、aarch64 组装、OHOS Native/HAP 构建、sign/verify-app 全部通过。
- 新包通过 hdc install -r 覆盖安装；未卸载、清数据或修改原 1.bat。
- 首次 aa start 被系统锁屏拦截，错误码 10106102；未绕过系统解锁要求。
- 解锁后 aa start 成功；该新包完成上文约 10 分钟设备采集，真实显示与消费证据保存，已记录用户反馈的遮挡问题。

全部测试仅运行到所需覆盖通过，不用重复采样扩大性能结论。运行中显示速率、CEF 的唯一内容帧率和游戏帧率须分开统计。

## 安装包身份

文件：VintagePomeloPro-proton26-remote-2add84d3-debug-signed.hap。

- 应用版本：1.4.5-proton.26-alpha（1004035）。
- 大小：358,145,113 字节。
- SHA256：ef4a8b7b1ee7c86a89a2079e1fa9cefa2148c312574063a4eef1ffb0ba29f5fa。
- 包内 Win32u SHA256：7d5bbca191a5bdcc13291eb63464bc4b188dda1d49927c2a24769866af77d20a。
- 当前 Wine Vulkan 源码 SHA256：9852bd55e304f5b670937cac6527833bb702f878657dfeb0790c05ddc3441ff1。
- Debug/native O0 诊断构建；文件名、commit 和 SHA256 用于区分同版本号构建。不能据此推导 release 性能。

## 产物和范围

完整本地采集目录为 `F:/VintagePomelo-Workspace/workspace_temp/gpu-remote-review-20261004/`，其中含安装包、package-identity.json、device-commands.json、报告、复现脚本、截图及脱敏设备日志。source-snapshot 保存本次远端修改的文件；remote-update.patch 为 960bf939..2add84d3 的精确差异。

构建日志：host-tests.log、contracts-baseline.log、weak-owner-repro.log、wine-build.log（第一次缓存误命中）、wine-build-current-source.log（实际新编译）、assemble.log（第一次架构失败）、assemble-current-source.log、hap-build.log、sign.log。失败与成功均保留，不能只看最后一条“完成”。

本地 SHA256SUMS.txt 为完整采集材料的哈希清单；仓库证据目录有独立的 SHA256SUMS.txt，只对应本次提交的文件。review-evidence.zip 包含报告、源码快照和必要验证材料，不重复打入 358 MB 安装包。包内的 SHA256SUMS.txt 只列实际打入 ZIP 的文件；目录外层清单另含当前 ZIP、HAP 和独立库文件的哈希，避免重复打包时把旧 ZIP 的哈希写入新包。

设备阅读入口：device-session-analysis.json（按时间段分析）、device-session-key-events.txt（关键时序）、resumed-observe/session-summary.json（完整当前 PID 会话计数）、resumed-observe/frame-loop-rows.json、resumed-observe/cef-lifecycle.txt。summarize_session.py 已将原 stableWindows 改名为 postFirstFrameWindows，并明确混合场景，避免将加载后所有窗口误称为稳定导航。

本轮完成同步、复核、构建和调试，当前提交仅保存报告与核心证据，未另作业务代码修改。已有脏子模块和本地备份保留；Wine 0022 通过构建脚本应用。签名输入仅在权限受限的临时目录使用，签名后已删除。设备命令采用目标别名，凭证日志行过滤，SteamID 与设备序列号脱敏；截图仅存本地。
