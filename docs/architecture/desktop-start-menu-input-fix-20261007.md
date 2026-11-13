# 首次 Wine 桌面输入修复及 64 位 FEX 配置验证（2026-10-07）

> 2026-10-07 后续更正：本报告的独立 driver 构建遗漏 Wayland EGL。桌面输入验证仍成立，但该旧 HAP 不应继续用于游戏测试。完整原因、修复构建门禁和替代产物见 [Wayland EGL 回归修复](wayland-egl-startup-regression-20261007.md)。


首次桌面开始菜单失效已定位并修复，签名候选已覆盖安装到 MatePad Mini 的 `com.vintage.pomelopro`。32 位与 64 位 FEX 均沿用默认 `FEX_X87REDUCEDPRECISION=1`；64 位的实际计算和显式关闭对照也已通过。

## 桌面问题的原因与证据

Explorer 的 `load_graphics_driver()` 先创建 Wayland 事件线程，随后 shell 主线程执行 `SetThreadDesktop(shell)`。事件线程保留原桌面。硬件输入带 HWND，wineserver 会选择窗口所属桌面；相对模式 enter 校准调用的 `NtUserSetCursorPos()` 不带 HWND，使用当前事件线程的桌面。因此点击到任务栏的坐标写入另一份桌面状态，shell 的鼠标坐标仍为 `(0,0)`，按钮事件无法正确命中开始按钮。

冷桌面基线、Wayland trace 和两轮同会话状态探针一致：宿主命中独立任务栏、发送 pointer enter 和鼠标按下／松开事件，客户端收到相应事件并计算出 `(28,788)`，但持续探针读取 shell 光标仍是 `(0,0)`。任务栏按钮可见且 enabled，实际按钮矩形是 `(0,780)-(54,800)`。同一 Explorer 进程中，主线程和事件线程的桌面句柄不同：修复前分别为 `0x6c` 和 `0x14`。探针跨进程读取返回的句柄不属于探针句柄表，不能用它直接查询桌面名称；这里仅比较同一目标进程的线程返回值，并结合 Explorer 初始化时序判断。

单纯创建 Explorer 窗口在这次自动化复现中也没有修复任务栏点击；不能将用户观察到的“通常打开窗口后恢复”误写成每次都能恢复。普通窗口带 HWND 的移动可能重新建立 shell 光标，但本次没有把该机制作为正式结论。

## 改动

新增 `patches/wine/0044-wayland-input-thread-desktop.patch` 并注册到 `scripts/build_wine.sh`。在 Wayland pointer/keyboard enter 时，验证窗口属于当前进程，取得窗口所属线程桌面，并在必要时切换事件线程桌面。相对模式仍使用原有 enter 校准；切换失败时，pointer enter 退回带目标 HWND 的硬件定位路径，避免继续校准错误桌面。

保留此前窗口身份、输入命中、多窗口合成、前台切换与游戏恢复修复。没有新增 UI 控件或启动时自动打开 Explorer 的变通逻辑。FEX、Mesa、图形画质和其余性能参数继承已验证的默认性能包。

## 真机验证

| 场景 | 结果 |
| --- | --- |
| 修复前冷桌面点击开始 | 两次点击均无菜单，截图完全相同 |
| 修复后冷启动第一轮 | 第一次点击打开开始菜单 |
| 修复后完整退出、冷启动第二轮 | 第一次点击打开开始菜单 |
| 修复后持续状态探针，随后创建 Explorer | 冷桌面与 Explorer 出现后均可打开菜单 |
| shell 鼠标状态 | 从 `(0,0)` 正确更新为 `(28,788)`，命中开始 Button |
| shell 事件线程桌面 | 与 shell 主线程一致为 `0x70`；独立 clipboard 线程仍保持原桌面 |
| 菜单“运行” | 触摸后成功显示运行对话框 |
| 对话框“取消”和 Explorer 列表项 | 取消关闭对话框，点击“我的电脑”成功选中 |

本轮没有发送 Steam 或游戏启动命令，没有据此宣称游戏输入、相对视角或所有窗口模式完成全量回归。桌面静止画面的宿主 0 FPS 是没有新帧提交，不能当成新的性能故障。

## 64 位 FEX

采用同一源码编译 PE64 探针，直接 Want 启动，并在两轮之间完整退出 Wine/FEX。探针在 x87 PC64 下计算 `(1+2^-60)-1`，不是只读取环境字符串。

| PE64 配置 | 实际结果 |
| --- | --- |
| 不传覆盖 | `requested=1, observed=reduced-f64, result=0, complete=1` |
| 显式 `FEX_X87REDUCEDPRECISION=0` | `requested=0, observed=strict-f80, result=2^-60, complete=1` |

设备 ARM64EC FEX DLL SHA256：`8aba586e7988b01cd1d24685bef778ed71f64da9dc3bf525bd720ef6596f0cfc`。

设备 WOW64 FEX DLL SHA256：`002a4a3a399db0743b0563d53ab7880b1d9bfa32ae19b7f513256a1938cbadfa`。

共享环境基线同时覆盖两条 FEX 路径，因此不需要另给 64 位默认设置。旧 x64 Wine/Box 架构不注入该变量。ReducedPrecision 优化 x87 的 F80 路径；以 SSE2/AVX 为主的 64 位游戏可能收益较小。本轮没有测 64 位游戏 FPS。若个别游戏依赖扩展精度或指数范围，可显式设 `0` 并完整重启会话。

## 构建与包身份

Wine driver 使用独立源码快照 `workspace_temp/wine-desktop-input-source-20261007` 和独立 `build-desktop-input-20261007`。目标 Wine 对象和 native Wine tools 均从该源码构建；仅复用已有第三方依赖。正式来源记录和 `--check-cached-identity` 通过，构建配置包括 `WINE_ARCH=aarch64 NATIVE_ARCH=arm64-v8a GUEST_ARCH=aarch64`。复制快照没有 Git 元数据，来源记录使用完整源树 SHA256，不能把它描述为带 Git HEAD 的干净 checkout。补丁正向及反向 dry-run、此次源文件与构建注册的 whitespace 检查通过。

签名包基于默认性能包 `1416cdb0…`，只替换 `libs/arm64-v8a/winewayland.so`。其余 HAP payload 逐项字节一致，签名时仅允许 `.pages.info` 元数据重生成。官方 `hap-sign-tool verify-app` 通过，HDC 安装返回 `install bundle successfully`，bundle 核对版本仍为 `1.4.5-proton.26-alpha (1004035)`。

交付文件：`F:/VintagePomelo-Workspace/workspace_temp/desktop-start-menu-20261007/entry-desktop-input-fixed-debug-signed.hap`。

HAP SHA256：`11d26c91b3d0a905da1448b9c861f6e1e94421b034edbb6d6f28d1defea8327f`。

Wine Wayland driver SHA256：`2f289ae86a19866de40ccb5d97d84f9ee84079c5ecb58323159b14e240731723`。

临时桌面 trace 已从源码撤销，正常 `libwine_child.so` 已重编；设备最终包也继承原默认包中的普通库，没有保留临时 trace。修正 driver 已同步到 `entry/libs/arm64-v8a/winewayland.so`，便于后续正常 HAP 打包。源码未 commit/push，HAP 和原始采集不加入版本控制。

原始截图、日志、探针源码、受控打包与签名记录位于本目录。精简证据保存在 `docs/architecture/evidence/desktop-start-menu-20261007/`。
