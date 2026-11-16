# Mini Zink 的 OpenGL 版本提升评估（2026-11-16）

当前范围已按用户要求收敛：只评估 Zink 对齐 VirGL 的真实 OpenGL 3.1；3.2 及以上暂停，不增加用户设置。这里只完成源码和已有证据研究，未修改渲染代码、重建 Mesa、操作设备或新增跑分。

结论：3.1 是合理目标，但尚未发现可直接复用的小修。VirGL 已有系统 GLES Transform Feedback，之前的 3.1 工作主要补 sRGB 存储语义；Zink 的 Vulkan 路径缺少 Transform Feedback，需要新的实现。不能把两条路线理解为共用同一组底层能力。

## 当前代码与证据

历史采集路径相对于工作区 `workspace_temp/mini-virgl-performance/`，用于追溯已有记录，不是构建输入。本文只归档结论，不将临时目录作为产品依赖。

- 权威源码：WSL `/home/liufeng/src/vpp-account-cloud`，应用提交 `4ba188e4`。Mesa 源码版本 25.0.1；原有第三方工作树仍有修改。
- 产品 Zink 路线是 Wine/WineD3D → Mesa Zink → Venus → 宿主 Vulkan。没有再经 VirGL 的 GLES 转译；Mini 也没有启用系统 Vulkan 直通。
- `entry/src/main/cpp/wine/wine_env.cpp` 的 Zink 分支没有设置 `MESA_GL_VERSION_OVERRIDE` 或 `MESA_GLSL_VERSION_OVERRIDE`。实际 Wine candidate 的 `wined3d_main.c:121` 默认 `max_gl_version=4.4`，不是产品将其固定在 3.1。本轮没有检查设备上每个游戏自己的注册表覆盖。
- `backend-heaven-comparison/candidate-gl-zink/probe.txt` 记录 Zink GL 2.1，3.3 core 上下文失败，基本 FBO/绘制通过。这个探针早于最终 2.0.3，不能冒充最终包的最新实测。此前确认的 VirGL GL 3.1 也不能当成 Zink 已支持 3.1 的证据。
- 使用较新的 `backend-heaven-comparison/zink-r12-vulkan-capabilities/probe.txt`，这是 query-only 的物理设备能力查询，设备为 `Virtio-GPU Venus (Maleoon 910)`、Vulkan 1.2。没有把 DXVK 日志中“未启用的 feature=0”误判为硬件不支持；例如该查询实际显示 tessellationShader=true。

机器可读的增量要求对照见同目录 `zink-opengl-version-research-20261116.json`，包含原始查询 SHA-256。它不是完整 Vulkan Profiles 结果或 OpenGL CTS 认证：格式能力及部分扩展 feature 没有完整记录，前级缺项也会影响后级。

## 逐级缺口

| 目标 | 已有证据与源码对应 | 研究判断 |
| --- | --- | --- |
| GL 3.0/3.1 | 没有 `VK_EXT_transform_feedback`，transformFeedback=false；Zink `max_stream_output_buffers` 因而为 0，Mesa 不开启 `EXT_transform_feedback` | 目前最明确的升级障碍。GL 3.1 新增的采样器/UBO数量记录足够，但仍依赖 3.0 |
| GL 3.2 | depthClamp、geometryShader、shaderTessellationAndGeometryPointSize、depthClipEnable 都为 true | 相比 3.3/4.1 新增硬件缺口少，值得在前级补齐后逐项验证；还要检查格式、GLSL及实际语义 |
| GL 3.3 | dualSrcBlend=false，且未列出 `VK_EXT_vertex_attribute_divisor` | 需要补双源混合与实例属性步进；仅让 3.3 上下文创建成功不够 |
| GL 4.0 | 细分、sample shading、cube array 已有；本地 profile 还列出 maintenance4/5，当前记录未列出，RGB32 texel-buffer 格式尚未核验 | 不能因支持细分就推断 GL 4.0 可用；需区分 profile 清单与实际执行路径、前级依赖 |
| GL 4.1 | multiViewport=false，maxViewports=1；1D/2D/cube尺寸上限均8192，要求16384；3D2048与array layers2048已满足 | 需要真正的多视口和大纹理支持，或较复杂的虚拟化。当前不适合作为 Mini 的短期目标 |

GL 2.1 的完整行为本身也尚未得到证明：记录仍缺 logicOp、自定义 border color、线光栅化等功能。Zink 最低报告 2.1，不意味着这些场景已全部正确。Heaven 跑通只覆盖其实际使用的功能子集。

核对入口：

- Mesa `src/mesa/main/version.c:269-345`：GL3.0至4.1的累计扩展与资源要求。
- Zink `zink_screen.c:1003`：Transform Feedback；`:929`：双源混合；`:919`：实例步进；`:1071`：多视口。
- Mesa `st_extensions.c:1129`：由 stream-output 能力开启 TF；`:1509`：双源混合扩展；`:1617`：至少16个视口开启 viewport array。

## 可以投入的方案

1. **只评估 GL3.1 所需的 Transform Feedback。** 当前 Venus 查询和较早原生 Maleoon 查询均未列出这个扩展，暂时没有“解除 Venus 屏蔽就能开启”的证据。先核对最终系统/包的当前能力；若仍没有，候选是 GPU compute/SSBO 捕获顶点结果。该设备 `vertexPipelineStoresAndAtomics=false`，不能直接往 vertex shader 插入存储指令就算完成。只就 GL3.1 核心而言，可以先不实现几何/细分输出、TF2 暂停恢复、DrawTransformFeedback、多流；输出顺序、primitive restart、索引/实例输入、缓冲边界、计数及同步仍必须正确。已有额外 shader 扩展的组合也不能冒报支持。
2. **先证实小范围可行性再决定是否改运行库。** 最小验证是无 override 的3.1上下文及确定输出的顶点捕获，不先更换整套Mesa、不增加空实现、不将TF buffer数量伪报为4。普通游戏没有使用TF时，候选实现应不额外执行compute、回读或等待。
3. **3.2及以上全部暂停。** 双源混合、多视口、大纹理虚拟化、FP64等不属于本阶段；前面表格保留为历史调研证据，不表示继续实施这些项目。设备专用桌面GL路线仍为未来保留项。

## VirGL 可以复用什么

本轮逐段核对确认：

- `thirdparty/virglrenderer/src/vrend/vrend_renderer.c:1637` 调用系统 `glTransformFeedbackVaryings`；`:6328` 调用 `glBeginTransformFeedback`；`:12748-12762` 根据宿主GLES TF能力报告4个输出缓冲。这是已有GPU功能的转发，不是CPU或compute通用模拟。
- `thirdparty/mesa/src/gallium/drivers/virgl/virgl_screen.c:367` 直接接收这些宿主streamout能力。`patches/mesa/0007-virgl-gpu-srgb-surface-storage.patch` 补的是VirGL sRGB表面，不实现Vulkan TF。
- `thirdparty/virglrenderer/src/venus/vkr_command_buffer.c:1309` 虽然存在TF命令分派，但只是转发Vulkan TF命令；没有在扩展缺失时替代执行的逻辑。增加扩展字符串不会提供实际执行。
- 旧的 `workspace_temp/gumu10-dx11-20261008/gl31-v2-final32.tsv:471` 和 `gl31-v2-final64.tsv:471` 均记录 GLSL140 下TF输出校验通过。这是此前VirGL证据，不是本轮新增真机结果。
- 可以直接复用 `scripts/probes/wgl_srgb_surface_probe.c` 的GL3.0/3.1语义验收，包括TF输出、GLSL140 UBO、texture buffer、实例绘制、primitive restart及sRGB；不需要另造一份同类探针。其现有TF测试只覆盖基础捕获，不能代替后续边界/计数测试。

因此能直接复用的是验证标准、Mesa共用状态跟踪和部分工具；VirGL的底层TF实现不能复制到纯Vulkan Zink路径。跨API共享GLES/Vulkan资源或把TF绘制改走VirGL，会引入上下文、资源同步和shader转换工作，不是当前的低成本方案。

优先看兼容性的实际收益。DXVK9直接翻译D3D9，不需要完整实现GL3.3；它能跑不能反证Zink具备全部OpenGL能力。提高GL版本也不保证Heaven/D3D9更快，新增模拟可能带来额外开销。

## 下一阶段最小验收

先在最终2.0.3上补一次无override的上下文版本和扩展查询，确认当前基线；复用现有GL3.1探针，与VirGL对照，不重复建包和跑Heaven。只有发现明确的小修或可行的GPU TF路径，才进入实现；若必须重做大块shader执行，则本阶段保留现状，不为抬版本牺牲游戏性能。实现后验收真实3.1上下文、GLSL140、功能绘制、TF数据/计数/边界和实际游戏，不追求3.2及以上。

## 一手公开资料

- [Mesa Zink能力要求](https://docs.mesa3d.org/drivers/zink.html)：能力取决于Zink实现及Vulkan驱动；分级要求、Vulkan Profiles入口。
- [Mesa 26.2.6发布说明](https://docs.mesa3d.org/relnotes/26.2.6.html)：Mesa实现GL4.6，不等于每个驱动或上下文都会报告4.6。
- [Mesa 26.1.0发布说明](https://docs.mesa3d.org/relnotes/26.1.0.html)：已有clip distance等局部模拟改进，不能外推为TF、双源混合和多视口已经通用补齐。
