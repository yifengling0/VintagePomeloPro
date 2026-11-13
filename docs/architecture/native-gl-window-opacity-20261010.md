# 普通 Wine GL 窗口透明度：桌面合成修复及验证（2026-10-10）

《东方虹龙洞》子弹周围的彩色底块、大片白色和过亮画面，来自 GPU 桌面合成误用游戏 framebuffer alpha。普通 Wine WGL 窗口应当不透明，游戏内部透明混合不代表这个窗口对桌面透明。修复后实际战斗画面恢复，用户确认“这回对了”。

基于 `feature/main_proton` / `f47090663537b1e70ad1ebe2f76bd2c34cfbb6af` 的现有工作树。本轮修改桌面合成库；Wine/FEX/guest Mesa/runtime zip 与前一候选逐字节相同。没有降级 OpenGL、停用 GPU 或恢复每帧 CPU 回读。

## 根因和证据

该游戏实际路径为 32 位 FEX → D3D9 → WineD3D → OpenGL/VirGL → OHOS NativeBuffer → GPU 桌面合成。这里宿主使用 Vulkan 合成器，不表示该 D3D9 游戏改走 DXVK/Vulkan Direct 渲染。

原 EGL 消费者在 `SnapshotZeroCopyScene()` 中用 `consumer->vulkanSource` 决定 opacity，GL 被视为透明；新增 Vulkan 桌面消费者也把 GL layer 设置为 `opaque=false`。游戏已经完成 D3D9 的透明和独立 alpha 混合，最终 RGBA framebuffer 的 alpha 可以为零或部分透明。宿主再将它作为桌面窗口透明度与底层 SHM 混合，破坏最终 RGB，留下背景色块和发白。

上游 `thirdparty/wine-valve/dlls/winewayland.drv/opengl.c` 为普通窗口请求 `EGL_PRESENT_OPAQUE_EXT=EGL_TRUE`。OHOS pbuffer/NativeBuffer 桥接应保留这一语义，不应根据 framebuffer 有 alpha 通道或 producer 为 GL 就把窗口视作透明。

定位过程还区分了两个阶段：`scripts/probes/d3d9_sprite_color_probe.c` 的真实 GPU 像素运算在 A8R8G8B8 与 X8R8G8B8 backbuffer 上均为 2160/2160，而屏幕仍显示错误。探针读取的是桌面合成前的 render target，因此只能证明这组游戏渲染像素正确；不能证明最终呈现正确。这一差别将调查指向宿主桌面合成。试过 OpenGL 2.1 会话对照，错误依然存在，降版本不是该问题的修复。

## 修改范围

最终源码集中使用 `MakeNativeWindowLayer()`：

- `entry/src/main/cpp/compositor/frame/gpu_desktop_scene.h`：构造普通原生 Wine 窗口图层，保留完整身份、父子关系、几何和源尺寸，明确 `opaque=true`。
- `entry/src/main/cpp/graphics/egl_renderer.cpp`：EGL 桌面消费者使用共用构造规则。
- `entry/src/main/cpp/direct/direct_vulkan_desktop_compositor.cpp`：Vulkan 桌面消费者使用同一规则，继续保留 GL 源的纵向采样变换。

layered/shaped SHM 窗口继续走原有透明处理，ARGB 菜单没有被全局强制不透明。此修复处理窗口呈现契约，不修改游戏的纹理 alpha、混合参数或 gamma。未来若加入显式支持透明原生窗口的路径，应携带窗口透明语义；不能再靠 GL/Vulkan 类型或 RGBA 格式猜测。

## 包身份和实机结果

设备当前验证的是两处直接设置 `opaque=true` 的候选；用户已确认画面正确。之后源码收口为共用构造函数，行为等价且 native 编译通过，但最终收口版本尚未重新打包或安装。

| 已验证候选 | SHA256 |
| --- | --- |
| signed HAP | 843771b887d019ff711e238f5b36a2b5870912a93cbbdf17a3de1c1b3172fbbc |
| unsigned HAP | 6051d4fe6e1bf8ec2f8a577999b68b7d6897076cb7ffba0af5f7a7f3d6260e61 |
| 包内 libentry.so | 37238c80875747c5fa33c38999b19ccc7d18ad864bfc8d6eed0b571f3aff1904 |

候选为 `sprite-window-opacity-20261010-signed.hap`，基包为 `alpha-child-path-fixed-20261010-unsigned.hap`。本轮记录时再次对照 ZIP member 内容，唯一变化是 `libs/arm64-v8a/libentry.so`。这些是本地包内身份核验；设备 shell 无权限读取 native ELF，不声称已直接核实设备库哈希。最终收口编译产物 SHA256 单独记录在 evidence/validation.json，不覆盖已验证候选的身份。

截图与同期日志见 [证据目录](evidence/native-gl-window-opacity-20261010/)：

- `th18-user-current.png`：修复前白块、彩色底块现场。
- `th18-opacity-title.png`：修复后标题颜色及 Logo 阴影。
- `th18-opacity-demo-1.png`：修复后实际战斗演示，白块消失；用户确认“这回对了”。

同期日志记录 `glSources=1`、`releaseErrors=0`、`directGameCpuReadBytes=0`、`directGameCpuUploadBytes=0`；Wine GL zero-copy present 从 11880 持续增长到 14640，而 `readbacks=2` 保持不变。这两次启动期 fallback 没有变成逐帧回读。SHM UI 的上传仍存在，不能将游戏 GPU 帧零回读扩大为整个桌面零 CPU 上传。

宿主约 79–110 presents/s、演示界面内部有 60 标记；宿主呈现统计不等于游戏模拟 FPS。本轮没有匹配场景、温度的性能 A/B，只能确认正确性恢复和 GPU 呈现路径保留，不能宣称严格帧率增益。

## 回归和保留项

`host_tests/gpu_scene_input_test.cpp` case 11 经过生产 `MakeNativeWindowLayer()` 和真实 scene snapshot，验证普通原生窗口不透明、ARGB SHM menu 保留透明、opacity 变化会触发重绘。`gpu_scene_input_test.py` case 0–11 全通过；最终源码 native 编译和链接成功，编译日志归档。Wine legacy alpha-test 和 Broker argv 相邻回归也已通过。

保留此前两个独立修复：`0053-wined3d-restore-legacy-alpha-test-state.patch` 修复 legacy GL alpha-test 状态遗漏；`0054-ntdll-broker-preserve-resolved-image-path.patch` 修复汉化启动器子进程完整路径丢失。前者解决合成前的透明纹理问题，本次修复解决合成后的窗口透明度错误，不能相互替代。原报告见 [透明纹理与汉化启动器](alpha-test-and-resolved-child-image-20261010.md)。

FEX 默认、OpenGL 3.1、Vulkan Direct 自动选择、共享低地址映射、减少复制和显示节拍都保留。本轮结果可修复经过相同错误桌面合成路径的普通 Wine GL 窗口；没有证明 PAL2 战斗缺层、《灰色的果实》的特定缺图或全部游戏兼容性已解决。当前成功现场不再做整包降级或额外渲染修改。

本轮记录和收口没有停止用户游戏、启动 Steam、commit 或 push。HAP 与大日志留在 workspace_temp，仓库证据只含截图、小日志、TSV、构建日志和身份信息，附 SHA256SUMS。

## 后续安装：菜单 viewport 修复包

2026-10-10 后续 `menu-viewport-20261010-signed.hap` 已包含最终 `MakeNativeWindowLayer()` 收口源码并覆盖安装。开始菜单分别经过 Vulkan 和混合 GL/EGL 桌面验证；旧候选及其《东方虹龙洞》验证身份仍按上表保留，不替换为本轮包哈希，也不把菜单测试当成游戏性能对照。包身份和范围见 [SHM viewport 菜单修复](shm-viewport-menu-geometry-20261010.md)。
