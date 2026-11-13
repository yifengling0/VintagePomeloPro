# 每游戏转译引擎与 Box WoW64 兼容修复

## 选择与兼容范围

原 OHOS Wine 的游戏设置现在可保存“跟随全局 / FEX / Box64”。默认跟随全局，旧配置不会被改成固定引擎。Box 才显示兼容性预设，FEX 才显示严格浮点精度；启动服务按最终选择注入进程环境并校验实际所选模块。退出旧游戏后从首页重新启动生效，Wine 桌面内启动继续使用全局引擎。Linux Steam / GNU Proton 不由此开关控制。

1.4.4（38e7522d，Box 子模块 0411b385）是 x64 Wine + 原生 Box64 完整转译；当前 ARM64 Wine 的 Box 选项为 PE32 使用 wowbox64.dll，PE64 仍走 ARM64EC / FEX。选择 Box 不等于恢复整个 1.4.4 运行时，也不宣称全部旧游戏已回归。完整旧 x64 运行时若要再次交付，需要独立管理 ABI、依赖、前缀升级与图形入口。

## 确认的通用缺陷

- 非 SMC 访存异常被当作 JIT 返回处理，导致同一条空指针指令反复执行。改为交给 Wine SEH，并从故障 ARM64 CONTEXT 还原 x86 GPR、EIP、flags、SIMD 与 x87，再恢复宿主展开栈。
- 原 Windows adjust_arch 为空实现；x87 多缓存寄存器的元数据使用赋值导致相互覆盖，寄存器位置还有移位/掩码括号错误。现补齐 Windows CONTEXT 入口，累积所有缓存位置与类型。
- x87 TOP、逻辑标签、物理寄存器排列和 FloatSave / FXSAVE 内容不同步。按 Box 内部栈约定与 Windows 上下文布局转换，保留故障时的浮点状态。
- SMC protect 经 NtProtectVirtualMemory 改了 Windows 可见权限，而解除保护走 POSIX，导致两边权限不同步。真实 PE32 测试在分配 PAGE_EXECUTE_READWRITE、执行后 VirtualQuery 读到 PAGE_EXECUTE_READ；修复后保留 PAGE_EXECUTE_READWRITE，自修改并重新执行也正确。SMC 的临时保护与解除统一用已注册的 OHOS 宿主接口，正常游戏权限仍由 Wine 管理。
- WoW64 缺少 BTCpuIsProcessorFeaturePresent 导出时，Wine 对所有 x86 CPU 特征查询都返回 FALSE。补齐与 Box CPUID 一致的 MMX / SSE / SSE2 / SSE3、RDTSC、CMPXCHG8B 基线能力；未核实的能力保持关闭。实机查询已确认 SSE/SSE2/SSE3/MMX 为 1；不能将它当作 NVorbis 故障已解决。
- 仅消费确认为 CALLRET 的 ARM64 UDF 0xcafe；其他非法指令送 SEH。SEH 备用 SMC 路径只处理写入且确有 PROT_DYNAREC，避免无条件重试真实写入错误。

这些改动没有按游戏名、哈希或媒体文件替换游戏内容。

## 构建交付

Box 基线、补丁 SHA-256 和源码前后身份固定在 box64-wow64.lock.json，补丁归入 patches/box64。build_box64_wow64.sh 自动应用；不直接发布未提交的子模块 revision。对容器挂载后无法读取父 worktree Git 元数据的情况，检查精确源码文件身份。

ExternalProject 外层 stamp 可能遗漏共享源码变更，脚本每次显式驱动内层增量构建。Makefile / assemble 校验补丁身份与 DLL runtime marker，拒绝混入未修复旧 DLL。stage_box64_wow64_runtime.py 支持只更新现有 Wine ZIP 中的 Box 模块，重算 builtin identity、payload manifest 和安装版本，保留其他运行库；检测重复 ZIP 项和并发内容变化。

## 已验证与边界

普通平板 1.5.6，PE32 wow64_seh smoke 验证读 0x24 / 写 0x28 的 VEH、GPR/EIP、live flags、XMM0、两项 x87 值、异常类别/地址与恢复执行；SMC 测试验证首次执行 42、改写后 84、Windows 权限保留。修复前后结果有对照。

每游戏选择的启动参数、旧配置迁移、Box/FEX 参数隔离、配置 JSON 往返测试通过；overlay 的干净源码重放、幂等、拒绝漂移与错误架构 DLL 测试通过。完整 ArkTS/HAP 编译通过。冷构建整套运行库与其他旧游戏列表尚未复测。

《苍之彼方》已能越过原白屏进入 sprite / 注意事项，但仍观察到 NVorbis 异常及后续 Mono 异常。X87DOUBLE 0/1/2 均不能视为该游戏通过，OP、声音和完整游玩未验收。不能将小程序通过当成游戏兼容已恢复。另有此前 FEX 严格精度对照，见 aokana-nvorbis-x87-20261105.md。

候选设置页仍未在设备验收：之前完整 HAP 安装被系统以空间不足拒绝，没有删除游戏与存档来腾空间。原始日志仅保留在固定工作区任务目录，不提交账号或游戏资源。

## 同 Mono 数值诊断

使用游戏的未修改 Mono 运行独立 C# 数学小程序，在 1..8192 的 entries、1..8 的维数上验证 NVorbis 同类的 Exp/Log/Pow/Floor 计算，并以整数运算校验结果。Box X87DOUBLE=2 的 65536 个组合出现 1407 个差异，例如 Math.Pow(5,1) 得到 4.9999999999999991；Floor 后成为 4，使 codebook 的维数计算越界。补齐 CPU feature API 后该数值差异仍存在。这是现有 Box WoW64 数学精度不足的独立证据，尚未定位全部误差来自哪段指令组合；不得用近整数强行取整等全局启发式掩盖问题。

此小程序没有修改 NVorbis 或游戏文件，私有测试中使用的原 Mono/mscorlib 副本不提交。该实验尚未在 1.4.4 整套运行时做相同测试，因而不能宣称查明了它与旧版的所有差异。

同一 Mono、同一数学程序切至 FEX 严格 x87（FEX_X87REDUCEDPRECISION=0），65536 组合全部通过（0 差异）。这进一步把差异限定在转译精度路径，未通过的 Box 游戏仍不能标记兼容。

FEX 缩减精度=1 的反测为 1398 个差异；严格精度=0 为 0。设备结束时恢复了原 Box DLL（已校验原始 SHA-256），不留游戏未验收的 Box 诊断库；本轮库仍保留在源码增量构建及候选 HAP 中。
