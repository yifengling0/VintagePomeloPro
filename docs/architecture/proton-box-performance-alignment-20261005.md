# Proton / FEX 对齐 main / Box64 性能的实施思路

日期：2026-10-05。基于[main / Proton 分支对照](richman8-main-proton-virgl-comparison-20261005.md)，以及本轮复核的生产源码。本文是工程方案，没有实施以下性能改动，也没有取得新的设备 A/B 数据。接手入口见 [dot 交接说明](dot-performance-handoff-20261005.md)。

## 建议采用的路线

先把现有 WineD3D / VirGL 路径做成“资源上传少复制、资源访问少等待、跨 WoW64 边界少往返”，再根据实测决定是否将更多 D3D 工作移到 ARM64。保留 FEX 基座；目标是用户实际玩的游戏在相同显示质量和正确性下，达到 main 的启动、首次交互及持续帧时间。

当前 ARM64 Mesa 已减少了 main 所需的 guest Mesa 指令转译，但 RichMan8 的 i386 WineD3D 仍由 FEX 执行，OpenGL 又逐次进入 native Unix 层。更少的转译代码量不等于更短的关键路径：跨边界、32 位映射、读回和等待仍可能抵消收益。

已有地图约 20–24 FPS 对应约 42–50 ms 的显示帧间隔，宿主已计时合成工作通常约 2.5–6 ms。优化宿主有价值，但不宜把主要投入放在末端 quad 绘制。CPU、GPU 与等待可以重叠，最终判断必须来自同一帧的时间线，不能直接相加所有线程 CPU 时间。

## 优先级与落地条件

| 顺序 | 改动 | 主要改善对象 | 当前依据 | 进入实施的条件 |
| --- | --- | --- | --- | --- |
| 1 | 修复像素格式查找；验证 0035 映射刷新 | 正确性、意外回退、人物残缺 | 已有静态下标缺陷和生产函数刷新反例 | 正确性问题可以直接修；不预设 FPS 收益 |
| 2 | 减少 buffer shadow 复制与无效读回 | 动态顶点、资源首次上传、稳态帧率 | WoW64 shadow 和 VirGL readback / wait 路径确实存在 | 记录实际命中、字节和耗时，选择收益最大的资源类别 |
| 3 | 恢复 guest Mesa 磁盘 shader cache | 重启后的首次 shader 准备 | VirGL 已实现 disk cache，当前构建禁用 | 验证 OHOS 文件操作、命中和失效；确认这些工作占首次卡顿 |
| 4 | 缩小 vtest 等待范围、优化 CSMT 等待 | CPU / GPU 并行、周期性停顿 | host busy-wait 忽略资源 handle，WAIT 可触发 context finish | 先证明等待耗时和 fence 完成行为，再替换同步策略 |
| 5 | 小批量 OpenGL Unix 调用 | draw / state 调用密集的 32 位游戏 | 生成的 thunk 每次调用 UNIX_CALL | 微基准和游戏计时证明边界成本明显 |
| 6 | 单个全屏 GPU 面合成快路径 | 桌面模式额外开销与延迟 | 当前多面合成每轮处理多个 consumer | 存在唯一可见不透明覆盖面；弹窗出现立即回通用路径 |
| 7 | 原生 D3D 后端或 FEX JIT / AOT 优化 | 更高性能上限 | i386 WineD3D 尚在转译，FEX 有 AOT 加载基础 | 前面收益不足且采样证明对应部分是瓶颈 |

## A. 最值得先做的代码工作：buffer 数据路径

关键代码为 `opengl32/unix_wgl.c`、`wined3d/context_gl.c`、`wined3d/buffer.c`，以及 Mesa 的 `virgl_resource.c` / `virgl_vtest_winsys.c`。

当前不仅可能有“低地址 shadow → 高地址映射”的复制，也可能在写映射之前做一次 host → guest 的资源读回。Mesa `virgl_res_needs_readback()` 明确区分：没有 discard、资源又不是已知 clean 时，即使是 WRITE / UNSYNCHRONIZED，也可能需要读回。`virgl_resource_transfer_prepare()` 在 readback 前后存在资源等待。

不能仅看到 GL map flags 中没有 INVALIDATE 就认定 Wine 丢了 DISCARD：Wine 已有 streaming buffer 的 DISCARD / NOOVERWRITE 轮转，busy DISCARD 也会创建替代 BO。应追踪同一个资源在 D3D、GL、Mesa 三层的实际处理，找出仍保留旧数据但实际上不需要保留的情况。

建议分两步：

1. **先完善现有策略。** 复用 shadow 容量、按有效 dirty range 发布，合并相邻上传；验证 DISCARD 后的旧 backing 是否正确退休、NOOVERWRITE 是否只写无冲突区域。按资源类型统计，避免反复 Map / Unmap 整个大 buffer 来更新小片段。0035 已负责显式 flush 发布正确性，后续必须保留。
2. **再为高频资源做低地址 backing。** 优先考虑 vtest 共享内存中 CPU 可写、生命周期明确的 buffer，由 Wine 协调低于 4GB 的保留区域，让 guest Mesa 返回游戏可以直接访问的地址，减少 shadow 往返。需要明确地址空间所有权、32 位区间不足时的回退、释放与重映射、alignment 及显式 flush 语义。仅适用于确有共享 backing 的路径，不把所有 GL mapping 一律改成低地址。

成功判据：每帧复制字节、native map 等待和 upload / transfer_get 次数下降；人物与贴图正确；帧时间的尾部改善。只看到 memcpy 更少而 FPS 不变时，应转查同步或转译工作。

## B. 同步优化：让等待只覆盖真正的依赖

当前 `vtest_resource_busy_wait()` 中资源 handle 被标为 unused；WAIT 且存在 `VTEST_SYNC_GL_FINISH` 时，会先执行 `virgl_renderer_context_finish()`，再检查 implicit fence。它保守地等待 context 的工作，可能阻断本可并行的资源更新。

建议按难度逐步推进：

- 先记录 WAIT 的上下文、提交 / 完成序号、总等待时间、finish 次数，以及触发它的 map / readback。
- 验证 fence 可靠推进后，用明确的提交完成点替代不必要的整 context finish，保留已知驱动问题的回退。
- 如果仍有明显假依赖，再维护资源的最后使用序号，让 wait 覆盖该资源最后一次使用；这要求提交侧跟踪资源引用，不能只在 host 改一个条件。
- CPU 写入、GPU 使用和 buffer 回收通过 ring / staging 资源并行；对真的读回或重叠写入保持必要等待。

WineD3D 的 command stream 也有独立成本：consumer 最多自旋 2000 次后进入等待，client 自旋达到 200 次后可走 NtDelayExecution。转译器不同，每轮自旋及原子指令成本也不同。采样若证明空转或反复唤醒占比高，可比较更短的自旋预算和可靠的事件等待；保留入睡前检查队列的握手，防止漏唤醒。CSMT 开关只作为诊断变量，单线程更快不代表所有游戏都应全局关闭 CSMT。

`VTEST_SYNC_GL_FINISH` 目前按变量是否存在判断，字符串 `0` 仍会启用。实验入口必须按代码语义设置，并记录最终有效值。

## C. 缓存：先解决可重复的首次准备

guest Mesa 的 `virgl_disk_cache_create()` 已存在：以构建身份和 host caps 生成 key，再调用 `disk_cache_create("virgl", ...)`。当前 `-Dshader-cache=disabled` 将它关闭，启用不是从零实现缓存。

建议启用后指定应用私有可写目录，设置容量与清理策略，验证打包后的 build ID、caps 和升级失效行为。测试四种状态：同一进程首次 / 重复访问，以及重启游戏后冷缓存 / 热缓存。以 cache hit / miss、compile/link 时间和首次角色帧时间证明效果。

这不等于 host GLES 编译器也被缓存，更不会缓存游戏解压或所有 WineD3D shader 生成。host program binary cache 是另一个能力受限的项目，需要验证系统驱动支持、驱动版本 key、加载失败回退和程序状态恢复。

FEX 的 Windows `ImageTracker::LoadAOTImages()` 也有加载缓存代码基础，但尚未核实本 OHOS 产品的生成、写入、加载与升级失效闭环。不能仅打开一个 AOT 开关就承诺持久缓存生效；优先于此的是 guest Mesa 已有缓存的完整验证。

## D. 中期架构：减少高频 WoW64 往返

当前生成的 `opengl32/thunks.c` 中，`glBindTexture()`、`glDrawArrays()` 都逐次执行 `UNIX_CALL`。一帧的状态设置与 draw 调用越多，边界开销越有可能被放大。这里值得做原型，但尚未测出它在 RichMan8 的实际占比。

可行的较小范围方案是**同线程、同 context 的有界命令批次**：

- 从无返回值、仅标量参数的状态调用开始，累积后一次进入 ARM64 Unix 层，按原顺序执行。
- 后续才考虑已绑定 VBO / EBO 的 draw；client arrays、传入指针等需要复制或固定生命周期，不能直接保存会失效的指针。
- 查询结果、GetError、Map、读回、Flush / Finish、Swap、context 切换、共享对象同步以及未支持调用之前，都排空已有批次。
- 批次有容量与提交延迟上限；保留错误顺序、线程 context 所有权和对象生命周期。
- 改生成器 `make_opengl` 及其配对 Unix 分派，而不是只改生成后的 `thunks.c`。

该方案先沿用现有线程，避免引入第二个 GL worker 与额外的线程同步。它仍有复杂的 GL 可见性边界，必须由 API 语义测试、32 位指针边界及实际游戏一起验证。若有效批次被频繁查询打散，收益可能很小，应据此停止扩大改动。

## E. 长期架构：把更多 D3D 工作放到 ARM64

性能上限较高的方向是让 FEX 集中执行游戏，图形状态处理、shader 翻译和命令执行尽量由 native 后端承担。

但 RichMan8 是 32 位，不能直接用 ARM64X DLL 替换 i386 D3D9。需要显式处理 32 位 COM 对象、调用约定、指针布局、回调、资源句柄、Lock 返回地址及生命周期。可选设计为薄 32 位 D3D9 前端 + 稳定命令 / 资源协议 + ARM64 后端；不能把现有含裸指针的 WineD3D CS packet 当成跨架构稳定 ABI。

这是独立的工程项目，适合在边界计时证明确实有足够收益后推进。64 位游戏的 ARM64EC / ARM64X 路径与这个项目应分别评估。

D3D9 → DXVK 也可以做每游戏可恢复的后端对照，但当前包未接管 d3d9；i386 DXVK 依然需要 FEX，后面的 Venus / Vulkan 同步也有成本。它能比较翻译后端，不能自动实现原生 D3D9，也不保证老游戏更快。

## F. 呈现优化：保留多窗口能力的快路径

对“一个可见、不透明、覆盖内容区域的全屏 GPU 面”，可以缓存场景结果、减少未变化 SHM 层扫描和重复状态设置，只在窗口身份 / 代次、几何、遮挡或内容变化时失效。弹窗、菜单、透明面出现后回到通用合成。

隐藏的 producer 仍要按 NativeImage 所有权协议消费和归还缓冲，不能只停止绘制后任其塞满队列。黑边清理、输入命中、前台状态和窗口代次也必须共用同一场景状态。

main 的 GLES direct 功能默认未启用；没有证据支持把“恢复 direct”作为恢复旧性能的必要步骤。当前先优化现有队列和合成快路径，更容易验证收益与回归。

## 如何验证“对齐 Box 性能”

建立两类对照，避免把目标与归因混在一起：

1. **产品对照**：main 包与 Proton 包，同游戏数据 / 存档、同分辨率、同窗口模式、同温度区间与前台状态。分别记录启动到菜单、首次选角色、重复选角色和地图稳态。用于判断用户体验是否达到 main。
2. **内部对照**：保持 Proton / FEX，逐项只改 buffer、缓存、同步或 batching。用于确认是哪项带来收益。若要单独判断转译器，再固定相同 Wine / Mesa / host 后端做专项实验。

主指标为游戏 Present 与实际消费帧时间的中位数、P95 / P99、长停顿次数、首次操作耗时和图像正确性。首次资源测试不能全在热缓存下做；冷 / 热顺序交替，观察多轮分布。建议把“Proton 稳态帧时间中位数在 main 的 5% 以内、P95 与首次操作耗时在 10% 以内”作为初始工程目标，按测量波动校准；这不是已取得的结果或对所有游戏的保证。

最小诊断不需要全量 GL trace：按一秒或操作窗口汇总跨边界次数、shadow 字节、map / readback / fence 等待、shader 编译耗时、CS 队列空转与积压，再用少量慢帧时间戳串起关键路径。诊断本身也要做开 / 关对照，避免它改变结果。

推荐首批落实：**像素格式安全查找 + buffer / wait 轻量计时 + shader cache 验证**。第二批根据结果处理动态上传和同步；只有边界开销被证实明显，才进入 OpenGL batching 或 native D3D 后端设计。
