# OHOS Proton X11 实验集成

状态：源码已集成，主机回归、ARM64 编译及实际构建输出闭包检查通过；设备统一验收待进行。**未合回主线，不视为发行资格、性能证明或干净 CI 重建证明。**

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
| Wine 驱动 | 基于现有 Wine gitlink，以受管理的 0072/0073/0074 overlay 接入；桌面和顶层子窗口使用显式 role |
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
- 19 个 production Wine startup cases、184 个真实进程生命周期 checks、完整 model suite。
- X11 宿主实际代码 fakes：身份/代次复用、延迟销毁、鼠标 capture 释放、细粒度滚动、buffer 引用平衡。
- 剪贴板 epoch/串行写入、真实 FD 所有权、日志限额/关闭时限、managed overlays 重放与不覆盖未知源码。
- Wine 全 overlay clean replay，图形 lock 与有效源码检查、Wine 构建身份守卫。

## 后续统一验收与合回条件

设备由用户稍后提供，本轮不操作其他在线平板、Edge 或手机。

1. 从上述源码提交按干净清单组装完整候选，验证最终 HAP 闭包与精确运行库身份，再签名。
2. Edge：经典/Deck 下虚拟桌面 × 后恢复、最小化、首页遮挡、连续启动、PC 多窗口；旧默认 Wayland 与 X11 分别跑。
3. 中文、大小写、退格、滚动、相对鼠标、拖拽缩放、菜单、模态、全屏与窗口恢复。
4. D3D9+VirGL、D3D11+DXVK 1.10/2.6，字体/颜色/模型/视频及声音/手柄对照。
5. 首页与 Steam 手动启动、普通/大屏往返、退出返回及宿主重建；GNU Proton 与 Linux 原生游戏的启动/音频/存档。
6. 连续启停、异常恢复、性能对照、全新 CI 重建后，再考虑合回 `main_proton_steamarm64`。

以上待验收项没有被主机测试或旧设备证据标记为通过。
