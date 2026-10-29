# 原生 Linux Vulkan 内存共享验证

2026-10-29：已在普通平板通过真实 GPU 及 Steam/FEX/Venus 全链路的 buffer 一致性测试。Banana 的画面、输入及稳定性仍在验收，不能把 buffer 通过等同于游戏兼容完成。详见 [脱敏证据](evidence/native-vulkan-memory.json)。

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


最后以临时启动器把同一 `PlayerMain` 切为 `-batchmode -nographics`，180 秒总采样预算后由测试器结束应用。此模式将 Unity 输出送往 client stdout，没有生成新的图形 Player.log；旧 Player.log 不计入本次结果。实际输出确认 Unity 2022.3.62f2、NullGfxDevice 和游戏 bootstrap 完成，未捕获相同 fatal signal。但它也明确报视频着色器不存在，未证明执行了相同视频处理路径，所以不能用“无崩溃”宣称 GPU 是唯一根因或 FEX/媒体集成已排除。官方 launcher 及基线图形组件已恢复。
