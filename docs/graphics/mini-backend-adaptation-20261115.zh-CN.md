# Mini 三路线适配补充：参考 DXVK/DX11 与系统桌面 OpenGL

2026-11-15。当前任务基线提交 f14294da。用户补充：Edge 上的铠大师在新系统 7.0.0.111 支持 OpenGL 4.1；本轮对这一分支只调研，移植成本高时暂缓。没有接触 Edge、手机或其他设备，没有安装新 HAP。

## 决策

Mini 主线继续保留 WineD3D → VirGL → GLES。DXVK9 已修好一个可重复验证的上传同步缺口，继续作为实验路线；固定功能顶点着色器仍未通过。Zink 仍需补全资源可见性与实际 GL 能力。系统桌面 GL 是 Edge/PC 的独立候选，不作为 Mini 当前优化的前置条件。

没有新的有效 Heaven FPS。历史 Low / 720p / D3D9 基线中位 16.3 FPS，+30% 目标为 21.19 FPS。小型探针正确性通过不等于 Heaven 通过或性能提升。历史 CPU 约255%（100%=一核）；连续 GPU busy/timestamp 仍缺失，不能断言 GPU 满载或排除 GPU。

| 路线 | 已确认的能力/结果 | 当前优先动作 |
|---|---|---|
| VirGL/GLES | 当前 Mini guest GL 3.1，宿主 GLES 3.2/Maleoon 910；稳定 Heaven 基线可用 | 保留稳定路线；按关键路径研究 socket 往返、常量传输和 draw 内等待 |
| DXVK9/Venus | 本轮可编程 RGBA、DXT1/3/5 绘制、像素读回、Present PASS；关闭上传修复回到黑色 | 修复固定功能 VS 编译；继续按 DX11 契约补齐其他 D3D9 资源更新路径 |
| WineD3D/Zink/Venus | 当前实际 guest GL 2.1；上一轮 clear/triangle PASS，Heaven 黑屏条纹 | 检查完整上传/读回/Present；列出真实 GL 版本门槛，逐项实现或明确限制 |
| VirGL/系统桌面 GL（候选） | 官方文档支持 PC/部分平板；Edge 7.0.0.111/铠大师 GL4.1 为用户提供的设备线索 | 只保留能力探测和接入设计；未实测普通 VPP 应用可用性 |

## DX11 适配中真正可复用的内容

`docs/customization/dxvk-legacy.md` 与当前源码已有：shadow flush/invalidate、BC CPU 解码、bool specialization、combined sampler、Cube/CubeArray Dref、border color、部分格式和查询兼容。它们横跨 DXVK 共用层、D3D11 前端和 DXBC 编译器。

D3D9 使用 `d3d9_device.cpp`、固定功能生成器和 DXSO 编译器。共用 DXVK 层不意味着 D3D11 的 Map/Unmap、常量和纹理上传发布点已自动覆盖 D3D9。Zink 的资源管理在 Mesa 内，也不能直接复制 D3D11 前端代码。

可复用的核心契约：CPU 写入结束后，按实际物理 slice 发布到宿主；GPU 写入完成后再 invalidate 到 guest；跨线程 rename、DISCARD/NOOVERWRITE 和命令顺序必须保留。应复用传输层能力，同时在每个前端能确认写入完成的位置调用。不能用每次提交复制全部映射内存或扩大队列来替代正确同步。

## 本轮定位与修复

### 1. 已解释先前 draw 后 readLock 不返回的失败链

宿主 `vkCreateGraphicsPipelines` 返回 -1（VK_ERROR_OUT_OF_HOST_MEMORY），guest Venus 异步创建先返回 VK_SUCCESS；后续 `vkCmdBindPipeline` 找不到 pipeline object135/type19，引发 decoder CS error 与 context fatal。后续 Wine 异常和 readLock 不完成不能直接归类成性能等待。

`rgba-exact-shaders` 捕获最终 shader 的大小/FNV 与宿主一致：VS 8068 bytes/0xdb324e82，FS 6328 bytes/0x3ee4a7c0。两份最终模块均通过 `spirv-val --target-env vulkan1.1`。标准校验通过不代表专有驱动一定能编译，也不排除管线接口/状态组合问题。

定向隔离结果：

| 探针组合 | pipeline / readback 结果 |
|---|---|
| 固定功能 VS + 固定功能 FS | pipeline=-1，随后 CS fatal |
| 全部 scalar 特化按实际状态固化，宿主 specialization=null | 仍 pipeline=-1；排除单纯去掉特化信息即可修好 |
| 可编程 VS + 可编程 PS | 创建通过，读回完成，初始黑色 |
| 可编程 VS + 固定功能 FS | 创建通过，读回完成，初始黑色 |
| 固定功能 VS + 可编程 PS | 仍 pipeline=-1、CS fatal |

这把创建错误集中到固定功能 VS 或与其相关的输入/输出与管线组合，尚未定位具体 SPIR-V 指令。该对照改变了 VS/顶点声明及坐标数据，不足以断言一定是单条 shader 指令。下一步应沿 DX11 的精确 pipeline replay 方法缩小，而非盲改线程数。

### 2. 0010：固定功能 bool 状态修复

原 `freezeBoolSpecConstants` 只为资源绑定 bool 使用实际值；其他 bool 使用 SPIR-V 默认值。D3D9 `fog_enabled` 默认 false，故会忽略当前 pipeline 的雾效开启状态。

`0010-preserve-pipeline-specialization.patch` 将 pipeline 常量范围内的 bool 取自当前 `state.sc.specConstants`，保留绑定掩码和其他 bool 默认语义。没有新增默认开关；仅修正既有 Venus/Maleoon bool workaround 的状态来源。主机回归执行生产函数，覆盖 fog on/off、绑定掩码、无 pipeline 参数和无关 bool；ASAN/UBSAN 与 SPIR-V 校验通过。设备雾效视觉验收仍待固定功能 VS 通过。

全 scalar 固化实验没有解决错误，已从生产补丁撤出；仅在本轮证据中保存小型诊断 diff、构建哈希和负结果。

### 3. 0011：D3D9 上传可见性修复

`0011-d3d9-opt-in-shadow-upload.patch` 在 DrawPrimitiveUP / DrawIndexedPrimitiveUP 的顶点与索引 slice，以及 RGBA/BC 纹理上传 slice 的消费前，调用 DX11 已使用的 `ctx->flushMappedBuffer`。发布在 CPU 写完之后按 CS 顺序执行，失败明确抛错。源纹理带偏移的 slice 长度改为剩余长度，避免新增 flush 范围超出资源末端。

开关 `WINEHUA_DXVK9_SHADOW_UPLOAD=1`，默认关闭。与之前的 `WINEHUA_DXVK9_SHADOW_READBACK=1`、BC 显式实验一起使用。没有修改系统/每游戏默认选项，也未替换 Heaven 的 DLL。

最终 DLL SHA256：`2d369eff1ddd7d690b2dec7256367d92e4de12cb3b86067621b9e5c26fe81f13`，4276224 bytes。

| 本轮最终 DLL 探针 | 结果 |
|---|---|
| rgba-upload-negative：上传=0 | Ff000000，黑色；读回完成 |
| rgba-upload-positive：上传=1 | Ffff0000，红色；Present=0；PASS |
| dxt-upload-positive：DXT1/3/5 | 各自红色、读回及 Present PASS |
| rgba-upload-product-route：非 trace profile | 红色、读回及 Present PASS；实际仍 Venus |

前两个对照使用同一 EXE/DLL、同一 trace-frame-association profile，仅改变上传开关。后一个使用 isolate-vulkan-direct profile，实际 Vulkan device 仍是 Venus，不能记为 Direct 已通过。两个 profile 的 host 同步设置不同，均不是 FPS 对照。

探针创建/上传 8×8 四级 mip，但像素只检查默认采样中心；没有完成逐 mip、部分更新、alpha 混合、全部动态 buffer、indexed UP、常量 buffer 和完整老游戏矩阵。UP 与纹理发布同时启用，尚未分别量化两者贡献。后续补齐常量、普通 VB/IB Lock/Unlock、软件顶点处理、converter 等发布点时，必须保证 flush 在最后一次 CPU 写入之后，不能简单加到提前入队的 invalidate lambda。

## 系统 OpenGL 4.1/4.2 调研

### 已证实、用户线索、未证实内容

- 当前 Mini 证据：`backend-correctness/gl-virgl.txt` 返回 `3.1 Mesa 25.0.1`、virgl(Maleoon 910)，clear/draw 像素 PASS；宿主日志为 OpenGL ES 3.2 B312。两者是不同 API 层，版本数字不能混用。
- 华为当前公开 OpenGL 文档写明：API20 支持 PC OpenGL3.0；API22 支持 OpenGL4.2，并扩展到部分平板；通过 `OH_Graphics_QueryGL` 判断。文档列出了 `libGLv4.so`、`libEGL.so` 及应用 `NEED_OPENGL=1` 配置。
- 用户提供：Edge 的 7.0.0.111 与铠大师支持 GL4.1。公开检索能确认 7.0.0.111 版本报道；本轮没有找到将该精确版本、Edge、铠大师和 GL4.1 对应起来的官方技术说明，所以不据此反推其内部后端。
- 铠大师鸿蒙6.0新版官方页面明确宣传 DX11 GPU 加速，未披露具体 GL/Vulkan/虚拟 GPU 实现。本轮不下载其程序、不提取闭源库，也不把通用 DevEco 模拟器的主机 OpenGL4.1运行要求当成铠大师实现证据。

### VPP 的接入成本

当前 `virgl_host_config.cpp` 固定 VTEST_USE_GLES=1；`virgl_surface_presenter.cpp` 创建 ES3 context；`shader_utils.cpp` 使用 `#version 300 es`；AppScope 没有 NEED_OPENGL 声明。这是一条完整 GLES 路线，不能只改版本字符串或取消一个环境变量。

可行设计是可选的 host API 分支：

1. 先在普通应用进程与实际 renderer child 中查询系统能力、创建真实 desktop context，记录 renderer/GL/GLSL、关键扩展与像素结果。只加载系统公开库；保留 GLES 回退。NEED_OPENGL 涉及应用启动配置，不能用 Wine child 晚设环境变量代替正式验证。
2. 能力通过后，为 libepoxy/renderer 选择正确 GL dispatch；桌面 GL producer 与共享纹理 consumer 使用兼容的同类 context。同步调整 shader、GL state、native-buffer 导入与 ready/consumed fence。不能假定 GLES 与 desktop GL 能直接共享 texture name。
3. 经真实 capset 暴露 guest 能力，运行 PE32/PE64 的 GL3.1回归、4.1 core功能和 GPU呈现。初始化失败须在 guest 启动前整体回退，避免中途混用 API。
4. 仅在已验证设备开放“自动 / GLES / 系统桌面 GL”；系统默认与每游戏覆盖共用同一解析逻辑，显示实际生效路线和回退原因。

收益机会：如果系统桌面 GL 原生支持 sRGB write control、dual-source blend 等，可减少部分 GLES 仿真和 GPU shadow copy；但 VirGL socket 编解码仍在，OpenGL 版本提高本身不保证 FPS。整体不是低成本开关改动。按用户最新要求，当前保留设计、暂缓移植和新 HAP。

## 三路线下一阶段的共同验收

- **DXVK9 优先正确性**：精确重放固定功能 VS；补资源发布；保留 fog/alpha/纹理格式、shader2/3、DISCARD/NOOVERWRITE、query、Reset/resize 的小型检查。D3D11 的可行补丁需分别核对 DXSO/固定功能语义。
- **VirGL 优先关键路径**：在同一时间线测 socket mutex/往返、guest CS、host draw 和异步 GPU timestamp。若 CPU 供给受限，减少冗余常量传输、同步查询或提交次数；若 GPU 时间主导，再优化 shader/copy/状态仿真。目标是降低每帧成本，不是提高 CPU 占用数字。
- **Zink 按真实能力分级**：`zink_screen.c` 仍按 Vulkan 的 transform feedback、dual-source blend、depth-clip 扩展和资源限制设置 Gallium caps；先对照 Mesa `version.c` 门槛找出当前 GL2.1 的具体阻点。结合已有 0015/0016，审计普通纹理、持久映射、常量与 Present 的可见性。DXVK shader workaround 不能直接套到 Zink/NIR。
- **配置以游戏 API 为中心**：D3D9 选择 WineD3D/VirGL、DXVK9 或 WineD3D/Zink；原生 OpenGL 选择 VirGL/Zink；D3D11保留已适配DXVK。Vulkan Direct/Venus和host GL API在内部按能力决策，不把一个“Vulkan”标签混同所有路线。
- **Heaven 最后统一比较**：三条路线均用相同 D3D9 模式、分辨率、画质、AA/同步、温度与缓存条件；正确性通过后进行完整计分与 A-B-B-A，记录平均 FPS、长帧和 CPU/GPU/等待。原生 OpenGL 与 D3D11 单列，不将跨 API 分数冒认后端收益。

## 构建、证据与设备状态

- 复用原 dxvk9-build，最终 PE32 DLL 增量编译通过。11个 managed patches / 29个文件全栈 reverse+replay 与当前字节一致，连续两次 verify通过；SHA锁与 git diff --check通过。
- 原十个 dirty 子模块保留，未提交 gitlink。源码新增独立关联日志探针 `scripts/probes/mini_backend_d3d9_draw.cpp` 和 bool语义回归。
- 原 `backend-correctness`、`backend-init-evaluation` 与历史 artifacts 不覆写。本轮证据固定于 `workspace_temp/mini-virgl-performance/backend-draw-analysis`；有界采集，目标进程日志偏移/时间可核对。
- 一次 shader dump 尝试因 HDC mkdir 权限失败，未取得shader；改由探针创建其目录。一次清理遗漏 `.dxso/.dxso.dis` 已按前次记录的确切文件名取回并删除，`cleanup-dxso/result.json`核验目录不存在。最终脚本检查 leftovers，不再将未核验清理写成成功。
- Mini 仍为稳定 UI 修正版 HAP；最终应用停止、临时 EXE/DLL/trace/目录已删除、锁释放。实验 DXVK9 DLL 仅用于探针。没有新 HAP、卸载、清数据、push或CI。

公开参考（2026-11-15查询）：

- https://developer.huawei.com/consumer/cn/doc/harmonyos-references/opengl/
- https://developer.huawei.com/consumer/en/doc/harmonyos-references/opengl/
- https://sanway.tech/hos/new.html
- https://kaidashi.helplook.net/docs/chao-ji-hui-yuan （适用机型包含 MatePad Edge）
- https://www.ithome.com/1/012/968.htm （7.0.0.111电脑版本报道，不作GL能力证明）

最终本地提交、存储维护与本轮文件哈希见交付目录的 final-state.json / manifest.json。
