# 灰色的果实：共享映射修复与通用资源路径调查（2026-10-10）

## 结论与范围

用户已明确：该游戏自身卡顿不必一定解决，重点判断有无影响其他游戏的共通问题。本轮结束专用调参，保留有证据的共享映射默认设置；动态缓冲同步和纹理回读作为后续通用调查入口。

已确认一个通用额外开销：32 位 OpenGL 资源映射在未启用 Wine/Mesa 共享低地址映射时，频繁建立 shadow 并复制完整缓冲。V7 同包开关对照证明可以消除这段复制；V8 普通启动未传 LOW_MAP 参数，也实际命中 map_direct。尚未证明《灰色的果实》的体感或 FPS 提升，更不能解释全部游戏的性能下降。

保留 FEX、已有 x87/FEX 故障边界修复、OpenGL 3.1、自动 Vulkan Direct、减少复制和显示节拍路径。未将 full SMC、单指令模式或 dynamic-buffer-sysmem 实验设成全局默认。

## 1. 已修复：共享低地址映射没有在普通入口默认启用

Wine 32 位程序经 WOW64 OpenGL thunk 调用本机 ARM64 Mesa。这里的 WOW64 是 Wine 的 32/64 位接口，不表示转译器换成 Box；本轮保持 FEX。

正文更新现场原路径按 window_us 归一化后每秒约 4,500–5,100 次映射，常见缓冲大小 512 KiB。每次完整 shadow 初始化累计 2.36–2.66 GB/s（中位数 2.42 GB/s），shadow_in 计时 157–176 ms/s（中位数 166 ms/s），而实际 flush 更新数据仅 1.24–1.83 MB/s。精确按 window_us 归一化的范围见 [map-metrics.json](evidence/gray-common-paths-20261010/map-metrics.json)。不同字段有嵌套，不能将 map_shadow 与 shadow_in 时间相加。

V7 的 WINEHUA_VIRGL_LOW_MAP=1 对照保留近似映射次数，但全部变成 map_direct，目标 shadow 复制消失。该计数项的 bookkeeping 时间降至0.56–0.80 ms/s（中位数 0.65 ms/s）；map_driver 仍约 3–4 ms/s。map_direct 的 requested 字节是映射范围，不是实际复制字节；它与 Vulkan Direct 是两个不同机制。

新增生产补丁：`patches/wine/0052-ntdll-default-virgl-shared-low-map.patch`。在 `dlls/ntdll/unix/env.c:init_peb()` 导入 child/BAT 环境之后、DLL/Mesa 初始化之前，仅在变量缺省时设置 `WINEHUA_VIRGL_LOW_MAP=1`。显式 `0`、空值或无效值继续关闭，不覆盖调用方选择。

能力边界仍由原实现约束：OHOS ARM64、WOW64、合法大小、成功预留且归属明确的低 2 GiB 地址；Mesa vtest 协议至少为 2，且只用于 PIPE_BUFFER。回调缺失或分配失败时回原路径。64 位程序不进入这套低地址分配。没有通过游戏名决定是否启用。

补丁没有修改 0040 的环境导入原块，避免破坏连续重放的 reverse-detection。两遍重放和 11 项共享映射专项通过。V8 未传 LOW_MAP 的 [启动条件](evidence/gray-common-paths-20261010/gray-v8-default-conditions.json) 与 [实际映射日志](evidence/gray-common-paths-20261010/default-shared-map-excerpt.txt) 已保存。

## 2. 通用候选：NOOVERWRITE fast map 未命中，反复经过命令队列

短期开 `+d3d_perf`，反复出现：

```text
Not accelerating a NOOVERWRITE map because the sub-resource has no valid address.
Mapping resource ... (type 1), flags 0x40001000 through the CS.
Waiting for queue 1 to be empty.
Unmapping resource ... through the CS.
```

对应 `dlls/wined3d/cs.c:wined3d_cs_map_upload_bo()`：NOOVERWRITE 只有拿到有效 client 地址／已映射 BO 才直接返回，否则 map/unmap 走 CS。共享低地址映射解决的是 thunk 后端复制，不能自动消除前面的命令队列往返。

这是可复用的资源更新慢路径，但尚未证明是错误、主要等待耗时或所有游戏的降帧根因。完整 trace 很嘈杂，也会扰动性能，只保留 [32 行现场](evidence/gray-common-paths-20261010/nooverwrite-cs-excerpt.txt)，不拿开启 trace 的 FPS 作结论。

已有 `WINEHUA_DYNAMIC_BUFFER_SYSMEM=1` 会话实验把映射降至约 0–1/s，改为约 1,900–2,400 次 buffer_subdata/s（实际上传约 300–383 kB/s），但截图出现正文文字显示不全。该实验不能视为优化成功，仍默认关闭，后续回归已移除参数。

可考虑的修复方向是按资源用途维护正确的 DISCARD/NOOVERWRITE 流式上传与缓冲生命周期，避免每次小更新都排空队列。需要先分清首次 NOOVERWRITE、DISCARD 后地址失效、BO 解映射及 CS 引用；不能直接跳过等待或无条件复用尚在 GPU 使用的内存。

## 3. 通用候选：纹理回读与诊断漏项

V8 默认正文现场采样出现：

```text
wow64_gl_glGetTexImage
  _mesa_GetTexImage -> st_GetTexSubImage -> _mesa_GetTexSubImage_sw
  _mesa_format_convert
  st_MapTextureImage -> virgl_resource_transfer_map
  virgl_vtest_busy_wait / transfer_get
```

这是纹理读取／格式转换证据，不是普通纹理上传的证据。[callchain 摘录](evidence/gray-common-paths-20261010/getteximage-callchain-excerpt.txt) 已保存。`dlls/wined3d/texture_gl.c` 在纹理下载及 load_sysmem 路径调用 glGetTexImage；`texture.c:wined3d_texture_evict_sysmem()` 在没有 pin、格式转换及频繁下载保护时可移除 sysmem 副本。这些是静态候选，尚未将现场回读归因到其中某一项。

还需区分游戏主动 Lock/read、WineD3D sysmem 驱逐／location 转换、RGB/sRGB storage 转换。当前有限日志未足以证明 RGB/sRGB reload 是原因，因此不改颜色转换规则，也不关闭必要 readback。

`opengl32/unix_wgl.c` 现有计时含 readpixels，缺少 glGetTexImage／compressed GetTexImage 覆盖。宿主日志 `directGameCpuReadBytes=0` 在 Direct compositor 内是对该呈现实现的固定声明，并非整个游戏的回读探针；不能用它证明 WineD3D 或 Mesa 内部零回读。OpenGL 共用该桌面合成器时 `[DIRECT-FPS]` 标签也不代表已走 Vulkan Direct，必须同时检查 direct/glSources。

建议下一阶段先用默认关闭、限频聚合诊断补齐下载原因、资源类型／尺寸／location、次数与耗时，再按资源语义修复多余转换。对确有重复 CPU 读写的资源选择性保留 CPU 副本，GPU copy/blit 只在语义和格式能力允许时用；不全局保留所有纹理，也不靠每帧读回修显示。

## 4. 构建、包和配套资产

权威仓库 `/home/liufeng/src/vpp-proton`，分支 `feature/main_proton`，基于 HEAD `f47090663537b1e70ad1ebe2f76bd2c34cfbb6af` 的 dirty 工作树。本报告不意味着其他未提交工作已经独立评审或发布。

新 ntdll 使用独立源码 `workspace_temp/wine-gray-lowmap-source-20261010`、独立 BUILD_DIR `build-gray-lowmap-v2-20261010`；产物 `wine-ohos-aarch64/dlls/ntdll/ntdll.so` SHA256：

`f12c3aec35db5d406ebaab2367de163f8baafc3df7844f6f89033fee332d677d`

V8 从 V7 unsigned HAP 仅替换 ntdll，逐 entry 比较其他内容一致；runtime closure 通过，签名后覆盖安装成功。设备 shell 无权读 native ELF，未取得设备 ntdll 哈希。安装身份来自签名包、安装成功记录和缺省映射命中。

| 产物 | SHA256 |
| --- | --- |
| unified-present-gray-v8-20261010-unsigned.hap | 66ed64863540b4a52af28f5c8ebedfdb85e75bc03d94901371ec702c56e7e548 |
| unified-present-gray-v8-20261010-signed.hap | 1015e9b319d539d8a965d0a36b0165590a373ceeb18a937c1e5954c1637c16c5 |

仅 allowlist 同步 12 项已验证的配套 native/runtime 资产到 entry，before/after 哈希见 [同步审计](evidence/gray-common-paths-20261010/gray-v8-runtime-sync-applied.json)。wine-data.zip 仅更新两架构 FEX、WineD3D 及 guest EGL/Gallium/构建身份；没有删除 member 或覆盖 Steam 解压文件，见 [ZIP 差异](evidence/gray-common-paths-20261010/gray-v8-runtime-zip-diff.json)。没有覆盖 generated libentry、probe 或其他 child/native 库。

本轮是增量候选 HAP 安装，不是完整发行 HAP/APP 的重新构建。

## 5. 有限回归及实际边界

| 验证 | 结果 | 能证明什么 |
| --- | --- | --- |
| 主机共享低地址映射 | 11 项通过，两遍重放一致 | 缺省开、显式关、分配失败回退、所有权与并发释放 |
| OpenGL present storage / FEX fault boundaries | 通过 | 当前专项约束；不等于全量兼容测试 |
| 32/64 位 VirGL UBO 像素 | 各 27/27，合计 54；覆盖 legacy、3.0、3.1 上下文 | 有限 UBO 绘制正确；不代表所有 OpenGL 3.1 特性完备或 PAL2 战斗已解决 |
| 32/64 位 Vulkan Direct | buffer/texture upload-readback bad=0，120 frames，PASS；同期 fresh route direct=1 | 自动 Direct 路由和基础资源正确性；小型 probe 速度不是游戏 FPS |
| WGL front/pbuffer 双窗口 | 32 位单缓冲、64 位双缓冲各四阶段，API 完成；真实截图颜色各 8/8 | API 与实际两窗口颜色一致；单看 PASS_API_ONLY 不足以断言像素正确 |
| PAL4 普通配置 | 第二格唯一存档“青鸾峰／序章／1-1 贡猪祭父”读入、人物与场景可见；20 秒新图发布平均 66.032 FPS，63.41–68.99 | 有限正常呈现回归；非同温度、同包严格 A/B，不能宣称性能提升 |

PAL4 的采样时间 11:50:50–11:51:10，普通 `-all,+err`，没有 LOW_MAP opt-in 或 sysmem 参数，固定青鸾峰木屋外场景、无测试输入。实际日志 `glSources=1,direct=0`，所以这是 OpenGL 游戏在共享桌面合成器上的新图发布速率。Battery 36.0°C → 36.0°C；未取得 GPU 时钟／降频证据。

本轮没有做 PAL2 战斗场景复测、Steam 回归、跨游戏性能 A/B 或完整全量测试，不能把有限像素检查扩展为“所有缺层与启动问题已解决”。

## 6. 后续通用验证优先级

1. 在同一个正常游戏现场测 CS map/unmap 等待时间与资源生命周期，不只看调用次数。保留失败回退，先修有效地址／上传缓冲持有问题再谈减少同步。
2. 补齐 texture download 来源和 GetTexImage 计时，区分合法回读与兼容层重复搬运；没有来源证据前不启用全局 sysmem 实验。
3. 用另一个已经确认受影响的 32 位游戏与一个正常对照游戏验证同一签名：shadow 命中比例、map/unmap 队列等待、texture download 次数／耗时。匹配分辨率、场景、输入、温度与运行时哈希，冷／热资源分开。
4. 有收益时同时验证文字、人物、透明层、RGB/sRGB、单／双缓冲多窗口；保持 OpenGL 3.1 与 Direct 自动策略。

当前结论是“发现并移除一段可复用的额外复制，另有同步与回读候选”，不是“《灰色的果实》证明 FEX、Wayland、UBO 或全部游戏存在同一个根因”。本轮不再追该游戏的专用优化，未自动 commit/push。

证据位于 `evidence/gray-common-paths-20261010/`，每个文件见 SHA256SUMS；原始采样和全日大日志留在本地 workspace_temp，不纳入小型证据包。
