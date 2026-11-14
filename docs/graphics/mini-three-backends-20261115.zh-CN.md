# Mini 三后端对比与修复决策

2026-11-15。当前可部署选择：Mini 的 D3D9 继续使用 WineD3D / VirGL / GLES。DXVK9、Zink 的同步修复以显式开关保留，尚未通过 Heaven 正确性，不开放为已验收产品选项。本轮没有新的有效 Heaven FPS，未证明 30% 提升。

## 同一张对比表

| 路线 | 实际调用链 | 本轮正确性结果 | 有效 Heaven FPS | 当前选择 |
| --- | --- | --- | --- | --- |
| VirGL / GLES | D3D9 → WineD3D → VirGL → 宿主 GLES | 稳定包恢复后，同一 FBO 红色清屏、绿色三角形均通过；已有完整 Heaven 基线 | 历史 Low 720p 中位 16.3，本轮未重跑计分 | Mini D3D9 稳定路线 |
| DXVK9 | D3D9 → DXVK 1.10.3 → Wine Vulkan → Venus → 宿主 Vulkan | 越过缺失 BC / multiViewport 的 CreateDevice 阻点；清屏回读修复通过；普通 RGBA、DXT 纹理绘制后的 readLock 均未完成 | 无，未通过准入 | 默认关闭，继续定位绘制后的等待 / WOW64 异常 |
| Zink | D3D9 → WineD3D → GL → Zink → Venus → 宿主 Vulkan | 修复后 FBO 红色清屏、绿色三角形均通过；Heaven 60 / 120 秒仍黑屏、条纹 | 无，画面错误不能计分 | 默认关闭，继续查完整游戏上传 / 纹理 / 呈现 |

这里的 Vulkan 实测通道是 Venus。不能将“请求 Direct”记作“已获得系统 Vulkan 直通”。

## 已修复的错误与证据

### DXVK9 的 GPU → CPU 可见性

Venus 的 OHOS 回退通道将 GPU 所用的 Host 映射和 Guest 影子映射分开。物理内存类型中的 HOST_COHERENT，不能替代跨进程影子拷贝。D3D11 已有显式同步，D3D9 的 LockImage 缺少相应处理。

- 修复前：仅 Clear 红色、GetRenderTargetData、LockRect 都返回成功，像素为 `00000000`。
- `0009-d3d9-opt-in-shadow-readback.patch`：GPU 等待完成后，仅对需要回读的非 emulated-BC 子资源调用现有 `invalidateMappedSlice`；失败恢复 readback 标记。使用 `WINEHUA_DXVK9_SHADOW_READBACK=1` 启用。
- 修复后：同一清屏读回像素为 `FFFF0000`，PASS。
- 不能据此推导纹理绘制或性能通过。新 DLL 没有替换 Heaven 目录中的 DLL；只在有界探针运行中临时部署并清理。

### Zink 的读回与上传

`0015-zink-opt-in-shadow-readback.patch` 补齐 buffer-map 和图片 staging BO 两条读回路径，在资源等待完成后显式 invalidate。只补 buffer-map 的第一版未修好图片回读，因为图片路径直接调用 map_resource。

`0016-zink-opt-in-shadow-uploader.patch` 在显式开关下关闭 persistent-coherent uploader 能力，改走现有显式 flush 路径，并发布 CPU 实际写入的 buffer slice；其余 slice 不随之上传。没有在每次提交时盲目复制全部已映射内存。

| 小型 PE32 GL 探针 | 清屏后像素 | 三角形后像素 | 结论 |
| --- | --- | --- | --- |
| 原始 Zink | 0,0,0,0 | 未验证 | 回读失败 |
| 只补读回 | 255,0,0,255 | 255,0,0,255 | 清屏正确，draw 结果仍旧 |
| 读回 + 显式 uploader | 255,0,0,255 | 0,255,0,255 | 基础 clear / draw / readback PASS |

开关为 `WINEHUA_ZINK_SHADOW_READBACK=1`、`WINEHUA_ZINK_SHADOW_UPLOAD=1`，默认都不启用。同一候选上的关闭上传开关负对照仍缺失绿色三角形；最终 validated 轮两个开关确实启用且通过，收据以实际 environment 为准。

### DXVK9 BC 资源兼容实验

`0008` 复用已有 D3D11 BC CPU 解码器，保持 D3D 可见的压缩 pitch / shadow，上传到 GPU 的 RGBA / R / RG 图像。DXT1 8×8 四级 mip 的创建和锁定已越过原阻点；完整纹理正确性仍未通过。

BC render-target、depth、autogen mip、StretchRect 重压缩暂不支持，明确失败。压缩 shadow 不从 RGBA GPU 图像回填。格式视图使用真实二元素数组，MarkAllNeedReadback 也排除 emulated BC。所有 BC 转换都要求显式 `WINEHUA_DXVK9_BC_EMULATION=1` 且设备缺少 BC。

multiViewport 改为只请求设备实际支持值；D3D9 绑定单 viewport，审计未发现 DXSO / meta shader 输出 ViewportIndex。其余必需 feature 没有伪造。

## 尚未修复的阻点

### 系统 Vulkan 在实际 fresh Wine child 中仍失败

复用现有 native Vulkan probe，放在实际 fresh child 中、加载 Wine 前运行。实际 loader 是 `/system/lib64/libvulkan.so`；loader API 1.3.275，ICD / DRIVER 覆盖为空。1.3、1.2、1.1、1.0 全部 `vkCreateInstance=-9`（VK_ERROR_INCOMPATIBLE_DRIVER）。

Zink 的额外诊断确认函数也来自同一系统 loader；去掉所有 instance extensions 后仍失败。这排除了单纯调低 API 版本、删掉某一个扩展即可解决的方案。驱动发现、进程 namespace / 初始化条件仍需进一步证明，尚不能认定某一项就是根因。

`WINEHUA_NATIVE_VULKAN_PROBE=1` 和 `WINEHUA_ZINK_DIAGNOSTIC=1` 都是显式诊断开关，正常启动不执行探针或重试。

### DXVK9 的绘制后异常

普通 RGBA 与 BC 纹理都能返回 DrawPrimitiveUP / GetRenderTargetData 成功，但之后 readLock 没有返回记录；相关日志出现 Venus fatal 和 Wine SEH frame 越界。只清屏可正确读回，三次 Present 均返回成功；Present 返回成功不等于屏幕像素通过。

低地址映射已经成功：`wine_owned=1`，地址在 32 位范围内，仍未解决纹理绘制后的问题。不能继续把它归因于“缺少 low-map 开关”或 BC 解码本身。

模块加载日志确认 `0x6ffc5309d8` 落在 `libwow64fex.dll`（基址 `0x6ffc510000`，RVA `0x209d8`），该位置有向保护页写入的指令。它不是已符号化的 GPU 驱动崩溃。FEX 自身存在按需提交内存机制，单条 early-fault 不能等同致命崩溃；最终根因仍需把异常处理、线程退出和 GPU 完成关联起来。设置 FEX_DYNAMICL1CACHE=0 的有界实验也没有取得纹理读回 PASS，不能当修复方案。

### Zink 的完整游戏正确性

基础 GL 探针 renderer 为 `zink Vulkan 1.2(Virtio-GPU Venus (Maleoon 910) (MESA_VENUS))`，legacy GL 报 2.1。Venus 当前缺少 Zink 所提示的 EXT_custom_border_color、EXT_line_rasterization。没有使用虚假的 GL_VERSION_OVERRIDE。

同一 WGL 探针请求 3.3 core 在 VirGL 上也失败；因此该调用失败不是 Zink 特有证据，legacy version 也不能直接写成硬件最大 GL 版本。

Heaven 使用 Low、1280×720 fullscreen、D3D9、AA0、tess disabled、VSync0，强制 builtin d3d9，并启用上述两项 Zink 同步修复。60 / 120 秒实际截图仍黑屏、上沿条纹，未通过准入。屏幕左上角 1 / 3 FPS 是应用 overlay，不能作为 Heaven 计分结果。游戏目录中的 log.html 仍为旧 18:49 日志，不用于本轮归因；以本轮启动收据、stderr、native 和截图为准。

过滤后的 stderr 已触发设备端大小轮转，Heaven 时间窗口内的进程头没有保留下来。无归属前缀中有 10,231 条 DISCARD / NOOVERWRITE 参数修正、105 条空 box 及 map box 无效警告，以及 31 条纹理 access 警告；它们是下一轮定向检查的线索，不能当作已证实的本轮游戏根因，也不能推断没有 shader 编译错误。详见 `backend-correctness/heaven-stderr-analysis.json`。下轮只在目标进程启动前后记录固定日志偏移、PID 和边界，限制重复警告；不再收集整天 stderr 后猜测归属。

## 30% 目标与优化优先级

16.3 FPS 对应平均约 61.35 ms / 帧；21.19 FPS 对应约 47.19 ms / 帧，需要净省约 14.16 ms，即减少约 23.1% 的帧关键路径时间。正确性修复本身不等于这个收益。

- 当前稳定产品：保留 VirGL。历史 FBO 缓存对照为 16.30 → 16.10 FPS（-1.23%，小样本），不推广；packed writes 也没有成立的收益。不能将几轮不独立实验的百分比相加。
- VirGL 性能主线：优先把 guest CS、busy 查询、socket mutex 等待、宿主提交和 GPU 完成放到同一时间线，判断是否是同步往返使 GPU 断粮。历史 busy 查询约 226 次 / 秒，但次数不能直接换算耗时。若确认存在可省等待，再考虑合批、缩短持锁回复区间或减少重复查询，保留资源依赖和同步语义。
- draw 内部优化：已有 DRAW_VBO handler 墙钟约 18.0%、FBO 约 7.3%、全部 batch 约 35.5%，相互有包含关系且混有等待。先分开 program / uniform、sampler、顶点状态与真正 glDraw 的耗时，再决定去重或缓存。不能把这些比例当成独立可相加的 CPU 节省。
- Vulkan 实验：DXVK9 先解决普通 RGBA draw 后的等待 / 异常，再验 BC 纹理；Zink 先验证纹理上传、采样和窗口呈现。即使修好，当前 Venus 仍有影子映射拷贝，不预设其一定快于 VirGL。系统 Vulkan 的 instance 准入是独立问题。
- GPU 是否满载仍未知。历史 CPU 约 255%（100% 为一个逻辑核）不代表无 CPU 瓶颈；也不能排除 GPU。收集连续 GPU busy / 异步 timestamp 并对齐 CPU 等待之后，才决定偏 CPU 提交优化还是降低 GPU 工作量。不以 CPU 占用越高为目标。

## 最终采用的方案与后续门槛

1. 当前 Mini D3D9 继续采用 VirGL。保留现有系统默认与每游戏覆盖设计；没有把未验收的 DXVK9 / Zink 作为可用产品选项，也没有更改用户原有默认配置。
2. 本轮确认有效的影子内存同步修复保留为默认关闭代码，连同独立 probe、patch SHA 锁和真机正反对照保存。它们是正确性修复，不宣称 FPS 收益。
3. DXVK9 下一步优先做同一失败进程的 CS / wait / Wine SEH / FEX overcommit 关联；Zink 优先做实际游戏的上传范围、纹理采样、draw 与窗口呈现分段定位。先覆盖不同 mip、子区更新、复制、锁定、sRGB、cube / volume 和 Present，再准入 Heaven。
4. VirGL 的性能主线仍应测 guest CS、socket mutex / busy-query 往返与 draw 内部耗时。已有 handler 墙钟混合驱动等待、调度和 GPU 时间，不能当纯 CPU 成本。不以提高 CPU 占用率本身作为目标。
5. 补连续 GPU busy 或异步时间戳，并与 CPU / 等待时间对齐，才能判断 GPU 是否跑满。当前仍无足够数据排除 GPU。
6. 只有画面正确、路线确认后才做完整 A-B-B-A；Low 720p 基线 16.3 FPS 的 +30% 门槛为 21.19 FPS。当前没有满足这个门槛。

## 复现与交付

- 源码基线 `cf5b76aa5d242cf750a9046d6d35dadfa72269fb`，`feature/fex-proton-upstream-sync`；本轮修复仅作本地提交，提交号见交付 `final-state.json`；未发布。
- 根仓保存补丁和 `scripts/probes/mini_backend_gl.c`、`mini_backend_d3d9.cpp`；保留原有十个 dirty 子模块，不提交 gitlink。
- Windows 证据：`F:/VintagePomelo-Workspace/workspace_temp/mini-virgl-performance/backend-correctness/`；native instance 诊断：相邻 `backend-init-evaluation/`。
- Linux 复用固定 `zink-build`、`dxvk9-build`；没有复制完整构建树。DXVK / Mesa 增量编译成功；DXVK 全栈洁净应用、幂等、verify-only、drift / digest 拒绝测试通过；Mesa 全栈 reverse proof 通过。
- 实验 HAP 只替换 Wine child / probe / Guest GL 相关库；UI 三项与已验证 UI 修正版逐字节一致。它是诊断 overlay，不是完整 release parity 声明。
- 不 push、不 CI，仅操作 Mini，设备动作持固定 mini.lock。结束恢复稳定 UI 修正版，清理临时探针、unsigned 和失败候选；稳定包已恢复，随后 VirGL 红色清屏 / 绿色三角形复验通过，应用已停止，公共目录临时 probe / DLL 不存在；最终状态与校验见同目录交付收据。

### 验证边界

- 最终 DLL 的清屏 PASS 与最终 Zink、恢复后 VirGL 的 draw PASS 已分别核对 EXE / DLL SHA。GL 两路线使用同一 PE32 EXE。最新收据不借用旧二进制的结果。
- DXVK 纹理失败轮早于最后的 view-format 数组修正；该修正后的 DLL 已编译且清屏通过，但没有完成新的纹理验收。普通 RGBA 失败本身不依赖 BC 视图格式，因此仍是继续定位的首要入口。
- 本轮只形成可用性、正确性与阻点对比；尚未完成三路线同设置 FPS 排名，也没有完成 High / Ultra 性能比较。当前选择依据是稳定性与已验收范围。
- 保留稳定 UI 修正版和最新 readback 实验签名 HAP 各一份。DXVK9 DLL 仅在独立 probe 中临时部署；实验 HAP 不是包含新 DXVK9 的完整发行包。
