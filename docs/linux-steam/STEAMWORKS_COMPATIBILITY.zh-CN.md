# Linux Steam 与 OHOS Wine 的 Steamworks 兼容路径

## 保留的执行边界

Linux ARM64 Steam 负责用户登录、下载、拥有权、Steamworks 服务与云同步；Windows 游戏仍交给已有 OHOS Wine/FEX/DXVK。GNU 侧只承载 Steam 与服务适配器，不另建一套 GNU Wine 或图形链路。

```text
Windows 游戏 / 自带的 steam_api.dll
  → 官方 Linux 客户端档案中的 Windows steamclient.dll / steamclient64.dll
  → Wine ntdll 原有 Proton 导出跳转
  → lsteamclient.dll（Windows ABI、回调布局转换）
  → 每次游戏启动的本地 RPC 服务
  → 官方 Linux steamclient.so → 当前已登录的 Linux Steam
```

这与直接安装完整 Windows Steam 不同：兼容模块来自固定版本的官方 `bins_misc_linuxarm64`，只导入经过大小、架构和摘要校验的三个 DLL，不运行 Windows Steam 客户端、服务或后台账号。官方 PE 文件在磁盘上保持原字节；运行时跳转与接口对象的数据区布局参照固定上游 Proton 提交 `e91ca2be0df2cef4c230cbbc0b86604d73a0bbf6`。

## 历史 ABI

生成器从锁定 Proton 的 SDK 099u–165 中提取各版本声明，同时生成 Windows 包装、GNU 分发及 ABI 清单。范围扩展到该上游的全部 212 个接口版本，涵盖 Client、User、Apps、Friends、统计成就、云存储、库存、UGC、HTTP、匹配、网络、手柄、截图、视频及其他接口族；按每个版本生成独立虚表。不能把旧名称映射到布局不同的新接口。`SteamUtils007` 使用 SDK 1.36 的完整定义，包含同版本后追加的 `SetOverlayNotificationInset`；`SteamClient014`、`SteamClient017` 分别保留经比对布局相同的 SDK 1.29、1.42 来源。唯一版本别名 `SteamApps001` 来自上游明确声明，指向相同 ABI 的 `STEAMAPPS_INTERFACE_VERSION001`。

锁定输入可用 `scripts/fetch_linux_steam_bridge_sdk.py --reference <cache> --offline` 校验。完整静态差距清单见 [STEAMWORKS_COVERAGE.zh-CN.md](STEAMWORKS_COVERAGE.zh-CN.md)；声明、可编解码和实机通过分开记录。

Windows 的同名重载要在首次声明位置分组并倒序，GNU 则保留原始顺序。生成器分别输出两端布局，包含 HTML 和网络接口的析构占位；Windows 结构返回使用调用方缓冲区，GNU 的 CGameID 参数按上游引用规则传递。审计逐槽核对固定 Proton 的全部 Windows 包装，并检查重载序号、析构和复制长度；主机运行测试通过实际虚表调用 GetStat 的整数／浮点重载以及旧 Friends 中不相邻的重载，避免只比较名字而漏掉错位。

通用传输按已审核的方向和长度合约覆盖输入／输出缓冲区、数组、const 值引用与指针、输出引用和容量指针。库存的空缓冲区查询、容量不足和 required-size 返回保留原语义；UGC 的 SteamParamStringArray_t 逐字符串复制到服务端本次调用拥有的存储，保留 null 与空数组的区别。IP 地址与网络身份的 union 按 tag 和字段转换。不能把 Windows 地址或整个原生结构的字节当作 GNU 对象传过去。CreateLocalUser／CreateGlobalUser 追踪官方创建的 pipe/user，来源核验失败及断连时释放实际句柄。

票据创建、取消和断开清理使用原接口类型，不能把旧 `ISteamUser` 指针转成新版来释放。所有结果继续来自官方对象；不存在伪造拥有权、票据或忽略重启要求的成功分支。

`STEAMAPPTICKET_INTERFACE_VERSION001` 从上游同版本私有头文件锁定，提供 `GetAppOwnershipTicketData` 的独立缓冲区转换。返回值、四个位置/长度字段及原始票据字节取自官方 Linux 对象；空指针形态和容量不足的大小查询分别传递，传输最大 64 KiB。票据及参数内容不写入日志。该接口没有票据取消句柄，不能与 `GetAuthSessionTicket` 的生命周期混用。

## 模块安装与更新

官方兼容模块由用户选择在线安装或配套离线包导入，不进入 HAP。启动前将核验后的三模块同步到私有 Wine 前缀 `C:\windows\hmosdeck-steam`。只有带有效本次桥接标记的游戏进程使用该注册视图，普通 Windows Steam 不改写。

`clientDataId` 改变时先要求显式更新组件；只有适配器改变时可复用已校验的官方资源完成离线更新。更新保持原安装位置、游戏库、账号及存档，不能靠修改激活标记跳过整组校验。

## 当前验证边界

2026-10-24 远端鸿蒙 PC 的 `logs-20261024-123107.zip` 确认《欧洲卡车模拟2》（227300）的接口桥在 `SteamUser023` 返回 `interface_unavailable`，同时服务端记录 `clientSourceVerified:true`、`request_completed/status:0`。当前阻断是旧桥缺少该版本，不能再归因兼容工具未选择、来源校验失败或笼统的磁盘权限。新桥生成独立 User023 虚表，支持含 `SteamNetworkingIdentity` 的认证票据第四参数、Web API 票据及相应回调；创建、取消和连接断开清理继续调用官方对象。新增主机回归验证真实数据转发和清理，不能代替欧卡在远端 PC 的实测。

本轮共生成 212 个接口版本、6539 个版本内方法；实际可编解码数与结构／回调统计由编译后的 C++ traits 自动计算，见覆盖审计。计数包含不同版本的重复方法，不能当作独立业务功能数。复杂指针记录仍明确不支持，单帧 1 MiB 和单连接生命周期限制保留；未声称完整兼容所有 Steamworks 功能或所有游戏。日志中较早的 Steam Runtime 下载仍有符号链接创建权限错误，此问题与本次 User023 阻断分开，尚未在远端验证修复。

2026-10-24 手机实测确认了连续故障点：缺少 `SteamClient014`；补齐后在没有官方 PE 外壳时出现 `Application load error 3:0000065432`。接入官方 PE 外壳及数据区对象布局后，游戏继续调用到 `SteamUtils007`，随后因该旧接口缺失而结束。补齐历史核心接口后，进一步确认缺少 `STEAMAPPTICKET_INTERFACE_VERSION001`。补充该票据接口后，《仙剑四》（681820）已进入主菜单，能打开游戏讯息页面、接收 Escape，关闭游戏窗口后官方启动工具与 Wine 均记录正常退出（0）。没有修改游戏二进制、读写存档或安装完整 Windows Steam。本次设备使用原有 WineD3D/VirGL 设置；菜单帧率不能代替实际游玩性能验证。

同一接口桥还回归了《新仙剑》（695630）的主菜单与窗口关闭（0）。自动化触摸的菜单退出未确认通过，不能把窗口关闭结果记作菜单交互或长时间游玩通过。

回归另外确认：CEF 的启动选项窗口可能不带 `WM_TRANSIENT_FOR`，旧 X11 适配器忽略其 `_NET_ACTIVE_WINDOW`，恢复 Wine 宿主时又将主窗口提到选项窗口之上。此时 Steam 日志停在 `ShowLaunchOption`，尚未创建游戏进程，不能归因 Steamworks 接口。窗口管理器现在单独记录有效的辅助窗口激活，恢复时保持其层级与焦点，主窗口仍负责视口尺寸；辅助窗口撤回后恢复主窗口焦点。该行为已用真实 X11 服务器回归，包括无 transient 提示、主窗口激活、宿主恢复和对话框关闭。

Deck 的显式 Play 请求现在允许新激活的 Steam 辅助窗口取得一次关注，解决启动选项藏在 Deck 后面的情况。关注只在本次请求仍有效、120 秒内且游戏会话尚未创建时发生；用户返回 Deck 或切到其他任务立即取消。确认当前启动提示保留首个游戏窗口的一次自动激活，不把它误认为用户另选任务。关闭/撤回的窗口、主窗口普通激活、游戏已开始以及过期的异步操作均不能触发关注。手机已验证《仙剑四》启动选项无需手动恢复 Steam 即可显示，第二次启动进入主菜单并以 0 退出。

## CEF 稳定性调查

2026-10-24 的新增诊断区分实际 `waitpid/waitid` 子进程退出码与父进程 `kill` 调用，不采集命令参数、账号和票据。应用内故障摘要使用系统 FaultLogger 查询本应用缓存，最多十条、每条最多十二帧；当前查询只返回早先记录，不能把它们归因本轮问题。

手机在一次《仙剑四》启动过程中重现 CEF 网络服务连续重启。日志明确记录 CEF 浏览器进程主动向其网络子进程发送 SIGKILL，并回收信号 9；尚不足以认定系统 OOM，也尚未确定最初 IPC 断开的原因。同轮游戏曾显示官方 `5:0000065432` 并以 83 退出，随后不修改游戏/账号/桥代码，重试即进入主菜单并以 0 退出。因此不能把本轮记作所有启动问题已修复，也不能直接将该官方错误归因缺少某个接口。

另做了 CEF 自带 `NetworkServiceInProcess2` 的实机对照。虽然日志确认未再创建独立网络 utility 进程，但官方本地 WebSocket 服务反复 `Connection rejected`，随后显示 `0x3008`。该方案未通过验收，相关产品代码和默认开关已撤回；不能为了规避重启而取消 Steam 的真实连接身份核验。继续保留独立网络子进程及现有同应用真实 FD 查询服务。该结果将后续排查指向本地连接身份/查询服务的生命周期与失败路径，尚未证明它就是原故障根因。

本地身份查询随后确认一个独立、可复现的缺陷：`socket_metadata.c` 先前依赖 150 毫秒 socket 超时及一次 `recv(MSG_WAITALL)` 收齐完整请求；发送首段后间隔 250 毫秒再发送其余部分，旧实现必现 EPIPE。现场查询日志也记录了 EPIPE/EIO，但仍不能据此认定所有 CEF 故障都由它引起。

查询协议现改用整次事务 1 秒的单调时钟截止时间，正确处理 EINTR、EAGAIN、部分读写；接收端跨分段保存并核验 SCM_RIGHTS 描述符，发送端发生短写时只在首个成功字节附带一次 FD。原有同 UID/PID、真实 inode、TCP 类型与最终复制描述符校验全部保留。回归覆盖分段请求、分段响应、重复/截断 FD 响应、未完成查询的有界退出，以及 512 次查询、关闭/reuse、错误身份和退出清理。旧实现失败/修正后通过的证据保存在固定任务目录。此项修复的手机实测结果另记，不能以主机回归代替黑屏根治验证。

该协议候选在手机的后续回归仍未通过：03:44:34 左右 Linux Steam 主进程、Wine 窗口宿主与 `iscriptevaluator.exe` 收到 SIGKILL，尚未创建新的游戏进程；此前的菜单成功记录属于较早候选。下一轮有界系统日志在 03:53:41 捕获明确的 `oom_killer` / `KHM_KILLER` 及其他应用被回收；当轮 CEF 浏览器进程 RSS 约 2.4 GiB、多个辅助进程各约 1.2 GiB。RSS 包含共享页，不能相加当作独占内存；这也不能反向证明 03:44 的每个 SIGKILL 都来自 OOM。

源码确认另一个执行语义缺口：原生 GNU exec handoff 保留旧地址空间，CEF 或 Steam 派生后重新执行时会同时保留上一代程序的堆、匿名 ELF 和共享内存映射。现增加独立 RX 跳板：先准备新 loader、栈及有界回收清单，切到新栈后仅通过原始 `munmap` 系统调用释放旧映射，再进入 GNU rtld；保留新映像、栈和跳板；手机对照还要求保留当前 TP 所在 VMA、真实 AT_SYSINFO_EHDR 所在映射、内核区域、初始 [stack]/[heap] 及鸿蒙系统与签名宿主模块。其余退役 GNU 匿名映像和旧匿名内存纳入回收。这是用户态的有界回收，不能称为完整内核 exec。整个过程仅适用于已核验的单线程 GNU 重执行，不修改 OHOS Wine 的图形路径、游戏和认证。旧 SIGSYS handler 先恢复默认，避免回收后继续指向上一代代码；新 preload 自行登记当前映像的诊断 handler。

新增真实 64 MiB 继承映射回归：重执行后的 `mincore` 确认旧页不存在，新 libc 可正常分配释放内存，父进程的原映射逐页保持不变。共 28 组 exec 场景及 dlopen/dlmopen 回归通过，覆盖多代 shell/taskset/reaper、PID、argv/env、FD、信号及 CPU 亲和性。手机上全面回收和只额外保留 TP 的两次对照均在新 GNU rtld 早期 SIGSEGV，已否决；不能以主机通过替代实机验收。收紧回收范围的 Release 候选 b043a40a6ea9380230152326cb102504b9e4029e172efa0eb8872a7dcff51b66（GNU 组件 8a3c1f9084c0d1c72aea00731db513d4148a9551b4aec618367172383ccb2acf）已在无线手机进入原版 Linux Steam 游戏库。《仙剑四》与《新仙剑》分别进入主菜单，经窗口正常关闭后 Wine 返回 0；未读档、未保存，菜单内退出的自动化点击仍未确认有效。新候选首次 GNU 重执行记录回收约 40.1 GB 虚拟地址，后续派生重执行也有回收记录；这些数值不是实际释放的物理内存。CEF 浏览器本轮一次 PSS 采样仍约 1.7 GiB，内存开销及长时间稳定性仍需后续验证。

声明存在不等于方法已经可用。复杂数组、输入缓冲区、结构与回调须分别序列化和验证；当前未支持的方法明确报错。接口源校验通过也不等于游戏服务器已验证票据，所以诊断中的 `authenticationVerified` 不提前置为真。

尚需独立完成：多进程/启动器到子进程的服务寿命、未转接导出的审计、超出固定上游范围的历史回调布局、安装脚本及复杂 Steamworks 对象的所有权。HTML 像素回调、网络消息分配／释放、自定义信令和函数回调需要各自的跨进程生命周期，不能靠自动复制函数地址完成。DRM、第三方启动器、反作弊与图形兼容仍可能独立阻止游戏运行，不能承诺增加接口列表即可兼容所有游戏。

## 主窗口边框缩放

Steam 普通主窗口的八方向边框拖动已接入 Wine 承载 HWND 的交互缩放，尺寸在 compositor、Wine 和 X11 间双向确认，避免拖动后回弹。全屏／大屏和最大化按原显示模式处理；辅助窗口遵循自身最小／最大尺寸。实现与回归范围见 [WINDOW_RESIZE.zh-CN.md](WINDOW_RESIZE.zh-CN.md)。本轮主机回归不代替远端鸿蒙 PC 上真实鼠标的验证。

## Wine 承载窗口的独立生命周期

Linux Steam、Xwayland 与 `hmosdeck-steam.exe` 是三个不同的生命周期。2026-10-24 手机回归中，承载进程正常退出（0），Linux Steam 主进程、CEF 浏览器和登录仍存活；旧实现保留了运行中状态和失效的 HWND，Deck 再次打开无法恢复。不能将这个表现直接登记成 Linux Steam 崩溃。

显式打开现在先检查 NAPI 的真实 Wine session 状态。只有明确退出或对象已停止才创建新的承载进程；临时撤窗和缺少窗口元数据不触发重启。重建保留当前 Linux worker、generation、登录、X11 模式及单调命令序号，丢弃旧 HWND 的状态文件；并发请求复用一次重建，过期请求不能抢占前台。重建失败清理替代对象，下一次显式操作可重试，正常退出仍由原会话串行处理。

Release 编译、debug 签名候选 `0772db347e73bfd1c190e093e536900d6c51499767d127caa82af17c28883651` 通过了手机定向故障注入：仅终止唯一的 Wine 承载进程（77），再从正常 Deck 入口打开，重新绑定原生 Steam surface 与新 HWND，Linux Steam/CEF PID 及 generation 均保持。恢复后《仙剑四》进入主菜单，点击游戏窗口 X 关闭，Wine 记录 0；随后从 Deck 大屏选项进入 Big Picture，切回普通窗口并打开游戏库，原进程持续存活。《新仙剑》在这次模式往返后也进入主菜单。未读档或保存，自动化点击游戏菜单退出仍未验收。

实测还捕获另一未解决的现象：Alt+F4 退出《新仙剑》时，游戏和 Wine 承载进程均以 0 退出，Linux Steam/CEF 留存。事件记录显示按键仅投递给游戏 surface，尚不足以说明承载进程的退出来源。此后一次承载重建及同应用内 Steam 重新启动失败，重启应用后从标准 Deck 入口又正常显示游戏库；因此不能将首次故障注入恢复成功扩写成所有 Wine 异常状态均已恢复。该问题继续保留，不据此修改通用输入或游戏二进制。

Steam 的 `gameprocess_log.txt` 中辅助 bridge PID 的 -1 也单独核实：其父启动工具在清理 `GameBridge` 时主动发送 SIGTERM（15），启动工具本身退出 0，Wine 游戏退出由真实 waitpid 确认。这不是独立的游戏崩溃证据，不能将日志的 -1 伪改成成功。

## 性能判断

本地接口调用增加了编解码及 RPC 往返；串行连接和全局锁也可能放大高频调用开销。它不承载图形命令，不能直接用游戏帧率证明接口桥快慢。评估启动时间应分开记录 Wine 准备、Linux Steam 就绪、安装脚本、桥连接、首个游戏窗口与首帧；功能正确后再按实际高频方法优化，不能缓存或伪造实时授权结果。

另一个已确认的升级开销是：适配器版本变化时，现有安装器仍逐项核验、暂存并写回清单中的官方客户端资源。这个过程只发生在配套组件升级，但会延长更新后的首次启动；后续应单独优化成经过校验的适配器更新事务，不能通过取消校验来缩短耗时。

手机冷启动的一次采样：组件核验约 0.22 秒，Wine 桌面约 7.2 秒，宿主窗口约 1.5 秒，Linux 客户端就绪约 10.4 秒，合计约 19.3 秒。之后等待官方启动选项确认的时间不计作 RPC 执行耗时。该采样不代表所有设备、网络或首次组件更新的速度。

上游参照：

- [Proton lsteamclient](https://github.com/ValveSoftware/Proton/tree/e91ca2be0df2cef4c230cbbc0b86604d73a0bbf6/lsteamclient)
- [Steamworks API](https://partner.steamgames.com/doc/sdk/api)
- [Steam 身份认证与拥有权](https://partner.steamgames.com/doc/features/auth)
- [Steam DRM](https://partner.steamgames.com/doc/features/drm)
