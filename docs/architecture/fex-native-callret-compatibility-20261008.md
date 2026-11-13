# FEX 原生异常边界与汉化游戏白屏修复

## 结论

`Z:\games\A214\haison2_chs.exe` 的原白屏现场存在 FEX 原生辅助函数异常、错误寄存器恢复和 Wine 回溯不前进。核心修复在 FEX 的 Windows 异常边界，无需更换 WineD3D/VirGL 或改成 Box。

2026-10-07 的独立对照使用干净 FEX WOW64 修复 DLL、原 ntdll、原 BranchOps，不包含临时 PC0 诊断或 L1 跳转保护。再次完全结束旧会话后，游戏进入完整中文主菜单，随后可进入剧情。本轮新增异常记录中未再次发现 PC0。用户也确认可用。此结果支持原生异常边界修复独立有效；不构成游戏帧率提升或所有游戏兼容性保证。

## FEX 修复

原现场中 FEX 原生 `WowSyscallHandler::HandleSyscallImpl` 在预测返回项读取指令 `ldr x9,[x22]` 发生访问异常。故障地址位于 FEX 自有 CallRetStack payload，未落在两侧 guard；此时原生 X25 保存 guest RSP。

旧 handler 不检查故障 PC 的归属，对这一访问异常也把 X25 当作 JIT shadow-stack 指针重置。这违反原生 ABI，可破坏随后写回游戏状态的 RSP。32 位 WOW64 和 64 位 ARM64EC 都存在同一调用模式，ARM64EC 对应 X17。

`scripts/patches/fex-windows-native-callret-fault.patch` 让两条入口显式区分 JIT/dispatcher 与原生 PC：

- JIT/dispatcher 保持原 guard 恢复行为。
- 原生异常只为当前线程拥有的 payload 页补提交并重试，保留所有原生寄存器。
- 原生 guard、其他地址、提交失败均交还正常异常处理，不扩大可访问范围。

Windows `VirtualDontNeed` 使用 decommit/recommit 清零。代码缓存失效会清空预测栈，因此这是原生读取遇到不可访问页的可解释来源；尚未把所有并发时序单独做成确定性真机重现。已确定的是故障指令/地址/寄存器角色、旧 handler 的破坏行为，以及干净修复对本游戏的有效性。

补丁已接入 `scripts/build_fex.sh`。新增组合反向校验解决了 native overlay 改动前一补丁上下文后，重复构建误判未应用的问题；只操作临时副本，不回滚已暂存的源树。

## Wine 回溯保护

ARM64 `virtual_unwind` 查询上一条调用指令时会将 PC 减 4。旧 `RtlVirtualUnwind2` 只比较传入 PC 与 LR，metadata-free 帧实际 PC=LR 时仍成功返回同一 PC/LR/SP，形成无限回溯。

`patches/wine/0045-ntdll-arm64-invalid-leaf-unwind.patch` 同时检查未经调整的 context PC。只改 ARM64 实现，不修改 ARM32，也不拦截带有效 metadata 的帧。它是故障处理兜底，不是绕过游戏异常；已接入 Wine 构建。

## 举一反三：运行时更新必须进入实际加载路径

真机加载 FEX 的路径是 prefix `windows/system32`。只更新包内 `bin/aarch64-windows` 不足以证明新 DLL 被使用。

普通 MinGW FEX DLL 缺少 Wine builtin 标记，WineFakeDlls 更新会跳过它。因此本轮加入两层处理：

1. `stamp_wine_builtin_identity.py` 将打包的 PE builtin 内容身份写入 wine.inf 注释；PE 更新时使 Wineboot 刷新，Unix/GPU 库单独更新则不触发这一身份变化。
2. `WineEngineService` 在 Wine 启动前同步两颗受管理 FEX CPU DLL。逐块比较，复制到同目录临时文件、验证后替换；不清空 prefix，也不操作游戏 DLL、字体和存档。覆盖中断后可在下次启动重试。

最终 release 载荷的本地设备签名包已覆盖安装。未手动改 DLL 的情况下，prefix WOW64、ARM64EC、ntdll 三颗文件全部与新包哈希一致，包括此前仍为旧版的 ARM64EC。

## 验证与边界

- 新旧真实 FEX handler 配合实际保护页重放，复现旧 guest-RSP 寄存器破坏；验证原生 payload、guard、提交失败和 JIT guard 语义。
- 实际 build 脚本相关段连续执行两次，源码结果不变。
- Wine 真实函数及真实 caller 重放：旧代码连续 10,000 次成功而不前进；修复在第二帧终止；有效 metadata 路径继续工作，ARM32 源码未改。
- 相邻 synthetic-return、构建身份、Wine patch detection 回归通过。
- 受管理模块的 ArkTS 生产方法以真实文件系统测试：32/64 更新、无变化、缺失文件、用户 DLL 保留、短读、残缺复制、替换失败和重试通过。
- 干净 WOW64 和 ARM64EC DLL 编译、独立 Wine ntdll PE 构建、正式 Hvigor release 构建通过。
- 平板上的真正 x86-64 PE32+ 基础执行程序经新 `libarm64ecfex.dll` 完成 50,000 次原生 API 往返并写出校验成功标记。这是 64 位基础兼容验证，未强制触发原生 payload 异常，也不代替 64 位游戏长时间测试。
- 原临时 `fex-jit-null-l1-target.patch` 未接入构建，未进入发布 DLL。独立使用它不能充分修复原白屏。

原始现场与构建身份保存在 `workspace_temp/haison2-diagnostic-20261007`、`workspace_temp/haison2-repair-20261007`；不提交大日志、设备截图中的私人内容、游戏文件、签名材料或构建二进制。
