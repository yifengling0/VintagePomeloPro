# OpenGL sRGB 目标写入：原生 GPU 共享与同步验证

2026-10-09，`feature/main_proton`，HEAD `f47090663537b1e70ad1ebe2f76bd2c34cfbb6af`。
Maleoon 910，宿主实际 `OpenGL ES 3.2 B312`，Vulkan 1.2.53。

本轮找到并验证了补齐 framebuffer sRGB 的两条 GPU 实现路径：
**NativeBuffer 两种 EGLImage 视图加服务端 fence 同步；普通纹理双存储加原始 GPU image copy。**
这是资源层实验，不是已经接入生产 VirGL 的 GL 3.0 实现。
当前 guest desktop GL 仍为 **2.1**，生产 cap 没有改变，没有 FPS 增益结论。

## 实测缺口，而非版本号推测

原生 GL 上下文实际查询：

| 能力 | 宿主结果 |
| --- | --- |
| `GL_OES_texture_view` / `GL_EXT_texture_view` | 均不存在 |
| `GL_EXT_sRGB_write_control` | 不存在 |
| `GL_EXT_texture_sRGB_decode` | 存在 |
| `GL_OES_EGL_image` | 存在 |
| `EGL_EXT_image_gl_colorspace` | 存在 |
| `EGL_KHR_fence_sync` / `EGL_KHR_wait_sync` | 存在并实际调用成功 |

这次没有从 guest 的扩展缺失反推 host 能力。探针运行在真正的原生
GLES renderer 初始化上下文中，读取完整的匹配扩展。

普通 GL 纹理的 EGLImage 颜色重解释未通过：RGBA8 的 linear image 可以创建，
sRGB image 被拒；SRGB8_ALPHA8 则可以创建 sRGB image，linear 重解释被拒。
已控制 RGBA8 / SRGB8_ALPHA8、创建顺序，以及先创建两个 image 再导入 texture，
仍出现 `EGL_BAD_ACCESS (0x3002)` 或 `EGL_BAD_PARAMETER (0x300c)`。
这是当前驱动和这些输入的实验结果，不能推广为所有 GLES 实现都不支持。

## NativeBuffer：共享存储可行，但必须显式同步

申请一份 RGBA8888 NativeBuffer，使用 HW_RENDER / HW_TEXTURE / MEM_DMA，
在同一份 window buffer 上分别创建 linear 和 sRGB EGLImage，并导入两张 GL texture。
两种附件的 `GL_FRAMEBUFFER_ATTACHMENT_COLOR_ENCODING` 分别正确返回 LINEAR 和 SRGB。

只成功创建视图不够。无同步、仅 `glFlush` 或仅 `glMemoryBarrier(GL_ALL_BARRIER_BITS)` 时，
跨视图 clear / draw / blend 会读取旧字节；不同轮次失败数略有变化。
第一次未同步试验 12 项有 6 项失败。重建新 backing 后仍能稳定复现此类错误。
这证明转换视图必须有跨对象的可见性约束；尚未定位 GPU 驱动内部具体缓存实现。

下面这组操作使验证通过：

```c
EGLSyncKHR fence = eglCreateSyncKHR(display, EGL_SYNC_FENCE_KHR, NULL);
glFlush();
eglWaitSyncKHR(display, fence, 0);
eglDestroySyncKHR(display, fence);
// 随后使用另一种颜色视图。
```

每个 API 返回值都检查。独立 backing 的服务端 wait 试验先于 `glFinish` 对照，
避免继承上一个试验的完成状态。每轮服务端 wait 15/15，59 次 wait 调用均无错误。
额外 rebind EGLImage 的对照也通过，但已验证的路径不要求每次 rebind。

15 项含真实线性 / sRGB 编码、clear、两种方向的写入可见性、目的颜色线性化后的 blend、
sRGB 采样解码、切换、颜色写掩码，以及 32 次相互依赖的连续混合写入。
这条连续写入链中没有中间像素回读、`glFinish` 或 `eglClientWaitSyncKHR`；
最后才回读一个像素核对数值。纹理内容没有经 CPU 复制。

`eglWaitSyncKHR` 是服务端等待，规范允许实现使用 GPU 硬件或服务端 CPU。
本轮证明的是命令和结果正确，不证明此 GPU 驱动内部等待完全由硬件执行，
也没有测量每次 fence / flush 的成本。不能把“没有显式客户端完成等待”写成零同步开销。

## 普通纹理：GPU 原始拷贝候选

NativeBuffer 试验只覆盖单层 2D RGBA8 backing，不能代表任意 mipmap、数组或 MSAA。
为普通 GL 纹理额外验证了双存储方案：RGBA8 与 SRGB8_ALPHA8 两张纹理，
用 `glCopyImageSubData` 同步原始字节，绘制和 blend 由相应格式的 GPU 附件处理。

每轮 **17/17**：上述颜色和依赖链验证，加 mip 1 写入与 mip 0 保留。
GPU 原始拷贝没有进行 gamma 转换；颜色转换发生在实际 sRGB 渲染或采样环节。
本探针为简单完整同步实现，每轮 108 次 GPU image copy，没有 CPU 纹理复制。
生产实现应按脏 subresource 和有效代次减少复制，不能照搬探针的每次切换全 mip 拷贝。
该方案需要第二份纹理存储，并产生 GPU 带宽与队列成本，尚未做游戏性能对照。

## 重复测试与恢复验证

最终诊断构建连续做三次完整退出并重启：PE64、PE32、PE64。
每轮资源实验本身都在原生 64 位 GLES 宿主执行；PE32 / PE64 指 Wine 启动程序，
不能据此说已经分别实现两个不同位宽的原生驱动。

| 路径 | 每轮检查 | 三轮结果 |
| --- | --- | --- |
| 普通 texture + raw GPU copy | 17 | 51/51 |
| NativeBuffer + EGL server wait | 15 | 45/45 |
| NativeBuffer + wait + rebind | 15 | 45/45 |
| NativeBuffer + Finish 对照 | 15 | 45/45 |
| 无同步 / Flush / memory barrier 负对照 | 各 15 | 每轮仍出现错误；不是通过项 |

每轮 PE UBO 探针另有 9 项通过，实际 legacy context 仍报 2.1。
最终诊断包 renderer SHA256：
`e07301ece5546a8bd3ea5331e9ca98a988690ec90e5d15c35e691e415f42debb`。
未签名诊断 HAP SHA256：
`9513ffc1a44b4c0155b74bd871d28d6bed0ccc4e5538fffac66ea4f50d5a8648`。

已恢复前一轮普通候选 `opengl-conditional-clean-signed-20261009.hap`，
renderer SHA256 回到
`165790221a950dda5b032e0c07873302a0780756686885d7dcea4e09fe032030`。
恢复后的 PE32 / PE64 条件渲染各 26、UBO 各 9、D3D9 颜色各 10，共 **90/90** 通过。
生产 UBO / conditional-render 主机测试及 diff-check 也通过。
本轮没有生成全产品干净 HAP 或上架 APP，没有提交 / push。

## 接入生产代码的具体方向

1. 在 `vrend_resource` 保存 backing、两种颜色视图、每个 subresource 的有效写入代次，
   以及尚未完成的 fence；view / surface 持有资源引用，避免提早释放 backing。
   NativeBuffer 路径先限定已验证的 RGBA8 单层 2D，BGRA / RGBX 必须另测。
2. `vrend_create_surface()` 按实际 surface format 选目标视图。
   对已经有真实 sRGB attachment 的路径不能再注入手动 gamma，
   也不能沿用“保持 sRGB attachment、靠 FRAMEBUFFER_SRGB 开关”的 native-control 分支。
3. 跨别名消费前同步有效代次。只在真正的写后读 / 写后写切换时插 fence，
   同视图连续 draw 不重复提交 wait。导入、上传、清屏、copy、blit 和 readback 都要更新状态。
4. 普通纹理以匹配 format 的双 GPU 存储接入，按 mip / layer 脏范围同步原始字节。
   同一 draw 同时采样另一颜色视图时也要同步，不能只在 framebuffer 切换时处理。
5. 数组 / cube / 3D、MSAA 与 resolve、MRT 混合颜色视图、scissor 与 write mask、
   跨 context、import / export 及销毁必须验证；所有公开支持的资源要有正确路径。
6. 上述实现完成并通过真实 guest `GL_FRAMEBUFFER_SRGB` draw / clear / blend / blit 回归后，
   再开放 VirGL `VIRGL_CAP_SRGB_WRITE_CONTROL` 所表达的目标 surface 控制。
   该 cap 目前只由 native feature 生成，须将“语义实现”与“宿主 native API”状态分开。
   Guest 的版本 / GLSL / 资源限制以及 CTS / Piglit 另行检查，不能仅按扩展表宣称 GL 3.0。

这给出了当前硬件上可实施的资源路线，不需要先换 Vulkan 1.3。
完整 GL 3.2 / 3.3 的 depth clamp / dual-source blend 仍是独立缺口，本轮没有解决。
Steam 弹窗与启动策略没有改动。

## 可复核代码和标准

- `scripts/probes/gles_srgb_alias_probe.c`：原生资源、同步、颜色和负对照实验。
- `scripts/probes/build_gles_srgb_alias_diagnostic.py`：独立诊断 builder。
  从当前 renderer 的编译参数生成实验对象和新 archive；不修改生产源码 / object / library。
- `docs/architecture/evidence/opengl-srgb-alias-20261009/`：三轮完整结果、恢复回归、包与构建身份、SHA256。

容器已有 native VirGL 构建后：

```sh
python3 scripts/probes/build_gles_srgb_alias_diagnostic.py
```

默认在 `/data/src/winehua/workspace_temp/srgb-alias-diagnostic-20261009/` 生成诊断 renderer。
它只用于单独实验包，不能用诊断包测游戏 FPS 或直接作为发布包。

标准：[EGL_KHR_wait_sync](https://registry.khronos.org/EGL/extensions/KHR/EGL_KHR_wait_sync.txt)、
[EGL_EXT_image_gl_colorspace](https://registry.khronos.org/EGL/extensions/EXT/EGL_EXT_image_gl_colorspace.txt)。
本次下载的标准文本副本也纳入证据哈希。
