# Linux Steam 与现有 OHOS Wine 的实验接口桥

目标：手机和平板统一由原版 ARM64 Linux Steam 提供登录及 Steamworks 服务，Windows 游戏保留现有 OHOS Wine/FEX/DXVK 路径。Windows Steam 安装目录、账号和接口 DLL 都不是依赖。官方客户端和最小 GNU 运行时仍在外置 Z 盘资源中。

## 当前实现

- `steamClientBridge=experimental` 显式启用。GNU compatibility tool 为每次游戏启动创建独立 helper，经已认证的宿主 broker 只向该游戏传递 AppID 匹配的端点凭据。
- `0065-kernelbase-linux-steam-session-registry.patch` 为该进程提供 Steam SDK 启动信息视图。SDK 的 `Software\Valve\Steam` / `ActiveProcess` 查询落到私有、易失的会话键，直接返回自建 `lsteamclient.dll` 的系统路径；不改写共享 Valve 注册值。无有效标记或 AppID 不匹配时仍使用原查询。
- PE32 / PE64 包装器将调用封送给 GNU ARM64 helper，原版 `steamclient.so` 对象指针留在 GNU。接口按固定 Proton 提交的 SDK 1.50 / SteamClient017 1.42 头文件生成；28 个版本、834 项方法声明并不代表所有方法均已支持。
- PE 虚表显式实现 MSVC 的对象返回缓冲区 ABI，不继承 MinGW SDK 类。`CSteamID` 等对象返回值写入调用方提供的缓冲区；生成时对对象返回类型做编译期检查，避免 SDK 更新后静默采用错误 ABI。GNU 侧仍使用真实 SDK 类，方法编号和传输协议不变。
- 支持有类型的标量、枚举、字符串、SteamID/GameID、明确列出的 stats 输出参数，以及有界票据缓冲。回调逐字段转换 GNU / Win32 ABI；不直接复制不同布局的结构。
- SDK 的 callback-registration 函数指针只保留在 PE 侧，不跨进程调用。GNU 与固定 Proton 一样请求所有回调；已支持的回调逐字段返回。未知回调只返回类型元数据并保持 pending，由 PE 调用游戏原有注册检查：确认未注册才释放并继续轮询；已注册或没有检查函数则明确失败。单次最多跳过 64 项，不生成登录、拥有状态或票据结果。
- 官方模块、受监督客户端 PID 和内核 socket 对照验证通过后才开放用户对象。未知 ABI / 未实现参数形态返回明确失败，不以假成功掩盖。
- PE 桥在 `C:\windows\temp\hmosdeck-steam-api-state.json` 写入有界技术状态，包含阶段、方法、状态、序号、进程号及单调时间；不写入调用参数、账号、nonce 或票据。该状态用于区分本次调用与旧记录，不代表认证通过。

## 验证边界（2026-10-20）

手机会话 025–027 已加载修正后的 PE 桥，越过 `GetSteamID`，完成 42 次官方对象调用并通过客户端来源核验。第 42 次 `Steam_BGetCallback` 收到未知类型 `1040044`（GNU 数据长度 784），此前直接返回 Unsupported 并终止游戏。现已实现上面的 PE 注册检查与 pending 释放协议，两端须成对更新；该修正在会话 028 已部署，但用户收回手机时客户端停在 `ShowInterstitials`，尚未取得本次游戏进入新桥的证据，不能宣称此问题已经解决。

手机注册视图回归已通过：ANSI 查询、Unicode 分步打开、正确桥路径、错误 AppID 拒绝、共享 Valve 值不变、无标记 DLL 加载拒绝。10 项桥接主机回归通过，覆盖 fake SDK 生命周期、typed 回调、未知回调仅传元数据及等待 PE 决策、票据清理、非法句柄、跨 AppID / nonce 拒绝，以及 `GetSteamID` / `GetAppOwner` 的调用方返回缓冲区、返回指针和边界保护。主机测试不替代真实游戏认证。

官方 Linux 首次安装辅助程序已完成来源锁定、导入与宿主精确白名单支持。来源为 Linux ARM64 清单中的 `bins_misc_linuxarm64.zip.7ec11b458eb4bbffc6ada9a85924c7d7db25132f`，只提取 `legacycompat/iscriptevaluator.exe` 和 `bin/d3ddriverquery64.exe`，不导入 Windows Steam、Steam DLL 或 SteamService。独立 ZIP 不重写客户端账号配置；导入器和宿主分别校验 SHA-1/SHA-256、PE 架构、路径、工作目录及角色，辅助程序不获得游戏桥凭据。会话 025–028 已实际执行工具：安装脚本退出码仍为 1；驱动查询在不同轮次为 0 或 1，均未完成成功验收。v73 将并发请求排队，辅助程序失败不再显示“游戏退出”，工具状态独立保存且不覆盖游戏结果。

平板此前能进入仙剑主菜单，但详细日志显示加载了旧注册路径所指的 XDGame.dll。不能据此认定本桥已通过，也不将该文件作为运行依赖；需要使用上述独立链路重新验收。没有修改该游戏、存档或其 DLL。

手机另有 CEF 页面进程反复退出、主界面变黑的问题。实际捕获到 V8 `Failed to reserve virtual memory for CodeRange` 和 `Re-embedded builtins: set permissions`，尚未确定地址布局和执行权限各自的责任。新增仅观测、不改变行为的 mmap/mmap64/mprotect 有界诊断；最后一轮只有两条成功的大映射记录，没有定位到失败调用。软件 CEF 仍为默认，未修改平板 GPU 配置或全局内存权限。

虚拟滚轮已修正一个可复现的宿主路由缺陷：合成器负责输入时，即使 overlay 传入桌面 root，也按指针位置命中实际窗口，避免滚轮落到 Wine 桌面。主机生产输入用例修正前失败、修正后通过，包含在 v73；实机持续滚动与回弹验收尚未完成。

## 手机暂停检查点（2026-10-20）

- 用户收回手机，暂停全部手机操作；后续默认仅使用 USB 平板 MLR-AL10。恢复手机测试前重新确认设备连接及可接管时间。
- 源码分支：`main_proton_steamarm64`，Windows 分支保持原状；不暂存/隐藏会员等现有功能。
- 已安装 HAP：`native-steam-external-v73-debug-signed.hap`；回退包保留 v56。对应固定 artifact 目录为 `F:/VintagePomelo-Workspace/artifacts/steam-linux-arm64-client-20261019`。
- 外置资源最新目录：该 artifact 下的 `external-payload-v3`。HAP 与该外置资源分别保存；不可只安装 HAP 而遗漏成对更新的 PE32/PE64/GNU 桥、官方 helpers ZIP 和 manifest。
- 最后会话：`steam-phone-028`。第 42 次失败的明确游戏证据属于 026/027；028 的旧桥状态不能当作本轮结果。
- 下一次先验证未知回调的真实注册结果，再定位 V8 失败的具体调用；随后验证可见、可操作的游戏菜单，以及虚拟滚轮持续上下滚动。保留账号、游戏与存档，不引入 Windows Steam 依赖。

## 尚未完成

- 真实游戏初始化、必要回调、适用票据取得/取消及正常退出，两端分别验收。
- 首次安装辅助程序的真实失败原因与成功验收；当前不伪造安装完成，不跳过官方脚本。
- 很多数值数组、一般输入/输出结构和私有函数指针仍未支持；EncryptedAppTicket 的输入 blob 尚未支持。
- 原型传输为 loopback TCP + 每游戏 256-bit nonce + AppID + 顺序号；产品化前仍需收敛到 Unix peer credential 方案。
- PE 桥和 kernelbase 会话视图目前为手机实验部署，尚未作为可发布 Wine 资源集成。未安装桥时不能开放依赖它的产品入口。
- CEF 软件路径维持默认；游戏 GPU 路径不随 CEF 软件模式关闭。CEF 页面稳定性需独立确认。

## 构建与测试

使用 `scripts/build_linux_steam_bridge.py` 指定固定 SDK 缓存、LLVM MinGW 和 GNU ARM64 工具链。`steam_bridge_sdk.lock.json` 校验构建输入；参考头文件不作为 HAP 官方客户端资源分发。

`host_tests/linux_steam_session_registry_probe.c` 是设备上的进程视图回归；它不执行原版客户端，不将 fake 标记视为认证成功。`host_tests/linux_steam_bridge_test.py` 使用明确 fake SDK 对象验证封送与清理。
