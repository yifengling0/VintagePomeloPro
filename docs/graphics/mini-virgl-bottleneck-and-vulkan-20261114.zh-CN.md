# Mini：VirGL 瓶颈与 GLES / Zink / DXVK D3D9 选择建议

日期：2026-11-14。对象：Mini MLR-AL10 / Maleoon 910，Heaven 4.0 PE32。
本轮为既有证据、当前源码及上游公开资料审计，没有新建性能候选、构建、安装或运行设备。
源码检查点 0458a89261644978e659d0c4cd31d84dfaa04722；原有十个 dirty 子模块保留。
下文源码发现是待测假设，不把当前子模块源码自动等同于已安装二进制。

## 决策

**当前 GLES 保留为兼容基线；DXVK 1.10.3 D3D9 优先进行能力与接入审计；Zink 作为原生 OpenGL 游戏的专项候选。**
这不是将所有老游戏统一改成 DXVK，也不是立即开始三条路线的完整跑分。

| 路线 | 适用范围与潜在收益 | 本项目现状与门槛 | 建议 |
| --- | --- | --- | --- |
| WineD3D → virpipe/vtest → VirGL → 系统 GLES | 已验证的 D3D9 / 部分 OpenGL 兼容路径；仍可减少无效往返、资源同步和 GL 状态成本 | 本轮 Heaven 正在使用，零 CPU 整帧回读上屏；关键等待尚未定位 | 保持默认与回退，做有因果链的等待分析 |
| WineD3D → guest Zink → 系统 Vulkan | 保留 WineD3D，绕过 VirGL 的 GL 命令编码与远端执行；原生 OpenGL 游戏可直接使用 Zink | Mini 历史 PE64 探针已出图，但纹理 6/8，且当时上屏仍经回读/SHM；Heaven PE32 未验收 | 原生 GL 专项；先正确性与 GPU 上屏，后性能 |
| WineD3D → virpipe/vtest → host VirGL → Zink → Vulkan | 替换宿主 GL 驱动路径，保留 WineD3D 和 VirGL 事务链 | 未有当前可用候选；系统 EGL 的 context/texture/GLsync 不能视为与 Mesa Zink 自动互通 | 仅当证据指向宿主 GLES 驱动成本时再推进 |
| D3D9 → DXVK Legacy → Wine Vulkan → 系统 Vulkan Direct | 同时绕过 WineD3D 的 GL 翻译和 VirGL GL 提交链，覆盖的可消除开销更多 | D3D9 原生 DLL、功能缺口与呈现仍待补齐；既有 D3D11 Direct 成功不能代替 D3D9 验收 | Heaven 性能候选首选，但先做准入检查 |

如果 DXVK 实际走 Venus，它仍有 Vulkan 虚拟化/传输开销，不能标成系统 Direct。
“GLES / Zink”是 OpenGL 实现路径，“DXVK D3D9”是 Direct3D 实现；它们不完全处在同一层。
游戏实际 API、翻译层、传输后端、呈现通道应分别记录。

## 当前确实使用 GLES；上屏直通不等于绘制直通

本轮链路为：

```text
Heaven D3D9 → WineD3D → Mesa virpipe → vtest → virglrenderer → 系统 GLES → GPU
                                                                     → surface queue → 屏幕
```

证据：
- `entry/src/main/cpp/graphics/virgl_child.cpp:745`：ARM64 使用
  `--use-egl-surfaceless --use-gles`；同时有 `--venus` 表示具备另一种服务能力，不代表本次 GL 走 Vulkan。
- `F:/VintagePomelo-Workspace/artifacts/mini-virgl-fbo/runs/virgl-fbo-a2/transport-receipt.json`：
  `backend=virgl guestMode=mesa-virpipe`、`SURFACE_QUEUE`。
- opcode 诊断目录的 `native-tail.log`：app 能力探针为
  `OpenGL ES 3.2 B312 / HUAWEI / Maleoon 910`；Heaven renderer 为 `virgl (Maleoon 910)`。
- `virgl_surface_presenter.cpp:140` 取得来源 EGL display/context，
  `:395` 创建共享 context，通过共享 texture 和 GLsync 完成呈现。
  宿主替换为 Zink 时，需重做共享/导入与 fence 契约；不能混用两套驱动的对象句柄。

当前零 CPU 整帧上传不代表没有纹理上传、GPU copy 或同步等待。

## 已有性能证据能说明什么

| 证据 | 结果 | 可得结论 |
| --- | --- | --- |
| 最近完整 Low A-B-B-A | 基线 16.3 / 16.3；FBO 缓存 16.0 / 16.2 FPS | 未证实 FBO 缓存收益，保持默认关闭；30% 未达到 |
| Low 基线进程 CPU | Heaven 255.16–255.70%；VirGL child 60.25–61.32%；相关进程约 3.4–3.5 核 | 100% 是一个逻辑核；无法由总占用判断关键线程余量、自旋比例或 GPU 利用率 |
| opcode 独立诊断 105.063 s | batch 35.45% 墙钟；DRAW 18.04%；FBO 7.26% | 宿主热点在 draw/FBO；包含驱动等待、调度和诊断成本，不是纯 CPU 时间 |
| busy 独立诊断 105.246 s | 23984 次请求，完整 host handler 0.4018%；context_finish 0.0100% | 该 handler 内 glFinish 不是优先目标；不包含 guest 锁、往返和宿主队列 |
| 常量命令 | 约 5.21 GB，占编码量约 93%；host 保存阶段 0.706% | 应查重复率、guest 打包与 draw 中上传；不能直接判带宽瓶颈 |
| 旧 High 单轮 | 9.8 FPS，低于 Low | 画质增加确实有压力，但样本和条件不足以归因 GPU |
| GPU | 缺连续 busy、异步 GPU 时间与队列关联 | 既不能说 GPU 已跑满，也不能排除 GPU |

不同轮次的阶段比例不能直接相减或相加；未计时部分不能归类为 GPU 等待。
日志 `GPU_ACTIVE` 是路径状态，不是硬件利用率。

16.3 FPS 提升 30% 的目标是 21.19 FPS。用吞吐倒数理解，相当于约 61.35 ms 降至 47.19 ms，
需要减少约 23.08% 的有效瓶颈时间。这个换算不是实测逐帧耗时，也不是把多线程累计时间直接加减的依据。
目前的小缓存没有支持此量级收益；架构路线更值得研究，但收益仍须实测。

## 最值得定位的等待链

### 1. guest 非等待查询仍是同步事务

`virgl_vtest_socket.c:792` 的 busy 查询在发送后两次 blocking read 回复；
`:920` 在整个事务期间持 `socket_mutex`，submit / present 也共享这把锁。
flags=0 的含义是宿主不等 GPU 变 idle，并不意味着 guest 不等 socket 回复。
busy 记录中 23755 次为非 WAIT，约 226 次/秒。

因此可能出现“guest 等回复并持锁 → 其他提交无法发出 → GPU 缺少后续工作”的链条。
但频率不是耗时，现有数据尚未证明这条链主导。
锁曾修复真实协议交错问题，不能删除锁、伪造 busy=false 或无条件缓存完成状态。

`vtest_renderer.c:2432` 目前忽略资源 handle，以整个 context 的 implicit fence 判断 busy，
有把无关工作算进资源等待的可能。资源级完成序号/代次跟踪值得研究，但需要正确处理所有资源读写、
CPU map/transfer、跨 context、销毁复用与序号回绕，不能按资源名简单缓存。

### 2. 宿主 handler 外还存在排队与 poll

`vtest_server.c:944` 读 header 后在 `:974` 执行全局 busy poll，再进 handler；
外层事件循环顺序分发 client。已有 handler 计时没有覆盖完整请求生命周期。
需要区分等待输入、已到达请求的排队、poll、执行与回复，而不是给未覆盖时间取一个推测名称。

### 3. WineD3D 队列与自旋可能消耗 CPU

`cs.c:3331` 队列满时反复检查 tail；`:3403` finish 等队列完成；
map 回落会 wait_idle、排队后 finish；Present 根据最大帧提前量等待 event。
`wined3d_private.h:3660` client wait timeout 为 0，`:3855` 在 200 次循环后调用
零超时 NtDelayExecution；worker 在空队列时先旋转，再转 event wait。

这些是运行时命中率未知的源码路径。Heaven 的约 255% CPU 可能混合游戏计算、FEX、WineD3D 和自旋。
目标应是减少每帧有效工作与阻塞，不能以拉高 CPU 占用、扩大提前帧数或盲目扩线程作为成功标准。
现有 uploadBO / DISCARD / NOOVERWRITE 快路径已经存在，先查回落原因，不重复提出已有优化。

### 4. draw 内部状态与常量值得定向细分

`vrend_renderer.c:6256` 每 draw 调用 use_program；部分路径每 draw 绑定 VAO。
`:6265` 的 GLES texlod mask 命中时会构造名字并查 uniform location；
`:5869` 某些 texture-level uniform 会重复上传。
可研究 link 后 location 缓存和状态去重，但先测命中率/成本。

常量上传在 `:5614` 已有 dirty / new_program 保护，sysval 已有 cookie 去重；
guest encoder 已使用 memcpy。优化不能破坏 shader 切换、clear/blit/compute、make-current、
共享/separable program 与对象销毁后的状态恢复。

### 下一次定向测量

先完成一个“谁等谁”的有界诊断，而非继续多开关 A/B：
- 同一固定场景覆盖 guest socket 锁等待、锁内 write/read、事务往返、调用来源；
  host poll/dispatch/execute/reply；WineD3D map/finish/队列满/Present wait。
- 用 session、context、线程和事务顺序关联；支持时记录 monotonic 与线程 CPU 时间，
  给出分布和占比，并通过队列水位/活动区间判断等待是否处于供给关键路径。
  wall 减 thread CPU 只能表示未在该线程执行的时间，不能自动等于 GPU 等待。
- GPU 通过实际支持的 busy / 异步 timestamp 补证据；处理结果可用性和 disjoint，禁止每帧 glFinish。
  不支持就明确记缺失，以同画质不同分辨率对照作为辅助，不能据此单独定责。
- 默认关闭、固定日志位置、时长约 120 秒、单文件不超过 32 MiB；先核对实际库 SHA。
  release 无权读取线程信息时，不重复无效 shell 采样，使用进程内聚合或受支持 profiler。
- 定位到可消除关键路径后才做一项候选，再关诊断做同 API / 同画质完整对照。

## Zink：有试验基础，但还不是可切换的成熟性能后端

[2026-10-09 Mini 记录](../architecture/chaos-opengl-color-and-capabilities-20261009.md) 已验证：
真实 renderer 为 `zink Vulkan 1.2(Maleoon 910 (ARM_PROPRIETARY))`；
清屏/回读通过，8 项纹理检查通过 6 项，RGB565 与 ALPHA8 混合失败。
当时仅创建 legacy GL 2.1，3.0–4.6 请求失败，且是 PE64 探针。
这些是旧包、旧系统日期的证据，不能当作当前能力复测或所有游戏结论。

已有 `build_ohos_guest_gfx.sh --mode zink` 真正构建 zink driver，
补丁 0005 可用 `WINEHUA_ZINK_VULKAN_LIBRARY` 指向系统 loader。
默认包内 Vulkan loader 曾指向 Venus 并初始化失败，因此不能只改 GALLIUM_DRIVER。

历史缺口包括 transform feedback、dual source blend、logicOp 和部分资源上限。
Zink 对桌面 GL 的要求与 DXVK D3D9 的要求不同；不能把前者的失败直接推导为后者不能运行，
也不能用伪造能力声明绕开语义实现。Zink 优先用于原生 OpenGL 程序的兼容/性能研究；
Heaven D3D9 若使用 WineD3D + Zink，仍保留 WineD3D 这层工作。

## DXVK D3D9：上游支持，本项目需要补接入和能力适配

[DXVK 1.10.3 README](https://github.com/doitsujin/dxvk/blob/v1.10.3/README.md)
明确包含 D3D9 实现、32/64 位 DLL，并要求用加载日志/HUD 确认实际生效。
[上游驱动要求](https://github.com/doitsujin/dxvk/wiki/Driver-support)
对 legacy 1.10.3 列出 Vulkan 1.1；API 版本只是门槛之一。
当前上游新版本的要求更高，且不提供 Android/mobile 驱动的一方支持，
所以不能从桌面 DXVK 的成功直接推断 HarmonyOS Maleoon 成功。

本项目新增审计发现：
1. `wine_d3d_policy.h:6` 明确两档 DXVK 保留 D3D8/9 为 WineD3D，包含 `d3d9=b`。
2. `scripts/assemble.sh` 的 DXVK overlay 复制 D3D11/DXGI 和适用的 D3D10 链，没有 D3D9；
   `build_dxvk_arm64x.sh:172` 仅链接 DXGI/D3D11。已有源码、编译过对象不等于有可用 D3D9 成品链。
3. legacy 源码版本为 v1.10.3；`d3d9_device.cpp:3889` 的 GetDeviceFeatures
   仍将 multiViewport、textureCompressionBC、shaderCullDistance 等设为 VK_TRUE，
   交给 createDevice。Mini 历史探针已有 multiViewport=false。
   若当前所选 Direct 路由仍暴露相同能力，这就是创建阶段的潜在阻断项；
   D3D11 的适配成功不能证明 D3D9 所需适配已经完成。
4. D3D9 使用共用 Vulkan Presenter，但 device reset、lock/readback、窗口生命周期以及 PE32 thunk
   仍要独立验收。不能通过删掉必需 feature 请求却保留对应调用来宣布兼容。

推荐先做 PE32 的能力审计及最小 D3D9 bring-up，复用现有 1.10.3 fork、Wine Vulkan 和 Direct 呈现。
正确性至少覆盖：固定管线/SM2/SM3、动态资源 DISCARD/NOOVERWRITE、managed/default pool、
RGB565/alpha/BC 纹理、sRGB RT 与混合、深度格式、query、读回、Reset/resize/切全屏。
先验证 Heaven 所用功能，再扩展到已知问题老游戏，不要求在第一个候选前完成所有 D3D9 CTS。

Direct 不成立时，可用 Venus 作隔离诊断，但单独标记路由。旧 Heaven D3D11 19.0 FPS 是 Venus 功能回归，
API、shader/资源路径不同，不能作为 D3D9 改 DXVK 后的收益数字。

老游戏还可能实际是 DirectDraw / GDI / D3D8，或者卡在视频/音频、FEX、输入/窗口；
不能只凭年代归入 D3D9，也不能把旧问题笼统归咎于 DXVK。
回归应看画面、启动/重开、长帧和稳定性，再看平均 FPS；冷 shader 编译与热缓存稳态分开。

## Winlator 官方的做法与可借鉴之处

以下为 2026-11-14 读取的官方 winlator-app main 源码，不代表所有发行 APK 或第三方 fork。

- [GraphicsDriverPicker](https://github.com/brunodev85/winlator-app/blob/main/app/src/main/java/com/winlator/container/GraphicsDriverPicker.java)
  把 Vulkan 与 OpenGL 分开配置。
- [GraphicsDrivers](https://github.com/brunodev85/winlator-app/blob/main/app/src/main/java/com/winlator/container/GraphicsDrivers.java)
  提供 Vulkan：Turnip / Vortek，OpenGL：Zink / VirGL / Gladio。
  当前源码默认 Adreno 选 Turnip，其余选 Vortek；OpenGL 默认 Gladio，
  不能继续把“非 Adreno 一律 VirGL”当作现行完整策略。
- [Container](https://github.com/brunodev85/winlator-app/blob/main/app/src/main/java/com/winlator/container/Container.java)
  Direct3D 默认 DXVK；[DXWrapperPicker](https://github.com/brunodev85/winlator-app/blob/main/app/src/main/java/com/winlator/container/DXWrapperPicker.java)
  允许 WineD3D / DXVK，D3D12 单独 VKD3D。
- [DefaultVersion](https://github.com/brunodev85/winlator-app/blob/main/app/src/main/java/com/winlator/core/DefaultVersion.java)
  当前写明 MINOR_DXVK=1.10.3、MAJOR_DXVK=2.4.1；Vortek 下读取 Vulkan API，
  低于 1.3 默认 1.10.3，达到 1.3 默认 2.4.1；Turnip 默认后者。这是默认选择规则，不是完整正确性证明。
- [Vortek 官方说明](https://github.com/brunodev85/vortek)
  描述了系统 Vulkan 上的兼容层，包含格式模拟、SPIR-V 检查、纹理解码。
  [Mesa Turnip](https://docs.mesa3d.org/drivers/freedreno.html) 面向 Qualcomm Adreno，
  不能将 Adreno 上 Turnip+DXVK 的结果照搬到 Maleoon。
- [Winlator releases](https://github.com/brunodev85/winlator/releases) 继续分别修复 Vortek 与 Gladio，
  并提供组件选择/安装。它并未让所有图形调用统一经过 Zink。

本项目可借鉴的是：按实际游戏 API 选择翻译层、按设备完整能力选驱动/版本、保留逐游戏覆盖和已验证回退。
Vortek/Gladio 是其平台适配成果，并不是可以直接换名塞进 OHOS 的驱动。

读取的 Git blob SHA（便于核对网页漂移）：
GraphicsDrivers d8d899004cf2a5bec381128d239387e8607d5e3d；
DefaultVersion 86aa7b1bb0bffe8b9612317d3dda433164205f0e；
DXWrapperPicker 22980d4b8362a0059942112d110dadcc17145cd0；
Container 2086786f2d68c9d4260bbb60b7dac12bef9d4196。
这些是文件 blob 身份，不是发行版本 commit。

## 实施顺序与停止条件

1. 现有默认保持；一次 VirGL 完整等待分析，回答 GPU 是否缺供给及 guest 实际阻塞在哪。
2. DXVK 1.10.3 D3D9 先审计能力/格式/PE32 成品链，修复具体语义缺口并完成小型出图验证。
   若存在代价很高的关键能力缺口，停止推进 Heaven 长跑，回到已定位的 VirGL 优化。
3. 功能通过后用同一个 Heaven D3D9 Low 场景比较 WineD3D/VirGL 与 DXVK Direct；
   保持分辨率、AA、同步、缓存策略、温度条件一致，记录 p95/p99、CPU、呈现与 GPU 指标。
   报告收益时分别说明 API 翻译与 Direct 传输变化，必要时用 DXVK Venus 控制组拆开归因。
4. 针对已证实有收益且正确的游戏保存选项；未知老游戏继续走兼容基线，
   回退在下次启动切换，不在运行中混用两套 D3D 实现。
5. Zink 等纹理正确性、实际 PE32/PE64 库身份与 GPU 呈现具备后，进入原生 OpenGL 专项；
   不与 DXVK D3D9 同时修改主路径，避免失去归因。

本轮没有新增大包或日志副本；既有冻结 artifacts 不变，保留当前基线与上一候选两份签名 HAP。
