# Wayland EGL 构建回归与 PAL4 / Steam 启动复核（2026-10-07）

## 结论与原因

修复首次桌面开始菜单后，独立重编的 `winewayland.so` 缺少 Wayland EGL 支持，造成 PAL4 持续白屏及 OpenGL 创建上下文异常。这是构建能力退化，0044 输入桌面同步补丁没有改变 D3D 路由或 OpenGL 实现。

原构建 `build-desktop-input-20261007/wine-ohos-aarch64/include/config.h` 为 `/* #undef HAVE_LIBWAYLAND_EGL */`，Makefile 的 `WAYLAND_EGL_CFLAGS/LIBS` 为空。Wayland 客户端依赖已找到，因此桌面仍能显示，但 `WAYLAND_OpenGLInit` 被编成返回 `STATUS_NOT_IMPLEMENTED` 的 stub。

PAL4 在完整退出后以纯 WineD3D / VirGL 重启仍复现；实际加载的是 Wine builtin `d3d9.dll` / `wined3d.dll` / `opengl32.dll`。不是 D3D9 被切到 DXVK。

白屏进程 6750 的异常地址 `0x40`，`win32u.so` 相对 PC `0x13a5a4` 对应 `egldrv_make_current`；LR `0x13da24` 对应 `context_sync_drawables`。缺少 Wayland GL 实现后，通用 EGL 路径将空 framebuffer target 传入，读取 `draw->surface` 时异常。本次补回真实 Wayland EGL 实现，不以空指针吞错代替修复。

## 修改

- `scripts/build_wine.sh` 在所有构建宿主显式传入目标 sysroot 的 `WAYLAND_EGL_CFLAGS/LIBS`，将缺少 `HAVE_LIBWAYLAND_EGL` 视为需要重新配置。
- 新增 `scripts/wine_graphics_capabilities.py`，构建前核对配置宏和链接参数，构建后核对真实 ELF 中的 `wl_egl_window_create/resize`。缺少 GL 的驱动明确失败，不再被作为完整图形驱动打包。
- 新增 `wine_d3d_policy.h` 作为 DXVK DLL 默认路由来源：DirectDraw、D3D8、D3D9 使用 Wine builtin；D3D11 / DXGI 保持 DXVK native。legacy 的 D3D10 链保持 native。保留最后合并的游戏专属覆盖设置。
- 保留 0044 桌面输入同步、FEX 默认及 32/64 位 `FEX_X87REDUCEDPRECISION=1`；本轮不改 FEX、Box、纹理、分辨率、TSO、SMC 或 exact-store 设置。

新目标构建目录为 `/data/src/winehua/build-desktop-input-egl-v2-20261007`。目标 Wine 对象从零构建，源码是上一版独立快照 `workspace_temp/wine-desktop-input-source-20261007`，仅复用同源码构建的 native host tools 和依赖。源码全树 SHA256 与配置通过 `wine_build_identity.py record/check` 记录；Docker 快照无 Git 元数据，身份依据明确为 full-source-tree-sha256。

新驱动 strip 后、Hvigor 二次 strip 前 SHA256：`64870c1f0ceb4bca3f1993d94ec28f1640721429401ac0294f78ef20bf6a0f09`。

## 验证

- `make test-dll-overrides` 通过，覆盖两种 DXVK 默认路由、后置游戏 override、非法规则保留与显式清空。
- `make test-gpu-followup` 通过，包括多窗口显示/输入、纹理统计、Wine 源码身份和 Make 缓存身份检查；新增能力检查已加入此目标。
- 缺 EGL 宏、空 EGL 链接参数及不含真实 GL 导入的 ELF 均被拒绝。实际旧构建被生产检查拒绝，新构建与最终 HAP 驱动通过检查。
- `bash -n scripts/build_wine.sh` 及本轮涉及文件的 scoped `git diff --check` 通过。未宣称全仓聚合测试通过。
- Debug `assembleApp` 和 proRelease release `assembleApp` 构建通过。Debug HAP、release HAP、上架 APP 的官方验证通过；release provisioning 为 `app_gallery`，APP 内嵌 HAP 与未签名 HAP 仅 `pack.info` 排版不同，JSON 语义及其余 payload 一致。

## 平板复核

旧柚 Pro `com.vintage.pomelopro`，版本 `1.4.5-proton.26-alpha (1004035)`。覆盖安装的 debug HAP SHA256：`35c6ed92f1d29667125df5d58d4857671d6a449f102f902ad19ca2cec18d4b57`。实际包内驱动 SHA256：`8b78c4fb34c3e6471703cd95c74d227bdb05215edeb74ebf71e47c6305184d90`；已确认含真实 Wayland EGL 导入，libentry 含两组混合 D3D 路由。

1. 冷启动 `winehua_keep.exe`，未打开文件管理器便点击开始菜单，菜单成功出现。初始白色应用画布是启动阶段，随后桌面和菜单正常；未把启动前截图误判为 GL 白屏回归。
2. 完整退出会话，以 `dxvk_legacy` 启动 PAL4。主菜单正常渲染；打开存档列表，选择第二格唯一存档“青鸾峰 / 序章 / 0:01:52”，点击确认。确认后 10 秒截图已进入青鸾峰场景，20 秒仍显示正常，画面计数约 67 FPS；鼠标点击有响应。
3. Steam 最初以 `dxvk_legacy` 和最小 CEF 参数启动，登录画面正常，日志确认 CEF 加载 overlay 的 native D3D11 / DXGI，D3D9 为 builtin。但随后大屏长时间停在加载动画，普通主窗口一度只显示边框，未将这轮判为完整通过。
4. 历史成功入口另外包含 `-nocrashmonitor -noshaders -no-shared-textures -cef-single-process -cef-in-process-gpu -cef-disable-sandbox -disable-winh264 -no-cef-sandbox -vrdisable -cef-disable-breakpad`。完整退出后按同组参数重启，没有禁用 GPU；日志诊断降为 `-all,+err`。Steam 显示库界面，随后大屏正常显示《新仙剑奇侠传》的背景、文字和手柄说明，进入运行 DirectX 安装脚本页面，启动白屏回归未再出现。观察到游戏启动后停止发送测试输入，只继续采集。

Steam 最小参数轮不能用于证明现有兼容入口失效；这组 CEF 单进程 / 进程内 GPU 参数来自设备已有成功记录。本轮不把这些参数自动写入全部 Steam/CEF 程序，也不将单进程重启结果声称为严格 CEF 故障归因。DirectX 安装脚本后的《新仙剑》实际游戏运行不在本次通过范围。

PAL4 菜单截图约 112 FPS、场景截图约 67 FPS 仅是短时截图观察；没有做严格 A/B，也不作为新的性能收益结论。FEX 的权限/SMC 信号记录仍存在，不将所有 `[FAULT-MIN]` 一概解释为崩溃；本次 PAL4 新进程未重现原先 `addr=0x40` 的 EGL 空 target 异常。

原始本地证据：`F:\VintagePomelo-Workspace\workspace_temp\pal4-steam-startup-20261007\`，包括日志、截图、设备命令记录、源码构建身份及包身份。旧桌面首次点击截图位于相邻 `desktop-start-menu-20261007/egl-desktop-unlocked-before.png`，菜单结果为本目录 `egl-desktop-after.png`。

## 替代交付物

旧 `release-proton26-20261007/` 两份交付物包含缺少 Wayland EGL 的驱动，已标记 superseded。使用 `release-proton26-egl-fixed-20261007/` 新文件，版本号保持本轮既有版本：

| 文件 | 大小（bytes） | SHA256 |
|---|---:|---|
| `VintagePomeloPro-1.4.5-proton.26-1004035-egl-fixed-unsigned.hap` | 350687268 | `0c443e0503e4af743ef530decfc3ba08ee3c2a07792c0aa7ea2f2d95e4d2f8c8` |
| `VintagePomeloPro-1.4.5-proton.26-1004035-egl-fixed-appgallery-release.app` | 272095761 | `c036afb9510f15a78b22b3526ff96cdbd33b19e579c0e7bca3ed4f19b7f2dc23` |

正式产物未用商店签名覆盖安装；设备验证使用同源码和同驱动的 debug 候选。两份正式产物完成构建、签名/无签名校验与 payload 校验；未发布到市场。本轮代码保留在工作区，没有自动 commit/push。
