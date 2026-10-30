# GNU FEX 内存 POP 异常状态修复

2026-10-30：已在普通平板的真实 Steam → Valve FEX 路径复现并验证修复；**它没有解决 Banana 的完整游戏崩溃，尚未替换正式运行时**。脱敏结果见 [证据](evidence/native-fex-pop-fault.json)。

## 来源和适用范围

HSL 的 `0014-preserve-pop-memory-fault-state.patch` 来自本工程原 Wine 路线已有的 [内存 POP 补丁](../../scripts/patches/fex-pop-memory-precise-stack.patch)。原 Wine 构建已应用；Linux Steam 下载的 Valve FEX `FEX-2607-76-g37265b1` 未包含。HSL 使用的较新 FEX 及其前端补丁不直接替换本工程二进制。

本次 [Valve 2607 补丁](../../scripts/patches/fex-valve-2607-pop-memory-precise-stack.patch) 只移植同一指令语义，适配精确 revision `37265b109a4bba9e10241972dfcb3d1fdb8f3f9a`：

- `POP [memory]` 的目标写入成功后才提交新 SP，避免页保护／SMC 写异常重试时再次弹栈。
- `POP [SP]` 的地址计算仍使用递增后的 SP；保持 x86 指令要求。
- 保持旧 SP 活跃于固定寄存器直到 faulting store，防止寄存器分配提前合并。
- 寄存器 POP 路径不变。因此此前在 `pop rsp; ret` 观察到的坏返回栈不能单凭此补丁认定同源。

## 验证

使用 HSL 的 GNU CPU 探针（LGPL-2.1-or-later，源 SHA-256 `f92531e620e064b326705e0d35bcc95eb60cc3962a2b49b278645290a5593c67`），已保存为本工程 [独立探针源码](../../scripts/diagnostics/native_fex_cpu_contract.c)，无需 Wine 或图形库。设备 A/B 使用完全相同的已校验 x86_64 探针 ELF，通过 Steam 实际执行，并检查本轮新输出。临时覆盖官方游戏入口只用于诊断，退出后按 SHA-256 恢复。

| 检查 | Valve 原二进制 | 同源码／工具链重编译，不含补丁 | 同源码／工具链，包含补丁 |
| --- | --- | --- | --- |
| XORPS／XORPD／PXOR 全 lane | 通过 | 通过 | 通过 |
| 目标页写异常时精确 SP 与重试 | 失败 | 失败 | 通过 |
| SMC 写异常透明重试 | 失败，SP 多推进 8 bytes | 失败，SP 多推进 8 bytes | 通过 |
| POP [SP] 目标寻址 | 通过 | 通过 | 通过 |
| 汇总失败数 | 2 | 2 | 0 |

本机 GNU x86_64 与 i386 参考运行也均通过。**i386 探针尚未在这份 GNU ARM64 FEX 上完成实机验证**。没有将 HSL 的设备结论移记为本工程结论。

完整 Banana 随后用补丁版及同一签名图形诊断包启动，能显示完整 UI、约 60 FPS，累计呈现至 720 帧标记，`pixel_readbacks=0`；启动请求后 124.9 秒仍发生 SIGSEGV。Vulkan allocation 峰值 2,444,012,288 bytes、失败 0。原始 host PC 为 0，LR 位于 FEX JIT，额外捕获了直接调用目标、shadow stack 和 CPU state；没有定位到最先写坏方。不能由最后保存的 guest PC 或这些零分配失败计数排除所有内存破坏。

前台／PID 核验后发送过点击，但前后截图的计数均为 2，**输入验收未通过**。日志另有 `SQLiteException: no such table: CollectionItem`，尚未证明与 native SIGSEGV 同源；未修改用户数据库或游戏资源。

## 可重建入口

```sh
python3 scripts/diagnostics/build_native_fex_pop.py \
  --repository /path/to/existing/FEX-git-repository \
  --output-dir workspace_temp/linux-proton/native-fex-core \
  --clang /path/to/llvm/bin/clang-15 \
  --gnu-toolchain /path/to/aarch64-none-linux-gnu-toolchain \
  --linux-uapi /path/to/arm64-linux-uapi/usr/include
```

构建器从已有 Git 对象导出精确 revision，复用固定增量目录，不修改共享源码或现有 thunks。依赖按照该 revision 的 gitlink 取值；缺失的 jemalloc、rpmalloc 和 drm-headers 可显式加 `--download-missing` 下载，URL、revision 与预期 SHA-256 固定。保留 GNU allocator，关闭 thunk 重新编译。未初始化的其他依赖明确报错。

`--stock-control` 在同一增量目录生成不带 POP 修复的负对照；不与设备测试并行重写当前正在核验的本地 ELF。构建清单记录源码／补丁／工具链身份及产物 SHA-256；相同输入、产物校验一致时复用。二进制 SHA 仍受上游编译时间字符串及链接布局影响，不能用一次实机记录覆盖另一个 SHA 的二进制。

```sh
python3 scripts/diagnostics/build_native_fex_cpu_probe.py \
  --output-dir workspace_temp/linux-proton/native-cpu-contract --host-check
python3 scripts/diagnostics/build_native_fex_cpu_probe.py \
  --output-dir workspace_temp/linux-proton/native-cpu-contract --bits 32 --host-check
```

这些命令只构建、运行本机参考，不部署设备、不更改兼容工具选择。发布默认仍为原版；原 Wine／DXVK／virgl 未接入本轮 FEX 候选。实机测试结束已停止全部选定应用 UID 进程，恢复基线 HAP、原 Valve FEX、WSI／Venus、软件 GL、supervisor 及官方游戏启动器并核对 SHA-256。

## 继续定位：视频函数的返回帧（2026-10-30）

之后在相同已验收 POP 二进制下扩充 opt-in 故障观察，取得当前 JIT block 共 4096 bytes 和 guest 栈共 512 bytes。按该 revision 的 `JITCodeHeader`／`JITCodeTail` 解码，最新 block 映射至 UnityPlayer ELF VA `0x18f0c60`：VP8 的 8 像素横向六抽头预测例程。它与此前覆盖的 16×16 完整预测器并非同一个入口，不能将后者通过外推为此处所有调用状态均正确。

最新完整游戏请求后 115.1 秒发生 SIGSEGV，原始 ARM64 PC 和故障地址为 0。当前块的结尾先用 `mov x8,x9; ldp x9,x20,[x8],#16` 恢复 guest RBP／返回 RIP，再执行 shadow-return／L1 查找。现场对应保存槽和返回 RIP 均为 0，最后间接返回到 0。周围栈包含连续零值，随后有 `0x6e` 像素字节。映射出的调用方为 8×8 预测器，正常 scratch 在保存帧之上；仅看到 scratch 内像素不能解释保存帧为何变零。

这将故障定位到“视频例程返回时读到坏帧”的阶段，**尚未定位最先写坏者**。完整 block 比 host LR 或旧 guest RIP 更接近当前执行入口，但仍不单独证明转译器、媒体并发或 GPU 共享内存中的哪一方有错。该例程的寄存器 POP 没有经过本轮内存 POP 修复路径，禁止把二者视为同一缺陷后继续堆 POP 补丁。

本轮 Vulkan allocation 峰值为 2,399,513,344 bytes、失败 0，BC upload 768 次，显示仍为 NativeBuffer 且 `display_readbacks=0`；这些计数不等于全进程物理驻留，也不能排除更早的内存破坏。游戏在输入验收之前崩溃，没有发送点击，不记录为输入通过。组件身份和脱敏结果见 [返回帧证据](evidence/native-fex-return-stack.json)。

下一步应观察上述保存槽从有效值变为零的第一步，并记录当时线程和写入范围；现有独立 VP8 全解码／16×16 预测器相同输入不再重复运行。测试结束恢复基线运行时并停止选定应用 UID，未发布游戏资源补丁或修改用户存档。

## 后续信号恢复对照

当前线程栈信号压力探针已取得可重复的原版失败／上游补丁通过对照，见[信号恢复说明](FEX_SIGRETURN.zh-CN.md)及其证据。完整游戏尚未验收，不再把寄存器 POP 或纹理压缩直接视为该现场根因。

## 最新交付状态（2026-10-31）

后续信号恢复补丁及 BC/缓存组合取得完整诊断画面、点击和 483.4 秒运行证据，见[信号恢复说明](FEX_SIGRETURN.zh-CN.md)。正式 HAP 不启用这里的内存 POP 候选，只交付原样上游 sigreturn diff。普通入口自动补丁已验证，原设备缺表问题仍在，见[交付说明](NATIVE_GAME_DELIVERY.zh-CN.md)。上述失败记录保留为历史阶段。
