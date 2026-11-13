# feature/main_proton Wine 补丁清单（2026-10-02 修订）

## 适用范围

- 基线：`feature/main_proton` 的 `7fcf397e184b82bcde01830015a4d86ce1022ec5`
- `main` 不是集成目标，本文也不主张把该分支的架构带到 `main`
- `AppScope/app.json5` 继续使用基线已有的 proton.22 / 1004023；本次不再升版
- `thirdparty/wine-valve` gitlink 保持 `cd547f7a0ee9d3d59b3ec47317a7852dedf0443b`

## 正常构建链

`build_wine.sh` 继续幂等应用 0001、0002、0006–0012，并且只注册一个 0013 系列补丁：

| 补丁 | 正常链状态 | 说明 |
| --- | --- | --- |
| 0013a | 注册，默认关闭 | 仅用于 RPGXP 取证；只有环境变量精确等于 `WINEHUA_RPGXP_DIAGNOSTICS=1` 才启用 |
| 0013b | 隔离，不注册 | 未验证的 wow64 异常分发帧实验，不是 RPGXP 修复 |
| 0013c | 隔离，不注册 | 手工应用的栈顶 pad 实验；收益未证明，且曾导致 Steam 回归 |

移除 0013b/c 的注册不会自动还原已经被旧构建脚本修改过的持久源码树。构建脚本因此只做安全检测：

- 明确检测到 0013b/c 已应用时，停止并要求改用干净的隔离源码重放
- 无法证明补丁完全未应用时同样停止
- 不 reset、checkout、反向应用或改写用户的 dirty `WINE_SRC`

## 0013a：默认关闭的取证工具

### 门控

`WINEHUA_RPGXP_DIAGNOSTICS` 在安装 signal handler 之前读取并锁存。只有单字节字符串 `1` 开启；未设置、空串、`0`、`10` 及其它值均关闭。`WINEHUA_DIAG_QUIET` 开启时该功能始终关闭。

关闭状态不做 0013a 的线程状态更新、guest 内存读取、WOW-TEB/SEH/异常输出或 EXIT-FAULT 输出。

### 读取与输出约束

- guest 读取保留原始地址，直接调用 `SYS_process_vm_readv`
- 只在返回长度与请求长度完全一致时成功，并恢复进入时的 `errno`
- SEH 节点使用初始化后的 `uint32_t node[2]`，精确读取 8 字节；`node[0]` 是 next，`node[1]` 是 handler
- ARM64 指令使用初始化后的 `uint32_t code[2]`，精确读取 8 字节，分别标为 pc 与 pc+4
- guest EIP 的 16 字节转储保持 byte-oriented；短读或不可读只输出 `(unreadable)`
- 新增 signal-path 输出只使用固定大小的 `ohos_signal_log` 与 `write(2)`，不使用 stdio、分配、锁或新增 maps 缓存访问

### 线程一致性

0013a 的 guard、限额、decrypt-family 计数和最后故障快照位于 OHOS-only 的 `ntdll_thread_data` 尾部。现有 syscall 字段的 ABI 偏移断言保留，同时继续断言整个结构适配 `GdiTebBatch`。

handler 从传入的 TEB 取得状态。整个探针由 per-thread guard 覆盖；嵌套进入会跳过探针。事件序号沿用现有 process-wide FAULT-MIN counter（即使 FAULT-MIN 日志超过配额不再输出，序号仍继续）；所有多行明细都带同一 tid/seq，避免多线程日志交错后误归属。最后故障记录通过 odd/even generation 发布，EXIT-FAULT 在当前终止线程上用 acquire/retry 读取一致快照。

## 0013b：隔离评审件

当前内容没有生产语义承诺。任何重新进入正常链的提案都需要单独合同并证明：

1. 形成指针前完成防回绕边界检查，例如先证实 candidate 在界内且 `frame_size <= candidate - limit`
2. 所有变量初始化，并检查 `RtlGetExtendedContextLength` 与 frame-size 算术
3. 先尝试原始 SegSs 选择的候选，只在验证失败后考虑替代候选
4. 保留合法 guard-page 增长，不能用 `[StackLimit, StackBase)` 一刀切拒绝
5. 失败策略保证不返回，并定义退出状态及 debugger/client 行为

## 0013c：隔离手工实验

该补丁有意改变 reported StackBase 上方的 committed layout。它没有建立 RPGXP 收益，过去曾使 Steam 启动回归，只能在干净隔离源码上手工应用并单独验证。

## 对旧取证结论的撤回

7fcf397 中的旧 0013a 存在两个宽度错误：

- SEH 使用 `uintptr_t node[2]` 却只读取 8 字节。在 64 位 host 上 handler 所在的第二个 32 位字没有按记录布局读取，后续 handler/链损坏推论无效
- 指令使用 `uintptr_t code[2]` 并读取 16 字节，却把第二个 64 位元素标成 pc+4；该元素实际覆盖 pc+8/pc+12，旧的 pc+4 解码和由此形成的地址生成推论无效

旧 EXIT-FAULT 又来自 process-global 字段，可能把不同线程的故障与退出混在一起。旧 vma_top/pad 自证属于现已隔离的 0013c，且会重复使用全局 maps 关键区。上述材料均只能作为历史原始日志，不能作为已确认的 SEH、pc+4、线程归属或 pad 结论。

## 验证要求

- host policy unittest：注册、精确门控、固定宽度/精确读取、signal-path 禁用 API、sticky-source 拒绝
- 干净 wine-valve 源码：正常链正向一次、第二次 no-op、全链反向 dry-run、还原后树一致
- 0013b/c：在正常链之后分别做正向/反向 dry-run，只证明机械可用性
- OHOS Wine：只通过项目顶层 `scripts/vpbuild.sh`，使用隔离 `WINE_SRC`/`BUILD_DIR`
- 真机：默认关闭与 opt-in 行为必须分开记录；未运行即 BLOCKED/UNVERIFIED

## 当前风险

即使修复后，显式启用 0013a 仍会扰动故障路径时序。0013b/c 仍是未验证实验，不是 RPGXP 修复。任何未完成的构建或真机门禁不得记为 PASS。
