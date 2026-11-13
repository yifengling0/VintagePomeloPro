# OpenGL 3.0 / 3.1 GPU 适配与真机验证（2026-10-09）

## 本轮结果

在 MatePad / Maleoon 910 上，Mesa VirGL 现已真实创建 OpenGL 3.0 与 3.1 context。
GLSL 1.30、1.40 的编译、链接、像素绘制通过。普通 context 返回 `3.1 Mesa 25.0.1`，
3.1 core context 返回 `3.1 (Core Profile)`，renderer 为 `virgl (Maleoon 910)`。
PE32、PE64 均通过，不使用 GL / GLSL 版本 override；FEX 运行方案保持原设置。
这是当前设备的专项验证结果，尚未跑完整 Khronos CTS / Piglit，不能等同认证或所有游戏兼容。

## 真正缺口与修复

前一轮宿主 GLES 3.2 没有 `GL_EXT_sRGB_write_control`，guest 因缺少 destination
surface sRGB control 停在 desktop GL 2.1。此前已经补齐真实 UBO 和条件渲染。
本轮在 guest Gallium 管理公开纹理身份和相反 colorspace 的 GPU shadow storage：

- 只有需要相反颜色 surface 时才分配 shadow，同一颜色 surface 连续 draw 合并脏状态。
- draw、clear、blit、采样、上传、读回、export、flush、context 销毁都维护有效性。
- 按 mip 同步所有 layers / depth；公开资源待同步时保留引用，清除 pending 后编码 copy，
  避免 command buffer 满时递归 flush 重入同一 pending 节点。
- CPU map 写入即使没有 staging，也必须让对应 shadow mip 失效；否则后续混合读旧内容。
- framebuffer 仅重新绑定时不把 shadow 当成 canonical 写入，避免无必要的反复同步。

初版真机暴露了关键错误：VirGL `resource_copy_region` 将 linear/sRGB pair 判为不兼容，
走 typed blit，关闭 sRGB 后仍得到编码后的 137/188/225，而正确值是 64/128/191。
PE32 初版还停在 MSAA 测试，没有完整结果。这版不能算通过。

renderer 现在只对 `resource_copy_region` 的普通同布局 linear/sRGB pair 开放 raw
`glCopyImageSubData`。它保留原始字节；一般 blit 仍做所需的颜色转换。
修改没有扩大 blit 的 raw-copy 条件，也没有打开虚假的宿主 native sRGB cap。
有 copy-image 能力且缺少 native control 的 host，guest 默认启用语义实现。

新增代码没有 CPU framebuffer 色彩转换、整帧回读或 CPU 纹理复制。
测试程序的 readback 仅用于检查像素，不属于游戏呈现路径。
双 GPU storage 会增加显存，并在颜色视图切换 / 公开资源消费 / 提交时增加 GPU copy，
因此不能称零复制或保证性能提升。

## 真机检查

| 检查 | PE32 | PE64 |
|---|---:|---:|
| GL 3.0 / 3.1 专项（每种 context 都测实际像素） | 163 / 163 | 163 / 163 |
| D3D9 颜色与显式 sRGB write / Present 后恢复 | 15 / 15 | 15 / 15 |
| 原条件渲染、12 个真实 UBO blocks、D3D9 相邻回归 | 45 / 45 | 45 / 45 |

共 446 项通过（包括 framebuffer 完整性与上下文建立检查，非 446 种扩展）。
覆盖启用 / 禁用 sRGB 的 clear、draw、采样、destination-linearized blend、
上传后混合、scissor、color mask、连续视图依赖、mipmap、混合 MRT、
array / cube / 3D layers、4x MSAA 与 resolve、共享 context、GLSL 1.40 UBO、
texture buffer、copy buffer / map、transform feedback、instancing、primitive restart。

现有 WGL capability probe 的 PE32 还验证了 3.2–4.6 context 请求被拒绝，
GL 3.1 之后版本所需的深度夹取 / dual-source blending 缺口没有被伪报为更高版本。

主机 UBO、条件渲染、socket 事务、guest build identity 检查通过。
Mesa 7 个补丁 / 18 个文件、renderer 4 个补丁 / 4 个文件，各从 HEAD 文件构造
干净源码 fixture 连续重放两遍，最终字节与生产源码一致。
这证明补丁可重放；不等同于完整产品从零构建。

## 游戏与性能

Chaos 通过原启动器进入主菜单与剧情，黑背景、文字、图像正常。
首段 30 秒 fresh-image 呈现平均 53.545 FPS，范围 50.72–55.94。
13 个 scene 样本均有 GL source，新 GL draws 持续增加，`uiUploadBytes` 增量为 0，
releaseErrors=0，保持 GPU 呈现。该 FPS 是新图像发布统计，不是 GPU 执行耗时。

本轮追加旧包对照结果见下方补录；此前 19 点的 59.03 FPS 不作为严格 A/B，
因为场景文字和温度不完全一致。

## 范围和仍需验证的点

- 验证设备是 Maleoon 910 / GLES 3.2 / Vulkan 1.2.53，没有借用 Vulkan 1.3 的能力。
- 窗口 sRGB pixel format 在当前 Wine driver 中无法由 `wglChoosePixelFormatARB` 选出，
  探针明确记录 optional unavailable；已验证的是纹理 / renderbuffer FBO，
  不把这个窗口路径记为通过。
- renderer raw sRGB pair 暂不扩大到“一侧 EGLImage、另一侧普通纹理”的资源，
  导入 NativeBuffer 的 BGRA / RGBX 颜色重解释、跨 backing copy 仍需专门整合验证。
- 还没有覆盖全部共享资源并发、所有 MSAA sample count、所有渲染格式与完整 CTS。
- 3.2 / 3.3 所需的 depth clamp / dual-source blend 仍是下一阶段，4.x 也未承诺。
- Steam 弹窗与启动策略不属于本轮修复内容。

## 可重建代码与候选包

- `patches/mesa/0007-virgl-gpu-srgb-surface-storage.patch`
- `patches/virglrenderer/0004-resource-copy-preserve-srgb-bits.patch`
- 两个 apply entrypoint 已加入对应补丁。
- `scripts/probes/wgl_srgb_surface_probe.c`
- `scripts/probes/d3d9_srgb_write_probe.c`

容器中沿用生产 guest 构建入口：

```sh
WINE_ARCH=aarch64 NATIVE_ARCH=arm64-v8a \
OHOS_SDK=/apps/harmony/sdk/default/openharmony \
bash scripts/build_ohos_guest_gfx.sh --platform wayland --mode virpipe
ninja -C build/native_arm64-v8a/virglrenderer \
  src/libvirglrenderer.so.1.11.0 vtest/libwinehua_vtest_server.so
```

正式 native 构建脚本 `build_native.sh` 会运行 renderer patch entrypoint。
候选 HAP 基于前轮普通包，只替换 guest Gallium、renderer / vtest 以及成对的 runtime
payload / manifest / content marker。不是全产品 clean assembleHap，也不是上架 APP。
已校验 native / payload Gallium 一致、payload manifest SHA256 匹配、ZIP 没有重复成员。
代码、补丁和证据保留本地，尚未提交 / push。

| 产物 | SHA256 |
|---|---|
| guest Gallium | `a5ce84e4a41f4ecaebca10230c4ea4e665ffb6ed4f9626674c4f12e682f01aa0` |
| host renderer | `5e77a30ce67bcf99af68862d5a0e222dbc2d6214e3c04e578f18b752bc7e8c8d` |
| 未签名 HAP | `a0c6116f96aa3e97c28f89456e806d445f3dbbc357012ea2ebe5591ccdc491d4` |
| 签名真机 HAP | `2cc5348cef113e604e9f5a040d968b7b25f941d4450a4a3c57d5291a1cd3d73d` |

产物目录：`F:/VintagePomelo-Workspace/workspace_temp/gumu10-dx11-20261008/`。
候选前缀：`opengl31-srgb-surface-v2-`，日期后缀 `20261009`。
证据：`docs/architecture/evidence/opengl31-srgb-surface-20261009/`。

标准语义参考：
[glCopyImageSubData](https://registry.khronos.org/OpenGL-Refpages/gl4/html/glCopyImageSubData.xhtml)、
[glBlitFramebuffer](https://registry.khronos.org/OpenGL-Refpages/gl4/html/glBlitFramebuffer.xhtml)。

## 最终补录：旧包对照与设备恢复

旧包 `opengl-conditional-clean-signed-20261009.hap` 的 30 秒采样为
21:28:00–21:28:30，平均 41.322 FPS（33.90–48.89）。它的截图已经进入
“背景大楼＋四行文字”，新候选采样停在“黑底＋三行文字”，不是同一场景。
采样前旧包 system_h / shell_frame 为 45.750°C / 43.218°C，
新候选为 43.125°C / 40.454°C。场景与热状态均未对齐，这次 A/B 无效；
不能以 53.545 与 41.322 的差值判断性能提升，也不能用此前 59 FPS 判断回退。

已结束旧包会话，重新覆盖安装 v2 签名候选；安装输出明确为
`install bundle successfully`。`bm dump` 确认 1.4.5.29 / 1004038。
重新启动后的 device Gallium SHA256 与候选完全一致，64 位 GL 3.0 / 3.1 专项
再次完成 163 项、0 失败，返回真实 VirGL / Maleoon 910 context。
这次重复运行是恢复安装的确认，不另外加入上面的 446 项总数。

HDC 普通 shell 和 bundle-aware shell 均无权直接读取包内 renderer / vtest。
签名 HAP 中 renderer 的所有别名与预期 SHA256 一致；native 库的身份依据
为已核验的签名 HAP 与成功覆盖安装，不能称直接取得了设备 native 库哈希。
第一次尝试通过 Wine 复制 native 库时，USB HDC 报 Device not found；
该次 stop / launch 均未成功，不作为游戏或库身份验证结果。
之后按用户提供的地址连接无线 HDC，确认设备型号 MLR-AL10。
无线第一次启动被锁屏拒绝；后续检查结果按实际输出补录。
没有再次切回旧包。恢复细节见 `opengl31-final-restored-device-identity.json`。

证据目录增加主机构建 / identity 检查输出、相邻回归原始结果、无效旧包对照截图 / 热状态、
最终恢复记录与本轮 device ledger 子集。`SHA256SUMS` 包含全部证据文件（不含自身）。
候选包用于继续游戏兼容性测试，尚未完成全量 GL 一致性测试或全产品发行构建。
