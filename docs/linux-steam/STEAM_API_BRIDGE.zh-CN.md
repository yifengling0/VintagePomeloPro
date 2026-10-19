# Linux Steam 与现有 OHOS Wine 的实验接口桥

目标：手机和平板统一由原版 ARM64 Linux Steam 提供登录及 Steamworks 服务，Windows 游戏保留现有 OHOS Wine/FEX/DXVK 路径。Windows Steam 安装目录、账号和接口 DLL 都不是依赖。官方客户端和最小 GNU 运行时仍在外置 Z 盘资源中。

## 当前实现

- `steamClientBridge=experimental` 显式启用。GNU compatibility tool 为每次游戏启动创建独立 helper，经已认证的宿主 broker 只向该游戏传递 AppID 匹配的端点凭据。
- `0065-kernelbase-linux-steam-session-registry.patch` 为该进程提供 Steam SDK 启动信息视图。SDK 的 `Software\Valve\Steam` / `ActiveProcess` 查询落到私有、易失的会话键，直接返回自建 `lsteamclient.dll` 的系统路径；不改写共享 Valve 注册值。无有效标记或 AppID 不匹配时仍使用原查询。
- PE32 / PE64 包装器将调用封送给 GNU ARM64 helper，原版 `steamclient.so` 对象指针留在 GNU。接口按固定 Proton 提交的 SDK 1.50 / SteamClient017 1.42 头文件生成；28 个版本、834 项方法声明并不代表所有方法均已支持。
- PE 虚表显式实现 MSVC 的对象返回缓冲区 ABI，不继承 MinGW SDK 类。`CSteamID` 等对象返回值写入调用方提供的缓冲区；生成时对对象返回类型做编译期检查，避免 SDK 更新后静默采用错误 ABI。GNU 侧仍使用真实 SDK 类，方法编号和传输协议不变。
- 支持有类型的标量、枚举、字符串、SteamID/GameID、明确列出的 stats 输出参数，以及有界票据缓冲。回调逐字段转换 GNU / Win32 ABI；不直接复制不同布局的结构。
- SDK 的 callback-registration 函数指针不跨进程调用。与固定 Proton `lsteamclient/unixlib.cpp` 同样，由 GNU 请求所有回调，Windows `steam_api` 保持注册与分发责任。该策略不生成登录、拥有状态或票据结果。
- 官方模块、受监督客户端 PID 和内核 socket 对照验证通过后才开放用户对象。未知 ABI / 未实现参数形态返回明确失败，不以假成功掩盖。
- PE 桥在 `C:\windows\temp\hmosdeck-steam-api-state.json` 写入有界技术状态，包含阶段、方法、状态、序号、进程号及单调时间；不写入调用参数、账号、nonce 或票据。该状态用于区分本次调用与旧记录，不代表认证通过。

## 验证边界（2026-10-20）

手机会话 020 已实际加载自建 PE 桥，创建官方 pipe、连接用户并通过客户端来源核验；SDK 的 callback-registration 调用已补齐，后续调用推进到 `SteamUser021::GetSteamID`。对照 MSVC、MinGW 调用代码及固定 Proton 的 record-return 生成逻辑，发现对象返回 ABI 不同，现已修正并完成 PE32 / PE64 编译。不能把来源验证或主机 ABI 回归称为 SteamAPI_Init、票据或游戏认证已验收。

手机实机注册视图回归已通过：ANSI 查询、Unicode 分步打开、正确桥路径、错误 AppID 拒绝、共享 Valve 值不变、无标记 DLL 加载拒绝。9 项桥接主机回归通过，覆盖 fake SDK 生命周期、回调、票据清理、非法句柄、跨 AppID / nonce 拒绝，以及 `GetSteamID` / `GetAppOwner` 的调用方返回缓冲区、返回指针和边界保护。该 ABI 用例在主机上显式模拟返回缓冲区调用，尚未替代 Windows 调用方的实机验收。

手机会话 024 已部署新桥，但原版 Linux Steam 先触发首次安装脚本，外置资源缺少 `legacycompat/iscriptevaluator.exe`，在解析路径时失败，尚未进入本轮游戏 PE 桥；不能把旧 helper 状态归于本次。官方 Linux ARM64 更新清单所列 `bins_misc_linuxarm64.zip.7ec11b458eb4bbffc6ada9a85924c7d7db25132f` 已下载并按清单 SHA-1 核对，其中包含该工具和 `bin/d3ddriverquery64.exe`。只完成来源及文件定位，尚未完成辅助程序依赖检查、可复现资源导入和宿主启动支持。

平板此前能进入仙剑主菜单，但详细日志显示加载了旧注册路径所指的 XDGame.dll。不能据此认定本桥已通过，也不将该文件作为运行依赖；需要使用上述独立链路重新验收。没有修改该游戏、存档或其 DLL。

手机另有 CEF 页面进程反复退出、主界面变黑的问题。候选 v71 将独立私有 stderr 日志与结果的短前缀分离，日志在 8 MiB 轮换，以便捕获后续页面退出原因；结果输出上限维持原有值。

## 尚未完成

- 真实游戏初始化、必要回调、适用票据取得/取消及正常退出，两端分别验收。
- 首次安装辅助程序支持：仅允许来源与哈希可核验的官方 Linux 客户端必要工具。现有 broker 仍限制 PE 位于游戏安装目录，后续需设计精确白名单及独立工具退出状态；不能直接放宽为任意外部程序，也不能伪造安装完成跳过脚本。
- 很多数值数组、一般输入/输出结构和私有函数指针仍未支持；EncryptedAppTicket 的输入 blob 尚未支持。
- 原型传输为 loopback TCP + 每游戏 256-bit nonce + AppID + 顺序号；产品化前仍需收敛到 Unix peer credential 方案。
- PE 桥和 kernelbase 会话视图目前为手机实验部署，尚未作为可发布 Wine 资源集成。未安装桥时不能开放依赖它的产品入口。
- CEF 软件路径维持默认；游戏 GPU 路径不随 CEF 软件模式关闭。CEF 页面稳定性需独立确认。

## 构建与测试

使用 `scripts/build_linux_steam_bridge.py` 指定固定 SDK 缓存、LLVM MinGW 和 GNU ARM64 工具链。`steam_bridge_sdk.lock.json` 校验构建输入；参考头文件不作为 HAP 官方客户端资源分发。

`host_tests/linux_steam_session_registry_probe.c` 是设备上的进程视图回归；它不执行原版客户端，不将 fake 标记视为认证成功。`host_tests/linux_steam_bridge_test.py` 使用明确 fake SDK 对象验证封送与清理。
