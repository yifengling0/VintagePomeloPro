# Wine 开始菜单压缩：SHM viewport 采样修复（2026-10-10）

Wine 桌面开始菜单和 Programs 子菜单的压缩来自 GPU 桌面合成遗漏 `wp_viewport.set_source`。已修复并覆盖安装到平板；Vulkan 桌面以及带两个真实 GL producer 的 EGL 桌面，菜单尺寸、子菜单和“运行”点击均验证正常。Steam 的整体窗口匹配与低帧率尚没有修复后实机结论。

基于 `feature/main_proton` / `f47090663537b1e70ad1ebe2f76bd2c34cfbb6af` 的现有工作树，包含此前透明度及兼容性修复。本轮未提交或推送。

## 根因

Win32 枚举记录菜单 `class=#32768 window=0,702 103x78 client=103x78`。同一现场 Wayland 日志记录：

```text
[MW-VP] set_source ... src=(0,0 103x78)
[MW-SUBSURF] stored layer 128x128 at (0,702) parent=#2
```

103×78 是有效内容；128×128 是包含填充的 SHM 存储。旧 GPU scene 只设置显示宽高，没有携带 source 采样范围，因此整个 128×128 缓冲被缩到菜单目标尺寸，文字和行距一起压缩。生产 compositor 的旧实现通过 `--viewport-baseline` 已复现断言失败。

正确处理为保存完整缓冲，只采样有效的 source，再映射到逻辑 destination。该实例 GPU UV 横向范围为 `103/128`，纵向范围为 `78/128`；不是改变 Wine DPI 或强行放大菜单。

## 实现与性能范围

- `compositor_layer.h`：SHM 子面保存 committed viewport，`ComputeSubsurfaceViewport()` 共用现有 viewport 转换规则。
- `wl_core.cpp`：桌面和 inline 子面提交时携带 source、buffer scale、transform。此处不裁切或新增复制像素。
- `desktop_compositor.cpp`：显示与输入使用同一逻辑宽高；GPU snapshot 携带对应 UV sampling，源缓冲尺寸保持不变。
- `shader_utils.cpp` / `egl_renderer.cpp`：SHM shader 使用采样坐标，普通主帧显式恢复 identity，避免继承前一菜单的 crop。Vulkan compositor 已支持同一 sampling 数据。

viewport 改变但像素 serial 不变时，重绘复用旧像素快照和上传缓存；case 12 验证非零 source offset 与缓存复用。这不是恢复游戏每帧 CPU 回读。SHM UI 原有上传仍存在，不能宣称整个桌面没有任何 CPU 上传。

旧 CPU `FrameBlitter::BlitSubsurface` 未在本轮完整改造任意 source/scale/transform；本轮验证和修复范围是 GPU 桌面 SHM 采样及共用命中尺寸。

## 验证

生产 compositor/输入回归 case 0–12 通过。新增 case 12 覆盖 128×128 存储、103×78 菜单、边界命中、非零 source offset 和 viewport-only 重绘；替换为旧生产 compositor 时，`padded menu buffer must be cropped before scaling` 断言失败。OHOS native 编译和链接通过。

平板实测分为两条路径：

1. 产品默认 Vulkan 桌面：打开开始菜单、Programs→Steam 子菜单，仅展开未启动 Steam；点击“运行”打开对话框。
2. `isolate-product-egl-desktop` 会话：先检查普通桌面，再运行两个持续换色的真实 WGL 窗口。日志确认 NativeImage 消费者持续出帧，截图同时包含两个 GL 窗口与正常菜单/子菜单/运行对话框，覆盖新增 SHM shader 的混合 GPU scene 路径。仅 SHM 桌面会走原 CPU 合成，不能单凭这种截图证明新 shader 生效。

WGL 探针报告 `3.1 Mesa 25.0.1 (git-330124bf18)` / `virgl (Maleoon 910)`。该轻量探针宿主呈现约 58–62/s，多段 `upload_bytes=0`，`failed_swaps=0`；打开菜单时出现 SHM UI 上传。这里是正确性与路径验证，没有游戏性能 A/B，也没有物理手机验证。

验证后已结束探针，并冷启动回产品默认桌面；日志再次出现 `DirectVkDesktop`，EGL lab 设置不留在当前会话。静态桌面显示 0 FPS 是无新内容时按需呈现，不能据此判断游戏卡死。

## 安装包与证据

| 本轮包 | SHA256 |
| --- | --- |
| `menu-viewport-20261010-signed.hap` | 9bd303e616e1fea50e5a45924960c4ae9680df85cc1e70a4cec2318abe2627ce |
| unsigned HAP | c10ae0f6f87b9dc6778677d69a567a2ab186b5e8ee80527cd2d4f6e2346b5343 |
| 包内 libentry.so | 1d77555cec71e78f2363e530bd950eef130205fc0ba373bbd259684864ab4586 |

对比基包 `sprite-window-opacity-20261010-unsigned.hap` 的 ZIP member，唯一改变是 `libs/arm64-v8a/libentry.so`；Wine/FEX/Mesa/runtime zip 未变。设备覆盖安装成功。本地包和构建库哈希已核验；shell 无权读设备 native ELF，因此不声称已从设备独立读取库哈希。

本轮包同时纳入 [普通原生 GL 窗口透明度规则收口](native-gl-window-opacity-20261010.md)，没有重新修改游戏纹理透明度。FEX、OpenGL 3.1、Direct 自动选择、低地址共享映射、减少复制和显示节拍都保留。

截图、协议片段、Win32 尺寸、EGL 路径日志、构建日志及源码哈希见 [证据目录](evidence/shm-viewport-menu-geometry-20261010/)，附 `SHA256SUMS`。HAP 与大日志仍留在 workspace_temp。

## Steam 后续检查边界

Steam 的 SHM 菜单若使用带填充缓冲，可受益于本次修复；其 GL/CEF 原生子面走独立 NativeBuffer 采样和几何路径，不能直接推论全部窗口已修好。后续应对齐实际 `set_source` / destination、已消费 NativeBuffer 尺寸、producer 帧流和点击命中，再决定是否修改原生层采样。

此前截图同时出现网络错误 -118、静态 0 FPS 与大小不同的 Steam 窗口。这些现象不能合并为一个 GPU 性能根因。Steam 帧率需要有持续动画或交互的同期样本；本轮没有主动启动 Steam，未完成该项性能验证。
