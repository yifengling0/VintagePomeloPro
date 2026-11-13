# GNU FEX 信号返回状态对照

2026-10-30：已定位并复现一个通用信号恢复缺陷。原 Valve FEX 在当前线程栈接收异步信号时，独立 VP8 探针发生 SIGSEGV；原样应用上游公开补丁后，同一探针完成 196,608 次检查、242 次信号处理，guard mismatches 为 0。**后续诊断组合完成 483.4 秒运行、完整 UI、约 60 FPS 和点击 64→65；正式安装可自动启用补丁，但原设备数据库缺表仍阻止普通入口完整 UI，不能将二者混为同一次验收。**

## 上游来源

[FEX issue #5901](https://github.com/FEX-Emu/FEX/issues/5901) 描述了 `rt_sigreturn` 在 guest RIP 未变化时未恢复 `InSyscallInfo`，使后续信号可能按照旧 spill mask 和旧 RSP 放置信号帧，覆盖活跃栈。报告附有现成 diff，本工程[原样导入](../../scripts/patches/fex-upstream-5901-restore-sigreturn-state.patch)，没有生成新的 FEX 实现；[来源清单](../../scripts/patches/fex-upstream-5901-restore-sigreturn-state.provenance.json)记录作者、时间和 SHA-256。该报告仍为 open，不能描述为上游已合入。

补丁 SHA-256：`de76b76140e2933a574ea5427b372aa2512980c694a9aa37f9aac8f37708ee3e`。它同时覆盖 x86_64、i386 非 RT 和 i386 RT 恢复函数；**本轮设备验证仅覆盖 x86_64**。

## 与 Banana 现场的联系

此前完整游戏在约 101 秒自行 SIGSEGV，信号历史包含 Mono 使用的 SIGPWR，guest alternate stack 未启用；坏返回地址和寄存器呈重复像素值，栈中有 xstate magic。这与上游描述吻合，但尚未直接捕获完整游戏的第一次非法写入。另一次 stack-write observer 将 RBP 当作 frame pointer 的假设不成立：相关 Unity 函数把它用作字符串缓冲区，不能把该观察器命中当作非法写入证据。

原 FEX 的备用信号栈探针通过，而当前线程栈探针失败。修复候选使用相同 revision `37265b109a4bba9e10241972dfcb3d1fdb8f3f9a`，仅应用信号恢复补丁，不带此前内存 POP 补丁，诊断阶段保留官方 FEXServer 和匹配 thunks；交付改为同次构建的配对 FEXServer，匹配 thunks 仍来自原官方 depot。候选 ELF SHA-256：`66a865b99c5682c5a760d994f2efe938634fffc4a38c67d81f018fb773f317ca`。

当前线程栈 A/B 的探针 ELF 完全相同：`3aab5b541d0e5a39c994285dc34983706bcdac848c47e7688b31a106a48b80ef`。4 个线程对真实 UnityPlayer 8×8 VP8 预测器施加 100 μs SIGUSR1，无 GPU 调用。原版新鲜输出包含 SIGSEGV；候选输出为 `complete cases=196608 guard_mismatches=0 result=0 signals=242`，正常退出码为 0。详见[脱敏证据](evidence/native-fex-sigreturn.json)。

## 完整游戏与交付边界

第一轮完整 Banana 在约 62.7 秒检测到整个宿主 UID 退出；退出前已创建 NativeBuffer/Vulkan surface，但没有可操作画面验收，也未捕获明确 fatal signal。系统日志未及时保存，不能判为 OOM 或将其当作补丁后的同一故障。第二轮仅保留默认故障观察，持续 482.7 秒但仍黄屏；第三轮再用私有配置持续 245.6 秒，仍黄屏；第四轮加上 BC/NativeBuffer 修复与 softpipe 缓存优化，持续 483.4 秒并完成独立点击检查。各轮在截止时间由 runner 主动收尾，未验证游戏菜单正常退出。详见证据 JSON 的 `diagnosticFullGame` 与 `input`。

GPU 路径保持原 NativeBuffer，未通过 CPU 帧回读绕过问题；没有修改游戏资源、用户存档和原始数据库。诊断过程中仅在选定应用 UID 全部停止后临时替换入口 ELF，退出时按 SHA-256 恢复。没有把 Steam 命令 ACK、旧 Player.log 或探针通过记作游戏启动成功。

## 复建与回归

```sh
python3 scripts/diagnostics/build_native_fex_pop.py \
  --repository /path/to/existing/FEX-git-repository \
  --output-dir workspace_temp/linux-proton/native-fex-core \
  --clang /path/to/llvm/bin/clang-15 \
  --gnu-toolchain /path/to/aarch64-none-linux-gnu-toolchain \
  --linux-uapi /path/to/linux-uapi/usr/include \
  --stock-control --upstream-sigreturn --ohos-guest-protection
```

构建器校验上游补丁 SHA，复用固定增量目录；`--stock-control` 去除内存 POP 变量，`--upstream-sigreturn` 添加公开 diff。2026-10-31 新增 `--ohos-guest-protection`，在上述组合上添加本地 MIT guest syscall 适配；当前交付不能再描述为仅含上游补丁。不得在设备试验仍引用当前本地 ELF 时并发重写构建目录。普通安装现在通过 HAP 独立 companion 交付，不覆盖官方 FEX 或游戏入口。未知官方 FEX 版本保持原版，防止跨版本 core/server/thunk 混用。当前普通入口已验证自动安装及实际选用，但原设备仍报 SQLite 缺表，画面仅背景；正常游戏退出也未验收。详见[交付说明](NATIVE_GAME_DELIVERY.zh-CN.md)。

## OHOS guest 共享映射权限（2026-10-31）

尖塔二 Linux 原生 Godot/.NET 在 `memfd:doublemapper` 的 `---s` 页上请求 RX，被宿主返回 EACCES，随后原页面 SEGV_ACCERR。当前 companion 在 FEX 的 GuestMprotect 层做有界回退：仅 `STEAMOHOS_NATIVE_FEX=1`、EACCES/EPERM 且请求 EXEC 时重试 host 权限去除 EXEC，guest VMA 仍记录原权限。FEX 翻译器读取 x86 字节，ARM64 JIT 分配器不经过该 helper。EINVAL、ENOMEM、禁止共享写等真实失败仍返回错误，不全局忽略 mprotect 失败。

本地补丁和头文件为 `scripts/patches/fex-ohos-guest-protection.patch` / `.h`，源码输入和产物 SHA 由构建 receipt、manifest、selector 同时核对。当前 FEX SHA-256 为 `1b2a7b7933722cf22b0b92c56fb81aeacdc2391556f7edf4fce6f4a35e0ca16c`，FEXServer 配对 SHA 保持 `059cf9217bc5221b5865242e53647e7b4bb9fe93bb4a0ed9940fe6e72e2a1683`。Docker 的真实 noexec /dev/shm 回归已通过；设备游戏结果见[原生引擎记录](NATIVE_ENGINE_VALIDATION.zh-CN.md)。
