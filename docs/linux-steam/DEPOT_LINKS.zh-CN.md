# Steam 安装库的符号链接适配

## 复现与原因

2026-10-27 用 Banana（AppID 2923300）复现：Linux 版要求 Steam Linux Runtime 1.0（scout，1070560）。Steam 已下载 75,258,128 字节，在暂存目录 `steam-runtime/amd64/etc` 创建符号链接时返回 `Disk write failure`，不是 CDN、账号或可用空间不足。此前 sniper 的 `pressure-vessel-arm64/bin/srt-logger` 报错属于同一类平台限制。

OHOS 应用在当前存储上不能创建真实 Unix symlink；简单切换 C/Z 盘路径不能补齐链接语义。

## 实现

`gnu_fixture/depot_links.h` 与通用 GNU POSIX 适配器一起编译。先尝试真实 `symlink`，仅在平台不支持或禁止时回退；缺少父目录、空间不足、已存在等错误不伪造成功。

- 作用域是配置的 Steam 库中 `steamapps/common/<安装目录>`、`steamapps/downloading/<AppID>`、`steamapps/temp/<AppID>`。不按游戏或 Runtime 名称列白名单；兼容 Steam 自动添加 AppID 的安装目录。
- 用一个小型普通文件保存原始目标字符串，提供 `readlink`、`lstat`、`fstatat`、`statx` 和目录项的链接类型。读取、打开、目录遍历、路径规范化等操作解析到目标；不复制目标资源。
- 按组件处理目录链接和 `..`，支持链式、悬空链接，最多跟随 40 次；循环返回 `ELOOP`。原始绝对目标可供 Steam 校验，但实际跟随限制在所属包内，越界返回 `EACCES`。
- `O_EXCL`、`O_NOFOLLOW`、相对 dirfd、创建时父目录、原子替换、更新提交和删除链接保留各自语义。私有 guard 协调并发创建和记录读取；创建失败清理本次记录。
- 同一条记录随 Steam 的目录提交/文件重命名移动，重新启动后仍可识别。正常支持 symlink 的 Linux 主机沿用真实链接。

一个记录仅包含格式标识与目标路径，通常几十至几百字节；实际磁盘占用以文件系统分配单元为准。目录或库文件不会因多个链接重复保存。此次官方 scout depot manifest 含 760 个链接，记录内容共 41,083 字节，最大记录 97 字节；平板逐个读取 stat 分配块数，实际合计 6,225,920 字节（约 5.94 MiB）。

## 交付与验证

更新 HAP 内的小型 `libsteamohos_posix_hap.so` 和输入/产物 SHA-256 清单，复用原 GNU 离线依赖包。已安装组件启动时从 HAP 更新适配器，Steam 子进程继承；不重打或覆盖 GNU 游戏依赖闭包中的不可变 POSIX 库。

主机回归 `host_tests/linux_steam_depot_links_test.py` 覆盖：真实 OHOS symlink 拒绝模拟、scout 目录布局、任意游戏包、读写与 no-follow、dirfd、statx、循环与悬空链接、并发排他、目录提交、重启后读取、原子替换、删除及存档保留。已有 CEF singleton、Cloud 路径、GNU exec 和基础文件系统回归同时验证。

实机结果见 [脱敏证据](evidence/depot-links-tablet.json)。官方安装器已完成暂存校验与提交，scout 显示 Fully Installed（StateFlags=4、BuildID=26115262），调度结果 No Error。提交后逐个核对 760 个记录的 SHA-256，全部匹配官方 manifest 的原始目标字符串。未另外执行菜单里的“验证游戏文件完整性”；安装完成和原生游戏运行分别记录，未验证项目不计为通过。

## 92% 长时间停留的调查

下载字节已齐时，Steam 仍需执行 Staging、Verifying Staged 和 Committing。此次进度停在 92% 的主要性能问题是适配器重复进行文件系统访问：每个 `readdir64` 条目又从包根目录逐级解析所有父目录，每个组件还重复执行 `lstat/lstat/stat`。平板 12 秒 CPU 采样中，前三个相关调用点合计占 76.32%；不能仅凭进度数字把它归因于网络。

- 路径组件复用同一次元数据查询；真实目录不打开为记录文件。
- 已打开的目录直接用 `fstatat/openat` 检查当前条目，不重复解析父目录。仅在 `opendir/fdopendir` 时识别该目录流的所属作用域，`closedir` 时移除；固定 512 槽、4 KiB 的原子表可安全继承到 fork，表满或 libc 内部目录流使用普通检查。
- 不缓存条目是否为链接、原始目标或目标内容，后续创建、替换和读取仍检查实际文件；目录重命名后相对已打开的 fd 访问。
- 主机测试扫描 256 个文件，浅目录需要 257 次元数据查询，增加七层目录后是 264 次，不再是每个文件重复七层检查；覆盖目录流复用、fdopendir、fork 和跨线程关闭。
- 仅在私有 IPC 目录存在应用所有的 `depot-profile.enabled` 时启用计时；按线程聚合、指数降低输出频率，默认关闭，不记录文件名或账号。实机验收后移除该诊断开关文件。

另有 Steam 自身的手柄配置 Workshop（241100）在启动后以 First 优先级抢占 Runtime，日志表现为 `Priority (Suspended)`。应将队列抢占、暂存工作和屏幕锁定后的系统挂起分别计时；不能把测试中断或重启前后的等待相加宣称为一次安装耗时。

优化后的一个缓存恢复轮次暂存耗时 28 秒，这不是完整安装耗时。随后通过 Steam 正常“重试”恢复此前中断提交留下的 604 个暂存缺失文件；没有手动移动文件、改安装状态或伪造校验成功。成功轮次的完整暂存校验为 05:40:38–05:42:53 UTC（135 秒），提交在 05:46:35 完成，05:46:36 调度返回 No Error。提交的 222 秒墙钟时间包含其他前台应用导致的挂起，不能当作纯 I/O 性能。当前文档存储的小文件校验和提交仍有分钟级开销；本次消除了重复父目录扫描的热点，尚未消除所有路径适配和底层存储开销。Banana 本体随后于 08:10:37 UTC 完成安装（StateFlags=4、BuildID=25834277、调度 No Error），运行仍未通过；安装结果不能外推为游戏执行成功。

## 边界

这是 Steam 安装和管理文件的 libc 适配，不是内核挂载。未加载该适配器的外部程序看到的是小型记录文件。下载成功不等于压力容器、任意 Linux x86_64 游戏或官方原版 Proton 已完整支持；这些仍取决于执行器、容器、图形和音频链路，不能把本次安装修复当作运行验收。

## Banana 安装完成后的启动调查

后续三次 Steam 启动均在同一秒或一秒内结束。实际路径是官方 ARM64 reaper → `FEX-Emu/fex-compat-tool` → soldier → scout → `Banana.x86_64`。游戏文件头为 Linux x86_64 ELF，不是 Windows PE，因此不会自动经过已接通的 GNU Proton/Wine 图形和音频链路。

已记录的最后一次 exec 尝试落在 FEX Python 脚本。`exec_compat.cpp` 仅对 `STEAMOHOS_CODE_ROOT` 内的 GNU 镜像作用户态重执行；游戏库里的脚本不在该范围内，进入普通内核 exec。文档存储中的该文件实际权限为 0660，同应用 shell 的只读帮助启动探针返回 inaccessible or not found。脚本还要求 `/usr/bin/env python3`，并默认依赖 `/usr/share/guestos/fex-mesa` 图形 provider/rootfs；当前最小 GNU 运行时未接入这套执行环境。未在 reaper 内取得 errno 专项跟踪，因此不能把 shell 文案单独当作精确错误码。

用户看到的 `VK_KHR_surface`、`VK_KHR_xlib_surface` 缺失及 `BInit - Unable to initialize Vulkan` 可归属到 Steam 的 `vulkandriverquery` 探针，发生在上述游戏启动之前。当前没有 Banana 图像入口或游戏渲染初始化的日志，不能将探针警告误判为本次游戏退出的直接原因。官方 reaper 上报退出码 0 也不能当作游戏运行成功。

要支持原生 Linux 游戏，需要通用地接入脚本/解释器及受约束的 ELF/FEX 执行、x86 GNU 依赖与路径语义，再对接现有 virgl/Vulkan WSI 接收端；不能只 chmod、增加一个 Vulkan 扩展名称或复制链接目标就宣称完成。未覆盖官方游戏库文件、未强制切换 Windows depot，也未修改已验证的 Windows 游戏启动路径。脱敏记录见 [原生启动证据](evidence/banana-native-launch-tablet.json)。
