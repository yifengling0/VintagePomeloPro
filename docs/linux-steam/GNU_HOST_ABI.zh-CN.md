# 最小 GNU 环境与 OHOS 适配边界

当前实现是原生 ARM64 GNU 运行环境的研究基线。用户已在平板登录 Steam，实际屏幕显示及商店／游戏库点击切换通过；完整输入、持久会话及游戏启动尚未验收。

## 实际执行路径

```text
原版 Steam ARM64 / CEF / GNU 工具
  → 上游 GNU glibc（带显式维护的 OHOS 补丁）与按依赖闭包提供的标准库
  → GNU 专用 OHOS 适配层
  → OHOS 内核允许的真实系统接口

图形：GNU Mesa → 共用 Wine 的 virgl/vtest 接收端
窗口：X11 → Xwayland → 已有 OHOS Wayland 合成器
资源：Z:\SteamLinuxARM64，独立校验与更新
```

CPU 直接执行原版 AArch64 指令。GNU glibc 负责 TLS、线程、动态链接和标准 C/POSIX 接口。Steam/Valve ELF 保持原版；glibc、GNU Mesa 与 Xwayland 的宿主适配有独立补丁记录。GNU ABI 适配代码集中在 `entry/src/main/cpp/linux_steam/gnu_fixture/` 与 `patches/glibc/`；不载入 OHOS 主进程。

## rootfs 的作用

rootfs 提供 Linux 的目录布局、库、工具和配置文件。它本身不会进行 CPU 指令转译，目录更完整也不意味着持续的 CPU 开销更大。

OHOS 应用沙箱对可执行文件映射、软链接和部分系统调用的约束仍然存在，提供完整 rootfs 之后同样需要处理。当前保留真实 GNU 库及所需工具，按 ELF 依赖闭包和显式动态加载模块供给文件，以减少分发体积和维护范围；常规目录通过受限路径映射接入应用私有目录。

## 已有适配的通用能力

| 能力 | 实现与验证边界 |
|---|---|
| 外置 GNU ELF | 校验后匿名映射原始代码段，设置最终权限、刷新指令缓存并保留 BTI；原始 Valve ELF 不改 |
| 线程与 spawn | glibc 已有 legacy clone 路径，避开宿主禁止的 clone3 |
| 原生 exec | 单线程子进程内进入新 GNU 映像，保留真实 PID 与非 CLOEXEC FD；最多八代 |
| popen/pclose | 真实 fork、双向管道、GNU shell、exec 与 wait；返回真实退出/信号状态 |
| IPC | 应用私有 inode、真实共享映射和信号量；有明确容量及未实现范围 |
| 文件目录 | 受限 /tmp、代码/数据、CA/NSS 路径映射；普通 dirfd 和 /proc 查询保留真实行为 |
| 单实例保护 | O_EXCL、flock 与真实 Unix socket；只处理 Chromium 保留的 Singleton 元数据 |
| 可选能力探测 | 对已确认禁止的 Landlock、公开 close_range 调用返回 ENOSYS，使用程序自身回退；不报告虚假成功 |
| 描述符状态 | AArch64 GNU libc 使用原生 fstat 查询完整内核记录，避免宿主不支持的 socket AT_EMPTY_PATH 形式 |
| 本地连接身份 | 显式适配的 GNU lsof 保留上游扫描、筛选与输出；通过同应用进程的真实 FD 查询 TCP 状态，未跳过 Steam 校验 |

## 收敛原则与性能边界

逐项定位用于建立兼容基线。长期将这些机制按系统能力整理，而不是按游戏堆叠例外；每个适配都应具有明确宿主约束、真实语义、负面测试和可复用的边界。依赖由 manifest/hash 管理，未实现能力应明确返回错误。完整环境与最小环境可以使用同一适配层，后续新增库和工具不需要重写运行机制。

这仍是有限能力的 GNU 兼容基线，不是完整 Linux 系统调用实现。动态库接口拦截无法覆盖程序内联的直接系统调用，宿主权限也不能通过补齐文件获得。遇到这些边界应先确认是否存在保留语义的实现方式；若不能实现，应明确列为不支持，而不是伪造系统信息或跳过应用校验。

减少环境文件主要改善体积、导入和更新成本。运行性能应实测 CPU 原生执行、图形传输/拷贝、同步等待、内存占用；不能仅凭未使用 rootfs 推导帧率更高。

原生 exec handoff 已增加单线程子进程的映射回收：独立 RX 跳板进入新栈后释放退役 GNU 映像和旧匿名内存，保留新映像、当前引导 TLS、内核区域及鸿蒙初始栈/堆和签名宿主模块。全面回收曾导致手机 GNU rtld 早期 SIGSEGV，因此保留上述宿主边界；这仍不具备完整内核 exec 语义，外置代码段也占用匿名内存。主机 28 组真实执行回归与手机双游戏主菜单/窗口退出已通过，详见 STEAMWORKS_COMPATIBILITY。这是研究阶段需要继续测量、收敛的内存成本。共享 Mesa 接收端的 GLX 小程序通过，也不能直接证明 CEF 或 Proton 的完整 GPU 路径已经可用。

external-48 至 54 已区分两个真实约束：socket 的 AT_EMPTY_PATH 查询不适配，以及应用被拒绝读取 /proc TCP 表和 NETLINK_SOCK_DIAG。glibc 0007 修正前者；后者使用显式启用的应用内 FD 查询服务。服务以 SO_PEERCRED 验证真实 PID/UID、按 inode 核验 FD，再通过 SCM_RIGHTS 复制内核描述符；适配 lsof 使用 getsockname/getpeername/TCP_INFO 查询，不生成伪造的连接表。普通未登记进程仍保留查询失败。

该能力目前覆盖已登记 GNU 进程的 TCP/IPv4/IPv6，不能当作全局 Linux 网络视图。每个使用该服务的进程按需增加一个查询线程，查询有超时；512 次查询的 FD 泄漏回归、错误 PID、关闭与复用 FD、目录权限及正常退出清理均已覆盖。每轮诊断使用独立端点目录，宿主仅回收该轮自己命名的数字 socket；普通文件、软链接及未知名称保留。直接 raw clone、绕过 libc fork 的路径和仅继承而没有后续登记操作的 socket 尚未完整验证。

external-55 至 57 的原版客户端已越过本地连接错误并实际渲染 Steam 登录页。Xwayland 桌面扩大为 1280×800 后，700×440 登录窗口在 (290,180) 居中且内容完整。图像证据来自真实 X11 帧回读；设备屏幕显示、端到端输入、账号登录和游戏启动仍需独立验收。当前关闭 CEF GPU 加速的诊断配置不能当作 CEF 或 Proton GPU 支持证明。

模块边界、交付与验收状态见 [MINIMAL_GNU_RUNTIME.zh-CN.md](MINIMAL_GNU_RUNTIME.zh-CN.md)。

实机阶段记录及证据索引见 [CLIENT_PHASE_5.zh-CN.md](CLIENT_PHASE_5.zh-CN.md)。

2026-10-19 人工登录补充：用户已确认平板实际显示原版登录窗口并成功登录。登录后 SIGSEGV 定位为缺少标准 Xlib locale 数据导致 VGUI 标题转换失败；外置标准数据与 XLOCALEDIR 已补齐。v29 / external-63 通过实机标题转换、18 帧回读、共用 GLX 预检及 120 秒无段错误运行。external-64 经正常宿主桌面入口恢复前台后，实际屏幕已显示 Steam 商店、游戏库与好友窗口；HDC 注入的设备点击完成商店／游戏库切换、关闭好友窗口及返回商店。它验证 OHOS → Wayland → Xwayland → CEF 的点击路径，不等于所有键鼠／手柄或 CEF GPU 已验收；Proton/Vulkan、游戏运行及持久生命周期仍待后续。详见 CLIENT_PHASE_5。
