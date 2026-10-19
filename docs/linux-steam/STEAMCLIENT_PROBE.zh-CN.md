# GNU Steam 接口与当前 Wine 的首轮验证

2026-10-19，USB 平板，宿主候选 v32，GNU 探针通过外置清单独立更新。已在当前 OHOS Proton/Wine 与原版 GNU ARM64 Steam 之间验证只读调用。没有修改 Valve ELF、游戏或现有 DXVK／Vulkan／virgl 后端，没有下载或购买游戏。

## 已完成与边界

- 独立 GNU ARM64 辅助进程加载当前外置清单中的官方 `steamrtarm64/steamclient.so`，通过 `SteamClient020` 创建 pipe，连接真实 global user，取得 `SteamUser021` 和 `STEAMAPPS_INTERFACE_VERSION008`。
- external-69 实机：初期返回 `steam_offline`，后一次查询返回 `steamclient_readonly_verified`、`loggedOn=true`、`userIdentityPresent=true`。两次均调用 ReleaseUser 并成功释放 pipe。初期离线不能直接判为桥接故障或跳过检查。
- external-70 实机：当前 Wine 下的 Windows x64 PE 测试程序通过本机通道调用 GNU 辅助进程，后者重新创建／查询／释放官方接口，Windows 侧得到 `wine_gnu_readonly_rpc_verified`、`nativeCode=0`、`loggedOn=true`、`pipeReleaseSucceeded=true`。辅助进程记录 `accepted=1 replied=1`，随后按 90 秒上限正常关闭通道。整个 Steam 诊断会话在配置的 300 秒预算到达后回收，不能将其记为自然退出或游戏崩溃。
- helper 可按显式 AppID 查询 BIsSubscribedApp，主机正反用例通过；本轮没有指定账号游戏 AppID，实机 `ownershipQueried=false`，不能记为某游戏许可已验证。
- `Steam_BGetCallback`、`Steam_FreeLastCallback`、`Steam_GetAPICallResult` 导出存在，但未实际派发回调或取得票据。`SteamAPI_Init`、完整 lsteamclient 适配、DRM、兼容工具、库下载及 Steam 启动游戏均未通过验收。

上述首轮验证证明：当前 Wine 无需改为 GNU Wine，也能与持有官方 Steam 接口的独立 GNU 进程交换真实结果。它不证明全部 Steamworks 接口可用。

## Linux 客户端来源对照（external-72）

用户已有可用的 Windows Steam，因此“GNU 库加载成功”或“读到在线状态”均不能单独证明服务端来源。本轮增加独立的启动前／运行中／停止后对照，每阶段均使用新 GNU 进程、新官方模块和新 Steam pipe。

| 阶段 | 设备真实结果 |
| --- | --- |
| Linux Steam 启动前 | 官方 ARM64 steamclient.so 和 SteamClient020 可加载；127.0.0.1:57343 连接被拒绝，pipe_unavailable，未连接用户 |
| 运行中第一次 | 已在线，但连接来源尚未匹配；返回 client_source_unverified（14），没有记为通过 |
| 运行中重试 | 官方接口在线，peerMatched=1；内核复制的对端 socket 与探针自己的 TCP 两端地址、端口完全反向匹配，所属 PID 为 supervisor 本轮启动并持有的原版 Linux Steam 子进程；nativeMachine=183（AArch64） |
| 仅停止本轮 Linux Steam 后 | 独立新探针再次连接被拒绝、pipe_unavailable；未连接用户 |

本轮 Linux Steam 为来源隔离测试主动停止，不能记为自然退出或崩溃；测试 supervisor 正常返回 0，未触发宿主超时。原始 Valve 文件未改，官方 Steam 和 steamclient.so 的 SHA-256 分别保持 `bd059af3d99c8b9979a5218838cce9ff44bef4c5ccc0b399b333db0ae1b84a89`、`8dec8579abad65d000079f146aee5f42c12a4bd9ca7a24051b691813cced11dc`。

`steamclient_identity.c` 从 supervisor 取得其仍受监督的原版 Steam PID，核验指定外置 ELF 为当前原生架构；GNU 模块通过 dladdr 核对实际 factory 所属路径及 ELF。然后通过现有 socket-metadata 通道的 Unix SO_PEERCRED 校验 PID/UID，通过 SCM_RIGHTS 复制真实 FD，再核对 inode 和双方 TCP endpoints。不会信任缓存的端口归属或进程名。OHOS 外置原生加载使用匿名 RX 页，`/proc/maps` 未出现文件 inode 是正常现象，不能将其误判为非原生执行；此处来源依据为受监督的原生启动和实际内核连接，并非远程代码完整性证明。

普通 read-only/RPC supervisor 也强制传入这一 PID；连接源不匹配返回 14，禁止报告只读桥接成功。新版 PE 额外要求响应 flag bit 12（clientSourceVerified），并仅统计 Windows steam.exe 进程数量作为辅助诊断，不加载 Windows Steam API、不以该数量判定 Linux 来源。源验证不等于 SteamAPI_Init、游戏许可或票据验证。

external-73 在普通 RPC 模式下再次实测：Wine PE 通过当前 OHOS Wine／FEX 执行，返回 `wine_gnu_readonly_rpc_verified`、`nativeCode=0`、`clientSourceVerified=true`；GNU helper 的两次实时请求均与原版 Linux Steam 的同一受监督 PID 匹配，通道记录 `accepted=2 replied=2`。PE 本轮统计 Windows `steam.exe` 数量为 0，判定来源仍以真实 socket/PID 对照为准。整个会话到达 240 秒诊断预算后回收，`timedOut=true`、signal 9，并非游戏崩溃或自然退出。结束后确认没有遗留 GNU 会话进程。新版本机测试 runner 在每次启动前读取旧结果的运行时间标识，防止把前轮文件误当作本轮成功。

安全摘要：本机 `steamclient-source-validation.json`、`steamclient73-source-rpc-results.json` 和 `steamclient72-source-isolation-results.json`。源隔离启动参数为 `--steamclient-source-probe --timeout-ms 160000`，配置 `steamClientProbe=source-identity`。它约 100 秒后只停止本轮 Linux Steam 子进程，完成独立后置负对照；不能与 RPC 模式同时使用。默认正常启动不执行此诊断停止。

## 实现

`gnu_fixture/steamclient_probe.c` 只调用固定版本的只读接口，按获取顺序反向释放 user／pipe；不输出 SteamID、账号名、密码或票据。模块保留到辅助进程退出，避免内部线程存在时卸载代码。状态文件独占创建，权限 0600；调用或清理失败均返回可诊断失败。

`gnu_fixture/steamclient_session.c` 在单独进程启动原版 Steam，20 秒后执行辅助程序，并强制核验本轮 Linux 子进程的实际连接；每次查询有期限，离线或失败按有界次数重试，保存每次真实状态。子进程设 parent-death signal，监督程序保留客户端退出码。RPC 探针运行期较长，以便 Windows 测试程序连接；这仍是诊断会话。

`gnu_fixture/steamclient_rpc.c` 为诊断提供临时 loopback TCP 端点，便于未增加 Wine Unix 模块前用 WinSock 验证通信。只绑定 127.0.0.1，随机端口、256 位随机 nonce、0600 私有端点文件；最长 90 秒、最多 32 次连接，单连接输入／输出有界。nonce 不写日志。生产 lsteamclient backend 仍应按主设计实现 Unix socket、peer 身份、对象生命周期和有类型的方法封送；本轮 loopback 快照端点不等于该 backend 已实现。

协议 v1 请求固定 48 字节：magic HDSQ、version、只读 opcode、request ID、AppID、nonce；响应固定 32 字节：magic HDSR、version、真实状态、request ID、AppID、结果标志及保留零字节。bit 12 表示本轮实际连接源校验通过，既不表示 SteamAPI_Init 也不表示票据通过。整数使用网络字节序；不发送原始指针、回调函数、Vulkan／窗口 handle 或账号凭据。未知版本、opcode、错误 nonce 及不完整请求均不执行查询。每次合法请求重新查询官方状态。

`host_tests/linux_steam_steamclient_pe_probe.c` 是诊断 EXE，使用当前 Wine／FEX 和 WinSock。它不是 steam_api64.dll 或 steamclient64.dll。端点在同一应用私有目录内转交到测试 prefix，结果记录调用状态与运行标识，便于排除旧文件。没有改动用户游戏 DLL。

## 构建与复测

```sh
python3 scripts/build_linux_steam_steamclient_probe.py --cc <GNU_ARM64_gcc> --output <复用的构建目录>
python3 scripts/build_linux_steam_steamclient_pe_probe.py --cc <x86_64-w64-mingw32-clang> --output <新路径.exe>
python3 host_tests/linux_steam_steamclient_probe_test.py
```

完整外置运行时构建也包含这两个 GNU 辅助程序；打包器仅显式允许成对出现，HAP 继续只承载适配代码。独立构建脚本不执行 Valve 程序，真实接口只在设备上测试。

安装匹配候选并更新外置清单后，可以显式启用：

```sh
python scripts/launch_linux_steam_client.py --hdc <hdc> --target <设备> --artifact-dir <本机记录目录> --steamclient-probe --timeout-ms 180000
python scripts/launch_linux_steam_client.py --hdc <hdc> --target <设备> --artifact-dir <本机记录目录> --steamclient-rpc-probe --timeout-ms 300000
```

默认启动不启用这些探针。配置分别为 `steamClientProbe=read-only` 或 `steamClientProbe=read-only-rpc`。运行寿命仍有明确上限，不把诊断超时强制回收记为自然退出通过。external-68 被候选更新主动打断，未用来证明接口结果。

主机 15 项测试覆盖接口不可用、离线、身份缺失、释放失败、拥有状态真／假、非法参数、账号标识不输出、私有结果文件、协议版本／opcode／nonce 拒绝、分段输入、辅助进程超时／回收、退出码保留、来源校验失败、真实内核 socket 对照、独立前后置负对照和已有可连客户端时中止。mock 仅验证这些契约，实机证据来自原版客户端。

安全摘要保存在本机 `steamclient-probe-validation.json`、`steamclient69-readonly-results.json` 和 `steamclient70-pe-result.json`。原始客户端输出、账号日志、截图和端点 nonce 均仅在本机 private 文件中保存，不随源代码发布。

## 固定 ABI 参考

- [ISteamClient020：SDK 1.47](https://github.com/ValveSoftware/Proton/blob/e91ca2be0df2cef4c230cbbc0b86604d73a0bbf6/lsteamclient/steamworks_sdk_147/isteamclient.h)：CreateSteamPipe、BReleaseSteamPipe、ConnectToGlobalUser、ReleaseUser、GetISteamUser、GetISteamApps 对应 slot 0、1、2、4、5、15。
- [ISteamUser021：SDK 1.50](https://github.com/ValveSoftware/Proton/blob/e91ca2be0df2cef4c230cbbc0b86604d73a0bbf6/lsteamclient/steamworks_sdk_150/isteamuser.h)：BLoggedOn、GetSteamID 对应 slot 1、2。
- [ISteamApps008：SDK 1.47](https://github.com/ValveSoftware/Proton/blob/e91ca2be0df2cef4c230cbbc0b86604d73a0bbf6/lsteamclient/steamworks_sdk_147/isteamapps.h)：BIsSubscribedApp 对应 slot 6。

公开参考源码与 SHA-256 保存在本机 proton-design-references/steamclient-abi-sources.json；未将这些参考头文件直接合入宿主。

## 下一步

按上游 lsteamclient 的接口版本与生成器增加当前 Wine 配套 backend，先接真正的 SteamAPI_Init 路径、必要 SteamUser／SteamApps／SteamUtils、真实异步回调和票据取得／取消。明确测试 AppID、接口版本与释放责任后，再注册 compatibility tool 并接现有 launchExecutable。不要把本轮只读快照端点直接当成通用 Steamworks 实现。
