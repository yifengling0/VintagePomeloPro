# 原生 Linux 游戏的 FEX 与 Vulkan 适配

本轮以 Banana（2923300）的官方原生 Linux 安装链为输入，实现可编译、可交付的通用启动入口与 Vulkan WSI。依照用户安排，先完成源码和主机验证，再集中做平板验收；本轮没有连接或操作设备。**尚未证明真实 ARM64 FEX 能运行 Banana，也未证明实机 GPU 出帧。**

## 启动和路径

Steam 的 ARM64 reaper 仍负责启动与生命周期管理。只将已配置库内的 `FEX-Emu/fex-compat-tool`、`FEX-Emu/FEXCompatTool` 精确映射到 HAP 自有的 `steamohos_fex`；不改官方文件，不执行库内任意脚本。入口保留 run/waitforexitandrun、原始 argv 和 Steam 环境，识别 soldier/sniper 与 scout 包装层后，直接调用同一官方包中的 FEX。

运行环境复用 Steam 已安装 Runtime 下唯一的 `soldier_platform_*/files` 或 `sniper_platform_*/files`，不复制 rootfs。启动前检查 loader 时也临时采用该 rootfs 的绝对链接语义，例如 `/lib → /usr/lib`，检查后恢复环境。游戏须为当前安装目录内的 x86 ELF。Runtime 缺失或存在歧义、路径越界、非支持入口、缺少 FEXServer 等情况明确报 `STEAMOHOS_NATIVE_FEX status=failed stage=...` 并返回 126。

FEX 和同包 FEXServer/FEXServerManager/FEXBash 使用既有 GNU 用户态 loader，解决文档存储没有文件执行位的问题。权限仅在本轮 FEX 进程树生效，保留有界 exec generation；其他 Steam/Wine 执行仍走原路径。JIT 可执行内存沿用 OHOS prctl，文件 syscall 接入已有 depot 链接解析；futex_waitv/userfaultfd 仅在此作用域返回 ENOSYS，让官方 FEX 使用已有回退。

官方 Steam FEX 使用 `STEAM_COMPAT_DATA_PATH/fex-emu/Server`，不能仅设置 APP_DATA_LOCATION。入口在应用私有 IPC 根目录创建短服务目录，校验归属和 Unix socket 长度，仅为 FEX host/server 改写兼容数据路径；通过 FEX Config.Env 恢复游戏看到的原路径。游戏库、存档和 Steam 父进程不改。临时配置独占创建，只清理本次三个配置文件；长期服务目录供官方 FEXServer 使用。

保留 Steam appinfo 的 TSOEnabled/Multiblock 和用户 `STEAM_COMPAT_FEX_CONFIG` 优先级。HostThunks、GuestThunks 和 ThunksDB 必须来自同一官方 FEX 包；检测实际 ELF，缺少能力时不会凭设置把不存在的 thunk 声称为可用。Guest 清除 ARM64 preload/audit 和 host 库搜索路径，保留游戏自己的 Linux 库路径。

## GPU 和输入

HAP 新增独立 `libsteamohos_native_vulkan.so`，复用已交付 GNU Proton 的 Venus ICD 与 OHOS 接收端。只在 native FEX 作用域把 host thunk 的 Vulkan dlopen 定向到它；Steam CEF 保持原软件图形设置。

该伴随库实现 Xlib/XCB surface 创建、presentation 查询、扩展枚举/过滤、GetProc 路由和既有 Wayland WSI。X11 surface 使用独立 Wayland xdg_toplevel，验证 XID、读取标题并默认全屏；configure 回写 X 窗口尺寸，关闭发送 WM_DELETE_WINDOW。鼠标、键盘和滚轮通过 XTEST 发回原 X 窗口，合并同帧 discrete/continuous 滚轮，避免重复事件。

渲染图像继续经 `vn_winehua_present → Venus → OHOS NativeImage/NativeBuffer`。Wayland SHM buffer 只用于窗口几何声明，不传每帧渲染像素；未增加 CPU 像素读回。窗口初始握手有时限，事件线程有界轮询并在销毁时回收。

已有 Wine 的 `libproton_vulkan.so`、Wine/DXVK 数据包和 GPU 接收端均未替换。共享 WSI 源码通过编译宏增加 native 路径；旧 Wine 二进制仍来自原锁定 GNU 包。源码差异由明确的 HAP companion 白名单和输入/成果哈希覆盖，不能因此重写大 ZIP 的历史身份。

## 交付与主机验证

`LinuxSteamClientSupervisor.ets` 在启动时校验并原子安装 POSIX 适配器、FEX 入口和 native Vulkan companion。它们随 HAP 更新，**本次不需要重新导入 Steam 离线包**；仍需要通过 Steam 正常安装对应 FEX 与 Linux Runtime。

已完成的主机检查：

- `linux_steam_native_fex_test.py`：7 项真实 rtld/fork/子服务握手测试，覆盖无执行位、参数/环境、私有 IPC/guest 原路径、Steam 配置、退出码、TERM 和配置回收。FEX 使用主机 stand-in，只验证启动契约，不能当作 CPU 翻译验收。
- `linux_steam_native_window_test.py`：独立 Xvfb 与最小 Wayland server 验证 configure、XTEST 键鼠/单次滚轮、关闭和资源回收；同时编译 Wine/native 两种 WSI，同步、超时、交换链退休、submit 错误和队列互斥检查通过。它不调用 OHOS GPU。
- 原 GNU exec 的 28 个模式、depot 安装/校验/更新/链接和有界扫描回归通过；干净组件恢复的 13 项测试通过，覆盖伴随库遗漏、损坏和来源漂移拒绝。
- GNU ARM64 POSIX、FEX 入口和 native Vulkan 编译通过；Release HAP 编译通过，包内三个伴随模块、清单和两个原锁定 GNU 依赖包验证通过。

包体对照前一设备候选：Native 库只有 `libsteamohos_gnu_child.so` 因新增能力环境而变化，Wine 数据包、GNU Steam/Proton 大 ZIP 和 GPU 接收端相同。旧的全局 1.4.9 runtime-core 二进制门禁仍对 `libvirgl_child.so` 报哈希不符；前两份设备候选也是同一哈希，因此属于已有基线差异。另一个 graphics-package-identity 检查对旧候选与本次候选均报 `effective Wine patch order/registration`，本轮没有修改 Wine 补丁栈。两项均保留失败记录，未修改门禁或宣称完整发布/CI门禁已通过。

## 实机验收和剩余范围

下一轮先确认实际官方 depot 的 thunk/ThunksDB 布局、FEX JIT/server、x86 GNU loader，再验证 Banana Vulkan 初始化、持续 GPU 出帧、输入、退出与重开，最后回归已有 GNU Proton/Wine 游戏。错误应按入口、依赖、窗口、GPU 阶段分别记录，不能再把 Steam vulkandriverquery 的警告当作游戏错误。

当前还没有实现完整的 native GL/GLX 共享呈现链，普通游戏 shebang 脚本入口也尚不支持。音频/手柄仅保留现有能力环境和 thunk 检测，未完成 native 游戏实测。它是原生 Linux Vulkan 接入候选，**不是全部 Linux 游戏或全部 Steam Runtime 容器能力已经兼容的发布结论**。原诊断见 [安装与启动调查](DEPOT_LINKS.zh-CN.md)，小型结果见 [本轮证据](evidence/native-linux-fex-host.json)。
