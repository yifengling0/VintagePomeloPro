# PAL4 / FEX x87 严格优化首轮实施与真机评估

日期：2026-10-07。分支：feature/main_proton；主仓基点 62ca61faa87351307daa18b59c7d6fd26cbc0db6；FEX pin 86ff33bbe299cd8959a6610198c169b67ec419db。

## 结论与实际交付

已实施一个严格 PC24 / nearest-even 乘法快路，并完成 D0 分线程诊断原型、真实 JIT/ABI 正确性检查、同目标差分、机器码审计和五组无 D0 诊断的 PAL4 对照。生产默认 FEX_STRICT_MUL24=0；本轮没有开启 preserve_all、修改通用 ABI、降低 x87 精度或同时升级 Wine/Mesa/FEX。

原版五轮平均 33.44 FPS，候选五轮平均 33.36 FPS。五个相邻配对的平均变化 -0.20%，配对变化分别为 +5.34%, -0.29%, -1.81%, -0.01%, -4.25%。描述性 t 区间 -4.58% 至 +4.17%，仅展示这五组的离散程度；热状态与缓存没有独立控制，不能作完整因果或显著性结论。

这不是把 1.4.5 恢复到 1.4.4 / 60 FPS 的证据。候选只改变乘法本体，宽调用边界及其他运算均保留。暂不默认启用；代码、测试与数据保留以便继续优化。

真机测试结束后已覆盖安装原版 A 包，核对设备 DLL SHA256 为 002a4a3a399db0743b0563d53ab7880b1d9bfa32ae19b7f513256a1938cbadfa，并自动读取同一第二格存档到木屋场景；没有启动 Steam。

| 附件阶段 | 本轮状态 | 尚缺的门禁或结果 |
|---|---|---|
| D0 | 身份、有限线程记录、输入分类、实际 dispatcher 机器码及目标微基准已取得 | 空边界校准、低采样率交叉检查、guest RIP/模块及帧关联未完成，不能确定纯边界占比 |
| P2 | PC24 mul 严格快路及数值/目标正确性测试完成 | 五组游戏对照无稳定收益，未通过默认启用门槛 |
| P1 | 通用调用边界保持原状 | 专用调用契约、异步恢复等门禁尚未完成，未实现轻量边界 |
| L0 | 有输入到场景截图的加载上下界 | 可操作终点、完整等待链及资源阶段归因尚未取得 |

## 数值路径与安全边界

补丁 scripts/patches/fex-wow64-strict-mul24.patch 仅修改 X80SoftFloat::FMUL；编译选择 FEX_STRICT_MUL24=1 仅传给普通 ARM64 的 WOW64 DLL，ARM64EC 排除。

输入域要求 SoftFloat precision=32（x87 PC24）、nearest-even、双方为 canonical finite normal、双方有效数低 40 位为零。两个 24 位整数的完整乘积占最多 48 位；整数规格化后保留完整 remainder，采用 ties-to-even 舍入。仍使用 F80 的扩展指数范围，完全没有转换成 native float/double。

先检查舍入前与舍入后的 normal 指数域，处理舍入进位。所有 guard 通过后才修改结果和 softfloat inexact sticky；拒绝时输出/状态保持不变，原操作数交给现有 extF80_mul。NaN、denormal、非规范编码、overflow/underflow、其他 PC/RC 均保留原路径。没有跨控制字或存储故障边界融合、延迟回写。

## D0 已证实的内容与局限

新增独立补丁 scripts/patches/fex-wow64-x87-boundary-profile.patch。它从最终修正版源码重新导出并正反重放，未混入 FPS 候选。每线程预分配 2048 条，随机有界抽样，热记录不 malloc/锁/日志；安全点有限输出。Frame 只增加受编译开关控制的 8 字节 scratch 指针，独立缓冲保存记录，没有放宽 4KB 页边界断言。

计时源真机核验 CNTFRQ=1,920,000 Hz，Sleep(200) 与 QPC 的三轮推算约 1,919,880–1,919,895 Hz；单 tick 约 520.83 ns。当前数字是包含 gate / 时间戳开销的 raw wall time，未完成空 ABI 的完整校准；不是 CPU cycles，也不是精确的 wrapper self-time。小于单 tick 的平均来自抽样，不能将单条 0 tick 解释为无成本。

青鸾峰木屋外延迟采样窗口得到 4096 条，两线程各 cap 2048，dropped=0：

| Wine TID / PC | 操作 | 样本数 | 完整路径 raw 均值 ns | helper inclusive ns | 边界 raw ns |
|---|---|---:|---:|---:|---:|
| 484 / 24 | add | 771 | 140.5 | 70.3 | 70.3 |
| 484 / 24 | mul | 957 | 125.2 | 49.5 | 75.6 |
| 484 / 24 | sub | 299 | 142.8 | 67.9 | 74.9 |
| 628 / 53 | mul | 981 | 204.4 | 74.9 | 129.5 |

TID 484 的 957 次 mul 中 779 次（81.4%）双方为 normal 且有效数低 40 位为零，满足初始输入筛选；这不是经最终结果指数 guard 核验的完整命中率。另一线程尚未以本轮 PID/TID/模块映射证实为 Miles，不能直接当作 MSS Timer；该线程 PC53 不进入本候选。caller 当前为 JIT LR，尚未完成 guest RIP/模块离线关联，也没有据 cap 样本推算整帧成本。该窗口的随机间隔均值为 32768.5 次调用，满 2048 条后停止收集。

交叉检查显示，无 D0 的简化 PE32 乘法循环热轮约 21 ns/op，而 D0 raw 完整路径均值为 125–204 ns。循环的输入、代码布局、寄存器压力与游戏并不相同，不能直接相减求边界；但这种差距说明 ISB、时间戳、逐次 gate 等扰动不能忽略。当前 75.6 ns 等 raw 边界值不足以证明纯寄存器保存占主导，也不能用于预测 FPS。后续需空 helper 校准及不同抽样率复核。

从设备 dump 的真实 D0 dispatcher 字节反汇编，确认普通 WOW64 为 x0–x3 / v0–v1 参数临时寄存器，而非 ARM64EC 的 x10–x13 / v16–v17。FMUL 路径在 helper 前后保存/恢复 8 个静态和 22 个动态 SIMD，单次往返 SIMD 寄存器数据量 960 字节；动态 SP 区 0x1a0。此处是实际执行指令的数据量，不是 DRAM 流量。额外诊断 recorder 位于 t3 后，计时数字没有直接计算它的耗时，但它仍可能扰动后续执行。

候选原 helper 地址 0x1800aa440，算法函数 0x1800aaf80。编译器新增了 out-of-line FMUL 一层调用、操作数暂存与 16 字节内部栈帧；所有拒绝情况还会调用原 extF80_mul。因此本轮没有把“运算指令更少”当作完整往返已缩短的证明。反汇编和实际 C/C++ -O2、链接命令随身份记录保存。

## 正确性与构建检查

- C 位级差分 2,400,000 组，覆盖 PC/RC、初始 sticky、两种 tininess、特殊编码、halfway even/odd、舍入进位、上下溢边界；命中 292,942、拒绝 2,107,058，0 新差异，拒绝时输出及状态不变。
- 原生 x86/x87 20,000 组实际指令，比较有效 80 位及 inexact，0 新差异。
- 同工具链 ARM64 PE 在同一 Wine 环境完成 2,400,000 组，相同命中/拒绝数，0 新差异。
- PE32 真实 JIT 测试 196,608 次 add/sub/mul/div，GPR、XMM0–7、flags 哨兵、三 PC × 四 RC 及特殊值；原版与候选 failures=0，结果/状态 hash 均 a44794e04dd9e0da。
- 双线程各 32,768 次，独立 PC/RC 持续切换，同时检查 GPR/XMM/flags；两包均 failures=0，thread0 hash=b34bdbd8490baa52，thread1 hash=0e8e6e5ec8567b84。尚未覆盖异步暂停、SVE/AFP 或完整 store fault 专项。
- 原版 FEX 与原生 Windows 在部分状态/特殊值上已有差异，本轮没有混入异常语义修复，不能宣称整个 FEX x87 已完全符合原生。
- 已编译候选、去掉优化编译开关的 ARM64 WOW64 对照、ARM64EC 对照；候选未编入 D0。官方 HAP 签名验证及剥离前后代码节一致性通过。
- 数值补丁、诊断补丁由最终源码导出，独立副本正向重放后的所有改变文件与已测源码逐字节一致，反向 dry-run 通过。
- 构建身份门禁包含开关值和补丁内容；切换 0/1 会重新构建，拒绝旧 stamp 和 ARM64EC 宏污染。真实 Make recipe 回归和相邻 exact-store 回归通过。

## 无 D0 诊断游戏对照

HUAWEI MatePad Mini / MLR-AL10；同一第二格唯一存档、青鸾峰木屋外、800×600、相同镜头和图形参数，音频保持开启。顺序 BAABBAABBA，每轮完整结束目标 Wine/FEX 会话、覆盖对应包、读存档并固定预热 10 秒，然后采 40 秒。只有 WOW64 FEX DLL 和相应 payload 身份不同，其他 Wine/Mesa/native/ARM64EC/ArkTS 成员已逐项核验一致。

| 轮次 | 构建 | 确认输入到场景画面秒数区间 | 新画面 FPS 均值 | 一秒样本范围 | 有效样本 |
|---|---|---:|---:|---:|---:|
| pal4-eval-1-b | mul24 | 29.67–32.92 | 34.40 | 27.57–41.59 | 35 |
| pal4-eval-2-a | 原版 | 29.86–33.01 | 32.65 | 27.95–40.91 | 35 |
| pal4-eval-3-a | 原版 | 29.44–32.61 | 33.38 | 23.38–41.24 | 35 |
| pal4-eval-retry-4-b | mul24 | 29.66–32.91 | 33.28 | 26.91–39.93 | 35 |
| pal4-eval-retry-5-b | mul24 | 29.42–32.62 | 33.40 | 23.77–40.63 | 35 |
| pal4-eval-retry-6-a | 原版 | 29.66–32.86 | 34.01 | 25.98–40.60 | 35 |
| pal4-eval-retry-7-a | 原版 | 27.88–31.06 | 32.91 | 26.98–38.97 | 35 |
| pal4-eval-retry-8-b | mul24 | 29.56–32.81 | 32.90 | 27.54–38.88 | 35 |
| pal4-eval-retry-9-b | mul24 | 29.53–32.75 | 32.80 | 27.06–39.28 | 35 |
| pal4-eval-retry-10-a | 原版 | 27.81–31.05 | 34.26 | 28.31–40.98 | 35 |

FPS 来自宿主 haveFrame 后的实际新到帧计数，约每秒一组；排除首个部分落在窗口外的组。每轮检验 sequence/timestamp 持续推进，旧文件/静止样本不作为性能证据。它比重复宿主合成帧更有意义，但不是完整 guest Present 逐帧序列；不从这些数据虚构帧时间 p95/p99。

加载起点为确认读取输入，终点为截图识别首个木屋场景及固定 HUD，截图轮询约 2–3 秒，只能给时间上下界，尚未证明终点已可操作。上界包含输入调用和截图传输开销。没有按 I/O、解压、JIT、shader 或 GPU 等待完成 L0 归因。

缓存/温控限制：复用现有前缀和相同存档，未清缓存或修改用户资产；文件系统、shader/JIT 缓存未独立快照，属于共享温缓存、有序配对。sysfs 温度、频率及供电状态不可读。没有用 1.4.4 做此补丁的直接因果对照。FPS 候选移除了本轮 D0，但继承基线包原有的可选 JIT 计数入口及日志；所有命名/计数开关关闭，这些既有静态入口在两组相同，不能称全部诊断指令都被编译删除。

## 同目标微基准与并发结果

ARM64 direct 测试比较常见乘法整数本体，PC24 固定；PE32 guest 测试是实际 fldt/fmulp/fstpt 的完整 JIT 循环，结果被消费。它们不是 PAL4 帧率；未提供原 dispatcher → 空 helper 的完整校准。两线程检查独立 FCW 反复切换及寄存器保持，不等价于异步暂停/异常恢复覆盖。

| 微基准 | 原版 | 候选 | 可支持的结论 |
|---|---:|---:|---|
| ARM64 direct 热轮，8,000,000 次 × 4 | 约 8.88 ns/op | 约 5.44 ns/op | 包含 volatile 输入、循环与 sink 的常见数值运算约下降 39%，不是纯 helper self-time |
| PE32 guest 第 1–4 热轮 median | 20.894 ns/op | 18.9995 ns/op | 本次 A→B 单顺序循环观察到下降；无温控，不能宣称完整严格 ABI 或游戏稳定提高 |
| PAL4 五组对照 | 33.44 FPS | 33.36 FPS | 没有稳定游戏收益 |

简化 guest 循环尚未用生成 JIT dump 证明其具体 F80 handler 路径及现场寄存器压力，不能据它替代 PAL4 的完整调用形态验证。

### mul24-a-guest-bench.txt

```text
pass=0 mode=guest-jit count=8000000 ns_per_op=21.044 result=f3d1bc0000000000:3fff
pass=1 mode=guest-jit count=8000000 ns_per_op=20.741 result=f3d1bc0000000000:3fff
pass=2 mode=guest-jit count=8000000 ns_per_op=20.961 result=f3d1bc0000000000:3fff
pass=3 mode=guest-jit count=8000000 ns_per_op=20.827 result=f3d1bc0000000000:3fff
pass=4 mode=guest-jit count=8000000 ns_per_op=21.784 result=f3d1bc0000000000:3fff
complete=1
```

### mul24-a-threads.txt

```text
thread=0 count=32768 failures=0 result_state_hash=b34bdbd8490baa52
thread=1 count=32768 failures=0 result_state_hash=0e8e6e5ec8567b84
complete=1
```

### mul24-b-guest-bench.txt

```text
pass=0 mode=guest-jit count=8000000 ns_per_op=24.851 result=f3d1bc0000000000:3fff
pass=1 mode=guest-jit count=8000000 ns_per_op=18.494 result=f3d1bc0000000000:3fff
pass=2 mode=guest-jit count=8000000 ns_per_op=18.784 result=f3d1bc0000000000:3fff
pass=3 mode=guest-jit count=8000000 ns_per_op=21.070 result=f3d1bc0000000000:3fff
pass=4 mode=guest-jit count=8000000 ns_per_op=19.215 result=f3d1bc0000000000:3fff
complete=1
```

### mul24-b-threads.txt

```text
thread=0 count=32768 failures=0 result_state_hash=b34bdbd8490baa52
thread=1 count=32768 failures=0 result_state_hash=0e8e6e5ec8567b84
complete=1
```

### mul24-target-direct.txt

```text
pass=0 mode=original-softfloat count=8000000 ns_per_op=10.207 sink=0
pass=0 mode=strict-mul24 count=8000000 ns_per_op=5.430 sink=0
pass=1 mode=original-softfloat count=8000000 ns_per_op=8.878 sink=0
pass=1 mode=strict-mul24 count=8000000 ns_per_op=5.442 sink=0
pass=2 mode=original-softfloat count=8000000 ns_per_op=8.876 sink=0
pass=2 mode=strict-mul24 count=8000000 ns_per_op=5.444 sink=0
pass=3 mode=original-softfloat count=8000000 ns_per_op=8.886 sink=0
pass=3 mode=strict-mul24 count=8000000 ns_per_op=5.444 sink=0
pass=4 mode=original-softfloat count=8000000 ns_per_op=8.887 sink=0
pass=4 mode=strict-mul24 count=8000000 ns_per_op=5.447 sink=0
complete=1
```

## 构建与回退

普通构建不设置 FEX_STRICT_MUL24，或显式 FEX_STRICT_MUL24=0。实验构建设置 FEX_STRICT_MUL24=1，用新的独立 BUILD_DIR；该变量是构建开关，不是游戏运行时选项。维持 FEX_EXACTSTORE=0，避免同时改变另一因素。

主机数值回归：make test-fex-strict-mul24 FEX_SRC=/path/to/materialized/pinned/fex。真实 JIT 和线程 probe 使用 llvm-mingw i686 目标编译 smoke/fex_x87_abi_probe.c / smoke/fex_x87_threads_probe.c 与 smoke/fex_x87_abi_probe.S，在目标 Wine/FEX 下运行，比较原版与候选的 hash。

目标 probes 的正式构建入口已实际跑通：

```sh
bash scripts/build_fex_x87_probes.sh /path/to/materialized/patched/fex /path/to/llvm-mingw /path/to/new/probe-output
```

D0 只在独立 staged source 上应用 boundary-profile patch，以 FEX_WOW64_X87_PROFILE=1 编译诊断 DLL；不要放入 FPS 候选。运行 FEX_X87PROFILE=1，并设置有界 delay、seconds、span。普通 scripts/build_fex.sh 没有默认接入该诊断宏。

运行环境变量为 FEX_X87PROFILE_DELAY（秒，默认 15）、FEX_X87PROFILE_SECONDS（默认 45）及 FEX_X87PROFILE_SPAN（随机间隔范围，默认 8192）。具体构建的完整编译/链接命令见 final-artifact-audit.json；不能将编译宏当运行环境变量使用。

原版 HAP SHA256 d6f2340843b1f6c76ea18833b6782697c727a68c2a80f8bf3ded8705d6193b27，设备 DLL 002a4a3a399db0743b0563d53ab7880b1d9bfa32ae19b7f513256a1938cbadfa。
候选 HAP SHA256 960b0b3f411fe59fc12b9403f95b7eef01a111ba4ae7fb411dd9b98dff6bc244，设备 DLL d49c9e645c82137d5824c05a6d5d8ee493d407bfffa3d8b502f217cb9404706d。
诊断 HAP SHA256 824aa809e717d235d54d1c1cf65e22216457e5f1313e8c62e456ee784cd1be60，设备 DLL 580ecdb4e04f1f49c7a337a7f0a11dd17963cd6397aa88d75e70ce005087d319。

HAP 继承已确认的实验基线，不是把整个当前 dirty 工作区全部重建。剥离前 SHA、工具链、完整编译/链接选项、代码节 hash 和最终补丁 hash 见 evidence/pal4-x87-optimization-20261007。原始截图/日志/采样、HAP、游戏资产及签名材料只保留本地。没有执行 commit/push。

## 审查入口与紧凑证据

仓库报告路径为 docs/architecture/pal4-x87-optimization-20261007.md；相邻 evidence/pal4-x87-optimization-20261007/README.md 列出十轮结果、包身份、最终补丁/编译审计、数值/线程结果、D0 汇总、输入分类与反汇编。SHA256SUMS.txt 覆盖紧凑证据文件，不包含原始操作数 records、大型日志、截图、二进制或签名材料。

数值补丁为 scripts/patches/fex-wow64-strict-mul24.patch；独立诊断补丁为 scripts/patches/fex-wow64-x87-boundary-profile.patch。其最终内容哈希分别为 02489f11b5c49261d49d6225bd4df39879373cd4f25090df54ab9b444647f2b4 和 c95d750af78f86cb41a6a62177a49bc0489a41417d7e973e32638849258e11ea。构建开关和身份门禁的修改叠加在既有未提交工作上，integration-existing-inputs.json 记录本轮实施前四个文件的内容哈希；不能把整个当前 git diff 当成本轮单独改动。

## 后续实施的优先项

1. 单独验证 FMUL 的局部内联，检查消除新增内部函数调用后的机器码与完整 PE32 JIT 往返；它是与本候选不同的编译因素。
2. P1 优先采用白名单严格整数路径的 JIT 内联，成功路径不调用 C，拒绝域完整回到现有 spill/helper/fill。以已验证整数算法为参考，明确普通 ARM64 与 ARM64EC、NZCV、SIMD 全宽及异常恢复契约；不能仅凭一次反汇编删通用保存。
3. 补多线程异步暂停、SVE/AFP 相关模式及同步 fault 门禁，再评估缩小边界；本轮没动边界，不能称 P1 已完成。
4. 将加载输入到可操作场景完整标记，独立采解压、JIT 与资源/GPU 等待链；本轮约半分钟的加载尚未解决。
5. 严格 PC53 数值路径只有在对该线程做模块关联与输入分类后再单独实施；音频 CPU 收益与主线程 FPS 分别验收。
