# 原生 Linux 游戏的 FEX 与 Vulkan 适配

以 Banana（2923300）的官方原生 Linux 安装链为输入，实现通用启动入口与 Vulkan WSI。2026-10-29 已在原平板覆盖安装标准候选，确认官方 ARM64 Steam → 官方 FEX → x86_64 Unity → Venus/Maleoon 的链路，Steam API 与游戏脚本均完成初始化，GPU 有持续提交与呈现。**仍只有黄色背景，未完成可玩性验收，并复现过运行中整个应用退出；不能作为游戏兼容通过或稳定性结论。**

## 启动和路径

Steam 的 ARM64 reaper 仍负责启动与生命周期管理。只将已配置库内的 `FEX-Emu/fex-compat-tool`、`FEX-Emu/FEXCompatTool` 精确映射到 HAP 自有的 `steamohos_fex`；不改官方文件，不执行库内任意脚本。入口保留 run/waitforexitandrun、原始 argv 和 Steam 环境，识别 soldier/sniper 与 scout 包装层后，直接调用同一官方包中的 FEX。

运行环境复用 Steam 已安装 Runtime 下唯一的 `soldier_platform_*/files` 或 `sniper_platform_*/files`，不复制 rootfs。官方的精简 `/usr` 树需要按 `usr-mtree.txt.gz` 恢复链接：只创建缺失的链接元数据、必要目录及 `/usr → .` 别名，不复制或覆盖文件内容。解析有压缩/解压大小、行长、数量与路径边界限制；完整验证清单后才写入，已有冲突明确报错。缓存以清单内容指纹和 loader/根别名检查复用，避免每次遍历全部链接。启动前检查 loader 时临时采用 guest-root 的绝对链接语义，检查后恢复环境。游戏须为当前安装目录内的 x86 ELF。Runtime 缺失或存在歧义、路径越界、非支持入口、缺少 FEXServer 等情况明确报 `STEAMOHOS_NATIVE_FEX status=failed stage=...` 并返回 126。

## 共用路径契约

路径处理按资源类型统一，不按游戏名或 AppID 写例外：

| 资源 | 逻辑路径与实际位置 | 约束 |
| --- | --- | --- |
| 游戏与官方工具 | Steam 单一库、client 别名下的 common | 规范化后必须位于已配置库；FEX 只接受官方包内已列出的入口 |
| 兼容元数据 | `steamapps/compatdata/<appid>` 或现有 Cloud 层的私有映射 | 同时核对原始库路径和精确私有目标；其他越界链接拒绝 |
| Runtime | 官方 soldier/sniper 精简 `/usr` 树 | 清单恢复链接，绝对链接以选定 guest root 为根；不允许逃逸 |
| FEX 服务 | 独立私有短目录 | 按官方双斜杠、最大 UID 和 socket 长度计算；保留根目录/游戏归属记录 |
| ARM64 ELF | Steam release 与同安装内 FEX 包 | 仅 FEX 子进程扩展映射范围；优先使用配对 GNU 核心依赖，避免另一份 libc/libstdc++ 抢先加载 |

文件访问统一复用 `SandboxAtPath → FollowCloud/DepotResolve`，普通 libc API 和 FEX 直接调用的 `syscall(openat/faccessat/fstatat/readlinkat/statx)` 看见同样的文件、链接类型及目标；保留 dirfd、NOFOLLOW、EXCL、相对路径和 errno。ELF 的 preinit 早于 preload constructor，文件符号因此按首次访问初始化，constructor 复用同一结果，避免将未初始化误报为 ENOSYS。

这覆盖路径别名、文件 API、运行库装配与 ELF 加载的共同基础。它不等于完整 Linux 容器兼容；后续新 API、图形后端及设备节点仍须在此边界内补齐并验收。启动失败输出有界路径诊断和明确阶段，不输出完整 Steam 环境或认证凭据。

FEX 和同包 FEXServer/FEXServerManager/FEXBash 使用既有 GNU 用户态 loader，解决文档存储没有文件执行位的问题。权限仅在本轮 FEX 进程树生效，保留有界 exec generation；其他 Steam/Wine 执行仍走原路径。JIT 可执行内存沿用 OHOS prctl，文件 syscall 接入已有 depot 链接解析；futex_waitv/userfaultfd 仅在此作用域返回 ENOSYS，让官方 FEX 使用已有回退。

官方 Steam FEX 使用 `STEAM_COMPAT_DATA_PATH/fex-emu/Server`，不能仅设置 APP_DATA_LOCATION。入口在应用私有 IPC 根目录创建短服务目录，校验归属和 Unix socket 长度，仅为 FEX host/server 改写兼容数据路径；通过 FEX Config.Env 恢复游戏看到的原路径。游戏库、存档和 Steam 父进程不改。临时配置独占创建，只清理本次三个配置文件；长期服务目录供官方 FEXServer 使用。

保留 Steam appinfo 的 TSOEnabled/Multiblock 和用户 `STEAM_COMPAT_FEX_CONFIG` 优先级。HostThunks、GuestThunks 和 ThunksDB 必须来自同一官方 FEX 包；检测实际 ELF，缺少能力时不会凭设置把不存在的 thunk 声称为可用。Guest 清除 ARM64 preload/audit 和 host 库搜索路径，保留游戏自己的 Linux 库路径。

## 宿主与客体依赖隔离

官方 FEX 的 tiny-json 配置读取器不读取 JSON 数组形式的 `Env`。其 `StringArrayType` 对应官方 `SaveLayerToJSON` 的重复标量属性，例如 `"Env":"LD_PRELOAD=...", "Env":"LD_AUDIT="`。入口已按该格式生成配置，保留 guest 库路径与原兼容数据路径。主机测试保留重复属性读取，并用本地缓存的官方 tiny-json 源码实际解析 launcher 产物，确认四条 Env 均可读；旧数组格式被拒绝。

x86 客体 `libsteamohos_native_ipc.so` 纳入 rawfile 清单、恢复脚本和设备安装名单，客体 preload 提供 SysV IPC 的既有私有文件实现。此前 Env 数组被忽略，导致游戏直接触发 OHOS 禁止的 ARM64 `semget`（syscall 190）；修复后标准 HAP 冷启动已越过此处并完成 Steam API 初始化。FEX 宿主则完全移除继承的 `LD_PRELOAD`/`LD_AUDIT` 环境项（包括空值），由 rtld 的 `--preload` 装载 ARM64 适配器，避免将宿主与 x86 依赖混合。

不再因开启 native memory 适配而强制 `SilentLog=0`；详细 FEX 日志须显式启用。

## x86 客体代码映射

FEX 的原生 Linux vDSO 加载器会自最高地址按页向下尝试 `MAP_SHARED | MAP_FIXED_NOREPLACE` 加 `PROT_EXEC`。如果失败源于 noexec 文件系统而不是地址冲突，这个循环可能表现为长期单线程忙等。适配层仅在 native FEX 进程、只读 ELF 头确认是 x86/x86-64 的普通文件、原映射实际返回 EPERM/EACCES 时，以同一个 fd/offset/flags 重试，移除 **宿主** PROT_EXEC。x86 字节由 FEX 翻译器读取；FEX 自身仍按原 guest prot 跟踪执行属性。保留真实文件映射、共享回写和私有 COW，没有匿名复制或整文件缓存。

分配器使用有界、无额外堆分配的区间记录，mprotect 仅对完整覆盖的已知客体文件页进行相同回退；munmap、固定覆盖、mremap 与 fork 更新/继承记录。缺口、混合未知区域、表容量不足均保守保留宿主失败。ARM64 host ELF、JIT 匿名页和非 ELF 文件不适用此回退，其他 Steam/Wine 进程不启用。图形 NativeBuffer/NativeImage 不经过该机制。

## GPU 和输入

HAP 新增独立 `libsteamohos_native_vulkan.so`，复用已交付 GNU Proton 的 Venus ICD 与 OHOS 接收端。只在 native FEX 作用域把 host thunk 的 Vulkan dlopen 定向到它；Steam CEF 保持原软件图形设置。

该伴随库实现 Xlib/XCB surface 创建、presentation 查询、扩展枚举/过滤、GetProc 路由和既有 Wayland WSI。X11 surface 使用独立 Wayland xdg_toplevel，验证 XID、读取标题并默认全屏；configure 回写 X 窗口尺寸，关闭发送 WM_DELETE_WINDOW。鼠标、键盘和滚轮通过 XTEST 发回原 X 窗口，合并同帧 discrete/continuous 滚轮，避免重复事件。

渲染图像继续经 `vn_winehua_present → Venus → OHOS NativeImage/NativeBuffer`。Wayland SHM buffer 只用于窗口几何声明，不传每帧渲染像素；未增加 CPU 像素读回。窗口初始握手有时限，事件线程有界轮询并在销毁时回收。

已有 Wine 的 `libproton_vulkan.so`、Wine/DXVK 数据包和 GPU 接收端均未替换。共享 WSI 源码通过编译宏增加 native 路径；旧 Wine 二进制仍来自原锁定 GNU 包。源码差异由明确的 HAP companion 白名单和输入/成果哈希覆盖，不能因此重写大 ZIP 的历史身份。

## 交付与主机验证

`LinuxSteamClientSupervisor.ets` 在启动时校验并原子安装 POSIX 适配器、FEX 入口和 native Vulkan companion。它们随 HAP 更新，**本次不需要重新导入 Steam 离线包**；仍需要通过 Steam 正常安装对应 FEX 与 Linux Runtime。

已完成的主机检查：

- `linux_steam_native_fex_test.py`：26 项真实 rtld/fork/子服务握手测试，新增 Cloud 私有映射、越界拒绝、精简 Runtime 的链接恢复/缓存/不复制、清单遍历拒绝及同安装 ELF 作用域检查。另覆盖无执行位、参数/环境、Steam 配置、退出码、TERM 和配置回收；诊断测试验证原始 kernel sigaction 的查询、旧处理器、处理后继续执行及真实致命信号均保留。FEX 使用主机 stand-in，只验证启动契约，不能当作 CPU 翻译验收。
- `linux_steam_native_guest_ipc_test.py`：客体 IPC 库编译、动态加载和 IPC 语义检查通过；另有恢复/安装名单门禁，防止漏交付。
- `linux_steam_native_guest_mapping_test.py`：注入真实 noexec 错误后验证共享回写、私有 COW、偏移、固定地址冲突、mprotect、mremap、覆盖、fork 和 host/JIT 隔离；测试未执行 x86 指令。已加入 CI。
- `linux_steam_native_window_test.py`：独立 Xvfb 与最小 Wayland server 验证 configure、XTEST 键鼠/单次滚轮、关闭和资源回收；同时编译 Wine/native 两种 WSI，同步、超时、交换链退休、submit 错误和队列互斥检查通过。它不调用 OHOS GPU。
- 原 GNU exec 的 28 个模式、depot 安装/校验/更新/链接和有界扫描回归通过；干净组件恢复的 17 项测试通过，覆盖伴随库遗漏、损坏和来源漂移拒绝。
- GNU ARM64 POSIX、FEX 入口和 native Vulkan 编译通过；Release HAP 编译通过，包内三个伴随模块、清单和两个原锁定 GNU 依赖包验证通过。

包体对照前一设备候选：Native 库只有 `libsteamohos_gnu_child.so` 因新增能力环境而变化，Wine 数据包、GNU Steam/Proton 大 ZIP 和 GPU 接收端相同。旧的全局 1.4.9 runtime-core 二进制门禁仍对 `libvirgl_child.so` 报哈希不符；前两份设备候选也是同一哈希，因此属于已有基线差异。另一个 graphics-package-identity 检查对旧候选与本次候选均报 `effective Wine patch order/registration`，本轮没有修改 Wine 补丁栈。两项均保留失败记录，未修改门禁或宣称完整发布/CI门禁已通过。

## 通用适配的边界与后续扩展

下载目录只保存一份 depot 数据；链接记录、目录别名和已打开 fd 共同提供 Linux 文件语义。运行状态、Unix socket 和需要 POSIX 语义的兼容元数据进入精确的私有目录映射。扩展时按 API 或资源类型增加契约，不新增游戏名单，也不将任意路径重写进私有目录；已有文件冲突明确报告，不能通过覆盖用户文件来修复。

主机回归检查真实返回值、文件内容与生命周期，不以日志中“成功”字样代替语义验证。真机按安装提交、Runtime/ELF、执行、图形、音频和输入分别记录结果；noexec 的 x86 客体页适配不能推导出 ARM64 native 执行、GPU 出帧或所有 Linux 游戏已经通过。

## 实机验收和剩余范围

2026-10-29 原平板的标准 HAP 覆盖安装与冷启动已确认：

- 官方 FEX 与 FEXServer 正常装载，精简 Runtime 的 1666 条链接命中缓存，没有复制 depot payload。
- x86 文件 noexec 回退与备用栈信号上下文适配已越过早期故障，进入 Unity 2022.3.62f2；历史 SIGBUS/空备用栈故障不再是当前启动阻塞点。
- Unity 识别 `Virtio-GPU Venus (Maleoon 910)`；本轮有界计数记录 12 个成功 shader、6 个成功 graphics pipeline、64 次以上 draw 与 1100 次 texture-upload 命令记录，无已记录的 pipeline 创建失败。这是调用计数，不能证明每个 draw 的像素正确。
- `Steam API has been initialized with App ID: 2923300`、游戏加载与点击控制器初始化均出现；未再触发此前 syscall 190 的退出。
- 原生 ALSA 已建立 OHOS 音频流，但本轮没有实听验收，不能据此声称声音正常。
- 可见窗口仍为黄色背景，约 1–4 FPS，输入、正确画面、持续运行、正常退出与重开尚未通过；正常库下也观察到整个应用结束；后续有界上传对照捕获到系统以 `ResourceLeak:Gpu Leak`（前台 GPU 资源超限）向渲染进程及整个应用发送 SIGKILL。正常库那次的具体结束原因未单独捕获，不能仅凭系统命名认定驱动内存泄漏。

游戏还记录 `CollectionItem` 数据表缺失、网络返回格式异常和重复 CoroutineRunner 等 Unity 异常。它们是待分类证据，尚不能认定为黄色画面的唯一根因，也不能直接归为认证失败。

进行过仅在任务目录构建的全映射上传刷新对照；该诊断会覆盖 GPU 写回数据且增加传输开销，没有形成有效画面对照。它未写入仓库 rawfile 或 HAP，实验后恢复正式 companion 并核对 SHA-256。后续必须区分 CPU 上传与 GPU readback，不能把每次提交刷新所有映射作为通用修复，更不能破坏既有 Wine/DXVK 路径。

只记录 Vulkan 分配、不改变行为的对照显示：前 96 次分配时，最后记录的 live allocation 为 660705280 字节，均选中 memory type 0；设备暴露的 heap 0 为 11712405504 字节。该有界样本不是结束时总占用；系统日志的 `virtualSize=4575420` 未标单位，因此不换算成字节。仍须核对实际 allocation/free 生命周期和系统额度，区分真实泄漏与资源需求过高。

另做过仅修改客体可见 cached memory coherence 标记的非发布对照。它在 `vkFlushMappedMemoryRanges` 处触发 host 对 memory object 77 的查找失败，随后 Venus ring fatal、游戏 SIGABRT。该方案已撤回，不能作为兼容修复。实验后设备恢复的是已安装包的 companion 基线（native WSI：174840 字节，SHA-256 `b868bfad5e67c983049d95a5bf0b470117bedaef558950285651b34bf1ec29be`），GPU 计数诊断标记已移除。仓库 rawfile 新增的 native-only 内存生命周期 guard 为 175800 字节，尚未进入该设备安装包，两者不能混为同一版本。

本轮按用户要求仅做源码评审：[原生 Vulkan 内存同步分析](NATIVE_VULKAN_MEMORY.zh-CN.md)已用实际缓存函数体确认 legacy invalidate 与标准 Vulkan 读回语义的差异，以及 coherent 首次写入漏发布的分支。它们尚不能解释全部黄屏与资源增长。独立 NativeBuffer BLOB 探针已交叉编译，用于后续区分驱动共享映射与 Venus 协议；未在设备执行。仅在应用停止时恢复原有 VirGL 设置并读回核对，未操作任何设备前台。证据见 [内存同步记录](evidence/native-vulkan-memory.json)。

现有 NativeImage/NativeBuffer 呈现保留，无 CPU 渲染像素读回。普通 GL/GLX 游戏的完整共享呈现、任意 shebang 启动器及原生手柄验收仍未完成。原诊断见 [安装与启动调查](DEPOT_LINKS.zh-CN.md)，小型结果见 [本轮证据](evidence/native-linux-fex-host.json)。
