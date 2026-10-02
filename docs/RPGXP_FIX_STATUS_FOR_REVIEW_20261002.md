# RPG Maker XP 取证状态（2026-10-02 修订，供评审）

## 当前结论

RPGXP 启动根因仍未确认，也没有可宣称的生产修复。本分支保留一个默认关闭的 0013a 取证工具，并把 0013b/0013c 移出正常 Wine 构建链。

- 0013a：只有 `WINEHUA_RPGXP_DIAGNOSTICS=1` 精确匹配时启用
- 0013b：未验证的 wow64 exception-frame 实验，隔离
- 0013c：改变栈布局的 pad 实验，收益未证明且有 Steam 回归，隔离
- 版本号保持基线已有的 proton.22 / 1004023
- wine-valve gitlink 保持 `cd547f7a0ee9d3d59b3ec47317a7852dedf0443b`

## 必须撤回的旧结论

### 旧 E1 的 pc/pc+4 解码无效

旧探针声明打印 pc 与 pc+4，但实现是：

- `uintptr_t code[2]`
- 在 64 位 host 上读取 16 字节
- 把 `code[0]` 标成 pc，把 `code[1]` 标成 pc+4

每个元素实际是 8 字节。第二个元素覆盖的是 pc+8/pc+12，而不是 pc+4。因此旧文档中的四条 ARM64 指令排列、pc+4 指令及据此推导的地址生成路径全部撤回。fault addr、保存的 host pc 和寄存器值仍可作为原始日志参考，但必须由修复后的 2×`uint32_t` 精确 8 字节读取重新采样后才能解码。

### 旧 E2 的 SEH next/handler 结论无效

旧探针用 `uintptr_t node[2]`，却只读取 8 字节。在 64 位 host 上，这 8 字节全部落入 `node[0]`；`node[1]` 没有由该次读取初始化。把两项强制转为 32 位再标成 next/handler 不符合 32 位 `EXCEPTION_REGISTRATION_RECORD` 布局。

因此旧文档列出的 `seh0/1/2` handler、链损坏、帧覆盖节点及“链头健康/异常”等结论都不能由那批日志证明，全部撤回。修复后的探针使用初始化的 `uint32_t node[2]`，精确读取 8 字节，并按 32 位 next 做 sentinel、范围和单调性检查。

### 旧 E3 的 last-fault 线程关联无效

旧 EXIT-FAULT 记录保存在 process-global 字段中。多个线程同时 fault/abort 时，一个线程可能打印另一个线程最后写入的 seq/addr/pc/x9。旧文档对 tid 31362/31355 与具体 last fault 的绑定不能作为线程一致证据。

修复后状态位于当前线程的 `ntdll_thread_data`，由 handler 的传入 TEB 定位，并通过 per-thread guard 与 odd/even generation 发布。EXIT-FAULT 只读取当前终止线程的一致快照。

### 旧 E4 不证明生产 pad 收益

vma_top 日志来自现已隔离的 0013c 布局实验。它最多表明某次实验进程中的 VMA 形态，不能证明 RPGXP 收益；0013c 曾导致 Steam 回归。0013a 不再读取 vma_top，也不再新增 maps reload/lookup。

## 修复后的 0013a 观察面

### WOW-TEB / SEH

- 输出 64 位与 WOW TEB 的栈边界
- guest EIP 的 16 字节转储使用初始化的 byte buffer；只有完整 16 字节才输出，否则只写 `(unreadable)`
- 最多遍历 16 个 SEH 节点
- 每个节点用初始化的 2×`uint32_t` 精确读取 8 字节
- `next == 0xffffffff` 是终止 sentinel；其它 next 必须处于 32 位用户范围并严格递增

### SMC-ANOMALY

- ARM64 指令使用初始化的 2×`uint32_t` 精确读取 8 字节
- `code[0]` 标为 pc，`code[1]` 标为 pc+4
- 保留固定限额的保存寄存器与 guest CPU 区输出
- 沿用 FAULT-MIN 的 process-wide event seq，所有 CODE/REGS/GUEST 行都带相同 tid/seq
- 不新增 maps-cache 临界区、reload 或 lookup

### EXIT-FAULT

- 只在 opt-in 且当前线程已有非零 published record 时输出
- acquire/retry generation snapshot 避免混合两次写入
- 格式化只走固定大小 builder + `write(2)`

## 默认关闭与信号安全

环境变量在 signal handler 安装前读取一次。未设置、空串、`0`、`10` 或其它值都关闭 0013a；global quiet 也强制关闭。关闭时跳过全部新状态写、guest 读取和日志。

新增 signal-path 代码不使用 `snprintf`、`fprintf`、malloc/free、stdio、阻塞锁或 lazy TLS。guest 读取通过 raw `SYS_process_vm_readv`，保留原始地址和 `errno`，短读一律失败且不检查/输出部分 buffer。

## 0013b 重新评审前置条件

当前 0013b 不进入正常链。若将来重新提出，至少要先解决：

1. candidate 减法和 frame pointer 形成前的防溢出检查
2. 变量初始化、context length 返回值及 frame-size 溢出
3. 原始 SegSs 候选优先级
4. 合法 guard-page growth，而非 blanket range rejection
5. 保证不返回的失败策略、退出状态和 debugger/client 语义

在这些条件与运行证据齐备前，0013b 不能称为修复。

## 0013c 处置

0013c 仅是手工隔离实验。它改变 reported StackBase 上方的 committed layout，没有已建立的 RPGXP 收益，并有 Steam 回归史。正常构建不会应用它。

## 后续有效取证要求

1. 从干净 wine-valve `cd547f7a` 重放正常链并完整构建
2. 默认关闭：分别在 env 未设置和 `0` 下运行 Steam 与已知正常 32 位程序；不得出现 WOW-TEB、SMC-ANOMALY 或 EXIT-FAULT
3. opt-in RPGXP：用原始 8 字节核对 pc/pc+4；用原始 8 字节核对 SEH next/handler；短读只允许 `(unreadable)`
4. 确认没有递归诊断风暴或死锁，并核对 EXIT-FAULT 与 aborting tid 一致
5. 设备不可用或测试未跑时标为 BLOCKED/UNVERIFIED，不沿用旧 proton.18–22 的 PASS 口径

## 剩余风险

显式启用 0013a 仍会改变 signal-path 时序，只适合取证。0013b/0013c 仍是未验证实验。修复后的日志在重新构建和真机复测前，不能恢复任何旧的 SEH、pc+4、线程关联或 RPGXP 修复结论。
