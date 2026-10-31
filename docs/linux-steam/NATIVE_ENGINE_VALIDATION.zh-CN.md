# 原生 Linux 引擎验证（2026-10-31）

本轮只操作普通平板；两代尖塔均通过 Steam 切换为默认 Linux Runtime 并完成原生 depot 安装，核对入口为 x86-64 ELF。Windows depot、原 OHOS Wine 或 GNU Proton 的启动结果不能替代以下原生验收。

| 游戏 | 原生 depot | 观测结果 | 尚未通过 |
| --- | --- | --- | --- |
| 星露谷 413150 | 413153 | 官方 FEX → guest shell → .NET/SDL/OpenGL，曾出现有动画的标题菜单 | 窗口尺寸、输入、正常退出及 GL 共享呈现验收 |
| 杀戮尖塔 646570 | 646573 | 官方 FEX → scout-on-soldier → Java 8/libGDX；补齐 guest PATH 后越过显示模式初始化，默认 JIT 与解释执行暴露不同后续故障；按用户要求暂停专项调查 | 菜单与可玩性；不能归为 DXVK 或 Steam 认证失败 |
| 杀戮尖塔二 2868840 | 2868843 | 官方 FEX → sniper → Godot/.NET 9.0.7，Vulkan swapchain 与音频初始化成功；默认双映射停在 .NET 初始化，临时关闭 W^X 后越过并加载资源，随后退出码 1 | 菜单、输入、游戏运行与退出 |

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

GNU ARM64 适配器与启动器已重编译并通过源码/载荷校验；新增能力策略、元数据拒绝合同及原生 FEX 启动回归共 34 项通过。本轮未重新打 HAP，平板安装包仍为上一候选；设备上的手动 W^X 对照不是新自动策略 HAP 的验收。还需将新包安装后复核自动日志和游戏后续退出。

最新有界 guest 信号诊断成功初始化并观察到 CoreCLR 注册处理器，但没有收到可确认的游戏故障回调，仍以 1 / signal 0 退出。这不证明没有底层信号或内存错误，也不能把辅助 sh 的退出栈当作游戏根因。诊断探针、临时启动参数已恢复并按部署 SHA 清理；账号、游戏文件和存档未改。仍保留后续定位任务，不宣称尖塔二已修复。
