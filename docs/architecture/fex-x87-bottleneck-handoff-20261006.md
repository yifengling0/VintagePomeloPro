# RichMan8 / FEX x87 关键路径与修复交接（2026-10-06）

用户要求找到关键点后暂停，由其他模型接手。本次按最新要求发布分析、实验补丁和必要文本证据；不再进行编译、采样、安装或游戏操作。调查结束时的现场状态仅供参考，接手前须重新核对。

先读 [证据入口与复现说明](evidence/fex-x87-20261006/README.md)。本次是资料交接提交，实验补丁放在 evidence 中，未接入生产默认构建。历史报告与待整合源码快照也已归档。

## 可以直接据此推进的结论

已定位到一个具体、可修改且已有正确性验证的开销来源：**32 位游戏及其 Miles MP3 解码器产生大量 x87 运算，经 FEX 的通用 ABI 保存／恢复和 SoftFloat 软件浮点回退执行。** 这条路径不能因为 Mesa 已原生 ARM64 就自动消失。

这不是“所有 VirGL 游戏减半的唯一根因已证实”。当前有力证据集中在 RichMan8；缺块和长片头仍未修复，64 位游戏未建立同条件证据。

### 1. 首次人物处理的浮点回退量明显增加

来自 [float-summary.json](evidence/fex-x87-20261006/main-proton-isolation-20261006/float-summary.json)，同一诊断会话 Wine 主线程 TID 428：

| 窗口 | 记录时长 | 回退次数 | 每秒回退 |
| --- | ---: | ---: | ---: |
| 首次人物切换 | 34.169 s | 349,865,894 | 10,239,369 |
| 重复人物切换 | 34.128 s | 113,597,011 | 3,328,509 |

首次窗口：store32 135,003,345 次（38.6%），mul 118,507,019 次，add 80,269,999 次。主线程几乎全部 PC=0（24 位有效精度），另一线程 TID 556 约 500 万次/s、以 PC=2 为主。

这些是调用次数，不是 CPU 时间占比。计时只采每类每 1021 次的一次，不含 ABI 保存恢复，不能将采样纳秒乘 1021 当总耗时。部分 load 已在 dispatcher 内联，不在计数中。TID 480 的长累计窗口跨过采样边界，不应纳入本轮速率。

### 2. 调用来源已从“FEX 内部”追到具体 guest 模块

新脚本 `attribute-fallback-callers.py` 重新解析**已有** PID 4062 的同进程 Hiperf + FEX map；结果 `fallback-caller-attribution.json`。不用重新装包即可复核。

冷窗口中可见的栈关联包括：

| 线程／ABI | 可见 guest 调用来源 | 样本数 |
| --- | --- | ---: |
| 主线程，F80 二元运算 ABI | `strat game.exe` | 704 |
| 主线程，F80→F32 ABI | `strat game.exe` | 332 |
| MSS Timer，F80 二元运算 ABI | `mssd/mssmp3.asi` | 800 |
| MSS Timer，F80→F32 ABI | `mssd/mssmp3.asi` | 196 |

例如原始可见栈 `0x7ffffe1758 (FEX_ABI_F80_I16_F80_F80_PTR) → 0x7ff5ec800c (mssmp3.asi)`；主线程 `0x7ffffe1750 → 0x7ff5e7161c (strat game.exe)`。另有不能恢复 guest caller 的样本，保持 unknown。

这确认相关软件浮点路径同时来自游戏执行和音频解码，不应继续把这些开销统称为纹理 IO 或 Mesa 编译。Hiperf 隐藏实际 leaf PC，以上是可见栈关联，**不是精确叶函数耗时**。旧采样首个人物无效，只用于调用归因，不拿旧整轮做严格帧率比较。不同会话的线程号不能直接对应。

### 3. 当前 GL 计时不支持“复制或 shader 编译独自解释数秒卡顿”

干净候选开启后的首次窗口，`gl-summary.json` 按前后日志尾锚点对齐：

- 1701 次 swap；shadow_in 累计复制 25,598,099,456 字节，用时 1475.592 ms，约 0.87 ms/swap。
- 240 次 tex_subimage 合计 159.203 ms，最大 10.688 ms。
- 一次 shader compile 3.726 ms，一次 program link 9.256 ms。
- wgl_swap 最大 139.567 ms；仍需区分提交阻塞与上游供帧停顿。
- 主线程／CS 之外的 IO、解码、同步等待没有被这些 GL 计时覆盖。不要把不同层嵌套计时直接相加。

复制仍有优化价值，但之前低地址共享映射／系统内存暂存已减少复制，仍未消除卡顿或缺块。无需再次把“先消灭所有复制”作为找到主因的前提。

## 已完成的修复候选

[fex-exact-store.patch](evidence/fex-x87-20261006/fex-exact-clean-20261006/fex-exact-store.patch) 是**无 FloatDiagnostics 依赖**的候选；源码在权威 WSL 仓库的 `workspace_temp/fex-exact-clean-20261006/source`。

仅修改：

1. `FEXCore/Source/Interface/Context/Context.h`
2. `FEXCore/Source/Interface/Core/Core.cpp`
3. `FEXCore/Source/Interface/Core/Dispatcher/Dispatcher.cpp`

开关 `FEX_EXACTSTORE=1`，默认关闭。在 `FABI_F32_I16_F80_PTR`／`FABI_F64_I16_F80_PTR` 通用 ABI spill 前做整数位转换：规范 ext80、显式整数位为 1、低 40／11 位为零、目标 exponent 是正常 float／double 范围才进入快路。其余编码、舍入、NaN、Inf、零、denormal 等全部走原 helper。保存 NZCV，保留 TMP4 fallback pointer，慢路前保留 VTMP1。

数学上只转换可精确表示的正常值，不改变 x87 精度／舍入模式，也不依赖 FPCR。不要将 PC=0 误解为可无条件用 ARM float 替换：x87 即使有效位数为 24，指数范围仍不同。

### 正确性和构建证据

- 真机同一 PE32 probe，3 种合法 PC × 4 种 RC × 4 类输入：367,680 个输入，每个分别写回 float／double；48 组结果 bit checksum、IE checksum 和 EFLAGS 检查，on/off 一致，flags_fail=0。
- 正常精确值与真实 x86 参考一致。特殊／非规范输入存在旧 FEX 与真实 x86 的既有差异，on/off 一致；不能宣称整个 FEX 已完全 IEEE 合规。
- 干净候选的三次中位、每次 200 万次精确转换：F32 30.3526 → 9.1614 ms；F64 34.0318 → 9.1745 ms。只是微基准，不是游戏提速倍数。
- 与 ThinLTO 基线比较 20,622 个源码文件，仅上述 3 文件变化；反向 patch 精确恢复，`source-validation.json`。
- 独立 `build-fex-exact-clean-20261006`；构建成功、PE imports 无变化、剥离前后装载 sections 一致、官方验签通过。
- 仅换 WOW64 FEX DLL 和配对 payload/version/manifest。Wine、Mesa、宿主、ARM64EC DLL、ArkTS 继承同一基线。
- 构建时 NeedDisabledSVE.py 在 x86 构建机打印异常并退回禁用 SVE；与此前实验一致，最终构建成功，日志保留。
- **未接入生产 `scripts/build_fex.sh`。正常构建尚不会自动带入此优化。** 不要覆盖 thirdparty/fex 或已有 dirty。

## 游戏验证完成到哪里

第一轮 HAP 含默认关闭的 FloatDiagnostics 代码，开关前后同包，仅改 FEX_EXACTSTORE。每个状态一个会话，先 off 后 on；以下是点击后 7 秒内宿主 120 帧统计窗口最低 FPS：

| 状态 | 钱夫人 | 花帽女孩 | 绿发女孩 |
| --- | ---: | ---: | ---: |
| off，首次 | 20.97 | 28.87 | 29.06 |
| on，首次 | 32.26 | 32.63 | 31.97 |
| off，重复 | 50.67 | 52.55 | 50.43 |
| on，重复 | 52.31 | 53.69 | 51.96 |

这是改善迹象，尚无反序多轮／温度匹配，不能称稳定收益。数据在 [comparison.json](evidence/fex-x87-20261006/fex-exact-store-20261006/comparison.json)，24 张操作截图及 4 份采样完整，lost=0。

第二轮去掉 FloatDiagnostics 代码：**仅完成 on 的首次／重复，off 尚未采集，用户要求此处暂停**。on 首次 28.71／36.72／37.79，重复 56.82／58.07／57.82。不能用它与第一轮 off 跨包比较得出收益。JIT diagnostics 关闭；`winehua_perf` 在两种计划状态中均开启，不能声称全诊断关闭。

角色头发、身体缺块在两种 CPU 后端和所有这些候选中仍能看到。softpipe 实验没有得到有效游戏画面，因此没有排除 VirGL。不要将缺图归到 x87 优化上或声称已解决。

## 架构归因纠正

本地 main/origin/main 为 `5383b784`，未 fetch。生产 `wine_env.h::Box64EmulatedLibs()` 明确把 libGL/libEGL/libGLES 列为 emulated：main 的 Wine Unix 与 Mesa 是 x86_64 路径，经 Box 执行；不是 libGL 包装直接跳 ARM64 Mesa。

Proton 的 Wine Unix／Mesa 是 ARM64，PE64 还有 ARM64EC 混合路径；本例是 PE32，游戏和相关 i386 Wine PE 模块仍经 FEX。不要把“Proton 有 ARM64EC”直接等同于本次 PE32 热点均在 ARM64EC。

旧同包 Box/FEX 对照仅说明更换 CPU 执行后端会影响本场景，不能代表 main 整套架构。Box 的扩展精度检查未通过，不是等价正确性目标。用户最新方向是**优先定位 Proton 自身的真实瓶颈**，main 仅作架构与历史实现参考。

`packaged-pe-math.json` 是实际 HAP 内 i386 DLL 反汇编；wined3d/d3d9/dsound/winmm 可见 x87 主要是 load/store/整数转换，没有扫描到 x87 add/mul。这进一步提示应查游戏／codec；它只是静态扫描，剥离后的最近导出标签可能跨多个函数，不是执行成本证明。不能依据旧构建目录的 flags 就重编所有 PE DLL 为 SSE。

## FEX 已成熟，为什么本移植仍可能特别慢

用户进一步询问：其他游戏是否同因，以及 OHOS 迁移是否漏了已有优化。不能用 RichMan8 一例覆盖全部游戏；当前找到了一个**具体的平台构建差异**，但未量化其在整帧中的份额：

- 固定版本 `thirdparty/fex` 的 **Git HEAD 原始代码** `CMakeLists.txt:223` 已在 MINGW 下写明 `Ignoring broken clang::preserve_all support`，把支持设为 FALSE。该限制不是本轮 OHOS 补丁临时加入的。
- `FEX_PRESERVE_ALL_ATTR` 被置空、`FEX_HAS_PRESERVE_ALL_ATTR=0`。`Source/Common/HostFeatures.cpp:660` 将它传给 `HostFeatures.SupportsPreserveAllABI`。
- `FEXCore/Source/Interface/Core/ArchHelpers/Arm64Emitter.h:202` 的普通路径执行 `SpillStaticRegs + PushDynamicRegs`；返回时 `PopDynamicRegs + FillStaticRegs`。支持 preserve_all 时改走更有选择的保存路径。
- 本工程 FEX 是 MinGW 编译的 ARM64 PE WOW64 DLL，所以每秒千万次 fallback 可能放大这种调用边界成本。不能把 Linux FEX 的经验性能直接套到此构建，也不能把全部损失叫作“OHOS 系统调用慢”。
- x87 fallback 本身是为保留扩展精度／指数／舍入等语义而存在，并不自动表明移植实现错误。当前已验 O2/RelWithDebInfo、ThinLTO、实际 DLL 哈希；未发现本候选误跑 Debug／旧 DLL 的证据。
- 原始代码明确标记 Windows preserve_all 为 broken，**不能只改成 true**。下一位可优先研究当前编译器／Windows ABI 是否已有受支持修复，或设计经过寄存器、异常展开和返回路径验证的专用 helper 调用约定；先统计真实回退的保存恢复指令成本。现有 exact-store 通过避开 helper 调用来回避这部分成本。

更准确的待验证假设是：“x87 密集游戏 + 当前 Windows/WOW64 回退 ABI 成本”在移植环境下形成热点；不是“FEX 原本已经解决，所以一定是 OHOS 算错”，也不是“所有低帧率都来自 x87”。下一位应先沿这个有源码和采样共同支持的边界继续，避免重新泛查整套 main。

## 接手后的最短路径

1. 若当前会话仍在，先完成干净候选 off 的同样三个人物首次／重复，验证现有补丁；无需重编和切 main。按当前截图操作，不要再次让用户跑 BAT。
2. 然后做**一次有界的 Proton 热点定位**：带 guest block 地址的 JIT map、主线程和 MSS Timer 栈、单次首选人物事件；把热点地址对回 `strat game.exe`／`mssmp3.asi` 的具体函数／指令。保留不在 CPU 上运行的等待证据，避免将解码 CPU 忙与 IO/锁等待混为一谈。已有模块级关联足够缩小调查范围。
3. 修复优先级：先复核并接入 exact-store；对仍大量回退的 x87 add/mul 研究有严格条件的原生／整数快路。限制 FCW、输入可表示性、输出指数范围，异常及边界继续 fallback，沿用当前 PC/RC/边界 probe 扩展验证。**不要直接启用 reduced precision 或源码标记 broken 的 preserve_all。**
4. 若证实主要时间在 MP3 解码，考察兼容的原生解码接口或解码结果复用能否避免反复 guest 计算；不能只静音，静音通常仍解码，也不能未经定位替换游戏 DLL。
5. 缺块单独定位：固定一个人物，在最终 draw 前对齐 vertex/index/texture 身份、更新范围和内容；先证明缺失发生在上传还是 draw/alpha/scissor。不要重复做未证实的随机 barrier/flush。

## 当前可直接接续的设备和命令

- 权威 repo：WSL Ubuntu-22.04 `/home/liufeng/src/vpp-proton`；Docker `vp-proton` 映射 `/data/src/winehua`。
- 分支 `feature/main_proton`，HEAD `284026ce64c631961394f2aee6fadb3a4380e579`。已有 Wine、控制器、Mesa、0037、FEX、vkd3d、dxvk-modern dirty 均保留。
- 目标 `com.vintage.pomelopro`。不要操作 `app.hackeris.winehua`。发布版 `automate.py` 通过 `VP_HDC_TARGET` 指定目标，未发布设备序列号；输出账本脱敏。
- 调查结束时记录的现场（未经本次发布操作重新核对）：干净候选 **FEX_EXACTSTORE=0**，选人空列表；宿主 PID 4170、游戏 OS PID 5478（`strat game.exe`），需接手时重新核对存活。尚未选择钱夫人／花帽／绿发三人。
- 调查结束时截图 `clean-off-selection.png` 保留在本地；当时应用及游戏未退出，无采样或操作脚本继续运行。环境参数仅限会话，无持久配置变更。

Windows 本地证据目录仍保留原脚本。若仍是同一现场，可接续以下命令（发布版脚本位于 evidence，需先设置 `VP_HDC_TARGET`）：

```powershell
python workspace_temp\fex-exact-clean-20261006\automate.py shot resume
# 看截图确认空列表，并核对实际 PID 后：
python workspace_temp\fex-exact-clean-20261006\capture.py off-cold --pid 5478 --host 4170
# 看 contact，确认三人添加与删除有效，再采：
python workspace_temp\fex-exact-clean-20261006\capture.py off-warm --pid 5478 --host 4170
python workspace_temp\fex-exact-clean-20261006\summarize.py
python workspace_temp\fex-exact-clean-20261006\summarize-gl.py
```

当前安装 signed HAP SHA256：`1ef71d635275f6eaeefc739a9a9517a7593ceb58377e5d1612b2e477e62bff33`。
unsigned：`5cebae8b9ee7426c23db6c371a330b55bc7e238f40f8501013e34767922820d5`。
设备已验 WOW64 DLL：`fe24f844ba4481c313ec715653a24c2c0f8f3547d13b9bfe16b4e949537911f7`。

`package-identity.json`、`build-validation.json`、`source-validation.json`、`probe-validation.json`、`clean-install.txt`、`exact-probe-on-identity.txt` 记录完整身份和验收。

## 证据目录和排除项

- [main-proton-isolation-20261006](evidence/fex-x87-20261006/main-proton-isolation-20261006/)：同包 Box/FEX 诊断、float 诊断、架构纠正脚本。
- [fex-exact-store-20261006](evidence/fex-x87-20261006/fex-exact-store-20261006/)：含 float 诊断代码的第一版 exact-store 完整 on/off；使用 `exact-cpu-current.tsv`。**`exact-cpu.tsv` 是误取的旧结果，排除。**
- [fex-exact-clean-20261006](evidence/fex-x87-20261006/fex-exact-clean-20261006/)：无 float 诊断代码的候选、正确性 on/off、游戏 on 样本、guest 来源归因及交接。
- Windows 各目录 `SHA256SUMS.txt` 覆盖本地顶层证据文件，排除哈希清单本身和 Python cache；原始采样、HAP、源码／构建目录均不应直接加入提交。
- 权威 repo 同步脚本、文本和本交接，Windows 保存截图、原始采样和签名包。本次只发布筛选后的文本数据、报告和补丁，未发布完整原始日志或修改记忆。

## 本次资料发布校验

远端 `origin/feature/main_proton` 在发布前核对为 `284026ce`，与调查基线一致。发布集有独立 [SHA256SUMS.txt](evidence/fex-x87-20261006/SHA256SUMS.txt)，只覆盖实际提交的 evidence 文件。本地 Windows 全量清单保留用于核查未上传的大文件，不能用于验证这个筛选后的目录。
