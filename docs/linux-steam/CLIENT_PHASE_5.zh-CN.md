# 阶段 5：原版 Steam Linux ARM64 客户端接入（进行中）

日期：2026-10-19。目标仍是原版客户端显示可交互界面，局部验证不代表 Steam 已可用。

## 当前结果

- 原生 AArch64 GNU rtld 执行，无 QEMU、TCI 或 CPU 指令转译。原版 Steam ELF 未修改，SHA-256 为 `bd059af3d99c8b9979a5218838cce9ff44bef4c5ccc0b399b333db0ae1b84a89`。
- 第 external-09 轮用仅跟踪本次诊断子进程的 ptrace 找到 SIGSYS 根因：`clone3`（AArch64 syscall 435）。旧 glibc 补丁只覆盖线程创建，`posix_spawn` 另有直接 `__clone3` 路径；信号被屏蔽且沙箱直接终止进程，普通 SIGSYS handler 无法留下记录。
- 新补丁 `0004-ohos-spawn-with-legacy-clone.patch` 使用 glibc 原有 legacy clone fallback，保留其信号处置与实际子进程语义；不修改宿主策略、不伪造成功。
- 第 external-10、11 轮已越过该处，无 SIGSYS，转为 Steam 的明确 fatal exit 1：`src/vgui2/src/surface_linux.cpp (1994): glXChooseVisual failed`。`steamui.so`、VGUI、ChromeHTML、SDL3 已实际加载，模块使用 namespace 0、RTLD_NOW，未发现新 namespace 绕过适配。
- 第 external-15 轮已通过 GNU Mesa softpipe/Xlib OpenGL 3.3 验证：交替 18 帧 RGB 绘制、glReadPixels 和 X11 窗口像素均通过。当前 Xwayland 为 Wayland SHM 输出，未启用服务器 GLX、glamor、DRI3；使用 Mesa Xlib GLX 实现，不伪称服务器支持 GLX。软件 GL 通过不代表硬件加速或完整 Steam 启动通过。
- `steamClientStarted`、`inputVerified`、`gpuAccelerationVerified` 仍为 false，套件保持 FAIL。

## 共用 Wine 的 Mesa 接收端

现有接收端以 `--multi-clients` 运行；GNU Mesa 和 Wine Mesa 通过同一 vtest socket 连接既有服务。NAPI 捕获当前 broker socket，通过 NCP 参数传入，只设置 GNU 子进程环境。架构及验证边界见 [SHARED_MESA_RECEIVER.zh-CN.md](SHARED_MESA_RECEIVER.zh-CN.md)。

- external-12 修正测试窗口深度不同造成的 BadMatch。
- external-14 定位 Xwayland 因 X11 MIT-SHM 调用被拒绝的 SysV IPC，现显式禁用该扩展并设置 `XLIB_NO_SHM=1`。Wayland SHM 输出继续使用。
- external-15 软件路径已通过，原版 Steam 已越过 `glXChooseVisual failed`。后续仍有更新器窗口 BadMatch、字体资源/配置及部分子进程 exec EACCES；未出现可交互登录页。
- external-16 共享 virpipe 在着色器缓存初始化崩溃，PC 对应 `build_id_length`，调用方为 `virgl_disk_cache_create`。新 GNU 工具链未默认添加 ELF Build ID，release 构建在 assert 后解引用空 note。构建已显式加入 `-Wl,--build-id=sha1`，并在产物阶段校验真实 SHA-1 Build ID；Mesa 源码无需为此修改。

- external-17 成功识别 `virgl (Maleoon 910)`、OpenGL 2.1，随后在 Xlib swap 传入空 pipe_context 的 `virgl_flush_eq` 崩溃。
- 新 `patches/mesa/0001-xlib-present-with-a-valid-pipe-context.patch` 只应用到隔离 GNU Mesa 源码副本，复用 drawable 的 helper context 完成呈现；Wine Mesa 子模块仍干净停留在 `557fa3e781ea6e78fcff3ee759c25f60c3055064`。
- **external-18 共享接收端验证通过**：`sharedReceiverVerified=true`，renderer 为 `virgl (Maleoon 910)`，18 帧 RGB 的 GPU 回读与 X11 窗口像素全部通过，GLX 预检 exitCode=0。此结果不等于 Steam 完整登录或零拷贝通过。

- external-19 用 30 秒原版 Steam 启动窗口复测，GLX 18 帧再次通过，`sharedReceiverVerified=true`。Steam 后续仍受字体/配置、更新器 BadMatch、命名共享映射及子进程 EACCES 阻塞；达到诊断时限后正常回收，未标记完整客户端启动成功。

## Z 盘外置资源

HAP 不包含 Steam、GNU ELF 或 Steam 数据 ZIP。external-v6 unsigned HAP 为 405530170 字节（约 387 MiB）；早期内嵌候选约 1.25 GB。资源以独立目录导入和更新，无需为每次 Steam 文件更新重新打包 HAP。

```text
Z:\SteamLinuxARM64
  releases/<外层 manifest SHA-256>/runtime/   原版 ELF、GNU 库、数据 ZIP
  client/.steam/steam/                     可写客户端资源与日志
  xkb-data/
  active-version.txt
```

实际位置由与 Wine Z 盘一致的 DOWNLOAD picker 解析 app-owned Download 目录。仅显式诊断套件读取本机 HDC staging；没有内嵌 Steam fallback，也不自动下载第三方资源。

- `prepare_linux_steam_external.py` 逐文件校验并导出 payload；数据 ZIP 禁止路径穿越、符号链接与 ELF。
- ArkTS 逐文件核对源和 Z 盘副本的大小、SHA-256，全部完成后写 `.verified` 并激活版本。外层 manifest 内容的 hash 就是版本，缓存 manifest 也重新校验。
- 新版可仅上传变更文件，其余从前一 Z 盘 release 复用；源 hash 不一致时拒绝激活。第 external-10 轮已验证清空 staging 后的增量导入，第 11 轮验证无 staging 的再次启动。
- 激活后仅删除 manifest 列出的已知私有 staging 文件；第 external-09 轮确认 `stagedFilesRemaining=0`。旧 release 保留，不扫描或清理用户的其他目录。
- Z 盘不支持 chmod，字节相同副本直接文件 RX mmap 仍为 EACCES。显式外置模式将原始 ARM64 ELF 段复制到匿名 RW 内存，刷新指令缓存后设置最终页权限，拒绝 WX。CPU 直接执行原始指令；这不是 CPU 转译。默认签名文件映射路径保留。

## 已验证的兼容能力

- GNU main、TLS、线程、版本符号、dlopen、析构、退出37、超时 SIGKILL/回收，NCP 父进程 TP 保持不变；外置实机见 `tablet-z-gnu-04.json`。
- 真实私有 SysV IPC 子集：key/exclusive、复合操作原子性、SIGKILL 后 SEM_UNDO、超时、删除与跨进程共享内存。
- 标准 XOpenDisplay → 私有 X11 socket → Xwayland → 现有 OHOS Wayland，18 帧 Xlib 绘制和真实像素读回。
- GNU 私有 `/tmp`、可信 CA 精确路径映射、`/proc/self/exe`、代码/数据资源查找、chdir、真实私有 FIFO；不关闭 TLS 证书验证。
- 原版 `-child-update-ui` 经原生 GNU exec handoff 执行，保留 PID、argv/env、非 CLOEXEC FD，关闭 CLOEXEC，重置信号。11 种 exec/相对路径/GNU shell/taskset 场景在两种内存模式下的 22 次主机回归均通过，包含真实 CPU 亲和性校验。

## 诊断与限制

`client-launch.conf` 只接受受限 CEF 参数、1000–60000 ms 超时，以及显式 `diagnosticSyscalls=1`、`softwareOpenGL=1`、`mesaDriver=virpipe`。调用跟踪默认关闭，只跟踪自己 fork 的 Steam 主进程；32 项有界历史、固定期限、退出回收，无任意进程 attach。主机测试覆盖正常退出、真实 seccomp SIGSYS 与超时。

原生 exec handoff 目前只允许同架构 ELF、单线程子进程、最多八代；旧映射保留，尚非完整 kernel exec。此接口直接处理同架构 ELF；原版脚本由外置原生 GNU bash 解释，未引入跨架构指令转译。未实现语义明确报错。IPC 限制为每组64信号量、256组、64 undo owner、单共享段256 MiB；完整 Linux IPC 权限及所有控制命令未验收。

尚需验证 CEF 子进程、网络、登录界面、实际键鼠输入、GPU/OpenGL/Vulkan 互通。GNU Vulkan/Proton 不能从现有 Xlib 或软件 GL 通过外推为已支持。

## 实施文件与证据

- 代码：`entry/src/main/cpp/linux_steam/`、`entry/src/main/ets/service/LinuxSteamDiagnosticData.ets`、`patches/glibc/`。
- 构建：`scripts/build_linux_steam_{glibc,client,software_gl}.py`；外置导出与 HAP 排除由 `prepare_linux_steam_external.py`、`package_linux_steam_gnu_fixture.py --external-runtime` 完成。
- 主机测试：`linux_steam_exec_test.py`、`linux_steam_filesystem_test.py`、`linux_steam_loader_paths_test.py`、`linux_steam_trace_test.py`；glibc 五补丁在干净 2.38 源包上另做应用验证。
- 本机证据：`artifacts/steam-linux-arm64-client-20261019/`，重点为 `tablet-steam-external-09.json`（clone3）、`10.json`/`11.json`（已越过、GLX fatal）、`tablet-import-progress-09.json`、`external-hap-payload-audit-v6.json`。

未 stage、commit、push，原有 staged 修改保持不变。


## 原生系统服务与 Chromium 初始化（2026-10-19 续）

external-20 至 external-27 的实机推进：

- POSIX `shm_open/shm_unlink` 使用应用私有真实 inode，支持创建/独占、跨进程共享、CLOEXEC 和删除后已有映射继续可用。`external-22` 起，独立预检在平板验证 robust mutex 持有进程被 SIGKILL 后，内核返回 EOWNERDEAD，再执行 consistent 和正常解锁。
- 系统禁止 `get_robust_list`（AArch64 syscall 100）。配套 GNU libc 导出读取当前线程真实 `robust_head` 的私有接口；只允许 tid=0 或当前 tid，其他线程/进程返回 EPERM。没有伪造空链表，也没有移除锁的恢复语义。
- 已实测的 EACCES 来自 `/bin/sh`；本轮未观察到公共 `posix_spawn` 参与，不能归因于其内部 exec。外置 Debian ARM64 bash 与 util-linux taskset 保留正常 shell/fork、CPU 亲和性和后续 native exec，原版 Steam 脚本不改。系统 taskset 拒绝原脚本的 `0x7c` 掩码格式，GNU 版本接通后原版 `steamwebhelper` 已执行到 Chromium 初始化。
- `~/.steam/root` 的精确配置别名映射到真实安装目录；GNU Fontconfig 使用原版字体资源及原子更新的应用私有配置。字体文件断言与默认配置缺失日志不再出现。
- `external-25` 的 Xwayland 诊断证实窗口使用 depth=24、visual=0x6e，visual 与该深度不匹配；并非先前推测的缺少 border 属性。GNU Mesa 优先选择满足深度/类约束的屏幕默认 visual，保留显式 visual 覆盖和不满足约束时的完整搜索；`external-26/27` 的 BadMatch 消失，18 帧 GLX 共享接收端验证继续通过。Wine Mesa 子模块不变。

当前界面尚未通过验收。Chromium 仍在处理宿主沙箱不支持的 Linux 能力探测；`steamClientStarted`、`inputVerified`、`gpuAccelerationVerified` 保持 false。

诊断宿主增加了本轮网页子进程日志的有界尾部采集及输出容量控制，避免把 helper 的真实退出原因淹没在主进程重试中。GNU syscall 转发使用原架构汇编保留参数寄存器，未做可变参数个数猜测，也没有指令解释。

## Chromium 单实例保护与加密模块（external-29 至 32）

- external-29 已越过 Landlock 探测：公开 GNU syscall shim 对宿主未提供的能力返回 ENOSYS，使用 Chromium 原有不支持分支，没有执行被禁止的系统调用。
- external-30 的真实预检确认应用私有目录也禁止 symlink（errno=13），仅搬动缓存不能解决 SingletonLock。
- GNU 文件适配仅为 Chromium 保留的 SingletonLock、SingletonCookie、SingletonSocket 提供私有普通文件元数据表示，使用真实 O_EXCL 和跨进程 flock；缓存置于私有目录。Unix socket 仍实际 bind/connect 并传输数据，未禁用单实例保护。普通软链接保持宿主原有行为。
- 主机验证覆盖两进程竞争（八轮只能一个成功）、readlink 截断、lstat/fstatat、dirfd、删除和真实跨进程 socket 请求/响应。
- external-31 的平板日志不再报告 SingletonLock / ProcessSingleton 失败，已进入网络、NSS 和图形子进程初始化。
- external-31 暴露 `libsoftokn3.so` 缺失导致 NSS fatal。构建现在显式包含 DT_NEEDED 无法发现的 NSS 动态模块及依赖；external-32 中模块实际加载，NSS fatal 消失，持久化数据库仍报错后使用 NSS 自带 fallback。
- 当前明确阻塞为 CEF 图形子进程重复 exit=139/133，最终 `GPU process isn't usable`。GLX 小程序仍通过，不把它当作完整 Steam 图形成功。登录页、交互和实际游戏启动仍未验收。

external-33 在资源导入阶段遇到设备 ENOSPC，尚未执行新 preload。后续宿主加入旧运行库保留策略：导入前保留 active、pending 及一个已验证回退版本，激活后保留新旧两版；只删除 manifest/hash 都一致的旧 runtime 文件，不递归删除目录。清理测试验证用户修改内容、未知文件、软链接与共享 client 数据保持完整。

当前能力方向、ARM64 Steam 与 x86/x64 游戏后端的区别见 [HmosDeck](HMOSDECK.zh-CN.md)。

## external-34 与空间回收

v12 宿主 ArkTS/native 编译、外置资源打包和签名通过。为满足系统安装临时空间要求，12 个旧诊断 HAP 的设备临时副本先备份到电脑并逐一核对 SHA-256，再删除确切临时文件；可恢复备份共 4164315504 字节。

external-34 的旧 release 清理实测删除 6867 个校验一致文件，回收 20269387030 字节，跳过 1 项；active/pending 与回退版本按设计保留。设备随后可用空间约 36 GiB。清理阶段不执行 Steam，不能将其时间归入客户端启动耗时。

该轮原版 Steam 再次运行，私有 NSS 数据库路径映射消除了先前的持久化初始化错误，共享 Mesa GLX 仍通过。收窄 `openat/fstatat` 路径适配保留普通 dirfd、已删除目录与 /proc 的真实语义，新增主机回归通过；但实机 CEF 图形子进程的 exit=139/133 仍存在，不能认为此处崩溃已修复。客户端验收字段保持 false。

v13 增加只读、有时间和条目上限的空间统计套件 `linux-steam-storage`，用于区分 HAP、应用私有运行环境、缓存及外置 Steam 数据；不会读取文件内容或执行清理。HAP 自身为 405558630 字节（v12），Wine 数据 ZIP 267613711 字节，解压内容 1163020169 字节；这些数值不足以解释设备设置显示的全部 7 GB，还需依据设备统计定位。

## 平板 7 GB 占用定位与定点回收（2026-10-19）

模块 filesDir 之外的应用级目录统计确认，私有数据为 7511751887 字节（7.00 GiB），其中 C:\windows\temp 占 4208172517 字节（3.92 GiB）。根因是此前 Unity／BC 兼容测试上传的多份纹理包、修改版和回读副本；并非 HAP 内嵌 Steam 资源。

v15 诊断包增加 Wine C 盘层级的只读大小分解。67 个明确旧测试文件先备份到电脑并核对 SHA-256，再按确切路径回收，共 4044717359 字节（3.77 GiB）。复测私有数据降至 3467077325 字节（3.23 GiB），临时目录剩余约 156 MiB；原游戏、存档、Wine 系统文件及 Z 盘当前资源未纳入回收。

签名 HAP 为 407413577 字节（388.54 MiB），不含 Steam／GNU 文件。统计未截断，两次各有 13 个元数据访问错误；采用普通文件逻辑大小，不能将 ArkWeb 共享挂载与应用独占代码重复累计。证据为 `tablet-storage-audit-37.json`、`tablet-storage-audit-38.json`、`texture-fixture-backup.local.json` 及 `STORAGE-AUDIT.zh-CN.md`，均在本机 artifacts/steam-linux-arm64-client-20261019。

后续测试仅上传明确资源清单，结束后按清单回收；不对整个 Windows 临时目录做无差别自动清理。Steam 完整启动验收状态不变，原有暂存区保持不变。

## AArch64 BTI 与原版 Vulkan loader（external-39 至 42）

独立 GNU 图形加载探针隔离运行 `dlopen("libvulkan.so.1")`，只在探针子进程记录故障 PC、LR 和模块地址；没有向原版 Steam 注入 SIGSEGV handler。external-40 中，普通和原始 `newfstatat` 的 /proc/self/fd 查询均成功，但 Vulkan 加载子进程退出 139。PC 相对 GNU loader 为 `0x12ce4`，对应 glibc `_dl_bti_protect`。

原版 Valve Vulkan 库声明 BTI/PAC。glibc 为开启 BTI 再次执行文件 RX `MAP_FIXED`，绕过外置匿名 PT_LOAD 适配；宿主拒绝该文件映射后，旧地址内容被破坏，访问下一 program header 时崩溃。新增 `0006-ohos-protect-external-bti-in-place.patch` 将外置模式校验抽到共享内部头，对已校验的匿名外置映射用 `mprotect` 实际开启 BTI，保留失败错误；正常文件映射继续原路径。不修改 Valve ELF，也不关闭 BTI。

六个 glibc 补丁在干净 2.38 源码上按构建顺序以 `--fuzz=0` 应用通过，增量编译通过。external-41/42 实机独立加载探针均退出 0，实际取得 `vkGetInstanceProcAddr`；CEF 中此前重复的 GPU exit 139/133 和 `GPU process isn't usable` fatal 均不再出现。共享 virpipe 的 18 帧验证继续通过。

此结果只确认库装载修复，不代表 GNU Vulkan 驱动或 Steam 界面可用。external-42 将观察时间从 30 秒延至 60 秒，Steam 在约 36.4 秒因 SIGSEGV 退出，之前报告内部 `Failed to connect to websocket`。下一步采集 CEF 详细日志及真实本地 TCP 结果；登录、输入和完整图形验收仍未通过。

证据：本机 `tablet-steam-external-40.json`、`41.json`、`42.json`，以及 `glibc-bti-clean-patch-validation.json`；宿主 v17 增加有界 CEF/console 日志采集。独立加载探针的主机测试覆盖真实 DSO 正常装载和构造器 SIGSEGV 隔离。

## 本地 WebSocket 身份核验（external-43，继续验证）

external-43 的完整日志确认 CEF 已创建 Shared JS Context，并实际加载、执行原版 `steamloopback.host` 网页资源。本地 loopback bind 成功，CEF 发起非阻塞 TCP connect；Steam 服务端收到连接后调用 `/bin/lsof -P -F upnR -i TCP@127.0.0.1:<port>`，反复报 `GetIPCConnectionDetails: popen failed`，随后 WebSocket 初始化超时。

GNU libc 内部 popen/spawn 使用隐藏的内核 exec 路径，绕过 preload 中的公开 exec hook；应用环境缺少可直接内核执行的 `/bin/sh`。新增 GNU 专用 `popen_compat.c` 以真实 fork/pipe/exec/wait 实现外置模式的子进程调用，继续使用已验证的 GNU 原生 exec handoff。没有配置外置 GNU 根时回到系统 libc 的 popen/pclose。连接身份检查没有跳过，也没有生成伪造的 lsof 输出。

`/bin/lsof` 与 `/usr/bin/lsof` 精确映射到 Z 盘 checksum-locked Debian ARM64 lsof 4.95.0-1；新增 libtirpc 的依赖闭包与许可记录。主机 24 项 exec 场景通过，包括两种映射模式下 popen 的读写、CLOEXEC、真实退出码、信号退出、多线程调用和缺少 shell 的错误返回。

external-44 已实际启动 lsof，随后因宿主禁止的 `close_range`（AArch64 syscall 436）退出 159。原版 lsof 反汇编确认其已有逐个 close 的回退；外置模式的公开 syscall 路由返回真实的 ENOSYS，使用该回退，不报告假成功。filesystem 主机回归覆盖 opt-in 下保留 FD 后逐个关闭，以及非 opt-in 下原系统调用行为。

external-45 中 popen 错误与该 SIGSYS 均消失，lsof 转为退出 1，未得到连接身份。该轮 Steam 主进程在约 37.4 秒 SIGSEGV 退出，此前仍报告 `Failed to connect to websocket`；独立 Vulkan 装载探针退出 0、共享 Mesa 18 帧验证继续通过。下一步需定位 lsof 读取的真实进程/连接信息，尚不能认定 /proc 权限为根因。未伪造 lsof 输出，也未绕过 Steam 的连接身份校验；登录界面、输入及完整 GPU 验收状态不变。

## 真实 socket 查询与登录页渲染（external-48 至 57）

独立父子 loopback 探针确认 bind/connect/accept 和 /proc fd readlink 成功，但 glibc 的 fstat 返回 EBADF；直接 SYS_fstat 成功。原因是 glibc 2.38 使用 newfstatat(fd,"",AT_EMPTY_PATH)，宿主不支持对 socket 使用该形式。新增 glibc 0007 使用原生 AArch64 fstat，保留内核完整 stat 与错误。七补丁在固定源码归档上以 fuzz=0 顺序应用通过，增量编译和实机验证通过。

另一个独立限制是 /proc/net/tcp、各进程 net/tcp 与 NETLINK_SOCK_DIAG 均被拒绝 EACCES。新增 opt-in 的同应用 GNU 进程查询服务：SO_PEERCRED 核验 PID/UID，按真实 inode 验证 FD，SCM_RIGHTS 复制后由查询方再次核验。适配 GNU lsof 保留上游进程扫描、筛选、field 输出，用真实 socket 查询 TCP endpoint/state；不构造假的 /proc 表，也不取消 Steam 身份验证。source、patch、工具 hash 和许可说明记录在外置 manifest。

external-53 只识别到监听端；external-54 加入有界阶段诊断后，真实父子连接两端均识别成功。没有把这一变化归因为已经确定的时序根因；后续完整客户端用于确认实际链路。external-55/56/57 不再出现此前的 GetIPCConnectionDetails/WebSocket 初始化失败，已创建 SP DesktopLoginWindow 并渲染原版登录页。主进程持续到诊断的 60 秒期限后被结束，不能将该超时记为自然崩溃。

独立 GNU desktop_session 通过 XQueryTree/XGetWindowAttributes 和 XGetImage 观察原版客户端。external-56 实际回读到登录页，但诊断根桌面只有 640×360；external-57 将 Steam 桌面默认改为 1280×800 后，700×440 登录窗口自动居中 (290,180)，全部内容可见。普通 X11 小程序仍使用 640×360；可配置尺寸受限在有效范围和总像素上限内。

主机回归覆盖受限 proc、opt-in 关闭、512 次真实 FD 查询无泄漏、错误 inode、关闭/复用 FD、错误服务端 PID、权限、正常退出，以及 session 端点回收不删除普通文件/软链接/未知名称。GNU 桌面监督程序在真实 Xvfb 验证窗口、像素、XTEST 字符和子进程退出码；独立输入探针不提交登录表单，不代替 OHOS 输入验收。

新增端点按每轮私有 session 分隔，宿主在回收该轮子进程后仅移除本轮数字 socket，处理 _exit 或超时留下的端点。普通应用及 Wine 不启用该能力。

证据：`tablet-steam-external-48.json` 至 `57.json`、`glibc-seven-clean-patch-validation.json`、`socket-metadata-host-tests.log`、`desktop-host-tests.log`。`steam-login-57-review.png` 为真实 X11 帧回读的复核副本，二维码已遮挡；原始帧仅留本机。候选 v22 已编译、签名并上机，验证 247 个外置原生 ELF，HAP 内 GNU/Steam 文件数均为 0。

完整产品验收字段仍为 false。当前 CEF 运行配置关闭 GPU 加速；设备锁屏期间的帧回读不等于屏幕显示或实际键鼠已验证。模块边界见 [MINIMAL_GNU_RUNTIME.zh-CN.md](MINIMAL_GNU_RUNTIME.zh-CN.md)。

external-58 / 候选 v24 进一步验证：独立 session 查询根经原生 exec 正确保留，完整 Steam 本地连接错误没有复现。XTEST 对真实焦点窗口 `20971528` 发送 8 个字符，之后的 X11 图像中账号框实际显示 `hmosdeck`；没有密码或表单提交。保留当前严格验收字段为 false，另用 `minimal-gnu-runtime-validation.json` 记录登录页和 X11 输入这两项已经取得的证据。

候选 v24 的 ArkTS/native、外置资源检查和签名通过，已经安装。主机回归同时覆盖窗口/颜色/XTEST/退出状态、会话端点清理的负面路径、24 个原生 exec 场景中的查询根保留、连接服务生命周期。`steam-login-58-review.png` 为二维码遮挡后的实机 X11 帧。设备配置已恢复 observe，避免下一轮自动输入；当前会话仍有 60 秒上限，持续运行和设备端输入尚未验收。

## 人工登录会话（external-59 / v25）

应人工登录测试需要，将客户端可配置上限扩展至 900000 ms（15 分钟），默认值仍为 15000 ms。Xwayland 另预留 20000 ms 供 readiness 和有界预检；原生进程上限为 920000 ms，Steam 专用 NCP 外层上限为 930000 ms，其他 NCP 探针仍为 70000 ms。扩展会话仍有明确期限，不是持久化产品会话。

本轮人工配置不启用 desktopSession、XTEST 或 CEF verbose logging，因此不会运行自动桌面截图和输入探针。外置 GNU/Valve 资源未变，不重新导入。v25 已编译签名并安装到 USB 平板，external-59 已启动；已检查 GNU 子进程和前台 DesktopAbility 存在。人工登录和实际显示结果待用户确认，不据此提升完整客户端验收字段。

主机验证通过：配置上下界及人工模式无 observer；原生 GNU main/TLS/pthread/版本符号、退出码、超时回收、有界输出、FD 白名单和父进程任务；新增最大允许预算下正常子进程立即退出的校验。git diff --check 通过，预先存在的暂存区保持不变。部署记录见本机 manual-login-session-59.json。

## 原版登录成功后的标题转换崩溃（external-59 至 64）

用户在 USB 平板确认真实登录界面可见，随后成功登录，切换主界面时黑屏。external-60 自动恢复登录后约 14.2 秒 SIGSEGV，未触发超时。external-61/62 的 opt-in 线程信号追踪保留真实信号与退出状态，记录 PC/LR 及有界帧链返回地址；没有读取参数或导出堆栈内容，也没有接管 Valve 的 SIGSEGV handler。故障栈为 vgui2_s.so +0xa012c → XFree → glibc free +0x90ee0。

原版 VGUI 调用 XwcTextListToTextProperty(XUTF8StringStyle) 后不检查失败返回，继续 XSetWMName 并 XFree。最小环境缺少 libX11 标准 XLC 语言数据，转换返回 XLocaleNotSupported (-2)，原版调用方随后释放未初始化记录。独立真实 Xvfb/Xlib 实验在缺少数据时复现 -2；供给固定 Debian libx11-data 后 ASCII 转换返回 0，并产生可安全 XFree 的属性。主机 C.UTF-8 下中文标题也通过；这不等于设备当前 C locale 的所有非 ASCII 标题已验收。

修正在通用运行环境：将标准 X11 locale 数据随已有外置 xkb-data.zip 提供，历史档名继续使用，包含 xkb-data/ 和 x11-locale/ 两个目录；宿主设置 XLOCALEDIR 并在原生 exec 中保留。数据不进入 HAP。启动预检实际调用相同的宽字符标题转换并设置 WM_NAME；缺少数据时先返回明确错误。没有修改 Valve ELF，没有拦截 XFree 或伪造文本转换成功。

候选 v29 编译、签名及安装通过。external-63 实机 Xlib locale 与 18 帧显示回读通过，共用 virgl GLX 接收端仍通过；自动登录后的原版 Steam 连续运行 120011 ms，faultDiagnostics 为空，最后因明确的 120 秒诊断期限被结束。日志已加载主界面相关窗口、弹出菜单及 Welcome 网页。这证明先前的 SIGSEGV 在本轮没有复现，实际主界面呈现与操作仍待用户确认。

已用同一候选启动 external-64，关闭信号追踪、模块诊断、自动截图与输入，保留最长 15 分钟人工会话。自动状态验收字段仍保持原严格值，用户确认与设备日志证据分开记录；CEF GPU 加速、Proton/Vulkan 与游戏运行尚未验收。

附带生命周期修正：Steam 客户端退出后即停止本轮私有 Xwayland，保留真实客户端信号与退出码，不再让已退出的客户端等待整个 15 分钟服务器期限。主机回归覆盖传入真实子进程 PID、主动停止和回收、后台线程 SIGSEGV、正常退出、超时、原 exec/popen 语义以及 XLC 数据缺失/存在的对照。

证据：post-login-summary-60.json（对应私有原始结果）、post-login-summary-61.json、post-login-summary-62.json、post-login-summary-63.json；带账号内容的原始客户端日志仅保留本机 private 文件。v29 外置 manifest 为 e22b49a7db4e6fc1424502078f45a16c29422dfd454026d5664640b8d6366b15，X11 数据包 SHA-256 为 38913e39f5cd34685eb324a32ccff010596bbdfd0870fc00038b3adfa6a4fd3a。

## 登录后主界面与设备点击验证（external-64）

v29 无需再次安装或导入资源，沿用已缓存的人工登录。本轮最初看到的是宿主游戏库：向已运行的 EntryAbility 再次发送诊断 Want，会把宿主入口带到前台；这与先前 SIGSEGV 的黑屏是不同现象。通过当前布局确认的 builtin Wine 桌面卡片恢复前台后，aa dump 确认 DesktopAbility 为 FOREGROUND，实际平板截图显示原版 Steam 商店和好友窗口。未将非导出的 DesktopAbility 改为公开入口。

使用基于当前截图位置的 HDC 设备点击，完成商店 → 游戏库、关闭好友窗口、返回商店。游戏库列表、封面与商店内容均实际刷新；点击未经过 XTEST，也没有购买、下载或启动游戏。这验证了 OHOS 输入 → 既有 Wayland → Xwayland → CEF 的点击通路，完整键盘、鼠标、手柄、输入法和游戏输入仍需分项测试。

本轮配置仍关闭 CEF GPU 加速，没有启用自动 desktop observer、自动截图、信号追踪或模块日志。手工设备截图原件仅存本机 private 文件，复核图缩小、模糊并遮挡账号区。原版 Steam ELF、已验证外置清单及 14 项预先存在的暂存内容均保持不变。不得将当前主界面成功推导为 CEF GPU、GNU Vulkan、Proton 或实际游戏通过。

已重新生成 post-login-summary-60/61/62.json，故障地址只匹配 Steam 主进程的映像，消除不同子进程同地址区间造成的错误模块匹配。分项状态及证据 hash 更新于 minimal-gnu-runtime-validation.json 与 post-login-ui-64.json。当前仍为最长 15 分钟的会话；持久启动、主动退出、重启和资源下载是后续产品化工作。

用户随后明确确认“主界面／游戏库已显示”，因此主界面呈现同时具有人工确认和设备截图证据。准备主动退出／重启验证时，平板已切到游戏画面，未执行退出、切换或重启；后台仍观察到本轮 GNU 会话进程。当前游戏不作为 Steam 启动游戏的验收证据，主动退出及重启仍未验证。

## GPU 与全屏的分层核验（external-64）

用户询问主界面是否使用 GPU、为何直接铺满屏幕。核对本轮私有结果和实际 helper 参数，确认 CEF 启用了 --disable-gpu 与 --disable-gpu-compositing，不能把页面访问快记为 CEF GPU 已启用。独立 GLX 预检实际报告 renderer=virgl (Maleoon 910)、OpenGL 2.1 Mesa 25.0.1，18 帧绘制及回读通过；这证明共用接收端的 GPU 绘制能力，不能证明当前网页走该路径。

Xwayland 本轮仍为 rootful、-shm 输出，构建关闭 glamor、服务器 GLX、DRI3 和 DRM；存在共享内存输出／拷贝，未实现全程 GPU 零拷贝。客户端 Mesa Xlib GLX 与服务器 GLX 是不同能力。宿主使用既有 DesktopAbility，全屏布局隐藏系统栏；本轮诊断桌面设定 1280×800，设备截图 2560×1600。整桌面承载与宿主全屏使 Steam 看起来直接占满屏幕，当前没有将每个 X11 窗口独立发布成鸿蒙系统窗口。

准确判断要分别检查：Steam 主窗口的 _NET_WM_STATE_FULLSCREEN、_NET_WM_STATE_MAXIMIZED_HORZ/VERT，XGetWindowAttributes 相对根窗口的几何，以及 OHOS 宿主窗口状态。external-64 没有启用 desktop observer，也未采集这些 EWMH 属性，因此保留“Steam 自身是否请求全屏”未确定；宿主全屏和 rootful 承载则有配置／源码证据。后续可先在整桌面方案内验证窗口承载和 CEF 加速，不以 rootless/XWM 集成为先决条件。

本轮最终运行 769447 ms 后返回真实退出码 42，无信号、无超时，私有 Xwayland 退出 0、NCP 回收通过；没有把 42 当作正常退出验收，也没有据此认定退出原因。脱敏证据见 graphics-window-assessment-64.json 与 post-login-summary-64.json。

## 窗口验证与 Steam 游戏链路设计（external-65 至 67）

external-65 在独立 fusion 子窗口完成主界面显示、设备点击和边缘缩放；Steam 菜单退出后，客户端退出 0（327418 ms）、服务器退出 0，均无信号／超时，NCP 回收通过。外层标题拖动和宿主关闭尚未验收。

external-66 使用共同 Wine 虚拟桌面，1024×640 Xwayland 与 760×570 Wine Notepad 已登记到同一合成器；实际 Steam 内容变黑、点击未见更新，未取得两者正常共存的画面证据，根因未确定。主动停止候选后，external-67 恢复 1280×800 原呈现；商店页可见 Error Code -118，不能把此网络页面结果解释为 Steam API 或 DRM 验收。

目录指针实测确认 GNU 外置资源位于 Wine Z: 对应的同一 Download 应用目录。游戏库 Z:\Steam 尚未配置；未下载或启动 Steam 游戏。保留现有 OHOS Proton/Wine 的后续方案见 GAME_LAUNCH_DESIGN.zh-CN.md，DroidDeck / Proton 固定源码调查见 DROIDDECK_REVIEW.zh-CN.md。GNU Steam 辅助进程及 Wine 远程 lsteamclient 尚未实现，拥有状态查询不能替代完整游戏认证。

本机证据 window-and-integration-validation-65-66.json、post-login-summary-65.json。原 Valve ELF、Mesa 子模块和预先存在的暂存区未修改。
