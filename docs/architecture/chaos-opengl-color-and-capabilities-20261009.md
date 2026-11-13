# Chaos 颜色、Gal GPU 呈现性能与 OpenGL 能力适配

2026-10-09，`feature/main_proton`，性能检查点 `f47090663537b1e70ad1ebe2f76bd2c34cfbb6af`。
本报告对应 MatePad Mini / Maleoon 910；不能把本机结论推广到所有手机和平板。


Chaos 泛白与 Gal 呈现瓶颈已经定位并修复：相同剧情画面由 SHM 对照的 13.42 FPS 提升到 GPU 队列的 58.28 / 58.62 FPS。32/64 位颜色及双窗口 resize/recreate 探针通过。修复保持 VirGL GPU 绘制，Zink 仍是独立实验后端；高版本 GL 能力矩阵与实际性能结论分别说明。

## 本轮实际结果

Wine 的 WGL 接口可以创建高版本上下文。限制主要在图形后端，不能通过修改 OpenGL 版本字符串解决。
同一包、同一 Wine/FEX，在真机得到以下结果。没有设置版本或扩展强制覆盖。

| 后端 | 真实 renderer | 可创建的上下文 | 清屏 / 回读 | 纹理采样小测 | 3.3 着色器绘制 |
| --- | --- | --- | --- | --- | --- |
| VirGL → 宿主 GLES | `virgl (Maleoon 910)` | legacy 2.1；3.0～4.6 请求失败 | 通过 | 8 / 8 RGB 检查通过 | 无法创建 3.3 上下文 |
| Zink → 系统 Vulkan，实验开关 | `zink Vulkan 1.2(Maleoon 910 (ARM_PROPRIETARY))` | legacy 2.1；3.0～4.6 请求失败 | 通过 | 6 / 8；RGB565、ALPHA8 混合失败 | 无法创建 3.3 上下文 |
| Softpipe，软件对照 | `softpipe` | 3.3 core / compatibility；4.x 请求失败 | 通过 | 8 / 8 RGB 检查通过 | GLSL 330 三角形绘制及回读通过 |

这些是上下文和小型绘制测试，不是 OpenGL CTS 全量合规结论。
Zink 的两项失败尚未定位；没有启用 Vulkan validation，不应作为默认渲染后端。
纹理检查比较 RGB、容许 2/255 量化误差，并要求 `GL_NO_ERROR`；alpha 单独记录，未作为通过条件，Windows 像素格式可能没有完整 alpha 存储。

PE32 原始 WGL 探针和本轮 PE64 VirGL 探针都只能创建 2.1。
后续 PE32 v4 探针在 VirGL 上通过 16 项 RGB / GL error 检查：在原 8 项基础上加入 RGBA 半透明、DXT3 / DXT5 半透明、GLSL 1.20 采样、颜色调制、独立 alpha MAX 混合，以及 packed BGRA 顶点缓冲的直接上传 / map-unmap 写回。
Softpipe 限制为 GL 2.1 / GLSL 1.20 的 PE32 v3 对照通过其中 14 项（该版尚未包含两个 VBO 检查）；输出实际版本已核对。
不能据此声称完整的游戏资源更新、所有 stride / PBO / FBO 或 shader 组合正确。
PE32 / PE64 Vulkan 只读探针得到相同的关键 features / limits。
这排除了“Wine/FEX 接口完全不支持 OpenGL 3.3”的解释，但不构成对两种架构所有图形调用的证明。

## VirGL 为何停在 2.1

下面 capset / 缺失扩展记录属于本日先前基线。后续候选已补齐 UBO 和条件渲染，仍报告 GL 2.1；最终 PE32/PE64 与剩余版本门槛见 [UBO / 条件渲染补齐报告](opengl-gles-ubo-conditional-render-20261009.md)。

宿主是 `OpenGL ES 3.2 B312`，VirGL capset 宣告 `glsl_level=430`。
GLSL capset、宿主 GLES 版本和 guest desktop OpenGL 版本是不同指标。
Mesa `src/mesa/main/version.c` 按扩展、资源限制及 shader 能力逐级判定桌面版本。

本机 VirGL capset：

```text
texture_srgb_decode=1
srgb_write_control=0
egl_image_srgb_import=1
conditional_render=0
max_uniform_block_size=65536
max_uniform_blocks=12
```

guest 的 legacy 扩展列表缺少 `GL_EXT_framebuffer_sRGB` 和 `GL_NV_conditional_render`，它们直接阻断 3.0。
列表还缺 `GL_ARB_uniform_buffer_object`、`GL_ARB_blend_func_extended` 等后续门槛。
4.3 需要 vertex-stage uniform blocks 至少 14，本机 VirGL capset 的 12 也不足。
高版本创建失败返回 `EGL_BAD_MATCH (0x3009)`；WineD3D 请求 4.4 / 3.2 后回落到 GL 2.1 / GLSL 1.20 / Shader Model 3。

PE64 legacy 上下文中对 UBO limit 的查询值为 0，因为相应扩展未暴露且查询可能报 invalid enum。
不要把这个 0 当作宿主物理 UBO 为 0，也不要把 capset 的 12 解读成当前上下文已经支持 UBO。

## Zink 的实际条件与障碍

本机 Wine 可见 Vulkan 设备为 Maleoon 910，device API 1.2.53。
以下来自只读查询，并非原始系统 `vulkaninfo`：Wine 会转换 Win32 external-memory 等扩展。
shell 直接执行 OHOS 探针被系统拒绝，未得到独立于 Wine 的 native 采样。

| 能力 | 本机查询 | 适配意义 |
| --- | --- | --- |
| geometryShader / tessellationShader | true / true | 部分高版本 shader 基础可用 |
| conditionalRendering | true | Vulkan 具备这一能力，宿主 GLES/VirGL 当前没有等价开关 |
| transform feedback | 扩展缺失 / false | 当前 Zink 无法暴露完整 GL 3.0 transform feedback |
| dualSrcBlend | false | 当前 Zink 的 GL 3.3 双源混合门槛不满足 |
| logicOp | false | Zink 基础绘制正确性缺口 |
| custom border color / line rasterization | 扩展缺失 | Zink 基础正确性还需处理 |
| multiViewport / maxViewports | false / 1 | GL 4.1 的多视口门槛不满足 |
| maxImageDimension1D / 2D / Cube | 8192 / 8192 / 8192 | GL 4.1 要求 16384 |
| vertexPipelineStoresAndAtomics | false | 后续版本的 shader storage 能力受限 |

没有 float64 / int64 的原生 Vulkan feature 不等于不能实现对应 GL 运算：Zink/NIR 有软件降低路径。
是否可用、代价和正确性必须单独测，不能统一把所有缺失 feature 当作硬件死限。
但当前 Zink 对 transform feedback、dual source blend 和 viewport / texture limits 的实际 cap 判定仍受上述缺口影响，真机 WGL 创建结果也印证了版本限制。

### 已修正的 Zink 接入问题

原 `--mode zink` 只改变环境文件，Mesa 仍编译 `virgl,softpipe`；现在真实编译 `zink,virgl,softpipe`。
默认 VirGL 产物目录保持兼容，Zink 使用隔离的 install / bundle 目录；命令行 `--mode` 在解析完成后决定默认路径。
打包要求实际 Meson build identity 与 Gallium 库 SHA256 一致，拒绝把旧 VirGL 库重命名为 Zink。
打包同时拒绝 `GUEST_ARCH` 与 `WINE_ARCH` 不一致，避免支撑库混用架构。

第一次真机 Zink 初始化返回 `VK_ERROR_INCOMPATIBLE_DRIVER`：默认名字加载到包内为 Venus 配置的 loader，而 Wine Direct 使用 `/system/lib64/libvulkan.so`。
新增实验变量 `WINEHUA_ZINK_VULKAN_LIBRARY`，不设置时保持 Mesa 原选择。
同时修正 instance 创建失败后可能返回未初始化 handle 的错误处理。
指定系统 loader 后，真机 renderer 明确变为 Zink，清屏及基本回读成功；高版本能力缺口及两项采样失败仍存在。

Zink 实验的窗口路径仍是 Wine EGL/pbuffer 的读回及 SHM 呈现。使用系统 Vulkan 做 Zink 渲染，并不代表已经接入 DXVK 的 Direct 零复制窗口呈现。
真正的 Zink WSI 还要匹配 OHOS native surface、窗口身份/代次、fence、resize 和销毁顺序；普通 `VK_KHR_wayland_surface` 不能直接当作 `VK_OHOS_surface` 使用。

## 推荐的适配顺序

1. **先完善能力判定与画面正确性。** WGL probe 同时验证 core / compatibility、shader 编译、实际绘制、像素采样和回读。后端选择以设备及实际测试为依据，能力查询与呈现测试分别记录。新后端先按游戏显式使用，验证完成后再设计自动回退。
2. **VirGL 优先补 3.x 的具体缺口。** 条件渲染需要正确实现 query wait / no-wait 与 draw、clear、blit 的抑制；CPU query 回退可行但会引入等待，不能直接打开 cap。sRGB 写控制应研究 linear/sRGB texture view 或同一 backing 的正确 attachment 切换，验证采样解码、目标编码、混合、MSAA、blit、import/readback；全局 gamma 补偿不等价。双源混合可调查 framebuffer fetch 的语义实现，但需要顺序、混合方程及多采样专项验证，成本可能较高。
3. **Zink 保留为长期 GPU 路径。** 按 Mesa 的 Vulkan Profiles 列出设备缺口，修正两个纹理采样失败，再接入 OHOS WSI 和现有窗口身份 / 同步机制。满足要求的设备才进入高版本 GPU 候选；不能把本机 2.1 实验直接推广为全局 4.6。
4. **3.3 软件路径作为诊断和显式兼容回退。** 本轮 Softpipe 已实际创建 3.3 并绘制成功；复杂游戏会付出明显 CPU 开销。4.6 软件参考可研究 LLVMpipe，但当前 OHOS LLVMpipe 未构建/验证，不能宣称已有 4.6 支持。
5. **用 CTS / Piglit 补验能力闭环。** 最少覆盖 conditional rendering、sRGB FBO 与 blend、dual source blend、UBO、transform feedback；4.x 再加入 SSBO / compute / image、buffer storage、indirect draw、SPIR-V 等。创建上下文或通过一个 shader 都不等于整个版本可用。

Proton/Wine 提供 Windows 图形接口和上下文/窗口适配，实际 desktop GL 实现来自 Mesa/厂商驱动。
DXVK 可绕过 WineD3D 的 D3D9/10/11→OpenGL 路径，但不能直接解决原生 OpenGL 游戏的 3.3/4.6 请求。
GL4ES 以旧版 desktop GL→GLES 为主，ANGLE 以 GLES→其他后端为主；两者不能直接替代完整 desktop GL 4.6 实现。

## Mesa 26.2.4 合并候选

已找到 `origin/codex/mesa-26.2.4-ohos-port`，HEAD `42f5525b1fe`，VERSION `26.2.4`。
移植提交包含 VirGL / Venus integration，共 26 个文件；本轮没有切换或合并，保留当前子模块未提交改动。
该分支 Zink 对 transform feedback、dual source blend 等 cap 仍有对应判断，升级不会自行补齐本机不暴露的 Vulkan 能力。

合并时应使用独立源码/构建目录，对齐原有 Mesa OHOS patch 和本轮 Zink / socket transaction patch，并重做本报告的全部矩阵。
重点对照 texture sampling、sRGB、32/64-bit 映射、Direct/Venus 呈现与游戏窗口切换；先保住画面和能力声明，再比较 FPS。

## Chaos 泛白根因与通用修复

正确入口是 `Z:\games\Chaos;Child R15\启动游戏.exe`，再由启动器运行 `Game.exe`。
游戏实际使用 PE32 / FEX → builtin D3D9 → WineD3D → Mesa VirGL → 宿主 GLES。
产品设置为 `dxvk_legacy` 不代表 D3D9 游戏也使用 DXVK；桌面的 Vulkan compositor 与游戏的渲染 API 是两个不同层次。

### sRGB 存储复用误判

设备 `EXT_texture_sRGB_decode=1`，但 `ARB_framebuffer_sRGB` / sRGB write control 不可用。
WineD3D 原来只根据 decode 能力，把普通 linear texture / render target 的内部格式也复用为 sRGB。
GLES 无法关闭目标编码，多次 RT / blit 导致重复编码，原始 SHM 像素在进入桌面前已经泛白。

`patches/wine/0049-wined3d-require-srgb-write-control-for-shared-storage.patch` 在缺少 write control 时关闭 Wine 内部共享 sRGB 存储优化，保留正确的独立 linear / sRGB texture 路径。
没有修改 Mesa 对游戏报告的 GL 版本或全局 gamma，也没有改用软件渲染。
这也解释了为何相同 GL 2.1 的 Softpipe 对照颜色正确：单独把 GL 版本低当作根因不成立。

`d3d9_srgb_render_target_probe.c` 覆盖 linear RT、四次 ping-pong、混合、显式 sRGB 读取及恢复 linear 读取。
旧版默认 10/10 失败，旧版关闭 decode 的控制实验 10/10 通过；修复候选默认 PE32、PE64 均 10/10 通过，设备 DLL 哈希与打包产物一致。
Chaos 标题中的黑色阴影恢复正常；OP 自身的白色特效不作为泛白回归证据。

另发现 VirGL shader blit 的两个 sRGB capability flag 接反，已经用独立补丁修正。
该赋值来自上游 `3b5eb5f9`（2021-07-08），修正后游戏仍泛白，所以它是另一项明确错误，不能独自解释本次颜色问题。

## Gal 低帧率：GPU 渲染后的整帧回读

旧 Vulkan 桌面只接收 Direct Vulkan 和 SHM，没有消费 VirGL OpenGL 的 NativeBuffer 队列。
因此即使游戏绘制已经使用 GPU，每次 Present 仍经过：

```text
WineD3D / VirGL / GLES GPU 绘制
  → glReadPixels → CPU SHM → Vulkan 上传 → 桌面合成 → 屏幕
```

`direct_vulkan_desktop_compositor.cpp` 现在消费已有 GL producer 的 NativeBuffer queue：

```text
WineD3D / VirGL / GLES GPU 绘制
  → NativeBuffer + acquire fence → Vulkan import / sampling → 屏幕
  ← GPU release fence 归还 producer
```

沿用完整窗口身份、binding generation、z-order、producer 接管规则和 acquire/release fence。
GLES buffer 的外部 layout 使用 GENERAL，原 Direct Vulkan source 仍保持 PRESENT_SRC_KHR 契约；GL 垂直方向经实际像素探针验证。
导入或身份验证失败时撤销 fast path、恢复已有 SHM 路径。没有按 surfaceId 跨进程猜测，也没有每帧 WaitIdle。
日志增加 glSources / glDraws，并把原先容易误读的硬编码 gameCpuReadBytes 改成 directGameCpuReadBytes：它只描述 Direct 分支，不能拿它证明 GL 无回读。

### 同场景 GPU → SHM → GPU 对照

同设备、同 1280×720 游戏输出、同入口、同一 New Game 首张剧情卡（2009-11-06 22:28）、默认产品设置，无实验后端覆盖。
每轮完整退出 Wine，覆盖安装对应包后启动；对照包已含颜色修复，三轮使用相同 WineD3D DLL。
前后截图中央两行中文所在区域逐像素一致，排除了 OP / 菜单内容不同的场景混淆。

| 呈现路径 / 顺序 | 30 秒平均 FPS | 区间最小 / 最大 FPS | 样本数 |
| --- | --- | --- | --- |
| GPU 队列，第一次 | 58.28 | 56.58 / 59.97 | 30 |
| SHM 对照 | 13.42 | 9.70 / 15.99 | 29 |
| GPU 队列，复装后 | 58.62 | 56.85 / 59.98 | 30 |

统计为桌面每秒新游戏图像成功呈现次数，保留原始区间日志；不是单帧延迟、GPU timestamp 或引擎内部 FPS。
旧路径的 `uiUploadBytes` 把来自 SHM 的游戏帧也计入 UI 上传：采样窗口内可见计数端点新增 **1,327,104,000 字节**；两轮新路径该计数不增长。
新路径 `glSources=1`、imports=2、reuses 持续增长，releaseErrors=0；Wine readbacks 固定为 2，仅启动接管前发生回读。
这证明稳态没有逐帧回读游戏画面，不代表纹理资源加载或普通 SHM UI 从此没有 CPU 上传。

GPU 首轮 / SHM 比值 4.34 倍；这是本设备、该剧情场景的结果，不能承诺所有 Gal 相同倍数。
没有锁定 CPU / GPU 频率，温度快照保存在证据中；复装 GPU 后再次达到接近 60 FPS，有助于排除单次热缓存或重启偶然性。
候选包含队列接入、通道并发保护和尺寸修复，尚未单独拆分每项对 FPS 的贡献。
之前名为 matched-title 的 54.87 FPS 样本，前后截图实际均为不同 OP 帧，已标注 sceneMatched=false，不用于同标题 A/B。

### VirGL 通道事务并发保护

真实现场抓到 `Illegal command buffer 14`，正文起始为 `0000000e 57485052`，即自定义 Present 的 `[14, WHPR]` 请求头插进 SUBMIT_CMD 正文。
原来多线程共享 socket，提交与 Present 没有覆盖完整 header / payload / response / FD 接收的事务锁。

`patches/mesa/0006-virgl-vtest-serialize-socket-transactions.patch` 使用独立 recursive socket mutex，并在 winsys caller 覆盖组合传输与 blob submit/create。
Present 的 pacing 使用另一把锁，sleep 不占 socket 锁；保持 cache → socket 的锁序。
Mesa 自带 C11 要求 `mtx_plain | mtx_recursive`，已修正第一版单独传 mtx_recursive 导致初始化失败的问题。

测试直接编译实际生产 socket 源码和 Mesa 自带 C11 实现。
旧生产代码确实复现交叉正文；新版通过 240 submits、61 presents、40 replies、30 legacy transfers，pacing 等待期间仍可提交。
这是通用正确性修复，不把它单独声称为四倍性能收益。

### GPU resize 与输入共用尺寸

双窗口探针放大到 480×320 后，旧 SHM 停在 320×240；GPU 合成和输入仍优先旧尺寸，右下角没有覆盖。
`zc_bridge.cpp/.h` 在消费 GPU buffer 时保存协议尺寸快照：协议没有后续 resize 就使用已消费 GPU 尺寸；协议再次改变则立即采用新尺寸，直到下一帧 GPU 内容到达。
GetLayerInfo、GetContentSize 和 ActiveOwner 共用规则，保留 viewport、generation 和 active producer 校验。
旧生产代码的 resize 断言可复现失败，新版十组测试通过，包括缩放后的输入中心坐标。

最终候选 PE32 / PE64 双窗口 probe 均为 errors=0、两个窗口各 1124 帧、resize=1、recreate=1。
四张 resize / recreate 截图共 32 个彩色角区域通过 RGB 检查，误差 ≤2/255；身份、方向与完整覆盖都正确。

## Zink 与 GPU 模拟能力的适用范围

这次 Gal 低帧率已经通过保留 VirGL GPU 渲染、移除逐帧 CPU 回读解决，当前收益不依赖 Zink。
本机 Zink 的实际 WGL 仍为 GL 2.1，且两个纹理采样用例失败，因此不能直接换成默认后端来宣称完整 3.3 / 4.6。

缺失特性可分项研究等价 GPU 实现，方案应把整帧留在显存中：

| 缺口 | GPU 实现调查方向 | 必须验证的条件 |
| --- | --- | --- |
| sRGB 目标写控制 | 正确的 linear/sRGB view / attachment，必要时 GPU 中间 RT 转换 | linear 空间混合、编码次数、MSAA、blit、纹理采样一致；不能仅做最后 gamma |
| conditional render | 已暴露 Vulkan conditionalRendering 的后端路径，或 GPU predicate / indirect draw 降低 | query wait/no-wait 以及 draw、clear、blit 抑制；避免 CPU 读取 query 阻塞 |
| dual-source blend / logic op | 按设备扩展研究 coherent framebuffer fetch / interlock，或有严格条件的 GPU 分阶段合成 | 每图元顺序、重叠、深度/模板、混合方程、多采样；额外 pass 未必通用或便宜 |
| transform feedback | 具备可写 shader storage 的设备上研究 shader / buffer lowering | overflow/query、stream 顺序、同步和 stage 能力；本机 vertexPipelineStoresAndAtomics=false，不能假定可行 |

这些是后续方案，不是本轮已经实现的 GL 扩展。硬件资源上限也不能靠 shader 伪造。
推进顺序为：先复用这次正确的 GPU 呈现和同步机制，再按游戏实际需求补能力并运行 Piglit / CTS；Mesa 26.x 放在独立目录做对照，保留已有修复。
收益可推广到走相同 Vulkan 桌面 / VirGL SHM 回读路径的程序；已使用 EGL GPU 队列、纯 GDI、视频解码或有引擎帧率上限的 Gal，需要分别测量。

## 最终候选、回归与边界

最终未签名候选：`chaos-gpu-final-clean-unsigned-20261009.hap`，SHA256 `115522044f40452be048bc24966b25f9af8996cfc05fb36a7bac9b3d2ed87fb5`。
libentry SHA256 `179cfcdbaff6c40aa0202584399c301b5b4bf69d7f1845eec6accd9b9dfb5635`；Gallium SHA256 `fe528b3c75f10ccc966aa967febdf12742cd8f83e234011ea7308548d6bae0e2`。
设备已经恢复最终修复候选；没有保留 Zink、Softpipe 或临时捕获环境。

原 DXVK Direct 回归：以默认入口运行 `Z:\games\GUMU10\ROTTR.exe -dx11`，启动器进入实际 3D 主菜单。20 秒平均 30.02 FPS，区间 28.84–30.99；日志 direct=1、glSources=0、releaseErrors=0，UI 上传稳态固定，确认新增 GL 队列没有接管原 Vulkan source。这是启动/菜单与路由回归，没有重测游戏内长期性能或新增增益。

已完成独立 Wine PE32 / ARM64X、OHOS libentry 和 Gallium 编译；窗口输入/resize、socket 事务、共享呈现分派、构建身份和相邻 Direct 检查通过。
未运行全量 CTS、所有 Gal、手机、Steam 和长时间压力回归。
本轮新修改保持未提交；没有 commit 或 push，原性能本地检查点仍为 f4709066。


## 复现与验证

```sh
# 在受支持的 OHOS build container / toolchain 环境内执行。
# Mesa 构建目录与安装目录独立；这是本轮实际完成构建的命令形式。
bash scripts/build_ohos_guest_gfx.sh --mode zink \
  --build-root "$PWD/build/gl-zink-experiment-20261009" \
  --install-root "$PWD/build/gl-zink-install-20261009" --no-package --no-fetch

# 先按相同架构准备真正的 zlib runtime，不使用 SDK stub。
GUEST_ARCH=aarch64 bash scripts/build_zlib_runtime.sh
GUEST_ARCH=aarch64 bash scripts/build_guest_gfx.sh --mode zink \
  --install-root "$PWD/build/gl-zink-install-20261009" \
  --output-root "$PWD/build/gl-zink-bundle-20261009"

python3 host_tests/guest_gfx_build_identity_test.py
```

新探针：`scripts/probes/wgl_capability_probe.c`、`scripts/probes/vulkan_opengl_capability_probe.c`。
已完成 PE32/PE64 编译；后者也完成 OHOS AArch64 编译，但 shell 运行被拒绝。
WGL probe 的参数为输出文件路径，Vulkan probe 同样为输出 JSON 路径；完整退出 Wine/FEX 后再改变实验环境。

本轮通过：Zink OHOS 编译/打包、VirGL 增量编译、6 个构建身份回归、8 个相邻运行库检查、补丁幂等应用、shell 语法和 Git diff whitespace 检查。
没有运行全量 OpenGL CTS、全量产品测试或手机 Zink 真机测试。

小型证据在 `evidence/opengl-20261009/`，含同场景三轮采样、GPU/SHM 路由计数、最终 PE32/PE64 探针和 SHA256 清单。
完整本地证据与候选包在 `F:\VintagePomelo-Workspace\workspace_temp\gumu10-dx11-20261008`；大日志与 HAP 未加入 Git。
性能检查点只做本地提交，没有 push。本轮 OpenGL 新改动尚未提交。

## 源码参考

- [Mesa Zink 文档](https://docs.mesa3d.org/drivers/zink.html)；本轮具体门槛以仓库中的 `thirdparty/mesa/docs/drivers/zink.rst` 和 `VP_ZINK_requirements.json` 为准。
- [Mesa 26.2.4 OHOS 移植分支](https://github.com/winehua/mesa-ohos/tree/codex/mesa-26.2.4-ohos-port)。
- Mesa：`src/mesa/main/version.c`、`src/mesa/state_tracker/st_extensions.c`、`src/gallium/drivers/virgl/virgl_screen.c`、`src/gallium/drivers/zink/zink_screen.c`。
- Valve Wine baseline `cd547f7a0ee9d3d59b3ec47317a7852dedf0443b` 加本地 OHOS patch：`dlls/win32u/opengl.c`、`dlls/wined3d/context_gl.c`、`dlls/wined3d/adapter_gl.c`、`dlls/winewayland.drv/vulkan.c`。
- VirGLRenderer baseline `bb33e52aa03199926f662e50ff22bd00d19d7e0a`：`src/vrend/vrend_renderer.c`、`src/vrend/vrend_blitter.c`。
