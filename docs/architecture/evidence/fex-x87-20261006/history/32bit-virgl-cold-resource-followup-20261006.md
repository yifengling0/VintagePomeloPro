# 32 位 VirGL：人物首次卡顿、整体帧率与缺图的联合核查

日期：2026-10-06。分支 `feature/main_proton`，基线 `284026ce64c631961394f2aee6fadb3a4380e579`。本地 main 参照仍为 `5383b784`，没有完成同条件 main / Proton 真机 A/B。

## 当前结论

用户确认：目前帧率约减半的其他游戏均为 32 位；RichMan8 首次切换未访问人物明显更卡，重复同一顺序更流畅。后者已经得到同一游戏进程的日志和截图支持：首次约 27 FPS，重复约 54 FPS。这是资源首次准备与卡顿相关的证据，不能换算为 FEX 比 Box 慢一倍。

本轮找到并修正了一个可复现的 32 位 FEX 跨界返回记账遗漏：WOW64 的 UnixCall/syscall 人工恢复 guest RIP/RSP，却未消费对应的调用返回预测条目。主机生产函数回归通过，独立构建与签名包核对通过，已覆盖安装；**正确 DLL 已在设备解压并核对 marker；用户实测仍有首次人物卡顿，尚未证明 FEX 候选收益**。缺图尚未修复，不把这一候选称为三个症状的统一根因。

## 真实选人采集与用户核对

证据根：`F:\VintagePomelo-Workspace\workspace_temp\virgl-lowmap-20261006\`。人物首次与重复采集均为同一 RichMan8 PID 49744、同一包、low-map 关闭、Unix OpenGL 聚合计时开启。程序及点击始终由用户操作。

| 项目 | 首次切人物 | 重复同一顺序 |
|---|---:|---:|
| 目录 | off-character-first-aligned | off-character-repeat-aligned |
| 设备时间 | 10:46:47–10:47:18 | 10:47:51–10:48:22 |
| 宿主 FPS 样本 | 27.26 / 29.60 / 26.39 | 53.85 / 54.31 / 52.60 |
| 记录到的 swap 数 | 743 | 1606 |
| swap API 总时间 / 最大值 | 2.106 s / 39.930 ms | 3.882 s / 8.425 ms |
| TexSubImage 次数 / 总时间 / 最大值 | 581 / 385.809 ms / 9.058 ms | 未记录到 |
| shader compile + link | 各 1 次，合计 12.056 ms | 未记录到 |
| shadow 初始化复制 | 18.197 GB / 977.772 ms | 38.882 GB / 2.018 s |
| shadow 初始化时间 / swap | 约 1.316 ms | 约 1.256 ms |
| map_driver 总时间 | 40.184 ms | 85.443 ms |

开始截图确认进入选人准备阶段，首次段的大头像尚未出现；重复结束截图的詹姆斯大头像完整。用户已确认完成先访问未选人物、再重复同一顺序，并明确反馈首次明显更卡。

计时聚合窗口可能跨过采集起点；调用时间存在嵌套，不能直接相加。宿主采样 FPS 不是逐帧 P95、输入延迟或 GPU 时间。没有特定 metric 记录，只表示窗口内未记录到该调用，不能证明游戏没有做任何相关准备。

由此能够排除的简单归因：

- 重复阶段每秒搬运更多却更流畅，复制量不能单独解释首次阶段的减速。之前 low-map 开启已证实 direct map 和目标 copiedBytes=0，但用户没有感知整体提升；不要继续把消除复制直接等同于解决卡顿。
- 这轮记录到的纹理上传和 shader 时间不足以直接解释长时间减速；仍需区分游戏读取/解压、WineD3D CPU 准备、FEX 编译/执行、命令生产与消费等待，以及跨 VirGL 同步。
- 已有约 60 FPS 的现场，不支持固定锁死 30 FPS。复杂场景错过显示预算仍可能放大退化，但需提交/显示时间线才能确认。

## CPU 采样的有效范围

`off-first-character-aligned-profile` 实际覆盖 10:46:47–10:47:49，60 秒，90638 samples，lost=0。仅前约 30 秒与首次交互窗口重叠，后半段不能算成首次人物加载。

`off-repeat-character-aligned-profile` 实际覆盖 10:48:35–10:49:06，30 秒，52971 samples，lost=0。它开始于重复切换采集结束之后，只能用于随后状态，不能作为同步重复交互的 CPU 样本。目录名称不改变实际时间。

原始采样大量 leaf IP 被隐藏，callchain 中有匿名/未知 PC；不能把全部未知用户地址称为 FEX，也不能把内核样本的用户 caller 当成内核成本的执行函数。离线 `inspect_perf_raw.py` 尚未处理 server_pid 再归属和时间裁切，其汇总不用于给出 FEX/游戏/Mesa 的精确百分比分摊。

这轮 winebus 的此前高 CPU 没有重现，因此它不是本轮已确认的共同瓶颈。CS 栈仍见 map/TexSubImage 到 VirGL busy 查询，需要继续核对同步频率与等待。

## FEX 跨界返回候选

源码 pin：`86ff33bbe2`。已核对旧构建 staged source 的 `HandleSyscallImpl()` 与本次测试使用的 pin 函数一致。

1. `FEXCore/Source/Interface/Core/JIT/BranchOps.cpp` 的 CALL 在预测栈中压入 `<GuestReturnRIP, HostReturnPC>`，每条 16 字节。普通 RET 弹出条目并尝试匹配，失败走正常 lookup。
2. `Source/Windows/WOW64/Module.cpp` 的 UnixCall/syscall 读取 guest 栈上的 ReturnRIP，调用原生函数后直接设置 RAX/RSP/RIP，绕过 guest RET。
3. 旧 handler 没有消费这次人工返回的预测条目，正常外围 RET 会遇到错位条目。上游有 guard-page 恢复机制，因而这属于预测效率和记账问题，不能单凭它声称游戏已发生不可恢复崩溃。

修正放在 `scripts/patches/fex-wow64-synthetic-return.patch`：进入 native 之前保存预测栈指针；正常返回且 guest 上下文仍走桥接返回时，仅在指针未变化、位置位于本线程有效分配内并对齐、顶层 guest ReturnRIP 完全匹配且非零时消费一条。它不扫描更深的条目，不直接跳转缓存的 host PC，不重置整栈。新上下文、回调改变了栈、缓存失效清空条目或地址不匹配时保留原有 fallback。

补丁通过独立入口加入 `build_fex.sh`，避免已应用旧补丁集的 `patched_source=1` 跳过新修正。Makefile 把补丁纳入依赖和时间检查，并检查 WOW64 DLL 存在，防止只凭 ARM64EC 产物判为已构建。

设备原 stderr 在 CS 中有重复的可恢复 fault 地址 `0x221690ff0`，PC 位于未知 JIT 范围。其页尾形态与预测栈下溢相符，但没有得到该线程分配基址，**尚未确认这些 fault 就是 CallRetStack**；fault 次数也不能用于证明一倍性能差距。

### 回归与构建

```sh
make test-fex-wow64-synthetic-return
ASAN_OPTIONS=detect_leaks=0 FEX_TEST_SANITIZERS=address,undefined \
  python3 host_tests/fex_wow64_synthetic_return_test.py
```

测试提取真实生产 handler 和候选 helper，原生 API、锁和上下文替换为边界桩；CALL/RET 按生产 JIT 的 16 字节布局模拟，**没有执行 ARM JIT，也不构成 FEX/Box 性能微基准**。旧 handler 的 60000 次循环积累 960000 字节，外层预测持续不匹配；候选 200000 次 UnixCall/syscall 循环平衡并保留外层匹配。覆盖上下文替换、回调改变/恢复栈、缓存清零、不匹配、零地址和 guard/对齐边界，另有正反两次补丁重放一致性检查。普通及 ASan/UBSan 均通过；LSan 关闭，不作 LSan 全量通过结论。

独立目录 `build-fex-synthetic-return-20261006` 完成 ARM64EC 与 AArch64 PE 构建，实际 CMake 为 RelWithDebInfo / `-O2 -g -DNDEBUG`。候选包仅使用新 WOW64 DLL，继承原包的 ARM64EC FEX、Wine、Mesa、renderer、ArkTS 与其余 payload entries。加载 PE sections 已与独立编译输出核对，运行库闭包、payload manifest 哈希和官方签名验证通过。没有重新使用旧 Wine 对象来构建修改过的 Wine。

源码检查：本轮修改的 Makefile/build_fex.sh `git diff --check`、shell 语法检查、make dry-run 通过。全树 diff check 仍提示既有 dirty 0037 patch 的 4 处空白，本轮未改该补丁。

## 缺图的独立证据与方向

`off-missing-character` 是 10:38:38–10:39:19 的主菜单动画，不是选人采集。FPS 59.51–60.63，小人物仍有明确背景色矩形缺块；静态 UI 正常。用户指出 main 也出现过同类缺块。

因此缺块可以在画面正常刷新时独立存在。下一步应对齐有缺块的纹理/模型资源和 draw，检查 atlas 子区域、stride/UNPACK、alpha/透明测试、动态 buffer 的有效范围和上传时序，再区分裁剪/depth。只提高 FPS 或补缓存不能当作画质修复；0035 的显式 flush 正确性保持。

## 两条架构路径与继续优化顺序

main 的 Box 与 Proton 的 FEX 都有跨架构调用，但边界不同。Proton 中 PE32 游戏、WineD3D、opengl32 PE 部分经 FEX；Unix OpenGL、Mesa、renderer 是原生 ARM64，Mesa 的系统调用不经过 FEX。不能把 ARM Mesa 不直接调用系统作为根因。

本地 main/Proton 的 Mesa transfer/busy 核心算法未发现重大变化，renderer 仍有 context/sRGB/呈现差异。TSO、x87 策略及宿主能力探测也不同，需匹配微基准和设备实际配置才能量化。本候选保持 FEX 默认、TSO、SMC、x87 精度、画质和 low-map 默认值。

继续按可测的成本推进：

1. 先在同一原包环境验证这次 32 位返回修正，确认实际 DLL 已加载，再采首次与重复人物切换，并与旧包比较按帧归一化的计数、长帧/等待和截图。若无收益，明确记录，不继续扩大为统一解释。
2. 为首次人物准备补当前缺失的游戏读取/解压、WineD3D CPU 准备、FEX JIT/浮点 fallback 与 CS 等待证据。已有纹理上传和 shader 计时不重复添加。
3. 对高频 UnixCall/GL call 做纯 API 边界微基准，对整数/SSE/x87/内存做结果一致的 PE32 微基准；结合实际 LSE/RCpc 识别和 TSO 请求返回值，定位 FEX 的真实共性成本。
4. 若热点是同步查询，减少每 draw 的往返，使用带资源代次和完成状态的批处理/fence；若热点是图形管理 CPU，逐步将合适工作移到 native 侧，保留明确的 32/64 ABI 与生命周期契约。
5. 将缺图资源最小复现单独回归。最后才做同条件 main/Proton 包对照；当前 27→54 是同包首次/重复差异，不能代替这个对照。

## FEX 候选安装记录（已由手柄配套包覆盖）

已在 MatePad Mini 的 `com.vintage.pomelopro` 覆盖安装，保留数据。版本仍为 `1.4.5-proton.26-alpha / 1004035`。签名包：`workspace_temp/fex-synthetic-return-20261006/entry-default-fex-synthetic-return-debug-signed.hap`。

- HAP SHA256（已纠正 runtime.version）：`87ffea5bf48f8ab21b1c03e606f246c4b5f784766427ef16f890e8fd4dee1276`
- WOW64 DLL SHA256：`5a11aa6a68d196a738fa3adff3cc745b9bd67232ee4033bd97dcde38c64a0750`
- 原 DLL SHA256：`3c99a04135c36a2c8fa0d4a8b07afb07ca148b0604b13f864289e6f03648287b`

安装输出明确成功，bm dump 核对版本；安装后应用未自动启动。本轮没有发送 Steam、游戏/BAT 启动或输入命令，没有卸载/清数据，没有提交或推送。新 DLL 设备文件 SHA256 与 runtime marker 已核对；后续用户仍反馈卡顿。

下一轮手动入口：`C:\vp-richman8-lowmap-off-perf-20261006.bat`，保持与本轮首次/重复样本相同的 low-map off 和计时设置。先进入选人停留，再同步提示首次/重复切换。

## 正确 FEX 候选复测与最新配套包

第一包 `81756137...` 漏更新 runtime.version，实际继续使用旧 DLL，因此不作为新 FEX 性能证据。修正包 `87ffea5b...` 的设备 DLL 和 `.winehua-runtime-version` 已核对：`content=1d6d19bfa4e80d0a;wine-valve=cd547f7a0e;fex=86ff33bbe2;arch=arm64-v8a:aarch64`。候选主机记账修正成立，**真机收益没有证明**。

证据根 `workspace_temp/fex-synthetic-return-20261006/`：

| 场景 | 时间／证据目录 | 结果 |
|---|---|---|
| 启动卡住 | observe-corrected-entered | 用户看到 SOFTSTAR／黑屏，有音乐；窗口内没有新 swap，不能当作选人采样 |
| 正常选人停留 | 11:17:03–11:17:34，observe-corrected-playing | 两端截图确认选人界面大老千；FPS 57.96／57.81／57.77；swap1798；shadow_in45.245GB／2.621s |
| 首次后重复切人物 | 11:20:28–11:21:14/15，observe-corrected-switch | FPS16.02／22.65／18.35／23.88，中位20.5；用户确认首次更卡、后续更流畅；末截图莱娜仍见矩形缺块 |

切人物段 TexSubImage868次／549.665ms／最大8.616ms，shader compile2次4.661ms、link1次7.934ms，swap1005次／3.120s／最大135.346ms，shadow_in24.737GB／1.396s。该段混合首次与重复，未逐人物打标，不作为严格两段对照；API 墙钟可嵌套。

同期 `observe-first-corrected-switch-profile` 45秒、68777 samples、lost0。游戏约82.91CPU秒／47秒、renderer约5.20秒；这是进程 CPU 增量，不是完整线程归因。所有 winedevice CPU 很低（其中63590约0.09秒，其余近零），Winebus 空转不是这轮冷切换根因。未知 IP／匿名 JIT 不能全部归为 FEX。

真实 ELF `addr2line` 将 `opengl32.so+0x15af6c/0x15af68` 定位到 `wow64_glMapBufferRange`、`unix_wgl.c:3620`。既有 map_driver/shadow 计时开始于取得 wgl_lock 之后，遗漏锁等待。新0042默认关闭计时为这个盲点提供证据，不提前给出瓶颈结论。I/O 路径读权限受限，没有编造读取／解压成本。

当前平板已由手柄配套包覆盖，HAP SHA256 `09c7f532f6b4ce5315a50187a70b63b5143cf165a5c6f98db129fc181592ffd6`；它继承上述 payload/FEX并更新宿主、Winebus与Unix OpenGL库。详见 `controller-main-alignment-20261006.md`。手柄迁移、Winebus异常进度修复不等于已解决冷资源／缺图问题。
