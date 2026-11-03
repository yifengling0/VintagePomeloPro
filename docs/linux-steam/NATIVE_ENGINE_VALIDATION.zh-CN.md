# 原生 Linux 引擎验证（更新至 2026-11-01）

本轮只操作普通平板；两代尖塔均通过 Steam 切换为默认 Linux Runtime 并完成原生 depot 安装，核对入口为 x86-64 ELF。Windows depot、原 OHOS Wine 或 GNU Proton 的启动结果不能替代以下原生验收。

| 游戏 | 原生 depot | 观测结果 | 尚未通过 |
| --- | --- | --- | --- |
| 星露谷 413150 | 413153 | 官方 FEX → guest shell → .NET/SDL/OpenGL，曾出现有动画的标题菜单 | 窗口尺寸、输入、正常退出及 GL 共享呈现验收 |
| 杀戮尖塔 646570 | 646573 | 官方 FEX → scout-on-soldier → Java 8/libGDX；补齐 guest PATH 后越过显示模式初始化，默认 JIT 与解释执行暴露不同后续故障；按用户要求暂停专项调查 | 菜单与可玩性；不能归为 DXVK 或 Steam 认证失败 |
| 杀戮尖塔二 2868840 | 2868843 | 官方 FEX → sniper → Godot/.NET 9.0.7，标准 HAP 自动交付修正后进入菜单和战斗，用户确认可玩 | 帧率优化、正常菜单退出与长期稳定性；已通过全屏显示和无诊断正常入口 |

## 2026-11-01：尖塔二已进入战斗，显示与性能继续验收

普通平板通过标准 HAP 自动交付运行时，从官方 ARM64 Linux Steam 启动 Linux depot 2868843（manifest 7881097836989620034），经 FEX、sniper、Godot/MegaDot、CoreCLR 9.0.7 进入菜单并实际战斗。用户确认“可以哦，能玩了”，本机截图也确认战斗画面。没有切换 Windows depot、Wine 或 Proton，没有修改游戏程序集或 FEX 源码。下方 10 月 31 日的退出 1、菜单未通过等记录是此前诊断历史，不能作为最新状态。

最终跨过启动阻断的两处自有适配器修正：

1. FEX 安装 guest SIGSYS handler 后，原来的 host seccomp 适配可能被它抢先消费。`FexObservedSignal` 在委托前，仅对真实 host 架构、`SYS_SECCOMP`、已列明可选调用处理 raw `-ENOSYS`，其他信号继续原处理链。真实 seccomp 回归先失败、修正后通过；单独该修复尚不能进入菜单。
2. 真实故障帧位于 `steamclient.so` ELF RVA `0x211e163`（文件偏移 `0x211d163`）。Steam 查询 self robust-list 被 OHOS 拒绝，随后断言写 0，Sentry 将其包装为退出 1。新增 x86 guest preload 仅在 self/current tid 查询被 ENOSYS/EPERM/EACCES 拒绝时，返回 guest glibc 实际注册的 head 和真实长度。通过真实临时 robust mutex 链发现当前 libc 布局，后续线程验证该布局，显式成功注册保持原值。没有空链伪造、owner-death 改写或认证绕过。真实内核对照、多线程持锁查询、坏指针、显式注册、owner-death 和 syscall 六参数合同通过。

此前按实际 memfd 元数据拒绝能力选择的 `DOTNET_EnableWriteXorExecute=0` 仍由 native-FEX 子启动器自动处理，保留用户显式选择，不再需要手动填写此参数。首个成功游戏实例仍带有可回退的退出诊断和 `--verbose`；随后已恢复原始空 LaunchOptions，关闭退出／模块诊断，经正常 Steam 入口再次进入菜单和战斗。POSIX、FEX 启动器、native Vulkan 和 guest IPC 的设备 SHA-256 均与 HAP rawfile 一致。

图形链路是 Vulkan 1.2.309、Forward+、Venus/Maleoon 910；swapchain 1920×1080，三图像，`pixel_readbacks=0`。宿主合成统计也为零 CPU 图像回读与上传。战斗约 18～24 FPS。一次 8 秒有界采样中，每 2 秒约 1.7～1.8 秒在等待新源帧，宿主实际提交与绘制 CPU 累计仅几十毫秒；这只能说明该采样下宿主 CPU 合成不是主要耗时，尚不能区分游戏转译、GPU 负载和上游同步。

已发现专用 fullscreen Wayland host 的 1200×800 configure 反向调整游戏 X11 geometry，而 Vulkan swapchain 保持 1920×1080，出现左上小画面和输入偏移。候选改为只 ack configure，保留游戏自身 drawable 尺寸，由宿主 fit 图像。真实 Xvfb/Wayland 回归用 800×600 host / 320×240 game 证明旧代码会改错 geometry，修复后 geometry 保持；键、鼠标、滚轮、关闭及两套 WSI 合同通过。候选 HAP 已标准安装验收：图像居中并铺满可用宽度；16:9 内容在 16:10 输出的上下留边保持比例。游戏自行生成的 1200×800 swapchain 与 X11 geometry 一致，用户继续进入战斗并出牌；实际 XTEST 点击也可打开暂停／设置。菜单约 39 FPS、战斗约 15～20 FPS，不能称全屏修复已提升性能。此修正没有改 Wine WSI、全局分辨率或游戏画质。

## 通用可选系统调用合同

OHOS 对部分未提供的 NUMA API 发送 seccomp SIGSYS，而运行库预期收到 Linux 的“不支持”返回。适配器只在 native-FEX 作用域、真实 SYS_SECCOMP 且 host AUDIT_ARCH 匹配时，对显式允许的五个 NUMA API 返回 raw `-ENOSYS`；原有两个可选 file-handle API 保持相同合同。保留 PC、其他寄存器及用户缓冲区，不伪造成功，不吞掉其他 SIGSYS。

主机测试通过真实 seccomp TRAP 和内联 syscall 验证五个调用、输出哨兵及继续执行；这验证适配合同，不替代 ARM64 FEX 设备测试。设备已出现 `STEAMOHOS_FEX_OPTIONAL sandbox_api=ENOSYS`，原来的 syscall236 直接退出不再阻断尖塔二。

## FEX guest JIT 共享映射

尖塔二随后在 `memfd:doublemapper` 的 `---s` 页上请求 RX，被宿主返回 EACCES，随后同页 SEGV_ACCERR。已确认它是 .NET guest JIT backing，而非 ARM64 JIT。FEX GuestMprotect 在 native-FEX 作用域内、遇到 EACCES/EPERM 且请求 EXEC 时重试去掉 host EXEC，guest VMA 仍跟踪原始执行属性；不改 ARM64 JIT 分配器，也不忽略其他错误。真实 noexec /dev/shm 的主机回归通过。新 HAP 经标准入口自动更新后，真机多次出现 `STEAMOHOS_FEX_GUEST_PROTECTION ... retry_ok=1`，越过原崩溃点。临时 `DOTNET_EnableWriteXorExecute=0` 仅用于后续对照，不是已验收修复；权限重试成功也不代表 JIT 执行正确。

## GPU toplevel 首帧绑定

随后首帧遇到 `VENUS-PRESENT target missing`，8 次有界重试后返回 EAGAIN，被既有 WSI 转成 DEVICE_LOST。源码确认显式 xdg-toplevel 的 GetLayerInfo 使用要求 SHM HasFrame 的可见性判断，纯 GPU 窗口因此不能建立自己的首帧目标。已将该目标查询改为同 subsurface 路径一致的真实协议身份及隐藏/最小化检查；查询不标记 producer 活跃，不制造 SHM 或首帧。只有 NativeImage 消费成功才激活内容。新增生产函数回归覆盖无 SHM 首帧、隐藏、最小化、错误 renderer 和销毁；10 个 owner 模式、ZC 生命周期、20 个 scene/input 模式通过。新包设备日志已建立 Vulkan swapchain，原有首帧目标门槛已越过；尖塔二主菜单画面尚未通过，不能据此宣称可玩。

## 原生游戏共性与尖塔二优先调查

已确认的共性位于 GNU/FEX/沙箱适配层：宿主 PATH 不能直接供 guest 子程序使用，可选 Linux API 的 seccomp SIGSYS 需明确返回不支持，guest 的执行权限不能等同于 ARM64 host 执行权限。这些是通用运行环境合同；目前没有证据证明两代尖塔的最终故障同源。

guest PATH 修复仅作用于 FEX 子进程：保留游戏自带搜索顺序，移除已知 ARM 宿主路径，去重并补齐 `/usr/local/bin:/usr/bin:/bin`；同时更新继承 envp 与 FEX Config.Env。实际 LWJGL2 的 Java 子进程已能找到 xrandr，原显示模式空数组停点不再阻断。未改 ARM Steam 或独立 OHOS Wine 的 PATH。native-FEX 及正确 seccomp/架构限定下，`get_robust_list` 暂返回真实 raw `-ENOSYS`，仅消除直接 SIGSYS；一代解释执行随后暴露 SteamController 对真实 robust-list 的要求，故这不是该接口的完整实现。生产函数主机回归 32 项与信号边界回归通过，设备标准入口交付的 POSIX/FEX launcher 校验与 HAP 一致。

按用户最新要求，暂停一代专项实验，先调查尖塔二官方 Linux depot 2868843（manifest 7881097836989620034），入口为 x86-64 ELF。以下均使用同一候选包，没有改游戏文件或套用 Windows depot：

| 临时对照 | 实际结果 | 解释限制 |
| --- | --- | --- |
| 默认 .NET W^X、默认 FEX mtrack，`--verbose` | Vulkan、FMOD、光标初始化后到 `.NET: hostfxr initialized`，没有后续场景加载 | 停点在 .NET 阶段，不足以断定具体双映射或信号原因 |
| `DOTNET_EnableWriteXorExecute=0`、默认 mtrack | 加载 C#、卡牌、图集、中文字体和音频 bank；应用全屏设置并收到 Steam 用户／全局统计后，以退出码 1、signal 0 结束 | 同一路径已重复，证明与内存模式相关，但未取得菜单或明确异常栈 |
| 默认 W^X、`FEX_SMCCHECKS=full` | 约六分钟的有界观察仍在 hostfxr 初始化附近，未出现资源加载；随后由诊断主动停止 | full 模式本身明显变慢；未证明根治，不能作为产品默认，亦不能完全排除代码失效问题 |
| W^X 关闭，加 `FEX_SILENTLOG=0 FEX_OUTPUTLOG=stderr` | 再次资源加载后退出 1，未捕获明确 FEX fatal | Steam 保留运行；没有证据把失败归为认证或 DXVK |
| W^X 关闭，Godot `--headless` | 无图形模式仍能加载资源、成功初始化 Steamworks 并收到统计，随后退出 1、signal 0 | 后续退出不依赖 Vulkan 呈现或全屏切换；不代表图形链路已经完整验收 |
| W^X 关闭，`DOTNET_TieredCompilation=0` | 仍在资源／统计初始化后退出 1 | 未形成修复；不保留此性能开关 |
| W^X 关闭，临时 .NET StartupHook 异常监听 | 再次退出 1，但未出现监听器初始化标记，事件数为 0 | 该观测器未生效，不能据此排除托管异常；已移除 6 KiB 诊断 DLL |

全屏设置附近同时出现 NativeBuffer 清理错误，但时间接近不是因果证据。SIGBUS/BUS_ADRALN 多数为 FEX 恢复的未对齐原子事件，不得直接认作最终崩溃。菜单、输入、持续运行和正常退出仍未通过。所有对照参数均可恢复，未设 AppID 产品特判。


本轮只读检查了安装版 managed 启动与异常处理入口：未发现启动代码直接调用 `Environment.Exit(1)`，普通写入失败路径有异常报告；这不排除其他退出入口，也不能替代异常栈。设备上的零字节 `profile.save.tmp` 仅是后续文件写入采样线索，尚不足以认定权限或存档为根因。检查未修改游戏程序；本地程序集和反编译临时副本已清理，仅保留大小、SHA-256 与结论。

结束时已恢复两代的原始 LaunchOptions、原始 Steam 启动配置，撤销 full SMC、W^X、tiering、CEF 遮挡诊断与 StartupHook 参数，移除诊断 DLL。保留当前与一个回退 HAP、有界压缩证据；没有提交新的二代默认运行参数。

## .NET / Mono / FEX 上游调研（2026-10-31）

本节是在线原始报告与固定版本源码的核对，不是新增设备通过记录。尖塔二仍未进入已验收的主菜单；已有 W^X、headless、tiering 对照结果保持不变。

### 实际执行方式

- Unity 的 Mono 后端通常在运行时 JIT，IL2CPP 则在发布前 AOT；Wine Mono、游戏自带 Unity Mono 和 Microsoft CoreCLR 是不同运行时，不能互换名称或直接替换二进制。参考 [Unity 脚本后端说明](https://docs.unity3d.com/6000.0/Documentation/Manual/scripting-backends.html)。
- 本机尖塔二实际加载的是 Linux x86-64 Godot 自带 CoreCLR 9.0.7。托管 IL 经 x86-64 JIT，再由 FEX 转为 ARM64；ReadyToRun 也不能保证完全不 JIT。它不经过 Wine / Proton，也不使用 Wine Mono。
- 不能只把自带 libcoreclr.so 换成 ARM64 版本：Godot、运行时、P/Invoke 与本地扩展之间存在同进程 ABI 边界。原生 ARM64 / NativeAOT 是发行方重新构建的路线，不是本轮运行现有 x86-64 游戏的直接修复。

### 原始缺陷报告及证据边界

| 来源 | 已核实内容 | 与本轮的关系 |
| --- | --- | --- |
| [FEX #5991](https://github.com/FEX-Emu/FEX/issues/5991) 及评论 | 同一个游戏、Linux 原生 x86-64、sniper、ARM64；Vulkan / FMOD / Steamworks 初始化后在 C# 资源预加载失败。报告现已关闭；评论报告 `DOTNET_EnableWriteXorExecute=0` 绕过启动失败，也有同时强制内存顺序后成功的记录 | 最接近的公开复现；关闭是报告者验证 workaround，不是已经取得上游修复提交。我们 W^X=0 后仍退出 1，不能把他人的结果写为 OHOS 通过 |
| [FEX #5766](https://github.com/FEX-Emu/FEX/issues/5766) | Barotrauma Linux CoreCLR 8 多处访问异常；W^X=0 只能越过早期故障。Sonicadvance1 的回复明确也影响 S&Box 与尖塔二。报告仍 open | 支持调查 CoreCLR / 转译器的共性；不同异常位置不证明都是权限或同一游戏引擎的问题 |
| [FEX #6014](https://github.com/FEX-Emu/FEX/issues/6014) | ARM64EC Proton + MelonLoader .NET 6，JIT heap 内非法指令；W^X=0 未解决，full SMC 则变为挂起。报告仍 open | Windows CoreCLR / ARM64EC 是另一条执行链。后续评论中“也测了纯 x86-64 Proton”的说法已由该评论者撤回，不能据此宣称两条路径均复现 |
| [.NET runtime #70910](https://github.com/dotnet/runtime/issues/70910)、[#106819](https://github.com/dotnet/runtime/issues/106819) | Rosetta 下修改双映射 JIT 代码会出错；Linux Rosetta 检测修复由 #102509 带入 .NET 9，并回移至 .NET 8.0.9。#106819 已确认新版解决其复现 | 这是生成代码映射与转译器兼容性的先例；不代表 Rosetta 修复会自动检测 FEX。当前游戏已是 .NET 9，单说“升级到 9”并不构成修复 |
| [.NET runtime #117819](https://github.com/dotnet/runtime/issues/117819) | doublemapper 的大 memfd ftruncate 在较小 RLIMIT_FSIZE 下触发 SIGXFSZ；报告已关闭并有后续修复 | 通用沙箱检查项。我们现在观察到的是 EACCES / 后续正常退出码 1，没有 SIGXFSZ，不能认定命中。memfd 逻辑长度也不等于实际分配了相同物理内存 |
| [Godot #123707](https://github.com/godotengine/godot/issues/123707) | Godot 4.7.2 的 Variant finalizer / 重复 Dispose 可出现 Disposable not registered；open，带 confirmed 标签，发生于非 FEX 系统 | 提醒异常栈也可能源于引擎生命周期。当前 MegaDot 基线不同，且我们未捕获该异常，不能直接套补丁或改游戏 DLL |
| [Proton #9599](https://github.com/ValveSoftware/Proton/issues/9599) | 同游戏在 Intel Broadwell 出现真实 i915 GPU HANG；评论报告改 OpenGL 后解决 | 不能只因标题相同就套用。我们 headless 仍退出 1，目前没有对应 GPU HANG 证据 |

补充的 [Armada 实测说明](https://github.com/SimonBits/armada-game-compat/tree/main/games/slay-the-spire-2) 将 W^X=0 明确限定为该游戏的启动 workaround，记录两款设备的启动成功，同时说明没有完整版本号、性能或长时间游戏验收。不能照搬 Armada 专有启动脚本到 OHOS。

### 固定版本源码核对

1. [.NET v9.0.7 doublemapping.cpp](https://github.com/dotnet/runtime/blob/v9.0.7/src/coreclr/minipal/Unix/doublemapping.cpp)：`minipal_detect_rosetta()` 为真时拒绝 double mapper；Linux 使用 memfd、同一 backing 的共享 RW / RX 地址。`CommitDoubleMappedMemory()` 的 RX mprotect 失败会返回空地址。[executableallocator.cpp](https://github.com/dotnet/runtime/blob/v9.0.7/src/coreclr/utilcode/executableallocator.cpp) 只在 mapper 创建失败时关闭 W^X；不能假设创建成功后的映射 / 代码一致性失败都会安全回退。我们现有 guest-only noexec 重试只跨过权限门槛，不等于完成共享别名与代码更新验收。
2. [当前匹配 FEX revision 的 SyscallsSMCTracking.cpp](https://github.com/FEX-Emu/FEX/blob/37265b109a4bba9e10241972dfcb3d1fdb8f3f9a/Source/Tools/LinuxEmulation/LinuxSyscalls/SyscallsSMCTracking.cpp)：已经有 shared resource / file offset 的多 VMA 失效与保护处理，不能声称 FEX 完全没有实现双映射。应验证 OHOS 的 memfd / fstat / offset / mprotect / SIGSEGV 适配是否保持这份合同。
3. [同 revision Config.json.in](https://github.com/FEX-Emu/FEX/blob/37265b109a4bba9e10241972dfcb3d1fdb8f3f9a/FEXCore/Source/Interface/Config/Config.json.in)：TSOEnabled 默认 true，VectorTSOEnabled / MemcpySetTSOEnabled 默认 false，HalfBarrierTSOEnabled 默认 true，SMCChecks 默认 mtrack。[Proton 11 FEX_Config.json](https://github.com/ValveSoftware/Proton/blob/proton_11.0/FEX_Config.json) 也没有全开向量 / memcpy TSO。当前启动器还保留 Steam appinfo / STEAM_COMPAT_FEX_CONFIG 优先级，因此应采集真正生效的配置，再判断“Force On”是否改变了行为；不能把默认值当设备生效值，也不能全局开启所有开关。
4. [同 revision Windows InvalidationTracker.cpp](https://github.com/FEX-Emu/FEX/blob/37265b109a4bba9e10241972dfcb3d1fdb8f3f9a/Source/Windows/Common/InvalidationTracker.cpp) 的 MonoHacks 检测识别 mono.dll / mono-2.0-bdwgc.dll，启用专门的 backpatch / SMC 处理。这个 Windows Mono 路径不能作为 Linux libcoreclr.so 已兼容的证据。
5. [Godot 4.5.1 gd_mono.cpp](https://github.com/godotengine/godot/blob/4.5.1-stable/modules/mono/mono_gd/gd_mono.cpp) 的导出程序通过 hostfxr 初始化，再调用 load_assembly_and_get_function_pointer / InitializeFromGameProject。[.NET StartupHook 设计](https://github.com/dotnet/runtime/blob/v9.0.7/docs/design/features/host-startup-hook.md)、[assembly.cpp](https://github.com/dotnet/runtime/blob/v9.0.7/src/coreclr/vm/assembly.cpp) 和 [ComponentActivator.cs](https://github.com/dotnet/runtime/blob/v9.0.7/src/libraries/System.Private.CoreLib/src/Internal/Runtime/InteropServices/ComponentActivator.cs) 区分了 Main 前的 RunManagedStartup 与原生 delegate 入口。这为前轮探针没有初始化提供了源码解释；MegaDot 是定制引擎，仍不能把上游基线当作游戏所有代码完全相同的证明。零事件不排除托管异常。

### 下一轮验证顺序

1. 先记录启动时有效的标量 / 向量 TSO、SMC 与 Steam 覆盖项；对 W^X=0 + 强制标量 TSO 做单变量对照，确认其是否区别于此前默认路径。不将所有 TSO / 优化开关全局开启。
2. 在自有平台测试夹具中验证共享 backing 的 RW / RX 别名：从可写地址多次改码、跨页、跨线程发布后，执行地址必须看到新代码；同时验证无 host EXEC 的 guest 映射仍保留正确 VMA / fstat 身份和失效通知。先分离 OHOS 适配与 FEX 既有实现。
3. 用真正生效的原生 / guest 退出观察或 CoreCLR 原生诊断区分 exit(1)、运行时 fatal 与未处理托管异常；先验证探针初始化，避免继续用无效 StartupHook 判断。headless 与正常图形运行分开记录。
4. 根据证据再决定是否增加只面向 native x86 CoreCLR 的能力回退策略，保留用户显式设置。不替换游戏自带运行时、不关闭认证、不做 AppID 特判，也不把研究用 W^X=0 直接发布成全局默认。

该调研阶段未改产品默认参数、FEX 上游源码或游戏文件；没有新增游戏通过结论。后续能力回退的实现与验证边界见文末。

## 启动与稳定性证据

标准入口本轮成功显示 Steam 游戏库，CEF 的 steamUI/clientdll 两个 WebSocket 均 connection ready。另有线上服务加载超过 60 秒的记录，以及先前 0x3009 本地 transport 失败；两者不能混为同一原因。仅诊断关闭 CEF occlusion 的两次快启动不足以证明根治，因此未修改产品默认参数或认证流程。

只保留有界私有日志及小型脱敏记录；未清空账号、游戏库、存档或数据库。当前修复没有添加 AppID 特判，没有替换成 Windows Steam，也没有将 GPU 呈现改为 CPU 帧回读。

## OHOS memfd 与 CoreCLR 双映射实测（2026-10-31）

已用独立 x86-64 夹具复现代码一致性错误，而不只是推测权限问题：同一个 memfd 建立共享 RW / RX 别名后，第一次执行返回 1，写入新指令要求返回 2 时仍得到 1。匿名 RWX 对照的 512 次改写、跨页及跨线程发布全部通过。

同一应用 UID 下另行运行 ARM64 原始系统调用，排除了 x86 stat ABI 转换的影响：

| 对象／操作 | 实际结果 |
| --- | --- |
| memfd 创建、ftruncate | 成功 |
| memfd 的 raw fstat / libc fstat | EACCES |
| memfd 的 raw fstatat(AT_EMPTY_PATH)、statx | EFAULT |
| 已 unlink 的 app-private 普通临时文件，同样的 fstat / fstatat | 成功 |
| 对自己的 memfd 做 fchmod(0600)、fchown(self)，以及跟随 /proc/self/fd 查询 | 仍被拒绝 |
| memfd flags 0、CLOEXEC\|ALLOW_SEALING | 元数据仍被拒绝 |
| MFD_EXEC、MFD_NOEXEC_SEAL | 此内核返回 EINVAL，不能当可用修复 |

固定 FEX revision 的 `TrackMmap()` 在 `fstat64(fd, &buf)` 失败后仍使用 `buf.st_dev / st_ino` 建立共享资源 ID；局部 `buf` 未初始化。该路径会影响共享别名分组，而 FEX 原本已有镜像失效追踪。不能把问题表述成 FEX 完全没有双映射支持，也不能通过编造 inode 或直接忽略查询错误修补。

自有 guest 实验 shim 只在显式 `STEAMOHOS_JIT_FILE_PROBE=1` 下，对无 sealing 的 CLOEXEC 请求比较真实 memfd 与 unlinked 私有稀疏文件。换用后者后，内核 fstat 返回真实身份，独立夹具的 512 次改写、跨页、跨线程和匿名对照全部通过，退出 0。主机另用 EACCES 故障注入验证同一诊断 shim、syscall 第六参数及 errno 转发，且没有残留已链接临时文件。

这只是诊断后端，**没有作为完整 memfd 模拟或产品默认发布**：尚未提供普通 memfd 的所有 seals、跨 exec、hugetlb、空间及并发合同，不能泛化替换 GPU / ARM64 JIT 的 memfd。没有修改游戏文件、使用 Windows depot 或关闭 Steam 认证。

实际游戏所带 `libcoreclr.so` 导入 `syscall()` 而没有导入 `memfd_create()`，因此只拦截后者的第一轮游戏测试没有生效，已明确区分。加入 libc syscall(SYS_memfd_create) 的诊断入口后，保留 .NET 默认 W^X，真实出现私有后端标记并越过 hostfxr，加载托管资源；后续仍退出 1，未进入已验收主菜单。

再加严格标量／向量／REP 内存序的临时对照并没有修复游戏，取得了明确的后续错误：`System.AccessViolationException` 位于 `ConcurrentDictionary.TryAddInternal → Godot.DisposablesTracker.RegisterDisposable → Variant.Disposer → ScriptManagerBridge.UpdateScriptClassInfo`，退出探针捕获 `libcoreclr.so → abort()`，最终 SIGABRT / 134。它与上游 CoreCLR / FEX 报告有关联线索，但还不能证明是同一个缺陷。该栈也不等于已复现上游 `Disposable not registered` 的 finalizer 异常。

结论：已定位并验证早期双映射代码失效问题的可行规避路径；游戏后续托管内存异常仍需独立处理，整体 accepted=false。未将严格 TSO、W^X 关闭或实验 memfd 后端设为全局默认。

## CoreCLR 最小夹具与能力回退（2026-10-31）

《尖塔二》使用 Linux x86-64 Godot/MegaDot 自带 CoreCLR 9.0.7，经 FEX 执行；不是 Wine Mono 或 GNU Proton。不能用替换成 ARM64 CLR 解决同进程的 x86-64 ABI。

新增自有 `native_coreclr_host.c` 与 `native_coreclr_contract.cs`，通过游戏原有 self-contained 部署的 hostfxr / CoreCLR 加载自有夹具。参照 Godot 导出程序使用 `hostfxr_initialize_for_dotnet_command_line`，只读取原应用描述来选择运行时；不调用游戏托管入口。最初误用 runtime_config 初始化得到 0x80008093，组件文件名与程序集名不匹配时得到 0x80070002；这两次属于夹具搭建失败，未当作 CLR 缺陷或通过证据。

平板在 W^X 关闭条件下已完成：

- 游戏原 CoreCLR 9.0.7 的普通算术 JIT 和 128 个 DynamicMethod。
- 真实隐式空引用的信号到托管异常处理。
- libc qsort 到 UnmanagedCallersOnly 回调，回调中创建并释放托管对象。
- 4 线程共 20,000 对象、WeakReference 字典注册/移除、Dispose、finalizer 与强制 GC，最终剩余登记数为 0。

Windows 参考使用本机 9.0.16，不能称其与平板版本完全相同；Windows 跳过 Linux qsort 回调。上述通过只覆盖此夹具，不能排除更复杂的 JIT、GC 或 Godot 交互缺陷。实际游戏另行关闭 ReadyToRun、分层编译和硬件 intrinsic，并强制标量 TSO 后仍退出 1，**未取得主菜单通过证据**；未将这些诊断开关设为产品默认。

### 已实现的通用收敛

`native_jit_capability.h` 在原生 FEX 子启动器探测 memfd 的真实 fstat 能力，仅在返回 EACCES/EFAULT，且用户未显式设置 DOTNET/COMPlus 的 W^X 开关时，设置 `DOTNET_EnableWriteXorExecute=0`。不按 AppID 匹配；不更换游戏 DLL；不替代 GPU/主机 ARM64 JIT 的 memfd，也不创建磁盘后端。memfd 创建本身失败时保留 .NET 已有回退，健康平台和用户显式选择不改。

此前 empty-path stat 实验也已收紧：先执行原查询，遇到对应拒绝才尝试真实 fstat；两者均失败时保留原错误。它不再将 fstat 成功假定为 memfd 可用，也不伪造 inode。

GNU ARM64 适配器与启动器已重编译并通过源码/载荷校验；新增能力策略、元数据拒绝合同及原生 FEX 启动回归共 35 项通过，信号栈边界回归另有 1 项通过。新签名 HAP 已安装普通平板，经正常 Deck／Steam 入口自动交付；设备适配器和启动器的 SHA-256 与 rawfile 一致，没有手动替换运行库，也没有在 LaunchOptions 设置 W^X。日志实际出现 `STEAMOHOS_NATIVE_JIT runtime=coreclr wx=0 reason=memfd_metadata_denied scope=native-child`，随后越过 hostfxr 并加载资源。**自动能力策略交付已通过，游戏仍退出 1，未通过游戏兼容性验收。**

最新有界 guest 信号诊断成功初始化并观察到 CoreCLR 注册处理器，但没有收到可确认的游戏故障回调，仍以 1 / signal 0 退出。这不证明没有底层信号或内存错误，也不能把辅助 sh 的退出栈当作游戏根因。诊断探针、临时启动参数已恢复并按部署 SHA 清理；账号、游戏文件和存档未改。仍保留后续定位任务，不宣称尖塔二已修复。

## 实际退出路径与调试器边界（2026-10-31）

适配器新增默认关闭的 `STEAMOHOS_NATIVE_EXIT_TRACE=1` 诊断，仅在 native FEX 作用域记录 libc `syscall(SYS_exit_group/SYS_exit)` 的真实状态及宿主调用点，每进程最多 8 条。它不展开栈、不改变退出状态，也不记录完整环境或调用参数。真实系统调用子进程回归确认 exit／exit_group 仍返回 37，getpid、close 的返回值与 errno 不变，未同时开启两个作用域开关时不输出。

没有连接调试器的游戏运行已捕获主 FEX 进程的 `exit_group status=1`。复用现有对象文件仅重链接本地符号镜像，核对其可执行 PT_LOAD 段与已交付 FEX 字节完全相同；将同进程主 ELF 基址与 caller 相减得到 RVA `0x253e10`，对应固定 revision `Thread.cpp` 中 guest exit_group handler 的宿主 syscall 返回点。OHOS maps 对该 RX 段不提供文件名和准确 ELF offset，日志里的 `map_offset=0x166e10` **不能直接当作 ELF RVA**。该证据确认进入了 guest 退出转发路径，不能据此排除 CLR 或游戏捕获异常后退出。

另一次 opt-in guest RSP 调试捕获 `T0b`（SIGSEGV），但读取单寄存器时对端关闭，未取得完整寄存器、故障指令或异常栈；该次退出状态为 191，与不连接调试器时的 1 分开记录。后续一次调试连接未取得信号即退出 1。CoreCLR 也会使用 SIGSEGV 处理正常隐式空引用，单个信号不能直接认作最终故障。没有将调试会话的行为当作正常游戏结果。固定 FEX 的调试器源码也注明 SIGSEGV 后恢复能力有限；整组 `g` 查询曾在 all-stop 下挂起，因此后续只使用 `p`，仍不足以确定最终根因。

在同一 HAP 和自动能力回退下，另行临时传入 Godot 的 `--disable-crash-handler`，不连接调试器，仍越过 hostfxr 并在资源加载后退出 1／signal 0。该对照没有形成修复，也不能证明游戏内的第三方崩溃处理器已被关闭。结束后已停止所选应用及全部同 UID 子进程、核对本任务 guest 调试转发为 0，恢复原始 LaunchOptions；不保留这些诊断开关。

后续应先捕获可复现的 CoreCLR／Godot 内存故障位置，再验证同路径的修复；不通过禁用认证、替换游戏程序集、全局关闭内存优化或 AppID 专用规则掩盖失败。尖塔二仍为 `accepted=false`。

## 分辨率对照（2026-11-01）

该版本游戏的视频设置在全屏时将“窗口化分辨率”显示为 N/A；没有独立的全屏内部渲染比例。退出游戏自己的全屏后，该设备下拉列表仅提供 1024×768。通过正常 X11 resize 可让游戏自行重建 960×540、640×360 的 Vulkan swapchain，宿主仍全屏 fit，保持 NativeBuffer 路径，不修改游戏程序或虚报 Vulkan 尺寸。

前期 1024×768／960×540 约 20 FPS 的观察夹杂用户战斗与焦点切换，不作为同场景定量对照。用户暂停操作后，在同一战斗的暂停菜单做一次低／高分辨率对照：640×360 约 24.4 FPS（600 帧／24.56 秒），1200×800 约 14.7 FPS（360 帧／24.47 秒）。计数来自每 120 帧的现有日志，单段约有 ±4.9 FPS 量化误差；后续全屏截图也见约 19～23 FPS，尚不能声称获得稳定提升或已定位唯一瓶颈。640×360 的字明显模糊，没有作为产品默认。

已通过游戏 UI 恢复全屏，保持用户的 30 FPS 上限、关闭 VSync、关闭 MSAA，并停在暂停菜单。未保存／退出战斗，不修改战斗存档。当前 HAP 未新增按游戏保存的分辨率控制。后续应把渲染尺寸和全屏输出解耦为通用用户选项，并分别测 GPU、转译和同步耗时；不应简单缩小 Vulkan 申报尺寸或全局强制低画质。

另观察到 windowed X11 game 在其他 Steam X11 窗口下方时，单独 set_input_focus 不足以让 XTEST 点击命中；临时将真实 game window raise 后可操作暂停／设置。本轮没有据此修改产品焦点策略，也未将该现象认定为低帧率根因。

## 渲染前开销源码审查（2026-11-01）

本节完成源码审查和运行中组件身份核对，没有更换产品代码、游戏设置或设备运行库，也没有中断战斗。
分辨率试验已有一定差异且计数误差较大，不能得出“完全不受分辨率影响”或“已经证明 CPU 瓶颈”的结论。
尖塔二的主体仍是 **FEX x86-64 → Godot/MegaDot + CoreCLR → Vulkan Forward+ → GNU Venus → OHOS Vulkan → NativeBuffer**；
宿主可能使用 GLES 合成，不代表游戏经过 WineD3D / VirGL / DXVK。

### 已确认存在的固定开销及处理顺序

| 优先级 | 操作与源码 | 优化方向与正确性边界 |
| --- | --- | --- |
| 1 | `gnu_fixture/native_bc/runtime.h` 的 `bindPipeline()`、`push()` 对普通命令也持 `stateMutex`；push 状态按 exact key 删除旧项，再新建 byte vector / 复制数据，持锁调用底层 Vulkan。`deviceProc()` 又在同一递归锁下查设备表及函数地址 | 先按 device 缓存 dispatch，复用 push 存储并保留更新顺序、layout 生命周期和重叠 range / shader stage 语义；再将命令录制状态放到每个 command buffer，缩短共享表锁。不能直接移除 BC 注入前后所需的状态恢复。Godot 多线程录制可能因此串行，实际占比尚未测出 |
| 1 | `vn_queue.c:vn_queue_submit()` 在每次提交前调用 `vn_device_memory_flush_persistent_mappings()`。后者扫描映射列表，对 HOST_COHERENT 且曾显式 flush 的 allocation 按整个映射范围逐项同步 RPC。write-seen 保持至 unmap；不是精确的本次 dirty 范围。native launcher 目前也启用这份 shadow 兼容策略 | 仅在成功协商 native-memory-v1、确认同一 backing 且真实 HOST_COHERENT 时，评估免除 shadow 重复发布；旧 shadow context 维持原保护，非 coherent 的显式 flush / invalidate 必须保留。仍需发布的 ranges 可在合法同步边界批量处理，不能简单清除 write-seen 后漏掉后续直接写入 |
| 2 | `proton_vulkan_wsi.cpp` 的 acquire 额外 QueueSubmit 用于 signal semaphore/fence，present 另一次空 submit 消费 wait semaphore；二者也进入上述映射扫描 | 先消除这些提交附带的重复内存处理。信号与等待语义必须保留，不能因 commandBufferCount=0 就跳过整个 submit |
| 2 | `vn_renderer_vtest.c:vn_winehua_present()` 每帧 ring roundtrip / wait-all 后，再持 socket mutex 发同步 private present；WSI 同时持 swapchain 和 queue 锁 | 长期将 present 及其 producer 序号纳入有序队列，减少跨传输往返，保留图像和对象存活、错误返回、有限在途帧及 resize / device-lost 处理。当前 drain 等宿主解码和 driver QueueSubmit，不等同于 GPU 完成；直接删除会重现跨传输顺序错误 |
| 3 | NativeBuffer 三槽旧 fence、NativeWindow RequestBuffer / FlushBuffer、GPU copy / barrier，以及 FEX / CoreCLR / 引擎脚本 | 先分开计时，再判断是 buffer 背压、GPU 工作还是 guest 帧生成。不放宽同步冒险复用图像，不通过全局关闭 TSO 或改游戏 DLL 换取帧率 |

### GL 路径可参考的部分

`virgl_surface_presenter.cpp` 的正常呈现每帧有 producer fence / glFlush、切换 presenter EGL context、GPU wait / 绘制、consumer fence / glFlush、eglSwapBuffers 和恢复 producer context。这些属于固定成本，GL 游戏需要单独测量；`glFinish` 只在 fence / context 失败时回退，不是正常每帧操作。后续可研究减少 context 切换与重复状态处理，但必须保持共享纹理读写顺序。它不能直接解释当前 Vulkan 游戏的低帧率。

当前已确认的保护和优化继续保留：NativeBuffer 显示链没有 CPU 像素回读；NativeBuffer 正常呈现没有 post-present CPU fence wait；宿主 GLES 合成已使用 `eglSwapInterval(0)` 避免与 NativeVSync 双重等待。`EnsureVulkanLocked()` 的 QueueWaitIdle 只在重建路径，不应算作稳态每帧等待。

### 本轮核对与下一轮计时

- 运行中安装目录的 GNU Venus SHA-256 与 HAP payload 相同：`53340d59c7a8245dd1a9486d18e5cd9f6f9a2cbb8287ab979f0dc7736192a6c7`。现有 GNU build receipt 指向同一产物；库中包含 `VN_WINEHUA_PERF_SUMMARY` / `VN_WINEHUA_PERF_LOG` 和 ring / present 统计。
- 实际 GNU Mesa snapshot 的 device-memory / queue 代码与当前第三方源码相同；vtest 的额外差异是已登记的 native-memory-v1 协商，不能误判为遗漏修复。
- 有界日志尾部仍记录 `pixel_readbacks=0`；draw / startup verbose 标记均未开启。此次没有得到 guest frame perf 分段统计，也没有完整 GPU scene 时间，不对任何一项声称已实测为最大瓶颈。设备拒绝枚举 proc task，原线程 CPU 采样不能作为证据。
- 下一轮在可安全退出战斗后重启游戏，复用现有低频统计，按同一场景、固定 FPS cap / 尺寸、预热及交替顺序采集：push / bind 调用与锁等待、persistent flush 数量 / 范围 / 耗时、ring reply top / drain、present RPC、NativeBuffer request / fence / flush。GPU copy 时间只表示 presenter 工作，不代表整场景 GPU 时间。
- 先处理可证明重复的分配 / 查表与 native coherent 发布，再评估有序异步 present。以帧时间分布、可见图像、BC / 非 BC 资源、多线程命令录制和长时稳定性共同验收，不以单个 FPS 截图作为完成标准。

## Native BC 命令录制优化与候选包验收（2026-11-01）

本轮已实现通用 native BC 录制热路径优化，区别于上一节只做源码审查的历史状态。每个 command buffer 分配时缓存底层 bind / push 函数及设备 BC capability；重复 exact-key push constants 更新复用 byte vector，以 rotate 保留重叠范围与 shader stages 的最终更新顺序；共享表和状态更新仍加锁，底层普通 bind / push 转发移到锁外。没有修改游戏、FEX 上游源码、BC shader、Vulkan 同步／内存发布策略或旧 OHOS Wine 图形路径。

真实生产源码夹具在预热后执行 10,000 次重复 push / bind，新增堆分配和 proc 查询均为 0；重叠范围／stage replay、layout 销毁后的保留生命周期、缺失 layout 错误、底层调用不持共享锁、两个录制线程可同时进入底层均通过。ASan／UBSan、native 与 Wine WSI、真实 X11／XTEST／Wayland 回归、GNU ARM64 构建及最终 HAP 图形合同通过。新夹具已登记到 CI，未声称远端 CI 已执行。

普通平板已覆盖安装新签名 HAP，账号、游戏库和存档保留。经标准安装自动交付的 POSIX、FEX 启动器、native Vulkan 和 guest IPC SHA-256 与 HAP rawfile 一致；native WSI 为 `7789fe17bd47fd1f06d6eca442c3412ed62e454b558c5019ac772ae4ddac867e`。尖塔二仍为 Linux 原生 Godot/MegaDot + CoreCLR → FEX → Venus → Maleoon → NativeBuffer，未切换 Windows depot 或 OHOS Wine。设备 X11 与 swapchain 均为 1200×800；全屏主菜单、中文／纹理、设置页往返及菜单确认退出通过，真实子进程退出码 0、signal 0，ps 确认进程已结束。引擎退出仍报告资源泄漏告警，不能称所有引擎告警已解决。

主菜单预热后连续 93.16 秒呈现 2760 帧，约 29.63 FPS，120 帧日志计数带来的单段量化边界约 ±1.29 FPS；采样全程核实了同一 live PID、surface 和尺寸，pixel_readbacks 为 0。当前视频设置实际显示帧数上限 30、VSync 开启、MSAA 关闭，本轮没有更改设置。此前暂停战斗样本缺少进程／swapchain 身份，已排除出对照；不同场景和不同 VSync 条件不能混用。此结果只验收短时菜单呈现，**没有证明战斗提速、P95 帧时间改善或长期稳定性**。

本轮普通 Deck 冷启动曾在 Steam 登录成功后继续加载，游戏尚未进入渲染；重试临时使用 CEF `-cef-disable-occlusion` 诊断和官方 Steam 启动，随后进入游戏。日志另见 `CPrepareConfigWorker failed`，没有证明它是全部启动延迟的根因。游戏内报告到主菜单约 71 秒；不能归为认证或 BC 录制的单一耗时。本轮未将 CEF 诊断改为产品默认，也未重新验收普通冷启动稳定性。原启动配置已恢复核对，游戏和临时 Steam 会话已退出，不遗留诊断启动选项。

候选与唯一回退签名 HAP 均复用固定任务目录。后续先做同场景战斗对照，并量化 persistent-map 发布、ring／drain、present RPC 和 NativeBuffer 背压；不能通过直接删同步来换取未经验证的 FPS。

## 尖塔二战斗性能对照与手动交付核验（2026-11-01）

普通平板以正常 Deck 游戏库卡片进行优化版 → 上一版 → 优化版三次启动，均进入 Linux 原生尖塔二；未使用临时 CEF 参数或手动替换设备 ELF。完整签名 HAP 的 SHA-256 已核对，ZIP 条目 CRC／大小筛查只发现 native Vulkan 支持库和清单不同，对差异条目另做 SHA-256；每版运行库也核对设备 SHA。路径仍为 Linux ARM64 Steam → FEX → Godot/MegaDot + CoreCLR → Venus → Maleoon → NativeBuffer，不是 Wine／DXVK。

相同存档为 Silent、暗港第 14 层、进阶 10、SKULKING_COLONY_ELITE、第 1 回合，人物 40/70、敌人 80/80、金币 112。尺寸固定 1200×800、上限 30 FPS；没有更改游戏设置。保存配置在上一版退出后和优化版复测前均为 `vsync=off`、`msaa=0`；前节菜单记录的“VSync 开启”来自当时画面的解释，不作为本轮设置证据，菜单结果也不混入战斗对照。

| 顺序／场景 | 秒数 | 呈现帧数 | 区间平均 FPS |
| --- | ---: | ---: | ---: |
| 优化版，战斗暂停 | 93.20 | 1800 | 19.31 |
| 上一版，同一战斗暂停 | 92.97 | 1920 | 20.65 |
| 优化版复测，同一战斗暂停 | 93.48 | 1800 | 19.26 |
| 优化版，取消暂停后第 1 回合待机 | 92.95 | 1800 | 19.36 |

每段均核实同一 live PID、surface、尺寸，日志为 `pixel_readbacks=0`。120 帧日志步长对应每段约 ±1.29 FPS 量化边界，并非统计置信区间；没有单帧或 P95 数据，设备温度／频率也未受控。优化版两段约 19.3 FPS、上一版约 20.7 FPS，**没有证实战斗提速，也不能排除小幅回退**。用户反馈实际出牌感觉有所改善，作为主观反馈单独保留，不能替代上述计数。取消暂停后的待机也约 19.4 FPS；没有测试出牌特效和敌人回合开销。

采样期间未出牌／结束回合；取消暂停的连续区间前后 76 份存档 SHA-256 完全相同。继续游戏和正常退出本身会重写 current_run、prefs／progress，所以不宣称整个测试期间所有存档字节未变，也不恢复覆盖用户存档。上一版通过游戏菜单正常退出，真实游戏子进程 code=0、signal=0，ps 确认结束。最后用户继续手动战斗后停止所有前台自动化，保留优化版和现场。

通用交付核验：`LinuxSteamSessionService` 在写普通 `startupAction` 前调用 `ensureLinuxSteamPosixSupport`，从 HAP 的清单／SHA 自动更新 companion，不依赖调试脚本复制文件。设备 POSIX、FEX 启动器、native Vulkan 和 guest IPC 四项均与 rawfile SHA 一致；启动配置除正常卡片的 `startupAction=launch 2868840` 外与原配置一致，无临时 `-cef-disable-occlusion`／超时改写。CoreCLR 能力回退由启动器实际探测并自动选择，未为本轮写游戏专用开关。共享交付服务的缓存复用、损坏修复、原子替换和路径保护回归再次通过。此前 GNU ARM64 构建、主机语义／并发／ASan／UBSan 及图形合同结果继续保留；本轮 source/payload verify-only 通过。相关测试已登记 CI，但未声称远端 CI 运行。

尚存的启动耗时和重复存档时间戳拒绝独立保留：游戏内计时到首次主菜单分别约 72.6／76.3／75.9 秒，不能等同 Deck 点击总耗时。时间戳拒绝伴随重复云存档拉取，未证明它是全部启动或战斗性能的根因。后续优先对 persistent coherent-map 发布、ring／drain、present RPC 和 NativeBuffer 背压做分段计时，再决定优化；不直接删除同步。本轮只新增性能证据和交付核验，没有再改游戏／驱动参数。


## 同机 HSL 性能对照与输入焦点（2026-11-03）

只使用普通平板，未操作 Edge、手机或修改 HSL。两边游戏均为 build 23811903、Linux depot 2868843 / manifest 7881097836989620034，ELF SHA-256 `0c21999e14b9ea44b7fa2cea38d3f27bbf63a784393d7932e2fa68a0819aa69a`。路线为 ARM64 Steam → FEX → sniper x86-64 Godot/MegaDot + CoreCLR 9.0.7 → 原生 Vulkan/Venus → Maleoon 910 → NativeBuffer。两边实际 swapchain 都是 **1920×1080**；X11 几何 1200×800 不是渲染分辨率。本问题不能归为 Wine、DXVK 或 GL 游戏开销。

HSL 已登录并安装尖塔二，同设备第19层第1回合待机参考约 30.31 FPS。VPP 正常控制包预热后约13～18 FPS，最新40.77秒/720帧约17.66 FPS。其间用户手动出牌，后一次场景与 HSL 参考并非逐像素相同，因此这些是同机差距线索，不能当作严格交替基准。VPP 曾从2～3 FPS自然上升，未改变代码或画质；不将自然预热计作优化收益。两者稳态差距仍未解决，Release/O3 与持续 JIT 编译开销检查未支持“误用 Debug”或“每帧重编译”的解释。

最初12秒/99 Hz/8097个样本、0丢失的 Hiperf 中，libc `__GI___fstatat64` 占11.66%，调用方为 `DepotLinkTarget` / `DepotResolveImpl` / `FollowCloud`。主线程32768次 resolve 累计15.20秒是累计操作时间，不是每帧耗时，嵌套计时不能相加。已减少通用 GNU 路径适配的重复遍历，复用真实叶子 metadata，并在原 syscall 返回真实 ENOENT 时结束合格的 dirfd 缺失探测。深9层普通叶子 lstat 为1次、readlink为2次 metadata，浅/深目录256个文件的扫描均257次。不存在持久负缓存或游戏名特判，链接并发发布、替换、rename、目录边界、权限和存档保护回归通过。最新6秒/99 Hz/1988样本、0丢失中，主线程 POSIX/readlinkat 调用点仍明显；采样比例不能等同帧耗时或全部根因。

输入调查中，正常控制包的主菜单触摸、键盘和战斗暂停可操作，用户也确认有手动出牌，未复现持续全部输入失效。随后抓到更具体的异常：屏幕仍是游戏暂停菜单，X11 焦点却落到 Steam；HUD 同时升到30 FPS，游戏按钮无响应。该状态下的较高 HUD 不计为有效性能收益。通用修复在真实 Wayland pointer/keyboard enter 时发送 EWMH activation；窗口管理器仅在已知不同 PID 且无 transient、client leader 或 window group 关系时清理无关的旧 activeChild。保留旧 CEF 启动选择器行为，不周期抢焦点。新增回归在修改前失败，修改后 Xvfb 桌面、真实 XTEST/Wayland 键鼠滚轮和 native WSI 同步测试通过。

新增小型 HAP 自带 `steamohos_gnu_desktop_hap`，与 POSIX/WSI 同样经资源清单和 SHA 自动安装；产品会话选择新宿主，诊断会话保留原宿主。这样窗口修复随应用交付，无须修改 Valve 文件或每次重打离线大包。构建脚本 `scripts/build_linux_steam_desktop.py` 支持 GNU ARM64 构建和 verify-only；CI、资源损坏修复/缓存复用/原子替换测试及最终 HAP 审计已接入。原生 WSI 和 POSIX companion 均已重建同步。实验分支全量增量 HAP 构建、最终包审计和本地签名验证通过，真机修复验收另记，不把主机测试当真机结论。

FEX 上游源码本轮只读研究，临时诊断已撤销，未关闭 TSO、删除同步或修改游戏 DLL。未做本轮全部存档前后 SHA 校验，不宣称存档字节完全不变。后续区分文件路径/文档存储、guest 帧生成与 Venus 呈现背压，不以焦点修复解释全部性能差距。脱敏证据见 [同机性能调查](evidence/spire2-hsl-performance.json)。


### 配套离线包和普通平板短时验收（2026-11-04）

新候选 HAP 已通过完整编译、实际包审计和本地签名验证。在普通平板经系统文件选择器导入完整 Steam 离线 ZIP，13 个组件／5830 个文件全部验证并启用，最终状态 ready。原来缺少 native SDK 的旧包不能充当当前完整包；随应用更新的小型宿主不要求重打官方组件大包，组件变更则由发布流程准备新包并核对实际 HAP/App。详见 [离线交付](OFFLINE_RELEASE.zh-CN.md) 和 [脱敏验收](evidence/offline-release-delivery.json)。

随后通过正常 Steam 会话启动 Linux 原生尖塔二，进入主菜单；实际触摸后 X11 焦点为游戏窗口，设置页可进入，Escape 返回，菜单确认退出后游戏窗口消失，父进程 waitid 报告正常退出状态 0。未手动调用 XSetInputFocus。新版 desktop、POSIX 和 native Vulkan 三项 companion 的设备 SHA 与 HAP 清单一致，确认用户正常安装可获得修复。只验证了短时菜单输入，不宣称间歇性长时焦点异常已完全消除，也不将本轮菜单 HUD 当作战斗性能结论。临时游戏画质／帧率四字段与启动配置已恢复；未推进战斗或覆盖用户存档。
