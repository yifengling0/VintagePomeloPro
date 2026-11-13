# FEX exact-store 真机评价与验证链路修复（2026-10-06）

基于 `62ca61faa87351307daa18b59c7d6fd26cbc0db6`，聚焦 RichMan8 的 32 位
性能。Steam 泛白／D3D11 调查暂停。既有 Wine、控制器、Mesa 和子模块 dirty
均保留；本轮未提交、未 push，也没有实现新的浮点快路。

## 结论

exact-store 确实生效，正常精确输入通过实际 WOW64 差分；转换微基准约快
2.83／3.31 倍。但本轮同一个干净 DLL 的 OFF/ON 人物测试没有证明稳定的
游戏帧率提升。长片头和人物缺块仍存在，当前不建议默认打开该实验。

诊断计数显示覆盖率随阶段变化很大，不能用探针的高命中率代表游戏，
也不能将初期片头的约 30% F32 命中率代表整场。下一步应统计 F32 回退
拒绝原因与实际 CPU 成本，连同 x87 add/mul、codec 和 helper ABI 一起定位。
现有证据不支持把所有 32 位游戏性能差异归因于 VirGL 或这一个转换操作。

## 已修复的验证问题

1. 固定 FEX Windows CRT 没有可用的静态析构出口，`fprintf` 也是未实现的。
   诊断改用显式 WOW64 自身退出回调和 Wine stderr bridge，带 PID／final／consistent。
2. 游戏系统调用在 `ExecuteThread` 内部往返，外层 simulation return 通常不发生。
   真机第一版只有探针 final，游戏没有运行快照。现在在 Unix/system-call
   返回之后、JIT lock 重新获取之前汇总，同时保留外层 return 和正常退出出口。
   最多每秒一次、600 次运行快照；外部 force-stop 无 final 保证。600 次用于覆盖
   诊断会变慢的长片头。干净 DLL 不编译这些统计代码。
3. 外层 `make fex` stamp 现在核对诊断 mode、选定 source／version、脚本和
   patch 内容 SHA，以及 WOW64／ARM64EC 宏隔离。缺身份记录的旧 stamp 重建。
   BUILD_DIR／FEX_SRC 传到子构建，无 `.git` 的隔离 source 使用明确固定版本。
   旧诊断源码缺系统调用出口或仍是 300 上限时不能被完整状态检查误认。

`make test-fex-exact-store` 通过 oracle／拒绝向量／补丁重放／ABI 隔离、
真实 reporter 与生产 unlock/call/report/relock 片段执行，以及 Make identity
回归。覆盖输出限频、退出 handle／after 过滤、去重、不一致快照和 cache 污染。
最后诊断 WOW64／ARM64EC 编译及官方 HAP 验签通过。

## 真机探针

MatePad Mini，旧柚 Pro `com.vintage.pomelopro`。每个开关启动前完整结束
Wine/FEX 树，实际 DLL SHA 与包内一致。所有操作由工具执行，未启动 Steam。

- 367,680 输入，每个输入做 F32／F64 写回；48 组 checksum 与 flags，OFF/ON
  全部相同，flags_fail=0。正常精确输入与 Windows x86 参考一致。
- 非规范／特殊输入有 24 行旧 FEX 与 native 差异，OFF/ON 一致；没有声称
  所有 FEX 浮点语义均已修好。
- sticky IE 在两个开关下 before=after=1，EFLAGS 相同。
- 200 万转换、各 3 次微基准，中位 F32 32.8484→11.5969 ms，F64
  31.8432→9.6073 ms。一次 OFF→ON 顺序，不是游戏 FPS 倍数。
- 初版和加入 syscall 出口版诊断探针正常退出均输出一致 final，F32
  6,367,680 次／6,030,576 命中，F64 6,367,698 次／6,221,250 命中。
  诊断包的 bench 数字不用于性能结论。

见 [probe-validation.json](evidence/fex-performance-followup-20261006/probe-validation.json)。

## 干净包的人物 OFF/ON

同一 O2／no-LTO WOW64 DLL，FEX_SILENTLOG=1、JIT／exact-store 统计关闭，
Wine 日志 `-all,+err`。RichMan8 本地 EXE、D3D8／VirGL 路径保持相同；
LOW_MAP=0、DYNAMIC_BUFFER_SYSMEM=0，未改变画质／浮点精度／TSO／SMC。
EntryAbility 的 mode=game 路由直接安装 extraEnv，探针已证明开关有效；
继承 ArkTS abc 中缺少新白名单字符串不等于此次开关失效。

| 场景 | 人物 | OFF 完整帧批次 FPS | ON 完整帧批次 FPS |
| --- | --- | ---: | ---: |
| 本会话首次选择 | money | 57.97 | 58.18 |
| 本会话首次选择 | flower | 57.75 | 57.88 |
| 本会话首次选择 | green | 57.51 | 50.62 |
| 重复选择 | money | 57.87 | 58.49 |
| 重复选择 | flower | 57.57 | 58.34 |
| 重复选择 | green | 56.95 | 58.49 |

这是加载窗口内两端均落在窗口内的完整 120 帧批次，**可能漏掉跨点击边界
的首次停顿，不是输入响应时延或地图稳态 FPS**。首次点击的 render 低点仍在：
OFF 钱／绿分别 48.64／48.29，ON 钱／花分别 45.64／50.16；ON 绿有一个
36.25 FPS 批次。不能据此称稳定回退或改善。所有三个人物添加／移除已逐图确认，
3 秒截图时均已显示人物；缺块在两种状态中仍可见。

仅一轮 OFF→ON，无反序、温度匹配或系统／文件缓存冷启动控制。主菜单动画
可能已预载人物；这里的 cold 标签仅表示本会话首次点击，不保证资源真正冷。
早先 clean-off 显式打开 FEX_SILENTLOG=0、输出 unaligned 调试的会话排除，
也不与以前 ThinLTO／float 诊断包跨包比较。当前 FEX SilentLog 默认本来就是 true。

见 [selection-summary.json](evidence/fex-performance-followup-20261006/selection-summary.json)。

## 独立诊断包命中率

初次真实游戏快照 F32 176,177,285 次，命中 53,415,773（30.32%）；
片头／转场累计至 300 快照时 F32 1,034,728,255 次，命中 606,237,084
（58.59%）。这些是进程内所有线程的累计计数，包含音频，不能当主线程
时间份额。最终诊断包扩展上限后，仍在进入选人之前达到 600 次；
因此没有取得首次／重复选人的有效区间计数，未使用停止增长的旧快照推算。
下面仅列各会话的有效累计快照，两个 PID 之间不相减：

| 阶段 | Wine PID | 快照数 | F32 累计次数 | F32 命中率 | F64 累计次数 | F64 命中率 |
| --- | ---: | ---: | ---: | ---: | ---: | ---: |
| 片头早期 | 464 | 33 | 176177285 | 30.32% | 2529670 | 99.95% |
| 300 次累计终点 | 464 | 300 | 1034728255 | 58.59% | 33692998 | 99.56% |
| 扩展诊断包片头 | 452 | 79 | 341086642 | 30.46% | 7826374 | 99.94% |
| 选人前 600 次累计终点 | 452 | 600 | 1893615956 | 59.41% | 63267347 | 98.92% |

只接受 consistent=true 且 attempt=hit+fallback 的快照；读取时钟表示日志
被读取的时间，不保证等于最后快照产生时间。累计计数包含等待时的动画／音频
运算。诊断包原子计数有开销，这里不使用其 FPS。见
[diagnostic-selection-summary.json](evidence/fex-performance-followup-20261006/diagnostic-selection-summary.json)。

## 长片头 CPU 现场与下一步

clean ON 片头 24 秒 hiperf：12,130 个样本，lost=0。系统隐藏所有 leaf PC；
约 75.3% 周期权重关联游戏主线程的可见 caller，几个 dsound mix 线程约
15.4%。多数 caller 无映射，不能将其作为具体软件浮点 helper 的 CPU 百分比，
也没有测出 off-CPU 等待。没有因此排除 IO、锁或帧同步。

优先增加 F32 拒绝原因分类：canonical ±0、非 normal／特殊编码、低 40 位
非零、输出指数范围；再以同场景实际时间决定实现顺序。可先研究 canonical
零值的严格快路，或正常范围内包含正确 RC 舍入与 sticky flags 的整数写回。
异常与边界保留 helper。不要直接用 double 替代扩展精度，也不要把 MinGW
下明确标注 broken 的 preserve_all 改为 true。若 CPU 主耗仍在 add/mul 或
Miles 解码，继续缩小到实际 guest 函数，不能只增加 store 快路。

人物缺块独立核对最终 draw 的 vertex/index/texture 内容与 alpha/scissor，
不再随机添加 barrier／flush。没有将 main 的 Box 行为当作浮点语义 oracle。

## 最终产物与恢复

| 身份 | clean | 最终 diagnostic |
| --- | --- | --- |
| 未剥离 WOW64 | `3c1bb5f807a4429e2e44690c10b2d3e367c6dfdaee2e999a5fd472d59d75aaf4` | `f84c6632fcf8733d1ebfbb805c00d54ec22e581054e3044f0b97bfe971102d3e` |
| 包内 WOW64 | `002a4a3a399db0743b0563d53ab7880b1d9bfa32ae19b7f513256a1938cbadfa` | `b60e1b565cc3bb64d48c5afdd7d4407540ce5397d1a69d25d79eac68d972018e` |
| signed HAP | `d6f2340843b1f6c76ea18833b6782697c727a68c2a80f8bf3ded8705d6193b27` | `a3df8a74b0e5562f4c2d5371936faaa54667433d3ba610e7a21bacecb86201f8` |

固定 FEX `86ff33bbe299cd8959a6610198c169b67ec419db`，Clang 23.1.0，
RelWithDebInfo/O2、ENABLE_LTO=False。WOW64 COFF-ARM64，EC COFF-ARM64EC。
只替换 WOW64 与 runtime 内容身份，Wine／Mesa／宿主／ARM64EC／ArkTS 字节
继承测试基线，不是整个 dirty 工作区的全量新 HAP。版本仍 proton.26／1004035。

测试结束恢复 clean 包，短跑关闭开关的状态探针，确认实际解压 DLL SHA 为
`002a4a3a399db0743b0563d53ab7880b1d9bfa32ae19b7f513256a1938cbadfa`，再关闭测试会话；开关只作用于启动环境，没有持久化。
见 [final-clean-restoration.json](evidence/fex-performance-followup-20261006/final-clean-restoration.json)。
Windows 原始证据／截图／采样／HAP 在
`F:\VintagePomelo-Workspace\workspace_temp\fex-perf-followup-20261006\`，
报告旁只同步筛选后的脚本和文本数据；未发布原始日志、截图、HAP 或签名材料。
