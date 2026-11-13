# Linux ARM64 Steam：GNU Wayland 接入第三阶段

实施日期：2026-10-18 至 2026-10-19。源码位于 WSL
`/home/liufeng/src/vpp-account-cloud`，诊断候选仍使用独立快照构建。
没有使用 QEMU、TCI 或指令转译；没有修改 Steam 或切换现有游戏后端。

## 本阶段结论

真正的 GNU AArch64 C++ 客户端，已通过 GNU libwayland-client 连接现有 OHOS
合成器，在 USB 平板 MLR-AL10 上建窗并显示棋盘格。三次关闭重开均通过。
这比第一阶段的 OHOS 客户端更进一步：GNU 和 OHOS 各自使用自己的 libc，
通过 AF_UNIX、标准 Wayland wire protocol 和内核 SCM_RIGHTS 互通。
没有把 OHOS libwayland 或 libc 当作 GNU 依赖。

**Steam、Xwayland、CEF、GNU Vulkan 和 Proton 仍未启动。** 不能将本结果表述成
Linux Steam 已运行。本阶段只证明原生 GNU 程序能够复用现有 Wayland 显示宿主。

## 代码与运行方式

- 新脚本 `scripts/build_linux_steam_gnu_wayland.py` 从固定哈希的上游源码构建
  libffi 3.4.6 和 Wayland 1.22.0。构建机器原生编译同版本 wayland-scanner，
  交叉编译目标库；Meson 明确 needs_exe_wrapper=true，未配置执行器，未在
  构建机器运行任何 AArch64 程序。
- 新 `gnu_fixture/wayland.cpp` 复用现有 `surface_probe.cpp` 和 xdg 协议代码，
  由 GNU C++ 工具链重新编译成 GNU ELF，不链接进 OHOS UI。配套 GNU
  libstdc++、libgcc_s、libm 隔离放置，全部 DT_NEEDED 闭包在包内核对。
- NAPI `probeLinuxSteamGnuWayland()` 在 JS 线程捕获本应用的 socket 路径，
  以官方 NCP 参数传递。NCP 再使用第二阶段的原生 rtld 执行后端运行 GNU
  客户端。它自行连接 socket，不改 WAYLAND_DISPLAY 或 Wine 的全局环境。
- 套件为 `linux-steam-gnu-wayland`，仍由默认 OFF 的 GNU_EXEC 开关控制。
  测试约 5 秒，父 NCP 最多等待 7 秒，收集结果、检查 TP 并回收执行进程。
- SHM 使用两张 480×270 XRGB8888 缓冲，configure/ack 后才提交，等待
  buffer release 和 frame callback，不覆盖合成器拥有的缓冲。
  诊断限速约 10 FPS，屏幕上的既有游戏 FPS 统计器不统计该客户端帧率。

双向 FD 验证有实际数据消费：GNU 通过 SCM_RIGHTS 发送 SHM fd，OHOS 显示
其像素；OHOS 通过 SCM_RIGHTS 发回 keymap fd，GNU pread 验证末尾 NUL 并
关闭描述符。GNU 客户端退出前后再次枚举 FD，确认没有泄漏。

## 平板结果

最终包 `native-gnu-wayland-debug-signed.hap` 已替换安装，应用数据保留。

| 项目 | 第 1 轮 | 第 2 轮 | 第 3 轮 |
| --- | --- | --- | --- |
| configure/ack | 1 | 1 | 1 |
| SHM 提交 / release / callback | 48/48/48 | 48/48/48 | 49/49/49 |
| display error | 0 | 0 | 0 |
| keymap fd 消费 | 通过 | 通过 | 通过 |
| 客户端 FD 前/后 | 4/4 | 4/4 | 4/4 |
| GNU exit / 回收 | 0 / 已回收 | 0 / 已回收 | 0 / 已回收 |
| 父 NCP TP | 未变 | 未变 | 未变 |
| NCP exit callback | signal 0 | signal 0 | signal 0 |

第 3 轮在应用内点击“Wine 桌面”后截屏，`tablet-gnu-wayland-visible-03.png`
只保留 GNU 棋盘格窗口区域；保存了 449166 个与诊断色匹配的像素作为定位证据。
这是独立的可见画面证据，协议结果仍保留 framePresented=false，不声称已测
scanout/presentation timing。前一轮过早截到加载页，保留为私有对照，不作为
显示通过依据。

客户端读取 keymap 的 FD 路径已验证，但这三轮没有注入按键，keyEvents=0、
inputVerified=false。第一阶段 OHOS 客户端的 8 键回环不能移作 GNU 按键证据。
窗口焦点、实际按键/鼠标回传、尺寸变化仍需单独验收。

扩展候选上再次运行 `linux-steam-gnu`，原有 main/TLS/双线程/dlopen/dlvsym/
析构、exit37、timeout9 和回收套件 PASS。Linux host reference 的 10 个 wire
场景、3 个 ABI 控制及 6 个真实 Wayland 夹具场景全部通过，FD 6→6。
默认 OFF 的最终 smoke_napi 编译/链接通过，未导出 GNU probe。

## 构建、签名与证据

GNU Wayland 构建示例（前置 GNU runtime 见第二阶段报告）：

```sh
python3 scripts/build_linux_steam_gnu_wayland.py \
  --cc <aarch64-none-linux-gnu-gcc> --base-runtime <native-runtime> \
  --glibc-build <paired-glibc-dir> --libffi-archive <libffi-3.4.6.tar.gz> \
  --wayland-archive <wayland-1.22.0.tar.xz> --output <new-dir> --jobs 12
```

归档来源及 SHA256：

- libffi：`https://github.com/libffi/libffi/releases/download/v3.4.6/libffi-3.4.6.tar.gz`，
  `b0dea9df23c863a7a50e825440f3ebffabd65df1497108e5d437747843895a4e`。
- Wayland：`https://gitlab.freedesktop.org/wayland/wayland/-/releases/1.22.0/downloads/wayland-1.22.0.tar.xz`，
  `1540af1ea698a471c2d8e9d288332c7e0fd360c8f1d12936ebb7e7cbc2425842`。

包准备脚本新增受限的 GNU Wayland 清单，严格核对 11 个 ELF。两个无扩展名
程序在正式签名前按原名加入 libs/arm64-v8a/gnu-native，其他库禁止 strip。
libffi、Wayland、glibc、Arm GNU Toolchain（含 GCC Runtime Library Exception）
许可文本一起入包。源码归档、配套补丁和构建材料保留在本机；正式分发仍应
交付相应源码与材料。本阶段不将诊断依赖和二进制加入正常产品入口。

完整 ArkTS/NDK/HAP 构建和官方签名均成功。安装后的实机结果原文为
`tablet-gnu-wayland-01/02/03.json`；summary 与 `wayland-stage-summary.json`
记录判据。`native-gnu-wayland-identity.json` 记录包、27 项宿主源码输入与 GNU
runtime 来源；`wayland-final-verification.json` 确认源码输入和全部 11 个 signed
ELF 字节匹配，原有 14 项暂存 raw diff 未变。脚本本身的最终身份另见
`wayland-build-inputs.json`。

仍使用第二阶段的独立 GNU_EXEC 开关，不新增产品购买、Steam 启动或更新入口。
本阶段未提交或推送。真实设备标识、签名材料、原始全屏截图和 layout 仅本机
私有保存，不作为公开附件。

## 下一步和兼容边界

原版 ARM64 Steam 的 steamclient、steamwebhelper、libcef 有真实 X11/GL
DT_NEEDED，不能只加一个 Wayland 参数就移除这些依赖。下一阶段优先验证
rootful Xwayland 与 X11 小程序，先 SHM 出图/输入/退出，再考虑 GPU 路径，
不替换现有合成器。CEF 多进程、JIT 与进程启动应独立验证。

原生 GNU ARM64 路线没有排除 DXVK/Proton 接入，但现有 OHOS musl Vulkan
库与 GNU 客户端存在 ABI 边界，仍需实现 GNU Vulkan/GL/EGL 桥或进程服务。
官方 Proton 还依赖 kernel exec、进程环境、Steamworks 等语义，当前 rtld
handoff 不能代替这些验收。Windows x86 游戏仍需其自身的 FEX/Wine 后端。

Steam 自更新和动态下载代码仍有平台门槛：目前证明的是签名安装目录的 RX
映射，可写 data 中的 ELF 不能据此认为可执行；/proc/self/exe 与内核 auxv
元数据也没有变成完整 kernel exec 语义。后续方案必须在正常 HAP 权限和
签名模型内解决这些问题，不能通过改系统策略或隐藏失败来跳过。
