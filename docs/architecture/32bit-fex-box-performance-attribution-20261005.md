# 32 位游戏 FEX / Box 性能差距：执行链与归因方案

日期：2026-10-05。当前基线 284026ce；main 参照为本地 origin/main 5383b784，未刷新或部署最新 main 包。本轮只读核查生产源码、构建缓存与既有真机采样，不启动应用或游戏，没有新的性能 A/B 结论。

用户反馈多款 32 位游戏在 Proton/FEX 下约为 main/Box 的一半帧率。这是应调查的端到端差距，目前不能换算成 FEX JIT 的独立效率比例。

## 已核对的执行链

- main（2026-10-06 更正）：32 位游戏和相关 x86 Wine DLL经 Box 执行；x86_64 Wine Unix 侧和 x86_64 Mesa 也经 Box 执行。main 的生产配置 `entry/src/main/cpp/wine/wine_env.h::Box64EmulatedLibs()` 明确将 libGL/libEGL/libGLES 列为 emulated。此前仅依据 Box 的 wrappedlibgl.c 把此链描述为 ARM64 Mesa，是错误的；包装实现存在不代表此项目实际选用了它。
- Proton：32 位游戏和 i386 WineD3D/opengl32 PE DLL经 FEX 执行；FEX WowSyscallHandler 将 UnixCall 转给 __wine_unix_call_dispatcher，进入原生 ARM64 Wine OpenGL Unix 侧及 ARM64 Mesa。
- 两者 Mesa VirGL 后端经 vtest socket / 共享资源进入 renderer 进程，再通过宿主 EGL/GL 及 GPU 驱动执行。Proton 的 ARM64 Mesa 及其 POSIX 调用不经 FEX 转译；main 的 x86_64 Mesa 则由 Box 执行。
- 原生 ARM64 Wine 不代表 32 位 WineD3D PE DLL已原生执行。32 位图形管理代码、游戏代码及其跨界频率都可能贡献 CPU 成本。

关键源码：FEX Source/Windows/WOW64/Module.cpp:433–479；UnixCall 在 449 调用 WineUnixCall，普通 Windows syscall 在 457 调用 Wow64SystemServiceEx。前后还有 JIT 控制状态与异常展开处理。LockJITContext 是线程控制字 CAS，不应仅凭名字认定全局互斥串行化。

## 值得优先验证的共性差异

| 优先级 | 差异与证据 | 如何判断 |
|---|---|---|
| 1 | FEX 默认 TSOEnabled=true；Box 默认 STRONGMEM=0，项目基线 BIGBLOCK=3 / CALLRET=2 / WEAKBARRIER=2，游戏档位可能覆盖 | 记录最终配置与 JIT 内存屏障/未对齐回退计数，比较内存及原子指令微基准 |
| 1 | FEX X87ReducedPrecision 默认 false，存在 SoftFloat F80 helpers；Box 文档的默认 x87 策略尝试 float，另有 double 策略 | 在相同宿主条件下分别测整数、SSE、x87；从实际游戏采样识别 F80/softfloat 热点，不先改变精度 |
| 2 | WoW64 UnixCall 每次从转译代码进入原生侧，需要状态保存/恢复、控制字及异常展开；两边也都有桥接 | 聚合调用数，分离桥接自身成本和原生被调用函数成本，按每帧归一化，避免把 Mesa 等待计入 FEX 独立成本 |
| 2 | FEX 从 Wine 注册表 CP 4030 等值检测 ARM 特性；Wine 依赖 HWCAP_CPUID 填写 ID regs | 一次性输出实际 LSE/RCpc 等识别结果、硬件 TSO 请求返回值；缺失特性可使 JIT 使用更慢路径，尚无设备证据 |
| 3 | 32 位地址范围导致高地址 GL mapping 使用低地址 shadow | 两边测 direct/shadow 比例、复制 bytes/frame，与调用/资源身份对齐 |
| 3 | JIT 编译、SMC 失效、代码缓存、线程等待和强制诊断开销 | 分离首次/重复/稳态，记录编译时间、失效次数、线程 CPU 与 off-CPU 等待 |
| 并行 | Present / swap / 宿主刷新节奏 | 对齐游戏提交序列、实际显示和等待；60→30 可来自越过刷新预算，不一定计算效率恰好减半 |

FEX 通过 ProcessFexHardwareTso 尝试启用硬件支持；当前 Wine NtSetInformationProcess 未找到该 class 分支，默认返回 STATUS_NOT_IMPLEMENTED。源码层显示该请求不能在当前实现中启用硬件 TSO，实际设备返回值及 HostFeatures 仍应记录。不得从这段代码声称 OHOS 内核普遍没有相关能力。

实际 FEX 构建为 RelWithDebInfo / -O2 -g -DNDEBUG，LTO关闭；FEXCore 和 wow64fex 的编译 flags 都如此，preserve_all ABI 优化标志为 0。已对比安装候选包中 libwow64fex.dll 与 build/fex-pe/Bin 输出的可加载 PE sections，匹配，Machine=0xaa64。故没有把本轮差距归因于 Debug/O0 的证据。LTO或编译优化仍可独立评估，不能保证填平一倍差距。

TSO、浮点精度、preserve_all ABI 都涉及正确性；改善方向是识别实际热点、减少冗余工作和完善宿主能力识别。不要将全局关闭 TSO、降低 x87 精度或伪报 ABI 支持作为默认方案。

## VirGL 是否不同

Mesa main pin 2939cbb8，Proton pin 330124bf。六文件差异主要是能力诊断与 Venus 修改；vtest_socket 的变化是可选 caps 日志，未找到 transfer/busy 核心算法在两 pin 间的更改。

VirGL renderer main pin fde243e1，Proton pin bb33e52a。renderer 存在 sRGB、context、vtest_server 与呈现差异，不能宣称两条链完全一致；vtest_renderer.c 在两 pin 间无差异。应量化实际 GL call、query、flush、等待和资源传输，先确认是否同等工作量。上述原理只用于已确认 WineD3D/OpenGL/VirGL 的场景，不能推广到所有 DXVK/Vulkan 游戏。

main 的 Wine opengl32 同样存在高地址 mapping 的 shadow 复制机制，因此当前并未证明 main 一定 direct map。既有 RichMan8 主菜单样本平均复制约 0.88 ms/swap、约 1 GiB/s。这证明可优化内存流量，不是跨游戏减半的独立因果证据。复制减少仍可能帮助接近刷新预算的场景；需结合 Present 时间线。

## 建议的验证矩阵

1. 同包、同 ARM64 Wine / Mesa / VirGL，用当前已有的单次 CPU 后端选择分别跑 FEX与 wowbox64；默认继续 FEX。以启动日志确认实际 HODLL，考虑 Steam 强制 FEX的例外。此实验较接近 CPU 模块对照，但不等于 main 的完整 x86_64 Wine / Box 架构。
2. PE32 纯 CPU 微基准：整数/分支、内存、SSE、x87、锁与原子；验证结果一致，再比较耗时。只有调用密集而纯计算差距小才优先优化桥接。
3. API 密集微基准：少量工作的大量 UnixCall/GL call；另做固定画面、固定 draw 数的渲染。统计每帧调用、上传量、同步查询数、CPU与等待，分开分析。
4. main / Proton 两份真实包同温度、分辨率、渲染器、内容、入口和刷新设置回测，分别记录首次、重复与地图稳态。将 Wine/CPU/Mesa差异逐项隔离，记录最终档位而非仅看 UI选择。
5. 按热点选择修复：缺少宿主能力则修探测；x87热点则优化相关合法 fast paths；高频跨界则批处理和瘦化桥接；shadow 热点则脏区/低地址资源池；同步等待热点则队列与 fence。长期将更多图形管理工作移到原生侧，需要明确 32→64 ABI接口与资源生命周期，不能让 PE32 游戏直接加载 ARM64 DLL。

本轮没有修改生产逻辑、编译或安装新包，也没有开启或关闭 TSO/浮点策略。已有动态暂存候选仍待手动真机对照，不能把它当这份归因研究的性能结论。
