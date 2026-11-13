# 原生 Linux Vulkan 内存共享验证

> 2026-10-31 最新状态：下文保留逐阶段诊断结论，早期“默认关闭／Banana 未通过”描述仅适用于对应轮次。BC、NativeBuffer 资源合同、softpipe 按需缓存和上游 FEX sigreturn 修复现已接入 HAP 自动交付。诊断组合已显示完整 UI、约 60 FPS 并完成点击，普通入口仍遇到原有 SQLite 缺表，不记录为完整游戏验收。见[交付说明](NATIVE_GAME_DELIVERY.zh-CN.md)和[信号对照](FEX_SIGRETURN.zh-CN.md)。

2026-10-30 最新状态：普通平板已通过 Steam/FEX/Venus 的 buffer、图像及实验 BC 适配测试，Banana 已真实使用 GPU 纹理转码，但完整游戏仍未通过画面、点击和持续运行验收。实验层没有默认启用到正式版本。最初的内存证据见 [脱敏记录](evidence/native-vulkan-memory.json)，后续实验按下文日期记录。

## 已验证的层次

| 路径 | 实际结果 | 范围 |
| --- | --- | --- |
| OHOS 系统 Vulkan，普通内存 | coherent 不显式 flush/invalidate、显式同步、GPU FillBuffer 均通过 | type 0，flags 0x7 |
| NativeBuffer 导入 Vulkan | 六组 case 全部通过显式 copy/fill；非 coherent 阶段正确跳过免 flush 测试 | 两种 handle，type 1，flags 0xb |
| Vulkan 内存导出 NativeBuffer | 两组 case 的 fd alias 与 GPU 同存储，copy/fill 均通过 | 两种 handle，type 0，flags 0x7 |
| 官方 ARM64 Steam → FEX → GNU Venus → OHOS receiver | 四轮 copy/fill，显式及免 flush/invalidate 的全部结果一致 | type 0，两个 64 KiB buffer，每轮校验 16384 个字 |
| 同一 Steam/FEX/Venus 链路，非 coherent 内存 | 显式 flush 后 copy、GPU fill 和 invalidate 后读回均通过；免 flush 阶段按契约跳过 | type 1，flags 0xb，两个 4 MiB buffer，每轮校验 1048576 个字 |

直接驱动探针通过 `OH_Ability_CreateNativeChildProcess` 运行，显式加载系统 `/system/lib64/libvulkan.so`，renderer 是 Maleoon 910。旧的 StartNativeChildProcess 诊断环境没有找到 ICD，不能当作 GPU 不支持的证据。直接测试通过不能代替跨进程验证；上述最后一行来自另一个实际执行的 x86_64 程序。

## 旧链路的确定问题

原 receiver 用匿名 shadow fd 承接客体 CPU 映射，与实际 GPU allocation 是两块存储。此前独立 buffer 测试已排除 Unity、游戏脚本、纹理和呈现窗口的影响：

- `full` 模式在等待后用零覆盖源数据；`precise-dirty` 保留源数据，但 GPU copy/fill 的目标仍全零。
- ARM64 Venus 映射能读到 x86 客体写入；host flush 路径的 shadow/host 内容一致。单纯归为 socket 丢数据不符合证据。
- coherent 首次写入若没有显式 flush，会被现有 persistent-map 标记漏掉；该标记也不能代表后续脏页。
- legacy invalidate 充当写入开始标记，不符合原生 Vulkan 读取 GPU 结果的语义。
- Venus 沿用 kernel 映射 coherent 的假定，把 type 0 从 0x7 改为 0xf，并隐藏非 coherent type 1 的 HOST_VISIBLE；这个假定不能用于匿名 shadow fallback。

这些问题足以要求修正通用内存契约，但还不能声称它们是 Banana 所有异常的完整根因。

## 新链路及隔离

权威源码保存在 [receiver 补丁](../../patches/virglrenderer/0007-ohos-native-memory-context.patch) 和 [Venus 补丁](../../patches/mesa/0010-venus-ohos-native-memory-context.patch)。实验构建入口 `scripts/diagnostics/build_native_proton_venus.py` 已接入补丁，缓存中的修改不是唯一来源。正式的 Venus 构建脚本和已发布依赖锁保持一致；诊断入口不会自动替换 HAP 驱动。

1. 客体显式开启 `VN_OHOS_NATIVE_MEMORY=1`，先查询 vtest `GET_PARAM 0x57480101`，只接受精确版本 1，再发送 Venus context flag `1 << 16`。
2. receiver 按 context 保存此能力。未知 flag、非 Venus capset、未协商激活和初始化后变更均拒绝。旧 Wine 不发送该查询，保留原内存契约。
3. 协商后的 Venus 保留真实驱动 memory flags。receiver 的 HOST_VISIBLE allocation 使用 Vulkan export → NativeBuffer → 真实 fd alias，不分配或复制 payload shadow。
4. 对 host-private OHOS 扩展单独查询驱动列表，不能依赖过滤后的 guest 扩展。内部开启扩展，不向客体伪装支持原生 OHOS API。
5. DMA fd 不是可加 seal 的 memfd。新增私有 fd 类，仅允许已协商的 OHOS 同进程 worker 接收；转换为内部共享资源后才走原有 vtest fd 映射。普通 SHM 的 seal 和 size 校验保留。
6. 分配大小按页向上对齐并检查溢出；导出 fd 校验实际 backing 大小，显式 flush/invalidate 直接调用驱动，释放时解除映射并归还 NativeBuffer 引用。

本轮先修正了 host-private 扩展漏查导致的 CreateDevice -8，再修正 DMA fd 被当成 memfd 拒绝的问题。随后 type 0 的四轮跨进程 GPU 数据测试全部通过，原先全零的目标缓冲区现与预期逐字一致。测试还原了官方游戏 launcher 和已安装 Venus，均验证 SHA-256。

## 边界与后续验收

- 独立小测试每次两个 64 KiB buffer、fence 上限 5 秒；直接探针进程上限 75 秒。全部日志、截图留在固定任务目录，脱敏摘要入库。
- 跨进程 type 1 的 4 MiB 验证已通过；仍需检查更大 allocation、suballocation、buffer/image 混用、反复释放及游戏持续运行。buffer 测试不能证明大堆或图像兼容。
- 不能用每次提交拷贝全部映射、强制去掉 coherent 或伪造同步成功代替直接共享。
- 原生 Linux 测试组件仍是实验候选；完成游戏验收和旧 Wine 回归前，不作为已验证发布版本。
- 曾记录系统 `ResourceLeak(GpuLeak)` / GPU 额度终止，事件的 `resource=4575420` 单位尚未确认，不能换算成 GiB；不绕过系统额度。应用分配与系统 DMA 统计分别记录，不能相加或互相替代。

## Banana 后续对照

打开显示阶段追踪后，首次 -EAGAIN 的目标约 109 ms 后附着，约 156 ms 后创建实际 OHOS swapchain。之后记录到 serial 621 的成功呈现，因此不能将整个黑屏期归为首帧目标永远未创建。游戏的 bootstrap 和原生 Steam API 已初始化，随后 FEX 子进程因 SIGSEGV / exit 139 退出，客体异常地址为 `0x615f616061616164`。该首帧追踪轮没有完整 live allocation 峰值，不能依据另一轮 GPU 额度事件认定此次也是显存超限。后续同轮分配统计如下。也尚未通过完整画面及输入验收。

新增私有 NativeBuffer fd 还补齐了 renderer 导出函数的 Debug 断言，主机测试执行该真实函数并启用断言验证。设备当前的 Release receiver 行为不受这一断言补齐影响；该源修改尚未重打入设备包。


## BC 展开与本轮崩溃的区分

2026-10-29 后续运行（FEX PID 53640）的 Vulkan 统计：应用分配峰值 **3,931,441,152 bytes（3.66 GiB）**，呈现 serial 1080 时仍存活 **3,400,417,280 bytes（3.17 GiB）**，allocation failures 为 **0**。当时有 1100 个 live image，其中 1062 个 RGBA8 image 的 memory requirements 合计 3,153,526,784 bytes。guest Vulkan 没有收到 BC 格式 image。因此显存压力真实存在，但此轮没有“超过 4 GiB／分配失败导致崩溃”的证据，也不能把全部 RGBA8 image 都认定为 BC 来源。

只读取两份 Unity 资产的 Texture2D 元数据，没有复制完整游戏资源：798 张 DXT1/DXT5 加 1 张 BC7，按实际尺寸及 mip 数计算，源数据合计 **619,433,416 bytes（590.74 MiB）**，展开为 RGBA8 约 **2,897,225,832 bytes（2.70 GiB）**。这是资产总成本估算，不代表全部同时驻留。它也没有包含视频解码帧、render target、buffer 及驱动额外开销。另一轮系统快照 Graph/DMA 为 3,685,140 kB，external Vulkan 为 3,432,071,168 bytes；它们可能统计重叠，不能相加。

这次 SIGSEGV 的指令位于 UnityPlayer 的内嵌 VP8 `decode_split_mv` 路径（ELF VA `0x18d4302`），读取本应为 0–3 的分区索引时得到异常大值。再一轮 GDB 捕获到 RIP `0x25e425e425e425e4`，栈及寄存器存在重复数据值；证明执行状态已被破坏，尚不能确定写坏来源。不能由故障 PC 就断言是游戏自身 bug、FEX bug 或 GPU 越界。

正在使用同一 Unity 库与未经重编码的原始 VP8 帧做独立 CPU 解码对照，不创建 GPU image。x86_64 主机单线程及四线程各 600 帧均通过，逐帧 YUV 汇总哈希一致。同一平板 FEX 链路也通过单线程及四线程各 600 帧，输出哈希均为 `18c145ad999bbd88`，因此没有在这个单视频独立条件下复现崩溃。四路并发、每路四线程各解码 150 帧也全部与主机哈希一致。继续检查图形混合负载；不能据此排除集成路径或其他片段问题。探针初始化/依赖失败不能作为解码失败证据。

### 再压缩接入边界

现有 Wine/DXVK 的 BC 解码及 ETC2/EAC/packed16 逻辑可作为参考，见 [纹理参考](../graphics/POMELO_DXVK_TEXTURE_REFERENCE_20261017.md)。Banana 当前为原生 Linux Vulkan，不经过 DXVK；且 Unity 已在调用 Vulkan 前展开 BC，单纯开启 DXVK 环境变量不起作用。

若选择通用 Vulkan 虚拟 BC 路径，必须同时适配 feature/format 查询、image/view/sRGB、memory requirements、上传、partial update、copy/readback、mip/layer 及释放语义，再按实际硬件支持选择 ETC2/ASTC。只伪报 BC 支持或在 CreateImage 时替换格式会破坏数据契约。理论上压缩 backing 能大幅降低本例的 2.70 GiB 成本，但目前没有原生 Vulkan 再压缩实现或画质/设备验收结果；有损压缩不得随内存压力自动启用。显存治理与 CPU/视频崩溃仍须分别验证。


完整游戏临时设置 `VPX_SIMD_CAPS_MASK=0` 的对照轮（FEX PID 12162）分配峰值为 **4,518,715,392 bytes（4.21 GiB）**、分配失败仍为 0。随后 20:06:30 整个应用及子进程消失，未记录到原先的 VP8 SIGSEGV，系统的具体终止原因仍待核实。此轮没有完成正常画面验收，不能把关闭 SIMD 视为修复，也不能认为未复现原崩溃就排除了该路径。两轮统计说明：BC 展开带来真实的大内存压力，但“4 GiB”并非本轮已确认的单次 allocation 硬上限。诊断 launcher、WSI、Venus 已恢复并核对 SHA-256。

另外，当前 receiver 沿用上游 Venus 的 external-memory 创建契约遗留问题：host-visible allocation 可被导出，而普通 image/buffer 未必带对应 external handle create-info。规范要求导出内存的 handleTypes 与资源创建匹配（[vkBindImageMemory，VUID 02728](https://docs.vulkan.org/refpages/latest/refpages/source/vkBindImageMemory.html)）。这是一项需要用图像/混合负载继续验证的风险，尚未确认为本次像素异常或 CPU 栈破坏的根因；不能仅凭 buffer 测试通过就验收全部 image 路径，也不宜只加一个 create-info 而遗漏 capabilities、requirements 和 dedicated allocation 约束。


## 诊断部署隔离

用独立小文件验证 HDC `file send` 覆盖会保留同一 inode；不能用它热覆盖进程正在映射的 ELF。媒体目录又拒绝 shell rename，不能假称已完成原子替换。本轮改为先停止旧柚并核实该应用 UID 下已经没有进程，再卸下旧 inode、上传 ELF 并核对 SHA-256，之后才启动 Steam。相同 SHA 的文件直接复用，退出和启动失败均按同一停进程流程还原。已用两个小 ELF 在平板验证部署、校验及复用。

此前热替换可能影响共享模块，因此其异常 PC 只能作为定位线索，需要在新流程下复现；尚未证明热替换就是前述栈破坏的根因。独立 VP8 默认测试没有更换共享图形库，其单流/多流校验仍单独有效。后一轮没有生成新的 Unity 日志，属于未触发游戏的无效对照，不能把旧 Player.log 的数字重复计算成新一次崩溃。


## 清单一致的重新验收（2026-10-29 晚）

启动流程会由 `LinuxSteamClientSupervisor.ets` 从 HAP 校验并还原 WSI，再从已验证的 GNU 组件还原 Venus。仅在停止应用时预上传实验库，并不能证明启动后实际用了它。已用启动后 SHA-256 确认先前的一轮被自动恢复为 baseline；该轮 132 秒后在 UnityPlayer ELF VA `0x18d5276` 发生 SIGSEGV，不能给它套用实验库的显存统计。

随后复用同一已编译 HAP，仅更换诊断 WSI 和对应清单；诊断 WSI 引用额外的固定 Venus 文件名，避免混用受启动同步管理的基线。签名验证、HAP 内容校验、启动后的两个组件 SHA-256 均通过。全应用 UID 没有进程时才上传/卸下 ELF；未热覆盖共享模块。退出后重装原 HAP，恢复官方游戏 launcher 和基线 WSI/Venus，核验校验值，清理额外 Venus。

这轮确实出现了 Banana 的完整正常游戏画面，截图约 30 FPS；但随后仍在约 330 秒因 SIGSEGV（code 2，fault address `0x67b67ff590`）退出。该地址靠近记录的原生 SP，仍有执行状态破坏线索，尚未找到写坏方。点击曾在这一阶段尝试，未完成响应验收；不能宣称画面、输入和稳定性全部修复。

同轮 Vulkan allocation 峰值再次为 **3,931,441,152 bytes（3.66 GiB）**、失败为 **0**，呈现追踪明确为 `pixel_readbacks=0`。因此这次非法访问并不需要“超过 4 GiB”或“热覆盖 ELF”作为前提。它与较早的系统内存额度终止、4.21 GiB 轮次分别保留证据，不互相借用峰值或死亡原因。

系统同轮对 receiver 记录了 ION 额度处理：`resource=3145728`、action `kill`，并执行前台核验；随后 receiver 的实际退出记录在测试器主动结束应用时才出现。该字段的单位仍未确定，不能把它直接当作本轮实际 GiB 用量或声称已证实系统先杀了 receiver。额度压力仍需治理，不能绕过系统策略。

另将原始视频不重编码 remux，增加 2000×2250（120 帧）、732×953（150 帧）、320×243（40 帧）三个 CPU 解码输入。相同 UnityPlayer 内嵌 VP8 在主机与平板 FEX 下，单线程和四线程全部通过；YUV FNV64 分别为 `a4be05545691efb4`、`064be0651a466276`、`1a8425d1296afe49`。这些结果补足了大尺寸和奇数尺寸边界，不等于所有视频或完整媒体集成都已排除。

后续应针对 Vulkan 资源 external-memory 创建/绑定/requirements 契约及图形与媒体的混合负载定位。BC 展开是独立的大内存成本；原生 Vulkan 虚拟 BC/ETC2 再压缩仍未实现，不能用 DXVK 的开关代替，也没有将关闭 SIMD、降低画质或改游戏资产作为产品修复。

## 资源创建契约候选（2026-10-30）

已在普通平板完成系统 Vulkan 图像探针及 Steam → FEX → GNU Venus → OHOS 的复杂图像测试，详见[脱敏记录](evidence/native-vulkan-resource-contract.json)。新候选是独立实验包；正式 1.5.3 HAP／APP、发布依赖锁和默认旧 Wine 通道未替换。Banana 官方启动器的持续运行与点击验收另行记录，不能将合成测试通过等同于游戏已修复。

系统探针的六组 64×64 UNORM／sRGB 图像，每组八轮数据校验全部通过。普通图像不要求 dedicated；两种私有 external handle（0x2000／0x8000）的 RGBA 图像均要求 dedicated。普通 ETC2／ASTC 可用，但 external ETC2／ASTC 不支持；BC1／BC3／BC7 均不支持。这表明普通纹理与共享显示资源的能力边界不能混用。

旧资源契约候选（SHA-256 `6b794305…6dcce9d`）的四轮 buffer 对照正确，但 127×65、4 mip、2 array layer 的第一个图像在 `vkCreateImage` 返回 -1，未进入图像分配或提交。该候选将普通 optimal image 也强制声明为 NativeBuffer external image。最小测试确定了这组参数下的创建失败；尚未分别隔离 mip／array 的驱动限制，也不能据此认定它是此前 Banana SIGSEGV 的唯一根因。

新 [0008 补丁](../../patches/virglrenderer/0008-ohos-native-resource-contract.patch) 与 [0007 补丁](../../patches/virglrenderer/0007-ohos-native-memory-context.patch) 对显式协商 `ohos_native_memory` 的 context 区分两类内存：

- 普通 optimal image 使用 GPU-only 内存类型，不添加 NativeBuffer external create-info，不导出 CPU 映射 fd。它可使用驱动正常的纹理、mip／array 和子分配能力。
- 在原 memory types 前插入一个 DEVICE_LOCAL 别名，仍指向原 heap，不增加或翻倍报告显存。原类型整体右移，cache/coherency flags 保留；allocation index、buffer/image requirements 的 type mask、fd properties 与 maintenance4 查询一致映射。传给独立 host allocator 的导出 metadata 保留真实 host index。
- HOST_VISIBLE 上传 buffer 继续 Vulkan export → NativeBuffer → fd alias 直接共享。linear／显式 external image 继续按实际 flags、usage、format、tiling 查询 external 能力，保留 pNext、format list／stencil usage，拒绝冲突 handle。
- dedicated allocation 保留请求大小；普通 heap 页补齐检查溢出。复用已有 export 节点，避免重复结构。此策略兼顾旧 Vulkan 1.3.204 与新规范；页补齐没有被确认为游戏崩溃根因。
- 旧 Wine 未协商 native-memory，memory types／indices 保持 identity；不进入私有资源查询。最终 OHOS GPU 呈现未增加 CPU 帧读回。

| 验证 | 结果 | 边界 |
| --- | --- | --- |
| helper／maintenance4／allocation 的主机测试 | 普通、ASan+UBSan 通过；包括 GPU alias、原 heap／cache flags、31→32 类型边界、分配失败后的 index 恢复、dedicated、冲突与旧 context | 驱动 stub，不代替真机 |
| native-memory 协商、DMA fd 隔离、启用断言的导出 | 通过 | 不证明游戏稳定 |
| 0007／0008 的 22 个小文件还原再应用 | 字节一致，完整补丁脚本重复执行通过 | 未复制构建树 |
| OHOS receiver 10 个单元及链接 | 通过；候选签名与 HAP 内容校验通过 | 包含新增的 physical-device 单元 |
| 主机 lavapipe 对照 | 128 次提交、mismatch=0 | 只验证探针 |
| 平板 Steam／FEX／Venus 图像测试 | 128 次提交、mismatch=0，四轮 buffer 同步均正确 | 不包含 shader sampling 或完整游戏 |

真机新候选实际报告 guest type 0 flags=0x1，上传 type 1 flags=0x7。两个复杂图像的 size=163840、alignment=4096、type bits=1、dedicated=0，在同一 allocation 子分配；staging 仍由 NativeBuffer 导出。此轮记录的 Vulkan allocation 峰值 983040 bytes，分配失败 0。组件在启动后核对 SHA-256，结束后已恢复官方 launcher 与基线图形组件。

探针由 `scripts/diagnostics/build_native_vk_coherence_probe.py --images --image-rounds 64 --steam-launch --memory-type 1 --output-dir workspace_temp/linux-proton/native-image-probe` 构建。它分别选择 staging 和 image 内存类型，校验 UNORM／sRGB、4 mip、2 layer 和 staging 首尾哨兵。单 allocation 不超过 16 MiB，fence 5 秒、进程 75 秒。读回只涉及合成像素，产品呈现没有加入 CPU 拷贝。

新候选的第一次启动停留在 Steam 启动遮罩，120 秒内未输出探针标记，属于未触发 GPU 的无效对照。随后请求普通桌面模式，启动请求后约 81 秒取得完整探针结果。Steam 登录／命令 ACK 不等于游戏已执行；各轮依照新鲜输出判定，不复用旧 Player.log。下一步以官方 Banana launcher 验证超过此前约 330 秒的故障时间及点击响应，独立记录真实退出原因。

### 官方 Banana 的本轮结果

同一 GPU-only 候选使用官方 launcher，确实显示完整 Banana 游戏 UI，直接截图约 58 FPS，累计呈现到 serial 2040，`pixel_readbacks=0`。这轮 Vulkan allocation 峰值 **3,935,635,456 bytes**，当时 live **3,404,611,584 bytes**，失败 **0**。约 397.7 秒仍发生 SIGSEGV（guest signal 11、code 1、address 0），所以复杂图像测试通过没有根治完整游戏退出，不能作为发布验收通过。

Unity 报告的末帧相对其 ELF load bias 位于 `0x18f1043`，指令为 `movq (%rsi), %xmm1`；前两帧属于信号处理／回溯路径，不能把它们当作原始故障点。该次没有保存原始 guest 寄存器，不能仅凭此断言是 FEX 指令错误或确定写坏方，现继续使用 opt-in RSP 捕获。

这轮点击尝试无效：测试器在设备 epoch `1793321065.576` 已因捕获 guest fatal 主动 force-stop，点击到 `1793321079.308` 才发送。因此未验证计数器响应，点击也没有触发这次崩溃。真实游戏退出与测试器随后关闭整个宿主需要区分；关闭后显示系统桌面不能另算一次宿主自行崩溃。官方 launcher、基线 HAP、WSI／Venus 均已按 SHA-256 恢复。下一轮仍使用同一候选，只增加调试器，不更改渲染、资产或压缩策略。

### 调试器复核及解码对照（2026-10-30）

第二次仍使用清单一致的 GPU 候选，只增加官方 FEX 的 opt-in 调试器。截获 SIGSEGV 时，guest RIP 为 `0x6cf86cf86cf86cf8`，栈首也是相同像素模式，多个寄存器包含像素样数据。已核对官方 FEX `FEX-2607-76-g37265b1` 对应 [CommandReadReg 源码](https://github.com/FEX-Emu/FEX/blob/37265b1/Source/Tools/LinuxEmulation/LinuxSyscalls/GdbServer.cpp)：该实现的 `p` 参数确实是字节偏移，`p80` 才读取 RIP；没有把标准寄存器编号误当作偏移。RSP 数据仍不能确定最先写坏内存的指令或所属模块。

调试器截获后会改变信号转交及恢复行为，因此此轮 `Player.log` 未出现 fatal 文本、测试器等满 480 秒，不能记为游戏稳定通过。该轮 Vulkan allocation 峰值 4,522,909,696 bytes、分配失败 0。GDB 已 detach、移除 fport 和诊断标志，官方 launcher 与基线组件按 SHA-256 恢复。

另在相同 GPU 候选中临时用 `VPX_SIMD_CAPS_MASK=0` 对照完整游戏，客体输出确认 launcher 已设置该选项，但没有直接读取运行中的 decoder dispatch 表，不能把环境标记单独当作全部 SIMD 路径均已关闭的证明。约 313.7 秒整个应用 UID 的进程已结束；系统在 09:13:00.714 记录主进程死亡，测试器到 09:13:02.925 才发 force-stop。这次未完成正常 UI 验收，未记录 guest fatal，分配峰值同为 4,522,909,696 bytes、失败 0；具体终止原因未确认，不能拿它证明 SIMD 是根因或已修复。

系统日志取证纠正：本机 `hilog -x` 与 `-z` 是互斥命令，组合会返回 CODE -31。此前该组合的空匹配不能当作没有系统内存事件的证据。本轮改用有界、带筛选的有效查询，仍未取得本次具体 kill reason。保留日志不超过 32 MiB；三次完整游戏的私有日志均以校验后的 gzip 保存，固定原始日志继续复用。

最小像素滤波对照另覆盖了故障附近同一真实函数（Unity ELF VA `0x18f0fea`）：源 stride 1984，height 1／16／21／32，8 个 subpixel phase，目标 stride 16／32／1984，4 种源对齐，栈与静态输出及 4 种像素模式，共 3072 组。按该函数真实系数表计算逐字节参考，连同输出首尾及行间哨兵一起检查；x86_64 主机和平板官方 Steam/FEX 均为 `guard_mismatches=0 result=0`，不创建 GPU 对象。首轮测试入口未成功加载 Unity 库，没有执行滤波，已按无效诊断处理；修正入口后才取得上述完整结果。这补足了该滤波指令和栈目标的独立检查，但不覆盖完整 Unity 中的调用参数、其他线程、GPU 混合负载或最先写坏内存的时刻。

完整游戏仍未通过验收。下一步需定位集成负载中最先出现的非法写入，并独立治理大纹理驻留压力；不能根据 SIMD 诊断或单个函数通过，发布关闭 SIMD、改变游戏资源或整帧回读的规避方案。

### Android 参考与压缩路线

核对了 [dxvk-ohos-legacy](https://github.com/PomeloTechLabs/dxvk-ohos-legacy) 的 BC→ETC2/EAC/packed16 参考和 [DroidDeck](https://github.com/Droid-Deck/DroidDeck) 的驱动加载方式。DroidDeck 的主要 GPU 路线是 Adreno／GNU Turnip，不能将其驱动用于马良。可借鉴它对 GNU 运行时和平台显示接口的分离。

更直接的通用 Vulkan 参考是 [GameNative Mesa wrapper](https://github.com/GameNative/mesa/tree/6bcfbaa701a8d1efa7f6c54145495eda5329f767/src/vulkan/wrapper)（`wrapper-25`，本轮核对 commit `6bcfbaa701a8d1efa7f6c54145495eda5329f767`）。其 BC GPU upload/copy/readback、HQ 编码／缓存和 dedicated memory 管理覆盖了单纯换格式无法解决的合同。它依赖 Android hardware buffer／驱动加载接口，仍需移植到 OHOS NativeBuffer 和现有 receiver；本轮没有直接引入 Android 库、复制整套 Mesa 源码或实现 BC 再压缩。

压缩的接入点必须在 Unity 查询 Vulkan 格式能力之前，否则本例已经展开为 RGBA8，无法从普通 RGBA8 上传可靠还原其 BC 来源。完整实现还须覆盖 BC 语义、sRGB／alpha、view、mip/layer、partial update、copy/readback、内存 requirements 和释放。只有这些路径可用后才能报告相应 BC 能力，并按真实支持选择 ETC2／ASTC；有损选项需明确质量策略。NativeBuffer 直接共享和最终显示仍复用现有后端。压缩有助于降低约 2.70 GiB 的资产展开成本，但它既未实现，也未被证明能消除已记录的 SIGSEGV。


最后以临时启动器把同一 `PlayerMain` 切为 `-batchmode -nographics`，180 秒总采样预算后由测试器结束应用。此模式将 Unity 输出送往 client stdout，没有生成新的图形 Player.log；旧 Player.log 不计入本次结果。实际输出确认 Unity 2022.3.62f2、NullGfxDevice 和游戏 bootstrap 完成，未捕获相同 fatal signal。但它也明确报视频着色器不存在，未证明执行了相同视频处理路径，所以不能用“无崩溃”宣称 GPU 是唯一根因或 FEX/媒体集成已排除。官方 launcher 及基线图形组件已恢复。

## GPU 再压缩原型与完整预测器对照（2026-10-30 续）

本轮新增的是可重建的合成探针，尚未向游戏报告虚拟 BC 格式能力，也未将未完成的转码挂到正式路径。脱敏的组件身份、原始测试事件和边界保存在 [GPU 转码证据](evidence/native-vulkan-texture-transcode.json)。没有重新复制游戏、修改资产或重打完整 HAP；复用已有候选包，结束后恢复原 HAP、官方启动器及基线 WSI／Venus，并核验 SHA-256、确认整个应用 UID 已停止。

### 原生 Vulkan 的压缩数据路径

新增 `scripts/diagnostics/bc1_etc2_probe.comp`：GPU 从 storage buffer 读取不透明 BC1 blocks，在同一次 dispatch 内解码到寄存器／线程局部变量，再输出 ETC2 RGB blocks；不用整张 RGBA 中间图，不依赖 Buffer Device Address。`etc2_sample_probe.comp` 随后通过真实 ETC2 image/view/sampler 采样。探针只回读合成测试输出用于校验，不读取呈现帧，也没有改变 NativeBuffer 显示通道。

通过实际 Steam → FEX → Venus → Maleoon 910 执行，而非通过主机软件 Vulkan 代替。设备报告 ETC2、ASTC、BDA feature 为 true、BC 为 false；BDA 仅查询，未启用或执行其地址功能。主机现有 lavapipe 不支持 ETC2，该对照明确记为跳过。

| 合成纹理 | 转码 block 数／每种颜色空间 | 检查结果 | 分配对照 |
| --- | ---: | --- | --- |
| 127×65 | 544 | UNORM／sRGB 编码和采样均零错误 | ETC2 为 8192 bytes；RGBA 原始像素为 33020 bytes，未查询该轮 RGBA requirements |
| 1021×509 | 32768 | UNORM／sRGB 编码、采样及尾部哨兵均零错误 | ETC2 requirements 262144 bytes；RGBA8 requirements 2097152 bytes |

转码 block 与独立 C 参考逐字比较，采样与标准 ETC2 individual-mode 解码／sRGB 线性化逐通道比较（允许 1/255 的舍入差）。边缘 block 和不整除的 dispatch 尺寸已覆盖。较大样本的提交到 fence 等待分别为 152.116 ms、9.171 ms，包含转码、上传、采样和合成回读；首轮还可能包括驱动延迟编译。这不是纯转码 GPU 时间、整批资产耗时或游戏 FPS 结论。

该原型使用 ETC1 individual mode（有效 ETC2 RGB block）再压缩，**有损**，尚无画质验收；范围仅为不透明 BC1、单 mip、单 layer。BC1 透明模式、BC3/EAC、BC7、mip/layer、partial update 与原生 BC image/view/copy/readback 均未完成。不能仅凭这两轮通过就向 Unity／Godot 伪报 `textureCompressionBC`。

可重建入口：

```sh
python3 scripts/diagnostics/build_native_vk_coherence_probe.py \
  --output-dir workspace_temp/linux-proton/texture-probe \
  --memory-type 1 --steam-launch --textures \
  --texture-width 1021 --texture-height 509
```

需要 x86_64 GNU GCC、Vulkan headers、glslangValidator；非默认工具位置用 `--glslang`。构建清单记录源代码、编译器、GLSL、SPIR-V 与可执行文件校验值，相同输入复用。此命令只构建，设备执行结果单独记录。

### 接入策略及真实节省边界

后续应在 GNU Vulkan 的格式／资源层提供通用 BC emulation，在引擎查询格式能力之前接入；对 Unity／Godot 都保持同一 Vulkan 接口。采样 image 使用普通 GPU-only ETC2／ASTC 存储，上传 buffer 和呈现继续复用已有 NativeBuffer 直接共享。压缩 image 不强制 external handle，避免设备已验证不支持的 external ETC2／ASTC 组合。

仅保留有损转码后的图像，再在回读时重新编码 BC，不能保证 Vulkan 压缩 block 的字节语义。完整实现优先保留原始 BC blocks 的紧凑 GPU buffer，partial update 同时更新它和采样 backing；copy/readback 从原始 blocks 得到精确结果，不把重新编码当无损。还必须管理命令提交后的临时资源生命周期及 mip／layer／view 状态。上述合同完成之前不启用对应能力。

较大探针的 **8 倍**仅为 ETC2 RGB backing 对 RGBA8 的单纹理 requirements 比例。保留原始 BC、处理 alpha、上传峰值和分配粒度后，总节省会变小。按之前资产元数据的 BC 590.74 MiB／RGBA 2.70 GiB 估算，若原始 BC 与等尺寸压缩 backing 各保留一份，约为 1.15 GiB（不含 staging／分配开销）；这只是可行性估算，既不是同时驻留量，也不是 Banana 已测得的新峰值。

### 崩溃路径继续缩小

新增 `native_vp8_predictor_probe.c` 及构建脚本，调用所核验 Unity ELF 中完整 16×16 预测函数（VA `0x18da0b0`），包括函数自身的小栈 scratch、横向与纵向 SSSE3 helpers。外部构建参数必须提供 ELF SHA-256、函数／版本偏移；构建器检查 ELF 架构、segment 权限和范围，保存函数字节指纹，不修改库。该诊断可对其他经过检查的 VP8 实现提供偏移参数，不构成游戏补丁。

主机与普通平板官方 Steam/FEX 均完成四线程 24576 组：64 种横纵 phase 组合、stride 32／64／1984、四种源对齐、栈／堆输出及四类像素模式；像素逐字节与独立 VP8 六抽头标量参考一致，输出前后／行间哨兵无错误。该结果补足此前仅直接调用纵向 helper 的覆盖，但不能排除完整 Unity 中的参数、并发媒体／图形负载或最先出现的非法写入。

**本轮没有对完整 Banana 再做相同输入的重复运行，也没有宣称其 SIGSEGV 已修复。** 新的实测进展是 GPU 再压缩与真实采样可行、完整 VP8 小栈预测器在独立条件下正确；下一步是补齐通用 BC 资源合同并定位集成负载的首个非法写入。正式运行时、原 OHOS Wine、DXVK、virgl、字体及媒体库没有被本轮探针替换。

## 通用 BC 运行时与系统退出原因（2026-10-30 后续）

上述原型之后已接入实验性的通用 BC 资源层，详见 [实现范围](../../entry/src/main/cpp/linux_steam/gnu_fixture/native_bc/README.md) 和 [本轮脱敏证据](evidence/native-vulkan-bc-runtime.json)。它在游戏查询格式前提供能力，保留原始 BC blocks 做精确拷贝／读回，GPU 输出 ETC2/EAC 或对应未压缩 backing；显示继续使用 NativeBuffer。正式交付默认关闭，旧 Wine／DXVK 路径未进入该层。

普通平板实际完成 Vulkan 1.1 应用的 11 类 decoder（每类 128 blocks）与独立 bcdec 对照，以及全部 16 种 Vulkan BC 格式的资源测试。31×17、3 mip、2 layer、非零子分配、行间 padding、跨压缩／普通 image 的两次原始拷贝、局部更新、采样和哨兵均零错误，计算 pipeline／push constants 恢复通过。Vulkan 1.1 下显式启用 KHR BDA，核心函数缺失时使用 KHR 入口；两者均缺失时返回错误，禁止空函数调用。SPIR-V 以 Vulkan 1.1 目标验证通过。探针仍不等于完整 Vulkan 一致性或所有 mip/layer 的采样验收。

官方 Banana 的两次完整启动分别在请求后约 76.4 秒和 67.9 秒结束整个应用 UID。实时系统日志明确记录 `StabilityCheckKill`：内存管理服务先发 SIG9 到旧柚主进程及 receiver，再由生命周期清理 GNU／Steam 子进程。第二轮框架同时记录 `FOREGROUND=1` 与相同 kill reason。两轮均未在此前记录 Unity fatal；不能把 runner 随后主动收尾的 force-stop 当作退出原因。此前的独立 Unity SIGSEGV 仍未解决，这两次内存回收不能替代其根因分析。

两次最后记录的 Vulkan allocation 峰值分别为 1,537,458,176 和 1,775,920,640 bytes，分配失败均为 0。这只说明本轮分配计数及压缩已工作，不是游戏峰值总物理内存，也不能同不同加载阶段的旧轮次计算节省比例。截图未完成正常 Banana UI 和点击验收，**Banana 仍未修复通过**。

恢复基线 HAP／WSI／Venus 后，仅启动 Steam、不启动游戏，浏览器进程 PSS 在两次约相隔 25 秒的采样间从 693,895 KiB 增至 2,630,680 KiB。第二次最大匿名映射 RSS 为 2,263,192 KiB。该增长也存在于没有实验 BC 的基线，需要继续区分分配器空闲内存和活跃分配。不得把不同进程 RSS 相加当作独占物理内存，或直接把匿名映射等同于纹理／内存泄漏。

诊断仍使用固定目录、有界日志和官方游戏文件；所有 ELF 在全 UID 停止后替换、启动后核对 SHA-256。结束后恢复基线签名 HAP、WSI／Venus 和原始启动器。游戏、账号、存档与用户图形配置未被此轮修改。

### BC1 透明度与 Steam 缓存驻留（2026-10-30 续）

BC1 RGBA 现改用 ETC2 RGB A1 backing，每 block 8 bytes，保留 0／1 alpha；此前为 16 bytes 的 ETC2 RGBA8。新增全透明、混合透明、不透明三色／四色块的硬件采样检查，并用独立 bcdec 参考核对 alpha。平板再次通过全部 11 类 decoder 和 16 种资源格式，`mismatches=0`，原始拷贝、局部更新及哨兵检查仍通过。小纹理受分配对齐影响，不能把 block 大小变化直接当成全游戏的显存节省比例。

另查明 Steam 浏览器的大块活跃分配来自 GNU Mesa softpipe 的 `sp_create_tex_tile_cache`：一次有效调用点采样累计请求 1,890,099,072 bytes、7,204 次调用（累计值，非同时存活量）。softpipe 按 shader stage／sampler slot 预建纹理缓存，初始化每个 tile 的 invalid 位会触碰大量尚未使用的像素页。该问题在不运行游戏、未启用实验 BC 的基线中也存在。

新增 [GNU softpipe 补丁](../../patches/mesa/0011-softpipe-demand-zero-texture-cache.patch)：保持原缓存容量及采样算法，匿名 demand-zero 映射按需驻留，使用紧凑有效位图，失效／flush／换绑时清除 last-tile，释放时配对 munmap。仅接入 GNU Steam software GL 构建清单，没有加入原 OHOS Wine 的 Mesa 补丁列表。实际缓存代码的地址 0 首次访问、命中、失效、hash 冲突、解绑／重绑及释放测试通过，ASan／UBSan 通过；未使用缓存仅驻留 1／65 页。这是缓存生命周期测试，不能代替完整 OpenGL 渲染验收。

使用同一签名诊断 HAP，仅替换 software GL 的平板采样中，浏览器启动早期活跃堆为 40,713,872 bytes，后续约 237,700,000 bytes；一份 PSS 快照为 643,847 KiB。此前基线同类进程曾为 2,630,680 KiB。阶段不同，不能作为严格等阶段 A/B 节省比例，也不能把不同进程的 RSS 相加。原始采样保留在私有有界日志，公开记录仅包含上述脱敏数字。

官方 Banana 入口在该缓存版本中越过了先前约 70 秒的系统回收阶段，但请求后 128.1 秒仍记录 Unity SIGSEGV，Vulkan allocation 峰值 2,399,513,344 bytes、失败 0。增加透明 guest signal 观察器后，139.8 秒再次失败，峰值 2,421,115,648 bytes、失败 0。因此内存治理与先前独立客体崩溃仍须分别处理。

临时通过 Steam 启动选项请求 `FEX_MULTIBLOCK=0` 的单变量对照，一度显示完整游戏 UI、约 54 FPS，随后 135.5 秒仍崩溃，峰值 2,436,333,568 bytes、失败 0。点击检查在游戏已结束时拒绝发送，因此没有点击通过证据。该轮无法读取 FEX 的受限 `/proc/.../environ`，只确认 Steam 保存了临时选项；不能宣称已读取转译器的最终配置。测试结束只恢复原 LaunchOptions，保留 Steam 对其他设置的更新，没有发布逐游戏配置。

新观察器显示该轮 guest PC 在所加载 x86 libc 的 `read` 系统调用（ELF offset `0x11496a`）；另一轮仍落在 VP8 路径。不能把在 native syscall／thunk 中保留的 guest PC 直接当作首个非法访问指令。下一步在 FEX 转交异常之前捕获原始 ARM64 MAPERR 上下文：独立编译宏、最多 8 次上下文与一次 96 KiB maps，保留真实信号转交，不影响发布构建。

上游 [FEX 多块错误状态修复](https://github.com/FEX-Emu/FEX/commit/7469fdb0d6d6deb2628c2f8f34ff834d19edcade) 与 [配置说明](https://wiki.fex-emu.com/index.php/Development:Configuring_FEX) 只作为对照线索；目前没有证据证明该提交与本例同源。**Banana 的完整输入及持续运行尚未通过，BC 仍默认关闭。**


### 原始 ARM64 故障与启动验收边界（2026-10-30 继续）

在 FEX 转交之前新增独立编译宏控制的 MAPERR 观察，最多 8 次，只记录现场后原样转交。首轮真实 host PC 为 0、LR 非零；客体最后保存的 PC 落在 libc read，不能把它当作原始故障指令。第二轮 127.5 秒再次崩溃，真实 PC 与 LR 落在 `[anon:FEXMemJIT]`，访问地址为 0，客体 PC 映射到 Unity ELF offset `0x18d3a27`。现场尚不足以区分非法 guest 指针、转译器代码错误或之前的内存破坏，不将“host context”直接解释为 FEX 本身缺陷。

这轮截图确认完整游戏 UI、约 60 FPS；经过前台/PID 检查发送一次点击后，计数仍为 2，因此点击未通过。新增 LR 周围指令定位发现 `/proc/self/mem` 无法取回代码，后续改为 raw `process_vm_readv` 自身内存读取，保留真实错误结果，禁止直接解引用未知故障地址。上述观察不进入默认发布编译。

后续两次 Steam 登录日志虽报告 Success，前台持续停留在 Steam 启动画面，gameprocess_log 没有新的 Banana 进程，均按“未进入游戏”记录；旧 Player.log 的 fatal、BC upload 和 allocation 计数不作为本次结果。为继续定位，使用同一候选、Steam 冷启动带 AppID 的独立路径进行对照，不直接执行游戏绕过 Steam。

### 有效单块对照及 HSL POP 修复（2026-10-30 后续）

后来通过 `STEAM_FEX_MULTIBLOCK=0 %command% -force-vulkan` 执行新一轮，在本轮新建 `app.json` 中实读到 `Config.Multiblock="0"`，确认选项生效。仍在请求后 188.2 秒出现 guest fatal，Vulkan allocation 峰值 2,440,527,872 bytes、失败 0。测试结束仅恢复原 LaunchOptions。该结果补足此前环境变量未核实的边界，不能将禁用 multiblock 当作修复。

随后对照用户指出的 HSL 修复，确认 Linux Steam 的 Valve FEX 缺少原 Wine 已有的内存 POP 精确异常状态补丁。真实 Steam/FEX CPU 探针中，官方二进制和同工具链重编译的未修复版均有 2 项失败；仅加入 POP 补丁后 0 项失败。SIMD 全 lane 与 POP [SP] 寻址均保留正确结果。源码、可重建入口和验收边界见 [FEX POP 修复说明](FEX_POP_FAULT.zh-CN.md) 及 [脱敏证据](evidence/native-fex-pop-fault.json)。

完整 Banana 用同 revision 的补丁版运行仍在请求后 124.9 秒 SIGSEGV。该次完整 UI 约 60 FPS，呈现至 720 帧标记，`pixel_readbacks=0`，Vulkan allocation 峰值 2,444,012,288 bytes、失败 0；点击前后计数均为 2，输入未通过。内存 POP 缺陷已确认，但它没有解决此前 `pop rsp`／坏返回栈相关的集成崩溃，不将两种指令混同。观察到的本地数据库缺表异常也未确定与 native crash 同源。

已恢复基线 HAP、官方 Valve FEX、软件 GL、WSI／Venus、supervisor 和原始游戏启动器，核验身份并结束应用 UID。补丁保存在诊断构建入口，未替换正式组件，实验 BC 仍默认关闭；**Banana 尚未完成输入及稳定性验收**。

继续捕获完整当前 block 后，最新 115.1 秒的崩溃已映射到 VP8 8 像素横向预测函数返回时的坏保存帧，仍未找到最先写坏者。详细结论与边界见 [返回帧分析](FEX_POP_FAULT.zh-CN.md#继续定位视频函数的返回帧2026-10-30) 及 [脱敏证据](evidence/native-fex-return-stack.json)，不重复用无分配失败计数排除内存破坏。
