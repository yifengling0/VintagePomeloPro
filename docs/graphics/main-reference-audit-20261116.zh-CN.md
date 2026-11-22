# main 对照喵基因图形修复清单的核对

核对基线：`7683a58b2f746b07979c9a56ac8e2dc240ad838f`，2026-11-16。

结论：不能整体合入参考代码。EGL 检查、Surface 生命周期、实例化入口回退、共享 FD 和设备清理已有对应实现。**收到 `GIVE2COMBINE.zip` 后，源码和新增主机模型确认 main 的 Zink shadow 同步仍有覆盖缺口；参考补丁也不是完整修复。** 优先修正非零 buffer 读取偏移、线性纹理读写方向和局部同步范围，再考虑 Maleoon 条件补丁。

用户后续确定：**配置不接入，Box64 已废弃。** 不迁移喵基因自动选择、游戏专用环境或 Box64 路线。其余修复交给另一个会话按 [合并方针](main-graphics-merge-plan-20261116.zh-CN.md) 实施；本文配置源码分析仅保留为审计记录。

这修正了仅凭文字清单时“shadow 同步已有对应机制，无需叠加旧补丁”的过宽判断。已有机制及其测试通过，不代表所有映射路径都已覆盖。这里确认的是源码和范围模型中的问题，尚未证明这些路径就是当前喵基因画面异常的实际原因。

本轮产品实现保持现状，只修复两份过时的主机测试桩，使已有回归能针对当前实现运行。没有新建安装包、部署设备、修改 gitlink、提交或推送。

## 基线与验证方法

Windows 仓库切到用户指定的 main，并从旧的本地 main 快进到 `origin/main`。原 `feature/main_proton` 分支指针未移动，原有未跟踪 `AGENTS.md` 保留。

复用 Linux checkout `/home/liufeng/src/vpp-account-cloud` 和已有 `winehua-dev` 镜像。第一阶段该 checkout 的应用 HEAD 与上述基线相同；Makefile、PluginManager、EglRenderer、VirGL presenter、重绘状态头及 Wine 环境实现逐文件 SHA-256 与 Windows main 一致。新增 ZIP 对比期间，Linux checkout 被其他工作推进到 `9e4f2f24daf8bb6c57f1fdf2aaf71004e40286ff`；因此新增脚本显式按 Windows main 提交读取 gitlink，并在运行前校验 Mesa 应用脚本及全部 Mesa 补丁的 SHA-256 与 Windows main 一致，未将变动后的 Linux HEAD 作为本报告基线。源码只读挂载，第三方导出和测试二进制位于容器临时目录，退出即回收。

第三方源码按 main gitlink 从 Git 对象读取所需文件，再在小型临时导出中重放当前生产补丁；未将已有 dirty 工作树冒充干净基线：

| 组件 | main 固定提交 |
| --- | --- |
| Wine Valve | `cd547f7a0ee9d3d59b3ec47317a7852dedf0443b` |
| Mesa | `557fa3e781ea6e78fcff3ee759c25f60c3055064` |
| VirGL renderer | `bb33e52aa03199926f662e50ff22bd00d19d7e0a` |

最初文字材料中的 `raw.githubusercontent.com` 未提供完整 URL 或提交；后来收到的 ZIP 提供了可逐文件比较的实际源码，但仍不包含完整上游版本、调用方或运行时二进制。`docs/upstream-sync/wine-candidate.lock.json` 记录的新 Wine candidate 尚未提升为生产 gitlink，也不能与生产 Wine 混用。

按用户补充，本轮直接以给出的行为清单分析，完整原文链接不是继续工作的前提；判断依据是 main 当前代码及针对性验证。

## 逐项判断

| 参考项目 | main 的实际实现 | 是否需要新增产品修复 |
| --- | --- | --- |
| 1. 喵基因专用 Zink 配置 | 参考头文件使用 Box64/x86_64 环境；main 有自己的 Zink 环境生成链 | **用户明确排除，不接入。** 不迁移头文件、游戏自动识别、专用环境或 Box64 路线 |
| 2. EGL 创建/绑定失败停止 | `egl_renderer.cpp` 的 `Init()` 检查 display、config 数量、context、surface；`RenderLoop()` 在 `eglMakeCurrent()` 失败后置 `running_=false` 并立即返回 | **已有，无需重复修改。** 这与着色器/管线创建失败是不同阶段 |
| 3. 实例化 ARB 入口回退 | 固定 Wine 的 `dlls/opengl32/unix_wgl.c::wrap_wglGetProcAddress()` 已有 `glVertexAttribDivisor` → `glVertexAttribDivisorARB`；递归进入 ARB registry 后仍检查扩展和驱动入口 | **已有，无需移植旧 `wgl.c` 补丁。** 当前 PE 侧 `wgl.c` 转发 Unix 调用，返回约定也不同 |
| 4. 旧 Surface 回调保护 | `PluginManager` 先创建替代 NativeWindow，成功后才关闭旧 renderer；Init 接管 window，失败由 renderer 析构清理；resize 比对 surfaceId；destroy 按真实 Surface 找当前 owner；Shutdown 停止、唤醒、join 后释放资源 | **已有，无需重复修改。** EGL 管理回归通过；本轮测试不将 X11 专属分支算作设备验收 |
| 5. Maleoon 冗余 ShaderLayer | 当前 Zink 没有 `remove_unused_shader_layer()`。`nir_to_spirv.c` 根据 NIR 的 Layer 输入/输出信息声明能力，未发现该参考补丁的 SPIR-V 后处理 | **条件性候选。** 需当前失败 shader 证明存在“无实际 Layer 使用但带能力声明”，并确认设备匹配范围；仅函数缺失不足以证明当前游戏失败由此导致 |
| 6. Guest/Host shadow 数据同步 | 已有 `0015`–`0017`、`0023`、`0024`，覆盖部分读取、staging 和上传；但 buffer invalidate 仍使用 `trans->offset`，线性纹理读映射及局部上传没有完整处理 remote shadow | **需要修复覆盖缺口，不能直接叠加参考补丁。** 参考补丁修正部分方向与偏移，但线性图像读范围遗漏 stride/depth，局部写范围还遗漏原始 map 原点。详见新增模型结果 |
| 7. 共享 FD 泄漏 | `patches/virglrenderer/0010-render-resource-fd-close.patch` 在 SCM_RIGHTS 发送后始终关闭发送方 FD，涵盖发送失败 | **已有。** 从干净 pin 重放后，2048 次延迟接收保持 FD 数稳定；旧行为反例按预期失败 |
| 8. 设备清理 UAF | `0009-device-object-cleanup.patch` 抽出 buffer/device-memory release，显式与设备隐式销毁共同使用；先解绑定关系和映射，再调用驱动销毁 | **已有。** 双清理顺序及重复 release 的 ASan/UBSan 测试通过；旧顺序反例触发预期 UAF |

### 现有图形链路与本次边界

当前链路是 Wine/WineD3D → Zink → Venus → Host Vulkan。`wine_env.cpp::AppendVulkanRuntimeEnv()` 配置 Vulkan ICD、库路径和 `VN_WINEHUA_REMOTE_MEMORY_SYNC=1`；Zink 分支再加入 loader、上传/回读、varying、3D 上传及 GPU present 开关。不能用参考清单的几行覆盖这整套环境，也不能将其改为所有游戏默认配置。

按用户后续决定，本次不实施游戏自动选择或参考环境迁移，Box64 已废弃。后续合并仅处理既有 ARM64/FEX 图形链路中的通用修复。`mewgenics-zink-v6` 只是参考方案标记，不能作为本项目完成相同验收的证明。

若确认 Maleoon 的冗余能力声明问题，应在当前 Mesa 源码上注册独立补丁，并更新 `apply_mesa_ohos_patches.sh`、`graphics-stack.lock.yaml` 和构建身份。扫描必须保留实际变量/结构成员上的 BuiltIn Layer，对异常 word count、越界指令和无法确认的布局保守返回；先用真实失败 SPIR-V 与保留 Layer 的正例做对照，再验证管线与游戏。不得对所有 GPU 无条件删除声明。

## 新增 ZIP 源码对比

输入为 `C:/Users/liufeng/Downloads/GIVE2COMBINE.zip`，28,204 字节，SHA-256 为 `32f522f7c7dcb93e9ba481687d06fa67675c21bd27a7aa717ba7802ace69c248`。共 10 个文件，展开 91,840 字节；压缩包及说明文字只作为参考材料，未执行其中内容。逐文件大小及哈希保存在 `artifacts/main-graphics-audit/reference-input.json`。

### 已有修复与不宜覆盖的文件

- `virgl-device-cleanup.patch` 与 main 的 `patches/virglrenderer/0009-device-object-cleanup.patch` **逐字节相同**。
- `virgl-render-resource-fd-close.patch` 与 main 的 `0010-render-resource-fd-close.patch` **逐字节相同**。
- Wine 实例化入口功能已在生产 pin 的 `unix_wgl.c` 实现；参考包针对旧 `wgl.c`，不能照搬文件位置和返回约定。
- IME 参考补丁还修改旧版 `STATUS_NOT_IMPLEMENTED` 分支，当前 pin 已没有该分支；main 的 `0071` 覆盖仍适用的 unconsumed 分支。
- 参考 `egl_renderer.cpp` 和 `plugin_manager.cpp` 不能整文件覆盖。main 已有更新的 Vulkan 呈现、X11、独立 owner、暂停和生命周期逻辑；所需 EGL/Surface 保护已有，不应覆盖这些后续能力。

### Zink：需要修复，但参考实现也需调整

从固定 Mesa pin 的干净导出分别构建“main 生产 overlay”和“参考 shadow 补丁”两种源码。参考补丁原样不能应用：两处上下文空行与 pin 不同；临时导出只补两处空行后即可应用，未更改其同步逻辑，也未写入第三方工作树。

主机模型提取实际 buffer 同步片段、线性图像同步片段及整个 `zink_transfer_flush_region()`，用独立 Host/Guest 数组模拟 shadow；Vulkan 操作仅模拟对应区间复制，atom size 为 1。它用于检验同步方向和字节范围，**不是完整 Mesa、Vulkan 驱动或游戏执行结果**。

| 场景 | main 生产 overlay | 参考补丁 | 处理建议 |
| --- | --- | --- | --- |
| 直接 buffer 读取 `[64,72)`，`trans->offset=0` | invalidate `[0,8)`，目标读取失败 | invalidate `[64,72)`，读取正确 | 在 direct/staging 分支统一使用实际映射偏移，并保留 BO 子分配偏移 |
| 线性 3D RGBA8 读取，非零 x/y、两行两层、rowPitch=32、depthPitch=128 | coherent remote 路径未 invalidate，正确行数 0/4 | 改成 invalidate，但按 width×height 计算连续范围，只覆盖 1/4 行 | 按 block、row/layer stride 和目标种类计算真实范围，检查各层/行；可优先考虑隔离 staging 路径 |
| 非 coherent 线性纹理 write-only map | 调用方写入前 flush，可能用旧 Guest 数据覆盖 Host | 不提前 flush | remote 模式区分 READ/WRITE，读取前 invalidate，写完才发布 |
| 局部线性 2D 写入，原点 `(1,1)`，两行各 8 字节 | flush 全部 512 字节，写入区域到达，但覆盖其他 Host 数据 | flush `[16,56)`，漏掉 map 原点，上传不完整且覆盖无关数据 | flush box 是相对 map 原点；必须加 `ptrans->box` 原点，并避免上传未修改的行间隙 |

原点问题另经生产 `transfer_unmap()` 核对：它复制 `ptrans->box` 后把 x/y/z 清零再调用 flush，确认传入的是相对坐标。参考补丁却仅以 `trans->offset=srl.offset` 加相对 box 计算位置，不能代表非零原点的直接 image map。

建议拆成独立修复提交：

1. **先修 buffer offset**：复用 main 的 `WINEHUA_ZINK_SHADOW_READBACK` 条件，在 remote readback 时使用实际 `map_offset`，验证非零 BO offset、direct 与 staging，覆盖 write-only 不回读。
2. **再修线性图像同步**：读取和发布都明确 map 原点、相对 flush box、block geometry、rowPitch、depthPitch/arrayPitch。不能简单将所有对象改成 noncoherent 或把整幅图像按一个连续区间发布。实际 Vulkan 的 nonCoherentAtomSize 可能扩大区间，需检查 transport 的精确范围能力；必要时改走独立 staging，或在明确 CPU/GPU 所有权的前提下保存扩大范围内的未修改数据。
3. **保留已有批量上传和 PBO 修复**：新增补丁应接在生产 overlay 上，更新补丁注册及图形锁文件；不能用参考补丁替换 `0015`–`0025`。
4. **最后做设备验证**：捕获命中的映射路径、实际 layout 和帧结果；分别验证初次上传、GPU 写后回读、局部更新、持续运行及退出。当前模型不能证明游戏必然命中线性 image map，也不能据此宣称斜向三角形或所有纹理故障已解决。

### 喵基因策略与运行时边界

实际头文件比最初文字描述更具体：只在 `device==phone`、`driver==virpipe`、`qualified && ready` 时生效；自动选择允许 `Mewgenics.exe` 或以 `/_windows/win64/darkest.exe` 结尾的路径。显式 `renderer=zink` 则允许该设备/驱动条件下的其他程序。`qualified` 和 `ready` 是调用方传入的布尔值；包里没有它们的资格评估、运行时检查或接入代码，不能把头文件本身视为完整安全门控。

环境固定 `venus_icd.x86_64.json`、`x86_64-unix` 与 `BOX64_*`，属于已废弃的 Box64 路线。main 图形锁的生产路线是 aarch64 Wine/FEX；本次不接入这份配置，也不借此清理仓库中残留的旧构建方案。

在当前 Mesa pin 的 `src` 及生产 overlay 文件中，未找到 `ZINK_WINEHUA_SHADOW_TRANSFERS`、`ZINK_WINEHUA_BO_CACHE_MB`、`ZINK_WINEHUA_READBACK_CACHE` 或 `VN_WINEHUA_SYNC_NOTIFY_RING` 的处理。参考包仅给前一个 key 的实现，没有后面三个 key 的配套补丁。main 使用自己的 shadow 开关和 `VN_WINEHUA_ALWAYS_NOTIFY_RING=1`；这些分析解释了参考环境的依赖缺口，不构成接入任务。`BOX64_DYNACACHE=0`、`ZINK_DEBUG=nobgc,noreorder`、禁用 shader cache 等均不迁移。

实际头文件编译验证通过：13 项断言覆盖手机/驱动/资格/就绪限制、路径大小写、显式 renderer、legacy override 及固定 x86_64 环境。

### Maleoon 候选

main 确实缺少参考 `remove_unused_shader_layer()`。提取参考函数后，9 项主机断言验证：移除闲置能力、保留变量及结构成员的 Layer、零 word count/越界提前退出、调整 tcs 索引，以及设备名子串匹配。这里只用了合成指令字，未运行真实 shader 的 SPIR-V 验证器或设备管线。

建议作为独立的 Maleoon 条件补丁准备；先记录设备实际暴露的名称，再保留失败 shader，验证声明确实冗余。`strstr(...,"Maleoon")` 能匹配 `Virtio-GPU Venus (Maleoon 910)`，但不等于已经验证所有 Maleoon 型号。该扫描检查了指令总长度边界，并不是完整的 SPIR-V 语法验证器。

## 其他四项

- **虚拟 Esc/IME**：`patches/wine/0071-ime-key-scancode.patch` 已注册；从生产 pin 重放后，30 组实际消息字段检查通过，包括 repeat、extended、keyup 和 IME 消息类型。
- **切回前台/恢复黑屏**：`RendererRepaintState` 用请求序号保留重绘；EGL 成功 swap 后、Vulkan presentation serial 前进后才确认请求。回归覆盖“绘制途中又来一次请求”，旧帧不会确认新请求。
- **中文字体与旧文字掩码**：已有 `0023`–`0026` 字体扫描、灰度 AA、locale/charset 回退，完整 Wine overlay 重放和四项字体回归通过。但这些不等于参考材料中特定字体别名或游戏 16 位 mask 修复。当前 Wine 通用 `draw_glyph_555/16` 已能处理部分灰度覆盖；要改旧游戏文字路径，仍需具体字体请求、目标格式及消失前后的像素/掩码证据，不能全局更改 AA 阈值。
- **DirectDraw 白屏**：`0070-ddraw-clip-surface-bounds.patch` 已注册，先保留原始 scale/source 映射，再裁剪目标与 clip list 查询；本轮完整 Wine overlay 重放通过，未运行 DirectDraw 游戏验收。

## 本轮落地与验证

只修改以下测试文件：

- `host_tests/gl_present_failure_test.py`：补上当前 presenter 使用的 source context、真实 `SurfaceQueueRecovery` 状态及 reset 语义。
- `host_tests/renderer_lifecycle_test.py`：补齐 X11 旁路和 isolated-toplevel 接口桩，使原有 EGL 生命周期测试继续编译；未用桩替换生产管理方法。

15/15 项主机检查通过：Mesa/VirGL 干净 pin 补丁重放、Zink batch upload/present、FD、清理 ASan/UBSan、GL 失败恢复、VirGL 呈现同步、renderer 生命周期/作用域、graphics contract、IME 补丁/消息、字体完整链和 WGL alias 组合。生命周期输出 708 checks；WGL 直接编译生产 wrapper，18 组组合覆盖核心/ARB 能力、驱动入口、无 context 和未知名称，禁用与本游戏无关的 Steam 特例。

主机结果、所用源码 SHA-256 与固定提交记录在工作区 `artifacts/main-graphics-audit/results.json`，详细输出为同目录 `verification.log`；小型复核脚本在固定 `workspace_temp/main-graphics-audit/`。它们是审查证据，不是产品构建输入。

ZIP 新增检查单独记录在同目录的 `reference-results.json`、`reference-verification.log`：两种 transfer 模型、Maleoon 9 项及游戏策略 13 项断言均完成；其中 transfer 模型的失败字段是有意保留的源码缺口观察，不是“图形验收通过”。这与原来的 15 项检查分开记录，不能合并成一次完整验收。`reference-results.json` 内还包含本轮 Windows main 的输入哈希和复用 Linux checkout 的实际 HEAD。

本轮未编译产品 HAP、未操作设备、未运行喵基因，也未验证低内存长时间退出。历史 Heaven/其他游戏结果见已有 Zink 记录，不能合并为本轮喵基因或所有设备的完整验收。

## 存储收尾

已再次执行工作区 `maintain-storage.ps1 -Apply -CompactLogs`，报告无错误、无可清理的已登记旧包或大日志，脚本回收 0 字节。本任务主动删除 ZIP 临时展开的 91,840 字节及先前的完整 WGL 临时导出；原始 ZIP 留在 Downloads，未复制。固定任务目录和证据目录合计保留不足 80 KiB 的结果、哈希、日志及复核脚本，第三方导出及测试二进制随临时容器退出释放。

前次统计 `workspace_temp` 45.26 GiB、`artifacts` 46.31 GiB，均高于整理目标；本轮收尾时 F 盘可用约 30.33 GiB。已有目录含私有回退备份、Git/源码、正式发布包、当前/回退调试包及历史证据，本轮未仅凭目录名或时间扩大删除范围。正式 release、唯一备份和共享增量缓存继续保留；本任务没有新增大包或复制运行时，其他任务的并发磁盘变化不归因于本次小型源码对比。
