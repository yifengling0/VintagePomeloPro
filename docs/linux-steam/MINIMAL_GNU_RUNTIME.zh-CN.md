# 可扩展的最小 GNU 运行时

本阶段目标是为 OHOS 提供可维护的原生 ARM64 GNU 用户态，使原版 Linux Steam/CEF 能复用已有显示和图形接收端。2026-10-19 的实机基线已完成人工账号登录、商店与游戏库显示及设备点击切换；持久产品会话、完整输入和游戏兼容性尚未验收。

## 组成与边界

| 部分 | 当前供给 | 扩展方式 |
|---|---|---|
| GNU 基础 | 配对 glibc 2.38、TLS、线程、动态链接、原生 exec、私有 IPC 和文件路径适配 | 维护统一宿主能力及带版本的补丁，不按单个游戏伪造结果 |
| 标准工具 | GNU bash、taskset、明确标记的适配 lsof | 独立锁定来源、许可、依赖及 SHA-256 |
| 桌面 | Xwayland → 现有 Wayland；1280×800 默认桌面 | 有界尺寸配置；后续窗口管理和显示输出能力独立增加 |
| 图形 | GNU Mesa virpipe → 共用 Wine 的 virgl/vtest 接收端 | 共享接收服务；GNU 和 OHOS 的 libc/驱动 ABI 继续隔离 |
| Steam 配置 | 原版 Valve ARM64 ELF、网页、字体及主进程/CEF 依赖 | 外置独立更新；不把 Valve 代码和 GNU 资源嵌入 HAP |
| 诊断 | 真实 X11 窗口/画面回读、可选 XTEST 输入、连接身份和独立加载探针 | 显式启用、有界运行、单独记录结果；不替代端到端验收 |

CPU 直接执行 AArch64 指令，没有 QEMU、TCI 或跨架构指令翻译。外置 ELF 在校验后按段映射原始字节，GNU loader 接管隔离子进程；OHOS 主进程的 TLS 和 libc 不变。原版 Steam ELF SHA-256 为 `bd059af3d99c8b9979a5218838cce9ff44bef4c5ccc0b399b333db0ae1b84a89`。

rootfs 本身不增加 CPU 转译开销。当前精简的价值是明确依赖、减小分发和更新范围；不能仅凭文件更少承诺更高帧率。主进程和 CEF 的依赖闭包不等于随包所有辅助工具的完整 Linux 环境，例如独立图形探测工具的依赖仍需按能力补齐。

## 外置交付

`Z:\SteamLinuxARM64\releases\<manifest SHA-256>\runtime` 保存运行库和工具，`client` 保存 Steam 用户数据，`xkb-data` 保存键盘数据，`x11-locale` 保存标准 Xlib 语言数据。适配入口留在 HAP，GNU/Valve 资源不随 HAP 打包。激活前检查清单和原版 Steam 身份，激活后保留已验证回退版本；仅按清单回收自己的旧资源。

当前 v29 验证清单包含 247 个原生 ELF，合计 745640928 字节；其中包含原版 Valve 库和诊断程序。Steam 数据 ZIP 为 108328798 字节。它们是磁盘资源大小，不是运行 RSS 或最终最小分发目标。候选包的打包检查记录 `bundledGnuFiles=0`、`bundledSteamFiles=0`。

新增能力应提供自身的文件、来源与许可清单、ELF 依赖及显式动态模块、宿主适配范围、探针和验收状态。继续复用现有构建清单与独立 release，不引入常驻包管理器或把完整发行版当作当前必需条件。

## 本轮验证

- glibc 七补丁在固定 2.38 源码包上按构建顺序使用 `--fuzz=0` 应用通过，原生 fstat 已在平板成功查询 socket。
- 连接身份探针识别真实父子 TCP 连接；主机模拟 /proc socket stat 和 TCP 表不可用时，适配 lsof 仍取得真实两端；关闭 opt-in 后查询失败。
- FD 查询验证错误 inode、错误服务端 PID、关闭/复用 FD、目录权限、512 次查询无 FD 泄漏及正常退出端点清理。
- GNU 桌面监督程序不改客户端，读回真实窗口和像素，保留子进程退出状态。主机 Xvfb 验证窗口、已知颜色、XTEST 字符输入和退出 37。
- 平板 external-57：原版登录窗口为 700×440，位置 (290,180)，根窗口为 1280×800；界面、图片和字体完整渲染。
- 平板 external-58：会话内独立查询端点工作正常，本地连接错误未复现；标准 XTEST 向真实 CEF 焦点窗口发送 8 个字符，帧回读确认账号框显示 `hmosdeck`。候选 v24 已编译、签名、安装；未输入密码或提交登录。

平板 XTEST 仅用于验证 CEF 的 X11 输入路径，固定输入 `hmosdeck`，不输入密码、不提交表单。即使此项通过，也不代表 OHOS 触摸/实体键盘/鼠标输入已经贯通。

当前入口仍是有界诊断会话，已为人工登录扩展为最长 15 分钟，期限结束后回收进程；不是可长期交互的产品启动入口。external-64 实际配置未启用 desktopSession、自动截图、XTEST 输入或信号追踪；本轮另做的手工设备截图与点击单独记录。

## 后续验收

账号登录、平板主界面呈现及 Wayland → Xwayland → CEF 的设备点击路径已经验证。下一阶段验证持久会话、正常退出／重启、完整输入及可控程序启动。诊断配置当前关闭 CEF GPU 加速；共享接收端的 GLX 18 帧与 GNU Vulkan loader 装载通过，均不能外推为 CEF、Vulkan 游戏或 Proton 已支持。

Windows 游戏接入按当前方案保留已有 OHOS Proton/Wine、DXVK 与图形后端，不以 GNU Vulkan ICD 为前置条件。只有未来另行支持 GNU Vulkan 游戏时，才需独立实现并验证 GNU loader／ICD 到 OHOS 驱动与窗口系统的调用、同步和资源互通。x86/x64 游戏继续使用当前 FEX 等已支持后端。Steam ARM64 原生执行与这些游戏后端分开验收。

诊断 JSON 中的 `steamClientStarted`、`inputVerified`、`gpuAccelerationVerified` 当前仍为保守的固定 false，不用于否认已完成的人工登录／画面与点击测试；分项证据记录于 minimal-gnu-runtime-validation.json。完整产品验收尚未完成，不能将这些常量作为各分项的自动检测结果。

2026-10-19 人工登录补充：用户已确认平板实际显示原版登录窗口并成功登录。登录后 SIGSEGV 定位为缺少标准 Xlib locale 数据导致 VGUI 标题转换失败；外置标准数据与 XLOCALEDIR 已补齐。v29 / external-63 通过实机标题转换、18 帧回读、共用 GLX 预检及 120 秒无段错误运行。external-64 已在平板实际显示主界面，并通过设备点击完成商店／游戏库往返及关闭好友窗口。CEF GPU、Proton/Vulkan 和游戏运行仍未验收。详见 CLIENT_PHASE_5。

Steam 与现有 OHOS Wine 的认证 ABI 边界不通过改为 GNU Wine 解决。2026-10-19 已在平板由独立 GNU 辅助进程加载官方 steamclient.so，并由当前 Wine 下的 Windows x64 PE 经有版本的本机诊断协议取得真实登录状态，验证 user／pipe 释放。此项只读快照验证不等于完整 lsteamclient backend；SteamAPI_Init、回调与票据仍待实现。详见 STEAMCLIENT_PROBE.zh-CN.md。显示融合与游戏认证分别验收，见 GAME_LAUNCH_DESIGN.zh-CN.md、DROIDDECK_REVIEW.zh-CN.md。
