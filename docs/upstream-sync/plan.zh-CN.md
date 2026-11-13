# VPP 2.0.0：FEX / Proton 完整上游同步方案

核验日期：2026-11-13。产品基线已汇合到 `main@c5fc06ed`，对应不可移动标签 `rc-2.0.0`。
后续工作分支为 `feature/fex-proton-upstream-sync`；发布 CI 使用冻结标签，升级评估在本分支独立推进。
本分支已登记生产补丁、完成当前有效 Wine 在真实 Valve 父系上的等价重建，并默认随包提供 x86/x64 Gecko。
尚未更换 FEX/Wine 生产 pin、构建升级候选或安装设备；Gecko 已完成本地 HAP 成品校验。
产品路线继续为 Windows Steam、OHOS Wine、HmosDeck，默认 FEX，Box64 保留为兼容选项。

## 上游基线

| 对象 | 已核验版本 | 迁移用途 |
| --- | --- | --- |
| FEX 当前 | `86ff33bbe299`，2026-05-10 | 保留当前可用对照 |
| FEX 最新发布 | `FEX-2611.1@4f9c1f1e3644`，11 月 11 日发布 | 发布版对照 |
| FEX 最新 main | `ab135df091d0`，11 月 12 日 | 用户要求的最新完整源码候选 |
| Proton 稳定发布 | `proton-11.0-2` | 正式发布对照 |
| Proton 稳定维护线 | `1f089f5610a8`，Wine `e60892cc2398` | 相对原 Valve Wine 基线只多 1 个提交 |
| Proton Experimental | `0764d2920a12`，Wine `e8ca5806eec4` | 最新实验维护线；相对原 Wine 基线多 477 个提交 |

建议用固定的 FEX main + Proton Experimental 快照建立迁移候选；每次发布前固定经过验收的组合。
该配对包含对 Proton 自带 FEX pin 的明确覆盖，不能称为 Valve 已验证的原装组合。
“最新”以此表与 JSON 的不可变提交为准，避免构建期间漂移。

## 已确认的维护问题

1. Wine 子模块的 `c0cc03e377e` 曾把“Valve Wine dc26e618 + OHOS 移植”整树快照接到原 WineHua 历史上，
   提交正文说明了这一点，但真正的 Valve 基线未接入该历史。直接 merge 容易把上游版本差异和平台适配混在一起。
2. FEX 当前 pin 与最新发布版分叉：最新发布版有 1260 个后续提交，当前 pin 一侧另有 2 个提交。
   这 2 项涉及模拟器身份记录及 Windows load-config 定义，也必须检查上游等价实现后再处置。
3. 适配分散在已提交 fork、构建补丁和有效源码变更中。只保留 patches 目录不足以保证功能不丢。
4. 当前稳定 Proton 的 Wine 更新很少；仅把“稳定版号换成最新”不会获得 Experimental 的 477 项后续提交，
   更不能据此承诺 Steam 的 CEF 卡顿已解决。

## 本地适配保全与审计结果

- FEX 当前生产源码的 20 项补丁按实际构建顺序重放，涉及文件与实际构建源码逐字节一致。
  诊断和严格乘法等可选开关保持独立，未默认开启。
- Wine 的 78 项登记补丁及诊断归一化在开发树、source-integrity 构建树均通过重放校验。
  此检查覆盖补丁管理的文件，不等于对所有 Wine 文件或运行行为作保证。
- 两棵 Wine 源码仍有登记补丁路径以外的差异：
  - 开发树独有的 kernelbase/registry.c 是旧 Linux Steam 桥接；有效构建已无此变更。保留原始差异记录，
    迁移时沿用 2.0.0 的移除决策，避免重新带入产品。
  - 两边的 winevulkan/vulkan_thunks.c 属生成输出；当前已用适配生成器重生成，8 项 Vulkan 输出均逐字节一致。
- 已保存两棵源码的二进制 Git 差异、未跟踪源文件及 SHA-256，按内容去重；辅助 .orig/.rej 只登记并保留原文件。
  全部保全内容约 0.8 MiB，不复制完整源码或运行时。
- 固定清单：`workspace_temp/v2-development/upstream-transition/local-adaptation-inventory.json`。
  其中含补丁顺序/哈希、开发树与有效构建树位置、提交及差异内容引用。
- 本次没有 reset、checkout 或修改任何现有子模块。

## 实施顺序与完成条件

### 1. 重建可持续维护的源码关系

- 为 FEX、Wine 建立独立的 OHOS 维护分支，以真实上游提交为父系。
- 从原 Valve Wine 基线与当前有效源码提取完整平台差异，再拆成有依赖顺序的主题提交；
  先在原上游基线上证明能重建当前有效内容，才向新基线迁移。
- 每项旧补丁必须登记为：保留并重放、已由上游等价修复替代、改写适配，或沿用已批准的功能移除。
  上游替代须给出提交及回归证据；冲突或不明来源项阻止交付。
- 新版本维护以整套上游版本合入为主；定向回移仅保留为紧急修复。
  不再制造无法追溯上游父系的整树替换提交。

当前进度：已重建 `ohos/current-baseline@a02acbd358e3`，79 个主题/补丁提交具有真实 Valve 父系；
209 项 fork 差异、78 项登记补丁及 11,227 个有效文件的完整 tree 均已核验。
详见 [等价重建](wine-baseline-reconstruction.zh-CN.md) 及对应 JSON。下一阶段开始 FEX 候选迁移。

### 2. FEX 完整升级

- 迁移完整上游源码，并检查当前分叉的 2 个提交；先保持现有 Wine 与图形组件不变，以便归因。
- 保住 OHOS committed lookup-cache、可执行内存/保护切换、SMC、
  native fault 路由、precise POP/moffs、合成返回、初始化/文件读取和 SIMD 正确性约束。
- 检查 preserve_all 与 Windows unwind 配套、SSE4.2 PCMP 内联等最新实现；不降低精度、屏障或同步要求。
- 以匹配的工具链和源码成组重建实际使用的 ARM64EC、WOW64 和 Unix 桥接产物；
  检查 ABI/导入导出/PE架构，禁止把旧桥接与新 DLL 混用。
- 先跑指令语义、异常、SMC、线程/退出定向回归，再在 Mini 测 Steam 页面响应。

### 3. Proton Wine 完整升级

- 接入所选 Proton 快照的完整 Wine 核心；迁移 OHOS broker、路径、信号、内存、
  ARM64EC/WOW64、图形、字体、音视频、输入和网络适配。
- Wine loader、wineserver、ntdll、win32u 与配套 PE DLL 成套构建、成套交付。
- Proton 的所用组件记录完整 pin；DXVK/VKD3D/媒体栈另分批同步与适配，保留必要兼容配置，
  不把未经验证的新图形依赖同时塞进首次 CPU/Wine 回归。
- 原上游 Linux 启动器、rootfs 与 Linux Steam 产品界面不属于 VPP 2.0.0 的交付路线；
  同步目标是 OHOS 平台使用的完整上游核心及组件实现，不能称为未修改的整个 Linux Proton 环境。

### 4. 固化防丢与构建入口

- 维护锁文件：上游 commit、OHOS 分支 commit、有序补丁 SHA-256、工具链、ABI、配置、组件产物哈希。
- 构建前从干净基线重放；检测未登记修改、漏补丁、重复补丁、冲突和旧缓存即失败，不自动跳过。
- 对成品 HAP 复核实际 DLL/Unix 库/运行时 ZIP 的来源与一致性；版本字符串和 Git 祖先不能替代成品检查。
- 复用固定增量构建目录，输入发生变化时可靠失效；当前仍按用户要求先做本地验证，不启线上 CI。

### 5. 验收与发布

- 先完成 WinSteam 登录/库列表/选游戏/点启动反馈、返回与重复操作。目标仍是常用操作 1–3 秒响应，
  超过 5 秒的无反馈单独记录；不能以个别指令加速比例代表整客户端收益。
- 此前用户限制当前只测 Steam 界面，游戏启动验收安排在用户恢复游戏测试之后，
  避免断开另一设备上同账号的游戏。
- 后续完成 x86/x64 游戏、DXVK/WineD3D、音视频/字体、输入坐标、后台恢复和退出重启回归。
- 各阶段分别留当前与一个回退包及小型结果记录；验证前不替换现有正式可用包。
- 完成条件是源码与补丁来源可还原、所用组件一致、定向回归通过和真机目标达成，
  “同步到最新”本身不等于性能达标。

## 本次交付边界

本分支完成初步迁移评估及当前 Wine 适配层的等价重建。Gecko 双架构默认打包已完成，
本地未签名 HAP 的运行时、Steam、产品边界及源码来源检查通过。
**没有完成上游升级，也没有新的性能或真机实测结论**。
详细结果见 [首轮评估](initial-assessment.zh-CN.md)、[逐文件证据](initial-assessment.json) 和
[适配台账](adaptation-ledger.json)。`rc-2.0.0` 仅发布冻结的现有运行时，升级候选不混入该版本。
发布构建结果另由 GitHub Actions 和 Release 的来源记录确认，不能将本评估视为发布通过证据。

## 官方来源

- [FEX 2611.1](https://github.com/FEX-Emu/FEX/releases/tag/FEX-2611.1)
- [FEX main 锁定提交](https://github.com/FEX-Emu/FEX/commit/ab135df091d005cc98dfddc56c3dc9c9e4bd501d)
- [Proton 稳定发布](https://github.com/ValveSoftware/Proton/releases/tag/proton-11.0-2)
- [Proton Experimental 锁定提交](https://github.com/ValveSoftware/Proton/commit/0764d2920a128cee74555afcab0819919ddf416d)
- [Valve Wine 稳定线与旧基线比较](https://github.com/ValveSoftware/wine/compare/dc26e61847081a1b5cb0733dc30feba6ee575482...e60892cc2398b0a83c332a2078c7b85e47bdb42b)
- [Valve Wine Experimental 与旧基线比较](https://github.com/ValveSoftware/wine/compare/dc26e61847081a1b5cb0733dc30feba6ee575482...e8ca5806eec488f5916e83d655ffc5349b6b1702)
