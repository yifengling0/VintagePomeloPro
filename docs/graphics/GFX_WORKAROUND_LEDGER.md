# 图形兼容性 Workaround 台账

## 使用规则

每条 workaround 必须有稳定编号。状态只能是 `active`、`candidate-remove`、`removed`。没有复现、回归测试和删除条件的 workaround 不得进入生产默认。

### Q001 — Maleoon 映射显存可见性与 inline upload

- 状态：`active`
- Owner：Guest Venus / Host VirGL shadow bridge
- 影响：Maleoon + Venus + 映射资源更新，DXVK 2.6 场景最敏感
- 症状：资源已由 Guest 写入但 Host/GPU 看到旧内容，表现为静帧、贴图错误或间歇花屏
- 当前措施：使用 precise dirty ring、inline GPU upload 和 coverage sort 组合，将 dirty range 在提交/展示前显式上传
- 生产默认：`product-vulkan` route 中的 DXVK Host policy 使用 precise-dirty、inline GPU upload 与 coverage sort；不暴露组合 profile 名称
- 主要位置：`napi_init.cpp`、`virgl_host_config.cpp`、`wine_env.cpp`、`wine_launch.cpp`；实现位于锁定的 Mesa/VirGL gitlink
- 回归：映射 buffer/texture 重复更新；Heaven 连续帧推进；上传 coverage/alias 边界测试
- 上游路径：最小化为 Mesa Venus 或 VirGL 的可见性/flush 复现，优先采用上游同步语义修复
- 删除条件：锁定上游版本在目标 Maleoon 设备通过相同矩阵，关闭 inline upload 后图像与 P99 帧时间不退化

### Q002 — 无 fence/query feedback 的远端同步模式

- 状态：`active`
- Owner：Guest Venus transport
- 影响：OHOS transport 无法导出所需 dma-buf/opaque-fd 同步对象的路径
- 症状：等待反馈可能死锁、长时间停帧，或 query/fence 结果永远不完成
- 当前措施：设置 `VN_PERF=no_fence_feedback,no_query_feedback`；部分路径同时关闭 semaphore feedback 和 multi ring
- 生产默认：由后端/transport 策略派生，不是用户选项
- 主要位置：`wine_exe.cpp`、`wine_env.cpp`、`SmokeRunner.ets`
- 回归：fence/query smoke、长循环帧推进、进程退出和前后台恢复
- 上游路径：验证目标平台是否具备可上游表达的外部同步能力；否则把 transport capability 做成显式能力位
- 删除条件：Host/Guest 双方支持并验证反馈通道，打开 feedback 后无死锁且结果一致

### Q003 — 转译边界与 ring 可见性

- 状态：`active`
- Owner：native Wine + FEX / Venus 进程边界
- 影响：ARM64 设备上的 x86_64 Guest Vulkan/DXVK
- 症状：弱内存序导致 ring 元数据或 payload 观察顺序错误，表现为随机卡帧、命令损坏或只在压力下复现
- 当前措施：`VN_WINEHUA_STRONG_RING_BARRIER=1`，保留原生 ARM64 Guest ring 的 acquire/release 语义；不将产品切回 Box64
- 生产默认：DXVK/VKD3D 相关 ARM64 路径启用
- 主要位置：`wine_env.cpp`、`SmokeRunner.ets`、Box64 preset 传递链
- 回归：多线程 ring 压力、连续 submit、10 分钟稳定性和退出清理
- 上游路径：用 acquire/release 最小复现定位 Box64 或 ring 实现中的缺失内存序
- 删除条件：上游或本地原子语义修复后，在弱屏障 A/B 下长期稳定且无性能/正确性回归

### Q004 — RGBA8 SNORM render-target 自动仿真

- 状态：`active`
- Owner：DXVK compatibility layer
- 影响：支持采样但不支持该格式 color attachment 的 GPU，Maleoon 为已知目标
- 症状：DXVK 创建 SNORM render target 失败，或相关游戏出现黑屏/缺失效果
- 当前措施：`DXVK_WINEHUA_EMULATE_RGBA8_SNORM_RT=auto`，优先原生格式，仅在能力不足时仿真
- 生产默认：`auto`
- 主要位置：`wine_env.cpp`、`wine_child.cpp`、锁定的 `thirdparty/dxvk-modern`
- 回归：格式能力探测、创建/清除/采样 round-trip、相关游戏 smoke
- 上游路径：评估 DXVK format fallback 的通用实现，避免设备名称特判
- 删除条件：目标 DXVK/driver 组合原生支持，或上游 fallback 在设备矩阵通过且本地变量不再被消费

### Q005 — VirGL sRGB 首帧 framebuffer 策略

- 状态：`active`
- Owner：VirGL renderer/context compatibility
- 影响：首个 framebuffer 的 sRGB/linear 状态与 Guest 预期不一致的 OpenGL/WineD3D 路径
- 症状：首帧颜色空间错误、过亮/过暗，后续 framebuffer 切换后才恢复
- 当前措施：VirGL managed 0005 恢复首相关 framebuffer 策略，并按附件 view/storage 统一 draw/clear；保留 Mesa sRGB shadow
- 生产默认：由 gitlink + managed patches 共同生效，无用户开关
- 主要位置：`thirdparty/virglrenderer` gitlink；版本由机器锁固定
- 回归：首帧截图像素/直方图、sRGB FBO 切换、WineD3D 老游戏 smoke
- 上游路径：先在纯 VirGL GLES 测试中复现，再核对上游 framebuffer sRGB 状态管理
- 删除条件：上游提交覆盖首帧与切换测试，目标设备像素结果与基线一致

### Q006 — GPU 名称正则驱动的 DXVK 2.6 门控

- 状态：`active`
- Owner：产品能力策略
- 影响：`Maleoon N` / `Mali-GN` 命名的 GPU；当前阈值为 920
- 症状：低于阈值的设备选择 DXVK 2.6 后不兼容；未知名称可能被误判
- 当前措施：通过 `getHostGpuName()` 和 `/(?:Maleoon|Mali-?G)\s*(\d+)/i` 判定，920 以下回退 DXVK 1.10
- 生产默认：未设置/auto 默认 Legacy；显式选择 2.6 经过名称门控。未知名称仍可能放行，后续以能力探测替代
- 主要位置：`SystemSettings.ets`、`WineEngineService.ets`、`host_vulkan_probe.cpp`
- 回归：GPU 名称解析单测、空/未知/变体名称、低于/等于/高于 920 的后端选择测试
- 上游路径：用 Vulkan feature/format/extension probe 替代营销名称；GPU 名称只保留为诊断信息
- 删除条件：能力 probe 能稳定预测 DXVK 2.6 必要条件，并在目标设备矩阵与当前门控结果一致或更准确

### Q007 — VKD3D 持久映射 UPLOAD 按资源 Width 刷新

- 状态：`active`
- Owner：VKD3D-Proton / Guest Venus 映射可见性
- 影响：VKD3D 2.6 limited-500K profile 中长期保持 Map 的 D3D12 UPLOAD buffer
- 症状：只在 Map 时刷新 1 字节会留下旧常量数据；刷新整个 8–16 MiB 父 slab 又造成明显带宽和帧时间回退
- 当前措施：Map 时按 `resource->desc.Width` 标记写入；`ExecuteCommandLists` 前批量刷新仍处于映射状态且不超过 4 MiB 的 UPLOAD buffer
- 生产默认：仅由 `product-vulkan` 的 VKD3D 后端适配器启用 `VKD3D_WINEHUA_FORCE_COHERENT_MAP_SYNC=1`，不增加独立产品开关
- 主要位置：`patches/vkd3d-proton/0001-*`、`0019-vkd3d-flush-mapped-upload-width-on-execute.patch`
- 回归：gears 小常量 buffer、D3D12SingleGpu 2.5 MiB persistent CB、映射/取消映射/销毁竞态、96 MiB 父分配带宽回归
- 上游路径：将 OHOS/Venus 非同址 coherent 映射语义最小化后提交到 VKD3D/Venus；若 transport 获得正确 coherent 可见性则删除显式 flush
- 删除条件：目标设备关闭强制同步后仍逐帧更新正确，且 P95/P99 不劣于 Width flush 路径

### Q008 — GL 隐藏桌面错误重试退避

- 状态：`active`（`refact/gl-optimization` 候选，Native 提交 `8de093d`）
- Owner：Host `SurfaceQueueTarget` / Guest vtest present pacing
- 影响：GL consumer 暂停但应用仍活跃，以及暂时性 fence/context/present 失败
- 症状：失败返回不带未来 deadline，清空 Guest 限速；隐藏桌面时实测约 328 次/秒错误尝试
- 当前措施：失败仍返回原错误，同时传递一帧至 50 ms 的有界退避；早到请求返回 deferred，成功或新 target 清零
- 生产默认：只在候选现有 GL 错误路径生效，不新增开关；健康帧、Vulkan target 和 `batchMappedFlush` 产品开启策略不变
- 主要位置：`present_pacing.h`、`virgl_surface_presenter.cpp`；不修改 Guest 库或 IPC
- 回归：退避边界/溢出 Host 测试；GL x86/x64 各五轮恢复；两代 DXVK 各 40/40 帧序；Modern 视频可见并正常结束
- 上游路径：先补齐 NativeImage consumer 与 producer 的暂停/恢复协调，再考虑用明确 queue-full/deferred 语义替代驱动模糊错误
- 删除条件：生命周期及故障注入均证明失败不再形成无界循环，恢复和长稳无回归；仅消除后台错误不足以删除通用失败限速
- 未完成：`0x505` 根因、完整后台停产、十分钟/资源增长及 GL 帧序门槛；详见 [证据与回退](gl-background-backoff.md)

### Q009 — GL 共享纹理呈现的 producer 上下文与双向 GPU 同步

- 状态：`candidate`（2026-10-17；平板 EGL 与默认 Vulkan 实机验证通过，尚未发布）
- Owner：VirGL vtest / Native `SurfaceQueueTarget`
- 影响：GL 游戏在共享纹理输出中复用同一 source texture；已在 Maleoon 910 的星露谷 1.6.15 复现
- 症状：游戏区域高速闪烁而工具栏正常；换宿主 EGL 合成仍闪；强制软件帧对照不闪
- 当前措施：managed 0006 在 present 回调前通过零长度提交选择请求所属 producer context，避免 dispatch 前 fence 轮询留下 ctx0；Native 在采样结束后将 consumed fence 排入 producer，约束下一帧覆盖
- 生产默认：GPU 共享纹理通道内自动生效，不按游戏名称特判；正常路径无 CPU readback、glFinish 或降画质，只有异常同步失败时等待已提交的读取结束
- 主要位置：`patches/virglrenderer/0006-gl-present-producer-context.patch`、`virgl_surface_presenter.cpp`
- 回归：真实 present handler 的 204 项上下文/协议/错误回复检查；双向同步 34 项、故障恢复 43 项、renderer 生命周期 698 项；旧 handler 在维护上下文场景真实断言失败
- 实机边界：用户确认 rc3 EGL 和默认 Vulkan 均不闪；菜单约 60 FPS，农场约 50–60 FPS。默认 Vulkan 连续呈现超过 23000 帧，`glSources=1`、`releaseErrors=0`，readbacks 固定为启动时的 3 次；独立 VPTest 读档、移动及室内外往返通过。未宣称全设备或所有游戏已验证
- 上游路径：本地 present 扩展需显式保存/选择 producer；保留标准 GL 共享对象的 ready/consumed 同步语义
- 删除条件：替代输出通道具有同等所有权与双向同步保证，并在相同纹理复用及故障恢复测试、设备游戏验证中通过

## 新增记录模板

新增条目需包含：状态、Owner、影响范围、可见症状、当前措施、生产默认、代码位置、回归测试、上游路径和可验证的删除条件。
