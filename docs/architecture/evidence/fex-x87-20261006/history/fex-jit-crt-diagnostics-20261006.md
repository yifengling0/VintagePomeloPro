# RichMan8：FEX JIT 诊断路径与内部 CRT 文件接口修复

日期：2026-10-06。基线 `feature/main_proton` / `284026ce64c631961394f2aee6fadb3a4380e579`，FEX pin `86ff33bbe2`。本轮未 fetch、提交或推送。

## 结论与范围

此前 JIT 入口无法取得有效 perf map，CPU 采样中的未知地址不能归因给 FEX 或游戏。本轮修复 Windows JIT 输出路径，并复现、修正 FEX 内部 CRT 文件接口错误；主机回归、独立构建、包内容及签名核对通过，候选已覆盖安装，保留用户数据。应用和游戏仍由用户手动启动。

**已在真机确认新 DLL、JIT 配置和有效 perf map；仍没有帧率提升结论。**首次人物卡顿、32 位 VirGL 与 main 的性能差异、人物缺块仍未解决。CRT 补丁影响 FEX 自身配置／诊断／缓存的文件接口，不能据此声称已修复游戏资源读取。

## 修复内容

`scripts/patches/fex-windows-jit-symbols.patch`：Windows 下优先使用显式 `FEX_JITMAPDIR`，其次 `TEMP`，最后当前目录。原先固定 `/tmp` 的路径会经 Windows CRT 按 DOS 路径解释，受当前盘及 cwd 影响。Android／Linux 原路径保留。修正 `WriteBuffer` 的时间差方向，使用 `Now - LastWrite`。显式提供非空目录时，WOW64 只输出一条 `[FEX-JIT-DIAG]`，记录 Wine PID、library/global/block 命名开关及目录；默认命名仍关闭。

`scripts/patches/fex-windows-crt-file-open.patch`：

- `_O_RDONLY` 为零，旧按位与判断永远不成立；现在正确请求 `GENERIC_READ`。
- ANSI `_sopen` 改为转发 `_wsopen`，保留共享模式和可选权限；旧调用 `_wopen` 将共享模式当作权限。
- RDWR 的共享写处理检查 `GENERIC_WRITE` 位，覆盖读写并存的访问模式。

两份补丁均通过 `scripts/build_fex.sh` 的独立、可重复入口应用，Makefile 纳入依赖与时间检查。保留既有 synthetic-return 修复，未修改 vendor checkout。

## 主机验证与包身份

已通过 `test-fex-jit-symbols`、`test-fex-crt-file-open` 及相邻 `test-fex-wow64-synthetic-return`。测试提取实际生产函数，文件 API 由捕获桩验证；旧只读／共享错误可复现，正反补丁重放一致。它们不构成真机 Windows 文件 I/O 测试或 ARM JIT 性能测试。

独立目录 `build-fex-jit-symbols-20261006` 已完成 AArch64 PE／ARM64EC 构建；staged source 位于 `workspace_temp/fex-jit-symbols-20261006/source`，继承已验证补丁后加入本轮修复。实际配置为 RelWithDebInfo，`-O2 -g -DNDEBUG`，未降低 TSO、SMC、x87 精度或画质。

候选以手柄配套包 `09c7f532...` 为基底，只更新 payload 中的 WOW64 DLL。HAP 仅有 wine-data.zip、wine-runtime-manifest.json、wine_runtime.version 三项内容变化；ARM64EC、Wine、Mesa、宿主和 ArkTS 内容逐字节继承。非 discardable PE 加载节与新编译输出一致，PE 导入依赖与旧 DLL 一致。签名后原 archive 内容不变，官方 verify-app 成功。

| 产物 | SHA256 |
|---|---|
| 编译 WOW64 DLL | `34ed9b92b0625ae6305091decb6b6a858419e4fccccc1bcb96f4e696219eb7e1` |
| 包内 WOW64 DLL | `2d16d500c8dc5153f01743cedb8473fc740c40b8bd5918abbf9698e75dfbab43` |
| 未签名 HAP | `bb0efe64712dbb1f24a4012c8d820b50fa9bcae6e87d88835c8532f073fa6af6` |
| 已安装签名 HAP | `ebc72140d68bba993a65447dea688dd0053634a1a465f28b3be40dfbf0e9bd54` |
| payload | `b9d7febad14975e194e5c24a1dc92079cb8f11b24d980ea7a4953d0466170e84` |

版本仍为 `1.4.5-proton.26-alpha / 1004035`。runtime marker：`content=b9d7febad14975e1;wine-valve=cd547f7a0e;fex=86ff33bbe2;arch=arm64-v8a:aarch64`。更新 marker 用于触发实际解压新 DLL，用户手动启动后，设备 WOW64 DLL 的 SHA256 已核对为 `2d16d500...`，marker 与该 payload 一致；不是仅凭 HAP 判断实际运行身份。

## 前序真机证据边界

证据根：`F:\VintagePomelo-Workspace\workspace_temp\controller-followup-20261006\`。

- 同期首次人物样本 FPS 为 58.09／57.75／29.10／19.69／24.03，重复阶段为 59.15／57.68／56.99／58.35；用户确认处理一次后切换流畅。
- 首次／重复 WGL 最大锁等待分别 0.135／0.112 ms，不足以解释长停顿。首次 TexSubImage 528 次、总 352.032 ms、最大 18.706 ms；窗口内未记录新 shader compile/link，不能推断不存在其他资源准备。
- 重复阶段搬运更多却更流畅，复制量不是已证实的冷卡根因。此前共享映射虽生效，用户没有感知整体提升。
- 旧 map 没有成功读出，不能声称命名开关生效。旧游戏 PID16689 仍运行时，第二实例 PID19325 发生 `0xC0000005`；不能作为干净性能对照，也未证明崩溃由命名设置引起。fault-maps 快照被 524287 字节上限截断，缺失地址不能证明未映射。
- 12:11:42–12:12:14 的 PID21177 Hiperf 为 49925 samples、lost0，实际截图停在 SOFTSTAR；目录 `observe-map-jit-background-profile` 名称不代表后台／选人稳态。12:12:05–12:12:37 日志没有新 GL/FPS 样本，因此也不作为人物切换采样。
- Hiperf 大量 leaf PC 隐藏，可见 caller 只能用于模块身份分析，不能冒充精确 CPU 执行成本。perf map 没有时间戳，地址复用／不同模块重叠须保留未知或歧义。

## 安装与下一轮入口

2026-10-06 12:25 已覆盖安装至 MatePad Mini 的 `com.vintage.pomelopro`。HDC 输出明确成功，bm dump 核对版本，安装前后确认目标进程退出；没有卸载、清数据、自动启动或输入命令。命令 ledger 与脱敏记录保存在 `workspace_temp/fex-jit-symbols-20261006/`。

新 `C:\vp-richman8-jit-trace-v2-20261006.bat` 已发送并回读逐字节核对。只供用户手动运行一次，准备 `C:\vp-jit-export`，保持游戏 cwd，设置 library/global 命名开启、block 命名关闭，以及原有 low-map off／Unix OpenGL 计时参数。无需另运行 export BAT。

上述核对均已完成：实际诊断为 `[FEX-JIT-DIAG] wine_pid=764 library=true global=true block=false directory=C:\vp-jit-export`。同进程 map 从 366102 字节／6244 条增至 461119 字节／7735 条。map 使用 **Wine PID**，不是 OHOS PID；物理路径为 `/data/app/el2/100/base/com.vintage.pomelopro/files/.wine/drive_c/vp-jit-export/perf-<WinePID>.map`。以同进程采样分析首次／重复人物准备，再决定实际性能改动。命名诊断本身有成本，应在诊断关闭、同条件环境复测后才判断性能收益。

## 本轮同步真机采样

应用、游戏、BAT 和所有交互均由用户手动执行；工具没有启动程序或发送输入。同一 RichMan8 OS PID27732、Wine PID764，renderer PID26838。后续恢复连接必须重新确认 PID。

| 场景／日志目录 | 设备时间 | 宿主 FPS 样本 |
|---|---|---|
| 主菜单，observe-jit-v2-initial | 12:37:25–12:37:56 | 59.16 / 60.32 / 60.15 |
| 人物界面，observe-jit-v2-character-transition | 12:38:36–12:39:37 | 20.82 / 18.89 / 21.11 / 32.55 / 50.72 / 51.94 |
| 选人进入剧情，observe-jit-v2-dwarf-switch | 12:44:18–12:45:04 | 56.70 / 11.46 / 19.40 / 37.97 / 57.88 |
| 新场景进入地图，observe-jit-v2-new-scenes | 12:46:28–12:46:59 | 18.38 / 40.46 / 42.30 |

用户确认“点击了几个新画面，都有卡顿”。第二次 DWARF 采样包含选人→剧情，随后采样进入地图；**不是纯重复人物对照**。目录中的 first/warm 名字不改变实际场景，没有逐人物打标，不能形成严格 cold/warm 或 main/FEX A/B。

最后新场景段：TexSubImage1185次／287.814ms／最大6.670ms；compile_shader38次／49.372ms／最大3.820ms；link_program24次／104.882ms／最大9.548ms；BufferSubData2518次／376.559ms／最大76.086ms；客户端swap1079次／5287.548ms／最大133.786ms。WGL锁最大0.404ms。shadow_in17.949GiB／984.766ms。API墙钟可嵌套，不能直接相加，聚合首窗口也可能跨过采集起点。

宿主GL成帧通常2–5ms、failed_swaps=0、真实SHM上传为0，不能据此排除整个图形链阻塞：客户端已有约134ms交换与76ms缓冲更新等待，仍需区分资源/GPU完成等待和宿主呈现节奏。

人物缺块仍独立存在：`observe-jit-v2-new-scenes/end-screen.jpeg` 显示地图中央角色眼部被背景色矩形遮挡，截图HUD52FPS，UI正常。不能归因成已确认的alpha问题；上传范围、stride、裁剪／深度、动态缓冲有效数据及生命周期仍待定位。

## DWARF 和符号的实际能力

FP下大部分leaf PC隐藏，许多用户栈为空；DWARF4096恢复了更多调用链，可关联同进程FEX map。游戏15秒探测8444 samples、45秒混合场景50979 samples、30秒新场景38023 samples，均lost0。新场景游戏进程16290 samples中16271条leaf隐藏；caller身份和采样cycle权重不能冒充精确执行成本或off-CPU等待。

小通用区 `0x7ffffe0000/0x1ae0` 尚未细分调度与ABI辅助；本轮不能把全部匿名JIT叫作游戏或FEX解释器成本。后续补丁的helper命名只能解释新进程map，不适用于旧地址。

可见调用链包含 `NtDelayExecution → NtYieldExecution → getrusage / sched_yield`。已核对本地main的Wine pin `1cb1ac93...`，两次getrusage实现与Proton相同，因此不称作新引入的分支差异或已证实共同根因，也未删除Sleep(0)语义。

## 独立 PE32 CPU／边界测试

源码 `smoke/winehua_pe32_route_probe.c` 已新增并部署；用户手动入口 `C:\vp32-route-probe-fex-20261006.bat`，结果 `C:\vp32-route-probe-<WinePID>.tsv`。整数、深度16调用返回、REP复制、SSE、x87、QPC和Sleep(0)各测3次。已确认机器码包含相应指令、循环及递归CALL；未开启fast-math，禁FP contraction，x87只在测试进程设PC64并恢复。checksum与精度检查是对照前提，不是完整正确性证明。

已部署EXE为65536字节、SHA256 `6bd5319d02653fa4c58b4f660fdce44ddcbe892421dedc21d6601433eb2109ef`；源SHA256 `bc2619f9fd2a6fb163250e40be0ac65cd9340e255a7e74b15e10052648365d21`。重新只读复核时无谓重编译曾因PE时间戳生成另一hash，已将repo证据镜像恢复为Windows及设备的6bd531版本；未把另一hash称作已部署。

此前电脑 HDC Offline，实际读取返回 `[Fail][E001005] Device not found or connected`；用户切换 USB 调试后，14:46:25 已恢复 Connected，当前文件读取正常。没有重置共享 HDC 服务或自动执行测试。14:52 用户手动运行 CPU BAT 后，进程在 FEX JSON 配置初始化阶段触发断言并退出，没有生成有效测试结果；控制台 Exit code: 0 不代表成功。详见下一轮报告中的实际异常链。

## 下一轮

默认关闭的 JIT 编译计时与 helper 细分候选已独立构建、签名并核对，详见 [fex-jit-cost-followup-20261006.md](fex-jit-cost-followup-20261006.md)。14:47:39 已覆盖安装，14:51:38 设备 marker／WOW64 DLL 哈希已核对，仍不是性能修复结论。先定位 CPU 测试的配置初始化失败，取得有效结果，再安排同一二进制的 Box 对照以及新场景同步采样。

原始证据仍以 `F:\VintagePomelo-Workspace\workspace_temp\fex-jit-symbols-20261006\` 为完整源，SHA256SUMS覆盖原始采样、截图、脚本及摘要。本轮没有fetch、提交、推送或更新记忆。
