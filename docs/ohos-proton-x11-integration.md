# OHOS Proton X11 实验集成

状态：源码已集成，主机回归、ARM64 编译及构建输出闭包检查通过；Edge 已完成部分设备验证，图形与输入全量验收仍在进行。**未合回主线，不视为发行资格、性能证明或干净 CI 重建证明。**

## 基线与来源

- 原分支：`main_proton_steamarm64`。
- Edge 基线：`79bfb6cfc6685f1aeb3cb7ee1bfea941be90708c`，包含虚拟桌面 × 后恢复、最小化恢复、首页遮挡前置、固定会话宿主与重复启动处理。
- 实验分支：`feature/ohos-proton-x11-integration`，从上述基线建立。
- 上游：WineHua `feature/proton-x11@22c46eb1255e7fdacc1f2a1a342b81f727b5ef18` 与 `master@a001e6d0ca277d5a5fb2b4bd6a030ac52e5e132e`。
- 按功能重放，没有整分支 merge，也没有切换到上游整套 Wine 二进制。

## 采用与保留矩阵

| 范围 | 本地处理 |
| --- | --- |
| winex11 → Xwayland → wlroots → NativeWindow | 移植显示模块、子进程宿主及必要的输入、窗口和构建接口 |
| Wine 驱动 | 基于现有 Wine gitlink，以受管理的 0072～0076 overlay 接入；桌面和顶层子窗口使用显式 role |
| X11 退格（8fca96bb）、IME 重聚焦（fc21897e） | 接入现有输入 seam；GNU 与 Wine 焦点分别路由 |
| 二次启动 EGL（6f0deec2）、停机清路线（9eba2d43） | 先释放最后 EGL 使用者，再废弃单例并允许下次启动 |
| Steam/GNU 窗口 | 原有 API 内按宿主后端分发；检查 source PID、wl_surface key、XID 和 generation，防旧帧串窗 |
| 本地 UI | 保留 Deck、经典首页、账号会员、游戏库和窗口外观；只增加“窗口后端”实验选项 |
| 音频/手柄 | 保留本地较新的混音、canonical controller 与 GNU 接入，未整体替换上游版本 |
| 兼容性修复 | 保留 PR78、VirGL RGB/模型、字体、GStreamer、网络、DXVK/Venus 及现有 managed overlays |
| 上游 libz 不打包策略 | 不采用；保留本地真实 zlib resolver 和 SDK stub 检查 |
| 上游 GUEST_ARCH 别名 | 只采用 GUEST_TARGET，GUEST_ARCH 保留本地 aarch64/x86_64 约定，防 Linux 图形构建回退 |
| 上游临时诊断 | 默认关闭 GPU 帧采样、像素落盘；Xwayland/XIM 日志各限 4 MiB；不记录输入文本字节 |

## 会话、窗口与图形约束

- 默认仍为 Wayland；设置持久化，下一次完整 Wine 会话启动时生效。仍有 Wine 进程或启动任务时拒绝换路线；拒绝不会结束正在运行的会话。
- X11 显示链具备阶段失败、停止请求及 120 秒启动期限。原有 Wine/前缀准备总保护仍为 210 秒，不将其误报为整个流程 120 秒。
- NCP 与手机 fork 后端共用启动逻辑。传给子进程的是私有 FD 副本，wlroots 持有的原始 FD 不转移所有权。
- GNU 运行任务持有共享 Wayland lease；退出 OHOS Wine 不全局杀掉 GNU、Xwayland、XIM 或 VirGL 独立运行时。
- 跨后端呈现传 NativeBuffer 引用、目标几何与来源身份，通过 EGLImage 接入；渲染器和消费者在合成器线程创建/释放。GPU 正常路径不启用 CPU 像素回读/复制。
- 粘贴板通过有界 UTF-8 选择传输，接收 1 MiB/1 秒上限，发送最多 4 个 writer；ArkTS 侧按会话 epoch 丢弃旧回调，系统写入串行处理。
- 未完成实机验收的项目包括：虚拟桌面内各子窗口控制、相对鼠标锁定、系统 IME/剪贴板权限、跨后端 GPU 队列与停止/恢复时序。源码和主机测试不能替代这些验收。

## 依赖与重建

固定 gitlink 与 overlay 顺序、SHA-256 在 `docs/graphics/graphics-stack.lock.yaml`。

| 组件 | 固定版本 |
| --- | --- |
| Wayland / wayland-protocols | 1.26.0 / 1.49 |
| libxkbcommon | 1.13.2 |
| wlroots / Xwayland | 0.20.2 / 24.1.13 |
| libX11 / libXext | 1.8.10 / 1.3.6 |
| Xrender / Xfixes / Xi / Xrandr | 0.9.12 / 6.0.2 / 1.8.2 / 1.5.4 |
| Xinerama / Xcursor / Xcomposite / Xxf86vm | 1.1.5 / 1.2.3 / 0.4.6 / 1.1.6 |

OHOS host 构建进入 `build/host-ext/<NATIVE_ARCH>`，Wine 侧进入 `build/sysroot-ext`；GNU 运行库仍由原有独立脚本构建，不能混用 glibc 与 OHOS ELF。

```sh
make display-route NATIVE_ARCH=arm64-v8a
bash scripts/build_deps.sh
# 为源码/配方身份改变隔离 Wine；共享依赖缓存不复制。
WINE_BUILD_ROOT="$PWD/build/wine-x11-runtime" JOBS=8 bash scripts/build_wine.sh
WINE_BUILD_ROOT="$PWD/build/wine-x11-runtime" bash scripts/build_wine.sh --check-cached-identity
```

`WINE_BUILD_ROOT` 默认等于 `BUILD_DIR`。需要从独立 Wine 树组装时必须显式使用相同的根：`WINE_BUILD_ROOT=... bash scripts/assemble.sh`；组装前校验完整构建身份，拒绝拿 targeted module 缓存或旧 Wine 目录充数。正常源码交付仍需按现有 CI 的编译前干净源码清单及 `source-build-candidate` 流程，不能将编译检查 HAP 作为候选包。

`verify_x11_wine_config.py` 强制启用 XInput2、RandR、Render、Cursor、Fixes、Xinerama。`verify_x11_runtime.py` 检查 ELF 传递依赖、架构、SDK stub、GNU ABI 混入、PE 驱动、XKB/XKM 和 locale 数据。`assemble.sh` 可以暂缓 Hvigor 模块检查；最终 HAP 不能暂缓。

## 已完成验证

- 完整 OHOS ARM64 Wine 与 wineserver；X11 所需扩展宏/SONAME 均启用。
- wlroots/Xwayland、host/guest X11 库与完整交叉依赖构建。
- ARM64 `entry`、`xwayland_child`、`xim_server_child`、`display_route_napi`。
- ArkTS/Hvigor 编译检查成功。其 rawfile 不代表本轮完整 source-build-candidate，未交付或安装该检查包。
- 实际构建输出闭包：40 个 ELF、ARM64/i386 两个 PE 驱动及真实 XKB/XKM/locale；记录 `build/x11-output-closure.json`。
- 19 个 production Wine startup cases、194 个真实进程生命周期 checks、完整 model suite。
- X11 宿主实际代码 fakes：身份/代次复用、延迟销毁、鼠标 capture 释放、细粒度滚动、buffer 引用平衡。
- 剪贴板 epoch/串行写入、真实 FD 所有权、日志限额/关闭时限、managed overlays 重放与不覆盖未知源码。
- Wine 全 overlay clean replay，图形 lock 与有效源码检查、Wine 构建身份守卫。

## Edge 图形调试记录（2026-11-02）

- Wayland：GDI、虚拟桌面 × 后恢复、最小化与首页遮挡后恢复已通过，进程和会话保留。
- X11：Xwayland/XIM 实际起链，GDI、大小写/退格/回车、虚拟桌面 × 后恢复、内部标题拖动已通过。中文 IME、双指滚动、PC 多窗全量场景仍待验收。
- X11 不支持原宿主 Direct Vulkan 的 Xlib WSI，按显示后端选择 guest Venus，保留原 Direct 能力探测和 Wayland 路线。
- 补齐 guest buffer 的 GPU release fence，归还前导出合成器完成点；扩展失败才同步等待，正常路径不做 CPU 像素复制。
- D3D11 + DXVK 1.10 的 NativeBuffer 实验对照：修正呈现节拍将短时约 102 FPS 提升到约 118～119 FPS，但修正前仍随运行时间逐步下降。进一步查明三类自定义 `wlr_buffer` producer 析构都漏调 `wlr_buffer_finish()`，导致 renderer addon/EGL 导入资源没有按生命周期回收。补齐后保持底层 NativeBuffer 有效直到 addon 清理结束。
- 仅资源回收改动的同策略设备对照：旧包累计帧率降至约 50 FPS、最近区间约 25 FPS，RequestBuffer 平均等待超过 15 ms；新包连续约 289 秒、34080 帧后累计约 118 FPS，RequestBuffer 平均等待约 0.46 ms。归还计数匹配、无旧帧串窗或未同步上屏归还。这只证明本场景持续掉帧已改善，不代表其他后端/游戏性能结论。
- 用户仍观察到闪屏。发现 acquire 返回的生产者 fence 被直接关闭，新增 EGL native fence 的 GPU wait；导入失败最多等待 50 ms，失败保持上一帧并将原 fence 归还队列。日志确认 GPU wait 已生效，但用户复测仍明显闪烁，不能将该项宣称为闪屏根治。
- 单变量对照：标准 FIFO 仍明显闪烁；改为 CPU shadow upload（仅诊断，画面仍由 GPU 呈现）后用户观察到频率降低但未消失，排除“只由 NativeBuffer 实验呈现引起”的判断。
- 分离 GL 错误归属后，Edge 在最终画布 `draw` 阶段报告 `GL_OUT_OF_MEMORY (0x505)`，不是此前日志所称的 EGLImage 导入失败。系统同一阶段返回 `NATIVE_ERROR_NO_BUFFER (40601000)`；不能仅据 `EGL_BAD_ALLOC` 推断实体内存耗尽。画布改用两个刷新周期、最多 50 ms 的有界队列等待，其他显式 RequestBuffer 调用保持非阻塞；用户确认该改动明显减少闪烁但仍有整个客户区黑／白闪动；同场景持续约 118 FPS，最终 EGL 绘制错误在后续有界日志中未再出现。剩余闪动继续调查，不能宣称已根治。
- 拖动时保留画面不跟随：子窗 GPU 帧节点原来只在新帧到达时改几何，游戏进入 Wine 模态移动/缩放循环暂停出帧后就滞留旧位置。现改为每个合成周期依据 XWM 缓存更新客户区几何，保留 generation 校验；已增加零新帧移动/缩放、子区域及 XID 复用测试。Edge 拖动中截图显示保留帧与内部窗口同行，松开后仍约 117 FPS；用户确认拖动改善，但仍有跟随延迟感及轻闪，未标记完整通过。
- “整幅场景重绘”单变量对照仍闪，已移除设备诊断旗标，不能以全屏持续重绘作为修复。用户补充：全屏时客户区稳定而标题栏仍闪；窗口化时两者都轻闪。
- 对齐现有 Direct/GLES 路径的 NativeBuffer 导入契约：三处 X11 导入显式要求 `EGL_IMAGE_PRESERVED_KHR=EGL_TRUE`，避免依赖允许丢失既有内容的默认行为。主机测试验证真实 helper 的入参及失败返回；设备能重新渲染约 118 FPS，但用户随后反馈画面冻结、窗口不能移动；帧计数证实停止，不能以“不再闪”认定通过。不能认定为闪屏根因。
- Edge 23:27:46 关闭后重开的崩溃已经取得系统 FaultLogger 栈：主应用合成线程 `wl_signal_emit_mutable → wlr_scene_node_destroy → scene_surface_handle_surface_destroy` 跳转至空地址。`ClientAssociate` 注册 `scene_node_destroy` 时遗漏 `.notify = ClientSceneNodeDestroy`；本地缓存的上游 `master@a001e6d0` 也包含该缺口。本轮补齐回调，并用真实 Wayland signals 跑 1000 次 associate/destroy、重复 associate、先 dissociate 及创建失败回归。新版设备上虚拟桌面关闭恢复、完整引擎停止再起、强制退出测试进程均未再触发空回调崩溃，主应用 PID 保持。当时仍存在独立的设备释放等待；后续已定位并修复 XIM 四字节断开包，见下文。
- 同次生命周期评审补齐：按 retained frame 的 generation 回收，而非依赖 X 窗口仍在册；OH_NativeImage 必须晚于旧场景帧释放。PC 多窗增加同口径纹理/帧清理，防止旧队列先于持帧者销毁，同时不误清 XID 复用后的新窗。此项是额外发现的代码风险，与上述已证实的空回调崩溃原因分开记录。
- 初期 D3D9 + WineD3D 失败于 GL context 创建，白屏计数没有算入性能；后续已修复 EGL/GLX visual 混用，并通过真实动态立方体验证，见下文。D3D11 重建停帧后续已定位为 XIM 断开死锁。
- 增加真实固定 wlroots buffer/addon 代码的 3000 次生命周期回归，以及 GPU acquire/release fence、FD 所有权、上下文恢复和有界失败测试；CI 声明对应的主机开发包。

设备包为完整配对 Wine/媒体/图形运行库的本地诊断包，带精确 ELF 身份清单；未被声明为干净 CI 或发行候选。

## 后续统一验收与合回条件

本轮仅操作用户授权的 MatePad Edge，不操作普通平板或手机。下列项目保留未完成状态，不能由部分通过结果替代。

1. 从上述源码提交按干净清单组装完整候选，验证最终 HAP 闭包与精确运行库身份，再签名。
2. Edge：经典/Deck 下虚拟桌面 × 后恢复、最小化、首页遮挡、连续启动、PC 多窗口；旧默认 Wayland 与 X11 分别跑。
3. 中文、大小写、退格、滚动、相对鼠标、拖拽缩放、菜单、模态、全屏与窗口恢复。
4. D3D9+VirGL、D3D11+DXVK 1.10/2.6，字体/颜色/模型/视频及声音/手柄对照。
5. 首页与 Steam 手动启动、普通/大屏往返、退出返回及宿主重建；GNU Proton 与 Linux 原生游戏的启动/音频/存档。
6. 连续启停、异常恢复、性能对照、全新 CI 重建后，再考虑合回 `main_proton_steamarm64`。

以上待验收项没有被主机测试或旧设备证据标记为通过。

### 2026-11-03 退出停帧补充

- 用户不再参与夜间反馈，后续通过有界自动化、动态帧计数和截图判定，冻结画面不算视觉通过。
- 12 秒固定时长 D3D11+DXVK legacy 测试渲染 1380 帧；真实 DLL 路径为 legacy/arm64x。结果 JSON 在资源释放之前生成，只有渲染 PASS 不能证明程序已正常退出。
- 逐阶段释放日志显示：ClearState、纹理/视图、交换链和 context 的 Release 均返回，随后停在 ID3D11Device::Release；与此前按 2 重建 D3D11 的停帧具有相同资源释放入口。具体内部等待仍在定位，不能直接归因 Vulkan fence。
- 强制结束挂住的测试程序后 bindings=0、scene_frames=0、release 计数配平；主应用存活。外部进程的 GetThreadContext 可用，但跨进程 ReadProcessMemory 受 OHOS 限制，不能据不完整栈下结论。


### 2026-11-03 XIM shutdown deadlock

Finite DX11 rendering reached 1,146 frames, then hung in device release. A
bounded in-process thread snapshot located the exiting render thread in
`user32!DLL_THREAD_DETACH -> win32u!NtUserThreadDetach`. Stage traces narrowed
this to `winex11!X11DRV_ThreadDetach -> XCloseIM`; USER/window cleanup completed.
The server receiver incorrectly rejected a valid zero-body `XIM_DISCONNECT`
(total packet size 4), so libX11 waited forever for `XIM_DISCONNECT_REPLY` while
Wine held its loader lock. Other exiting threads and device destruction then
waited behind it. Accepting and consuming the four-byte header fixes this
protocol error without disabling XIM or skipping thread cleanup.

`host_tests/xim_transport_test.py` executes the production receiver against
CLOSE/DISCONNECT wire sequences, CM padding, split/coalesced packets and both
byte orders (2,000 close cycles). The previous receiver fails this test;
the corrected receiver passes. Edge finite-run exit, reopen and in-window device recreation subsequently passed; see the device evidence below.

The smoke cube now writes final PASS only after renderer teardown returns; a
completed render loop can no longer falsely report successful lifecycle exit.
The complete managed Wine overlay replay also matches the effective source.


### 2026-11-03 后续修复与设备证据

- **XIM 死锁**：`RxFeed` 接受并消费合法的四字节零 body `XIM_DISCONNECT`，回送 reply；未关闭输入法或省略 Wine thread detach。受管理 Wine `0075` 仅提供默认关闭的 TRACE 阶段诊断。DX11 多轮 180 秒测试正常完成 teardown、退出码 0，按 `2` 重建后仍继续动态更新、拖动及退出。
- **D3D9 初始化**：EGL config 不能用作 GLX pixel-format 编号。无 GLX 构建中，`x11drv_client_surface_create` 选 default X visual (0)，真实 EGL pbuffer 仍使用原请求的 format；纳入 `0076-x11-egl-default-visual.patch`。D3D9+WineD3D/VirGL 30 秒 3443 帧，后端来回切换后的 20 秒 2315 帧，均为动态立方体、正常 teardown/exit0。此前白屏数据不计入。
- **NCP 回调遗漏**：一轮停止约 14 个 native children 时漏收 2 个退出回调；不以 kill 成功或 `/proc` 不可读判退出。可选 `pidfd_open` 共享 RAII lifetime handle，只有 poll 得到内核退出事件才以原 generation 收口；不抢 fork/waitpid 的状态，晚回调补真实 exit code。194 项生命周期回归通过；Edge 支持 pidfd，两轮 native drain 约 100ms，替代原来的 35 秒超时。
- **首页遮挡恢复**：PC 系统可能仍报 DesktopAbility 为 FOREGROUND，普通“Wine 桌面”卡片仍必须显式 resume/raise。保留唯一宿主、Wine explorer 与游戏进程，不用重启伪装恢复。Edge 首页覆盖后点卡片可恢复，DesktopAbility 数量为 1。
- **普通产品呈现策略**：不带 NativeBuffer LAB 覆盖的 DX11+DXVK legacy 180 秒 21037 帧，正常退出；最大化后后续区间约 119 FPS。30 张固定坐标截图为 30 个不同立方体帧，未采到整块背景黑/白。采样不能证明人眼瞬态闪烁为零；冻结帧也不算“闪烁消除”。
- **输入**：Edge 测试记事本实际显示 `X11测试Aa1`；退格、Shift+A、回车、File 菜单及退出保存确认可响应。真实中文候选选择、跨系统剪贴板、物理双指/相对鼠标尚未完整设备验收。
- **能力范围**：Edge Maleoon 916B 的 DXVK 2.6 不满足现有 capability policy，未绕过策略强行运行，也不计为通过。

### GPU 子窗遮挡与退出生命周期

虚拟桌面中，GDI 已由 X server 合成到桌面 surface，GPU 子窗则单独持有 NativeBuffer。仅把 GPU node 置顶会覆盖后来打开的记事本、菜单或模态窗口；上游固定 refs 也保留了这个限制。

新增受管理 `xserver-ohos-gpu-visible-region.patch` 与 `wlroots-ohos-gpu-visible-region.patch`：GPU 绑定时按 XID incarnation opt-in，X server 在 clip 变化时发布其 validated `borderClip`（包含自己的 drawable children、排除上层兄弟窗口），XWM 缓存有界的 v1 矩形区域，宿主换算到客户区/sub-drawable，scene 同时裁剪可见区、不透明剔除和命中测试。未知、未映射、尺寸不同或代次不符时不跨窗绘制。该私有区域不是 XFixes `WindowRegionClip`，后者仅为客户端 ClipShape，不能提供真实遮挡。

GPU 帧仍保留一个 buffer/texture，通过 GPU scissor 绘制，无 CPU 像素回读/复制。初次绑定仅一次有序请求；后续由 PropertyNotify 刷新，逐帧查询不向 X server 发同步请求。协议最多 1024 个矩形；超限不截断暴露区域，按空区处理。已通过生产 parser/cache 与 scene visibility 的 ASan/UBSan 回归，包括 10000 次无请求查询、中心遮挡洞、opaque culling、未映射/代次/尺寸变化及截断、溢出、空区。内嵌 GDI 子控件与透明/复合窗口的进一步覆盖仍需单独确认。

本轮包继续属于本地诊断包；targeted Wine 模块不冒充全量新身份缓存。完整干净 Wine/CI 重建、GNU/Linux 游戏回归以及未验收输入项仍是合回条件。

2026-11-03 Edge 补充：首个区域裁剪包已正确处理记事本完全/部分遮挡，但测试程序退出后 X server 崩溃，因此该包未通过生命周期验收。原因是 `DeleteAllWindowProperties` 发删除通知时，属性链仍包含先前释放的节点，新增回调在删除通知中再次查询属性，触发 use-after-free。修正为只在 `PropertyNewValue` 时查询，删除通知不遍历属性链；生产回调测试覆盖这一条件。

修正版 `cef547fa…` 完成两个 120 秒 DX11+DXVK legacy 用例（13856/13952 帧），以及 180 秒用例（21172 帧），测试程序均在 teardown 后写 PASS，并以真实退出码 0 结束，Xwayland/XIM 保持存活。包含按 `2` 重建、最大化、记事本部分遮挡/拖动、桌面 × 后恢复和最小化恢复；恢复前后主应用、Xwayland、游戏 PID 保留。D3D9+WineD3D/VirGL 的独立 30 秒用例渲染 3478 帧、退出码 0。另一轮 D3D9 被测试工具切换图形配置而结束，不计为通过。

该 180 秒用例中 30 次截图为 30 个不同立方体帧，未采到整块背景黑/白。截图采样无法排除短于采样间隔的轻闪，不能视为人眼轻闪验收完成。标准 WSI 路径仍有同尺寸 swapchain 重建及 `41210000` 缓冲区归还错误；尚需定位，不能用归还计数配平或自动重建代替错误消除。

### XIM 激活状态接入

本地 IME 服务收到跨线程通知后会查询当前激活状态，以丢弃旧通知。移植的 XIM 桥此前仅发通知，而 `wineTextInputActive/Enabled` 仍查询 Wayland text-input 对象，X11 Wine 不会创建该对象，导致系统 IME 无法 attach。现桥缓存真实 IC 激活状态，按宿主 armed 策略门控，NAPI 按焦点后端查询。后台只解除门控，保留 IC 状态供恢复时使用；服务 EOF/关闭清除状态，新通道不继承旧激活。

`xim_activation_test.py` 使用真实 bridge/socketpair 和 NAPI 查询，覆盖 100 轮通道、碎片通知、失焦/重聚焦、后台恢复、EOF 及 Wayland 分发，并通过 ASan/UBSan。`a030a1be…` 诊断包上记事本实际显示中文和大小写，退格、Shift 大写、回车正常；桌面 × 后恢复可继续输入“恢复中文”。此项验证使用系统自动化 IME 注入，物理中文候选选择与跨系统剪贴板仍单独待测。

### WSI 队列背压与重建诊断

增加有界重建原因日志后，Edge 标准 FIFO 对照确认 `same_source=1`、设备/队列/尺寸/格式不变。平台日志先出现 VulkanSwapchainLayer 的 `RequestBuffer failed: 40601000`，随后 acquire 返回 `VK_ERROR_SURFACE_LOST_KHR`，重建清空队列缓存，消费者归还保留帧报 `41210000`。因此缓存归还错误是重建后的后果，不能压制该错误或将归还计数配平当作成功。

原 WSI NativeWindow 配置 `SET_TIMEOUT=0`，正常的短暂消费背压也会立即失败；驱动将这一失败映射成 surface lost。现改为两个刷新周期的队列等待（向上取整毫秒，1～50ms），检查配置失败并保留真实错误。GPU 热路径不增加像素复制或 queue-idle 等待。生产 helper 的主机回归覆盖 120/60/30Hz、零值、溢出边界及底层配置失败。修改后的设备长跑及恢复结果另行记录，尚未据此宣称所有闪烁解决。

### 命令环冻结与白帧分离验证

固定动态标题的标准 DX11/legacy 产品呈现用例曾在约 102 秒、12,078 帧停止；此时合成线程仍约 120Hz，guest stderr 报 `stuck in ring seqno wait`。这不是以画面静止消除闪烁。远端 shadow ring 的 IDLE 字不能当作共享内存中的可靠唤醒条件，原 1ms 通知合并窗口存在丢失空闲后唤醒的风险。

仅增加现有 Mesa `VN_WINEHUA_ALWAYS_NOTIFY_RING=1` 的对照完成 180 秒 20,982 帧、真实 exit0；该轮桌面曾被首页覆盖，截图不计作视觉证据。随后保持动态标题及桌面前台的 360 秒用例完成 42,010 帧、真实 exit0，采样仍捕获白帧。统一 Wine/Venus 环境现启用每次提交唤醒，Direct Vulkan 的策略明确关闭此项，不按游戏名处理。默认入口的后续 180 秒用例亦完成 teardown、20,233 帧及真实 exit0；该轮带像素读回诊断，不能用其帧率评价正常性能。

白帧仍未验收：正常遮挡计算的 15 张截图捕获 1 次异常，关闭遮挡计算的 15 张截图捕获 2 次；静态标题对照较稳定，不能因此在产品中禁止标题更新。分层像素读回出现 scene/canvas 读取差异，同时引入队列背压和重建，诊断会扰动时序，不能单凭该差异认定根因。读回默认关闭、每 31 帧最多 32 字节、总量限 600 次；正常呈现不读回像素。`diag-scene-no-occlusion` 与 `diag-canvas-scene-texture` 只用于单变量实验，不代表默认策略或已完成修复。

本阶段完成增量 OHOS Release native 构建、40 ELF/PE/XKB/locale HAP 闭包、全部 X11 主机回归、194 项进程生命周期、桌面恢复/会话模型及图形行为合同检查；未运行全新 CI 构建，仍不合主线。

### 2026-11-03 标题压力与恢复补测

统一命令环唤醒后的当前签名诊断包 SHA-256 为 `1be0ae415ddf69c304566d742131957a959387b02223d9e6570c5cbff9a2c2bd`，原始构建仍基于 `5d1a5ea5` 加受记录的本地修改；后续提交不追溯更改该构建来源。

- 默认导入方式与复用场景纹理的单变量对照：同一 360 秒用例完成 41,607 帧、teardown 和真实 exit0。复用开启 60 张、关闭 20 张、再开启 20 张均为动态帧且未采到白帧；两种方式都未复现，因此不能把复用认定为根因修复，仍保持原方式为默认。采样不能排除瞬态闪烁。
- 独立标题压力程序每 16ms 更新标题，180 秒完成 16,573 帧、teardown 和真实 exit0；25 张截图是 25 个不同的立方体帧，没有捕获白帧。实际拖动使标题与客户区同步从约 (589,425) 移至 (990,681)，画面继续更新。发送过按键 2，但旧累计程序日志没有 runId，不能单独据此声称该轮按键重建已核验。
- 随后默认产品入口 180 秒完成 21,032 帧、teardown 和真实 exit0。桌面外部 × 后恢复、最小化恢复、首页遮挡后恢复，主应用、Xwayland 和游戏 PID 保持，恢复后桌面宿主唯一。截图及程序日志确认最大化产生 1560×1014 渲染目标，恢复后 960×640，动态画面继续。
- 标为 `x11-pc-multi-product` 的 90 秒自动化用例完成 10,604 帧、真实 exit0，但自动化入口强制虚拟桌面，因此只计虚拟桌面长跑，不计多窗口验收；多窗口必须从正常首页卡片入口独立验证。
- 活跃渲染期间未见 noBuffer/acquire rebuild/canvas GL 错误；自然退出时仍有 `41210000`。生产者销毁 swapchain 清缓存可能早于消费者归还 retained frame，需要跨进程退休握手；此项尚未修复，未隐藏错误或用引用计数替代验收。


### X11 客户端装饰与独立窗口控制

OHOS Wine 的 X11 路线改由 Wine 自绘非客户区，继续使用 managed X window；不改变 Wayland/GNU 的窗口配置。受管理补丁 `0077` 向 XWM 发布 frame extents，wlroots 按类型、长度和值域缓存；多窗口先绘制 GDI 标题与边框，再把 NativeBuffer GPU 帧限制在客户区，避免 GPU 内容覆盖标题。不通过 CPU 回读/复制重画游戏画面。

新增 XWM move/resize/maximize 监听，接入现有 ArkTS 窗口控制；缩放覆盖八方向、size hints 和有界坐标，拖拽越界也传递 release。宿主请求只保留最新几何和一个在途操作。`x11_window_controls_test.py` 执行实际 C 处理函数，ASan/UBSan 回归晚到/重复请求、失效窗口、边角、约束和溢出。

Edge PC 的独立 `WineWindowAbility` 最小化后进入后台，直接调用 `Window.restore()` 返回 1300004。改用 specified launch mode，AbilityStage 以进程内单调递增的 toplevel ID 作为身份，`startAbility` 前置原实例；不再从后台调用窗口恢复接口，不在 onNewWant 再激活 X 窗造成回环。前台/后台状态抑制已经前置的激活回声；并发恢复合并。该控制链也用于 Wayland 的独立窗口，子窗口和虚拟桌面的原恢复路径保留。

`af50240f…` 定向增量诊断包完成最小化恢复：系统窗口/mission ID 207 保持，主进程 27813 与游戏进程保留，onNewWant 复用，没有新增 onCreate，也未再报 1300004。此前 `f1ade480…` 证明系统前置已恢复窗口，但多余 Window.restore 调用仍报错，因此该中间包未保留、未计为最终修复。当前定向 core/ArkTS 编译、X11 主机回归、进程生命周期/会话回归、图形行为合同及 40 ELF/PE/XKB/locale 诊断包闭包检查通过；未据此声称干净 CI 或正式版本已验收。


同一 `af50240f…` 包补测：正常首页的真实 PC 多窗口（无 DesktopAbility）连续两次打开 DX11/DXVK legacy。首轮标题拖动从 (589,355) 到 (889,555)，2090×1394 缩为 1790×1154，最大化至屏幕并还原；客户区继续更新，稳定后约 116～120 FPS。尺寸变化期间仍会发生 swapchain 重建和 41210000，不能将这些控制检查当成队列退休问题已修复。两次标题栏关闭都取得真实游戏 exit0；主进程 27813 和 Xwayland 28591 保持，第二次创建新 toplevel 92/系统窗口 208，最小化后继续复用 208。两轮各 16 张截图均为不同图像、未采到整块黑白；此为采样证据，不能排除短时轻闪，也不代表用户目视验收。


X11 虚拟桌面同包对照：从正常首页启动 DX11/legacy，内部标题拖动、最大化/还原后客户区继续刷新；16 张采样均不同，未采到整块黑白。虚拟桌面退回普通窗口再点宿主 × 后，DesktopAbility 离开但主进程 31530、Xwayland 32087 和游戏 32234 保留；再次点击首页卡片恢复唯一 DesktopAbility，没有重复启动游戏。该项重跑了此前 Edge 的关键 × 恢复场景。拖动和尺寸重建当时可短降至约 14 FPS，稳定画面恢复约 114～118 FPS，不把拖动阶段宣称为始终 120 FPS。


同包独立 30 秒 D3D9 用例 `x11-final-d3d9` 在明确请求 WineD3D 的环境完成 3451 帧、angle regressions=0、渲染 PASS 和真实 exit0（pid 34346）。测试程序的 backend 显示字段为 unknown，因此不把该字符串当作后端识别成功；请求参数、运行库路径和程序结果分别保留。未见本轮 X server fatal/segmentation 日志。将后端设回 Wayland 后继续做默认路线对照。


Wayland 默认呈现对照：DX11/legacy 45 秒、5402 帧、render PASS，截图约 120 FPS；但根 NCP 仅报告 IPC death / unknown(-1)，不计为正常退出通过。D3D9 请求 WineD3D 的 30 秒用例完成 3424 帧、render PASS、真实 exit0。Wayland 期间无 Xwayland 进程；此前 X11 stderr 中的 broken-pipe/abort6 属于显式停止旧显示宿主后保留的历史文件，不能用来证明 Wayland 游戏崩溃。原始证据与这一范围限制均保留。脱敏结果见 `docs/evidence/x11-edge-window-controls.json`。


## 2026-11-03：持续轻闪复查（未完成视觉验收）

用户仍报告 X11 轻闪，认为原 Wayland 没有此现象。不能将此前退出／缩放的队列退休修复视为持续轻闪修复。

- 发现 X11 多窗口的输出 `RequestBuffer` 路径直接关闭消费者 fence，未等待系统合成器结束读取就复用缓冲区。现改为与 guest acquire 同一套 EGL GPU wait；不支持时有界等待，失败 abort 当前 slot 并保留上一帧。不增加正常路径 CPU 像素复制或 readback。
- X11 虚拟桌面最终 blit 直接借用当前 scene FBO 的 color texture。同一 EGL context 内按命令顺序完成场景写入和最终采样，不再每帧为同一 NativeBuffer 新建／销毁 EGLImage sibling。输出 commit 持有 buffer，最终 blit 不销毁借用 texture；来源不可用则不 swap。
- 最终画布在 clear/draw 前明确绑定默认 framebuffer、解除 array-buffer 绑定、恢复 color mask 并关闭 scissor，避免共享 GL 状态影响窗口调整后的完整写入。
- 新增生产函数行为回归：1000 次输出 slot／scene 复用，fence FD 关闭、失败等待 abort、FBO 恢复和 texture 身份保持。`test-x11-integration`、Venus present／退休用例、图形栈契约和 40 ELF/PE/XKB/locale 闭包检查通过。
- 对照使用同一 x64 D3D11 + DXVK legacy 程序：Wayland 60 秒 7202 帧；X11 100 秒对照；借用 scene texture 的旧候选诊断 90 秒 10581 帧。以上只说明程序持续呈现。Wayland 本轮退出仍只有 `ipc-death`，不能记作正常退出已通过。
- 部分截图被首页／设置窗口覆盖，不能用于轻闪验收。固定坐标截图未发现整幅黑白也不能排除短于采样间隔的闪帧。新候选实机结果另记证据；仍不宣称轻闪已经根治，不将 X11 合回主线。


## 2026-11-03：周期性掉帧与标题事务

用户观察到立方体转动中间顿挫，本轮区分了角度计算、渲染线程帧间隔和实际屏幕呈现。可选 `--frame-pacing` 只在内存累计 CPU 帧开始/Render+Present 返回耗时，忽略前 120 帧，结束写 JSON；没有逐帧日志或像素读取。角度仍按单调时间推进，所有有效用例 angle regressions=0。这不等于物理屏幕帧时间已被测量。

同一 Edge、同一 DX11/DXVK 1.10 程序的结果如下。第二组开启/关闭 `WINEHUA_CSD_TEXT_ONLY=0` 使用同一个 55040c35… HAP；重复开启用例使用仅更新打包清单的 54558fd3…，win32u/libentry/libvirgl_child 字节相同。

| 路线 / 处理 | 时长 | >16.67ms CPU 帧间隔 | 其中紧跟标题刷新 | 标题刷新均值 | P99 帧间隔 |
| --- | ---: | ---: | ---: | ---: | ---: |
| 原 X11 完整标题重画 | 70s | 149 | 133 | 13.81ms | 21.50ms |
| 原 X11，关闭单次 BitBlt | 65s | 138 | 124 | 13.79ms | 21.50ms |
| 确认的 Wayland | 65s | 5 | 3 | 2.93ms | 11.75ms |
| X11 文字快速重画开启 | 70s | 48 | 28 | 5.52ms | 16.00ms |
| 同包关闭文字快速重画 | 70s | 160 | 133 | 15.86ms | 23.75ms |
| X11 文字快速重画重复开启 | 70s | 50 | 34 | 5.95ms | 15.75ms |
| X11 固定标题诊断对照 | 70s | 16 | 0 | 无标题更新 | 15.50ms |

主要实测来源是 X11 `WM_SETTEXT` 同步执行完整非客户区重绘，包括边框、系统图标、菜单和三个主题按钮，阻塞游戏渲染线程；不是上一轮原子标题绘制单独引入的开销。平均约 118 FPS 仍会掩盖约每半秒一次的顿挫。`0079` 在 OHOS/X11 对标题文字变化只重画文字矩形，仍保留图标/按钮、正常标题更新及激活、尺寸、主题和样式变化的完整重画。RTL、隐藏、最小化、无标题、分配失败走原回退。不引入持久 bitmap 缓存、按游戏名判断或 GPU 客户区复制。剩余长帧尚未消除，也未据此宣称完全达到 Wayland 的帧稳定性。 同包固定标题的 70 秒诊断对照有 8198 帧、0 次标题更新、16 次 >16.67ms 帧间隔、P99 15.50ms，进一步支持标题事务是主要周期性来源；该对照仅用于定位，产品仍正常显示动态标题，未通过固定标题或降低测试负载掩盖顿挫。

`0078` 将标题背景、按钮和文字先画入小型内存 DC，再一次 BitBlt 到标题区，避免分步绘制的擦空状态被合成器呈现。7cf61588… 同包压力对照：开启 100 张标题空白 0，关闭 100 张标题空白 4；客户区均无整块空白且各有 100 个不同帧。该修复只缓存小标题，不拷贝游戏 GPU 画面；稀疏截图不能证明瞬态闪烁为零。

最终 54558fd3… 包完成 PC 独立窗口 150 秒 17067 帧、标题拖动、最大化/还原和缩小；20 张稳定几何截图标题/客户区空白均为 0，20 个不同客户帧。另一次 PC D3D9 请求 WineD3D/VirGL 的 50 秒用例 5840 帧；两者都有真实 ncp-exit=0。后者 backend 显示字段仍为 unknown，不把该字段当作驱动识别通过。显式 `virtual` 启动的 DX11 用例验证唯一 DesktopAbility 和完整输入根，60 秒 7146 帧、12 张动态截图无整块空白；渲染/teardown PASS。首次短日志尾未见退出回调，后续有界查询已按该轮游戏的精确 PID 找到真实 ncp-exit=0，补记为正常退出；没有借用其他轮次的回调。另一次冷启动虚拟桌面 12 秒 1384 帧亦取得该轮真实 ncp-exit=0。此前的未观察记录保留在私有原始证据中，没有据此修改会话退出机制。未见这些用例的 X server crash。

队列退休新增每 attachment 独立 SEQPACKET 通道，按 epoch 完成 Retire→消费者摘帧/排空→Ack→销毁→Resume。仅 X11 Vulkan attachment 启用；resize 非阻塞重试并有约 1 秒最终保护，设备 teardown 释放 manager/target 锁后最多等 400ms。主机生产代码回归包括 1000 次重复退休、旧 ack、延迟排空、超时和跨进程端点。最终有界日志尾中 retained frame 均归还，未见 40601000、41210000 或 device-lost；日志只覆盖当前存活进程的有界尾部，不代表所有历史场景。

补丁已经注册到构建脚本并锁定 effective source/脚本哈希。全受管理 Wine overlay 重放、X11/Venus 主机回归、图形行为合同、最终 unsigned HAP 图形清单与 40 ELF/PE/XKB/locale 闭包通过。诊断包清单中旧 VirGL/MF 哈希已按 exact-output receipt 更新，未用修改校验器跳过检查；没有修改其代码或换入其他图形版本。Windows CLI 签名/验签及签名前后成员 CRC 检查通过。源码分支仍为实验分支，包构建基线仍记录 3bfcaa51 加源码哈希，不把后续提交号追溯写成编译来源。

误标签已排除：`pc-pacing-wayland-product` 实际仍为 X11，不能当 Wayland 对照；`pc-caption-modern*` 实际加载 legacy，不能当 DXVK 2.6；`caption-text-desktop-virgl-product` 继承进程内 fusion override，实际是 PC 窗口。后续虚拟桌面用例显式传 virtual 并核验实际宿主。按用户要求跳过 DXVK 2.6。物理 IME/双指/剪贴板、GNU/Linux 游戏与完整干净 CI 等原合入条件仍未全部完成，不合回主线。

完整 CI preflight 已重跑，217 项全部通过。唯一修正是发布打包单测的隔离 fixture 补齐 X11 verifier stub（假 hvigor 不生成真实 ELF）；生产打包仍执行真实 X11 运行库闭包验证。此结果不等于干净源码的全量重编译已完成。

脱敏证据：[帧间隔对照](evidence/x11-edge-frame-pacing.json)、[标题和窗口回归](evidence/x11-edge-caption-sync.json)。
