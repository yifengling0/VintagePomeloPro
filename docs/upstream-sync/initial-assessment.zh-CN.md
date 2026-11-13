# FEX / Proton 上游同步：首轮评估

评估基线：`main@c5fc06edf915f01ff12b102f0bf0087d488c8c4f` / `rc-2.0.0`。
开发分支：`feature/fex-proton-upstream-sync`。本轮只新增台账与评估，未更换组件、构建或安装候选。

## 结论

可以按整套固定上游版本推进，但不能直接修改两个子模块 pin 后发布。
FEX 先独立迁移；Wine 先从真实 Valve 父系重建现有 OHOS 适配层，再接入 Experimental。
主线 2.0.0 继续使用冻结运行时，Box64 兼容选项保留，不恢复 Linux Steam / GNU Proton 产品线。

## FEX

候选固定为 `FEX-Emu/FEX@ab135df091d005cc98dfddc56c3dc9c9e4bd501d`。
通过 GitHub Git 对象接口获取受补丁影响的 30 个文件，逐个核验 blob ID；共 1,167,991 字节，未复制完整源码。
按生产顺序以 `--fuzz=0` 做稀疏文本重放：

| 结果 | 数量 | 下一步 |
| --- | --- | --- |
| 可按文本重放 | 4 | 核对语义、依赖和编译，不能据此称为已迁移 |
| 反向文本匹配 | 2 | 查明上游来源及语义，确认后才能登记为上游替代 |
| 需要审查或改写 | 14 | 逐项迁移；记录在同一文件上的前置冲突，避免将级联冲突误算成独立缺陷 |

四项可重放项为动态 L1 recommit、Windows CRT 文件打开、strict-mul24 适配和 memory-offset fault boundary。
此前分叉的两个提交也未忽略：目标模块中未发现原 emulator identity 输出，32 位 load-config 头仍有待核对的类型引用。
这两项保留为明确迁移门槛，不能因切到上游 main 而自动删掉。
查表缓存、异常路由、SMC、精确浮点/栈语义和内存保护属于优先审查及定向回归范围。
另已核实 SMC 补丁的原上游提交 `b21c49352ea5` 是候选的祖先，目标源码的两条后继路径均调用 `StartNewBlock()`。
因此将它登记为“上游实现存在、等待候选回归”，通过回归后再移除重复补丁；首轮文本冲突数量不等于仍需独立改写的数量。

## Proton Wine

真实原基线 `ValveSoftware/wine@dc26e61847081a1b5cb0733dc30feba6ee575482`，
目标 `e8ca5806eec488f5916e83d655ffc5349b6b1702`。
原基线、目标完整 tree 均通过非截断 Git tree 接口获取；与当前 fork `cd547f7a0ee9` 的提交树逐路径比较：

- 当前已提交平台差异涉及 209 个路径，不能只迁移 patches 目录。
- 上游在原基线之后修改了 280 个路径；其中 37 个也有本地改动，且当前 blob 与目标不同。
- 重叠集中在 ntdll 的 ARM64/ARM64EC、Unix 信号/内存/线程、WOW64、图形窗口和 wineserver。
- 另有 78 项生产构建补丁，继续单独登记；原退役 Linux bridge 沿用移除决策，生成的 Vulkan thunks 必须核验生成器。

已从全部路径、文件模式和 blob ID 重新计算原/目标 Git tree，并验证：原基线加上登记的 209 项变更，
得到的 tree ID 与当前 fork 完全相同。这证明已提交差异清单没有漏路径；尚不等于从真实上游父系重放全部有效源码。
路径重叠只是审查范围，不等于 37 项真实 Git 冲突，更不证明某项修复已被上游覆盖。

## 下一阶段门槛

1. 从真实原 Valve 基线重建当前有效平台差异，证明文件内容等价；每项适配记录保留、改写、上游替代或批准移除。
2. 独立迁移 FEX 全量固定快照，成组构建 ARM64EC、WOW64 和 Unix 桥接，检查 ABI 并完成指令、异常、SMC、线程/退出回归。
3. FEX 候选通过后再迁移 Wine；图形和媒体依赖分批升级，以便归因。
4. 按既定计划做 Mini 的 Steam 界面响应测量。游戏测试遵循当前账号占用限制；完整升级验收前不替换正式包。

机器可读证据：[initial-assessment.json](initial-assessment.json)、[adaptation-ledger.json](adaptation-ledger.json)。
本轮没有上游新版构建或性能结果，所有生产 pin 保持不变。
