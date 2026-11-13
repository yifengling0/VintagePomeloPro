# Steamworks 接口桥静态覆盖审计

上游：Valve Proton `e91ca2be0df2cef4c230cbbc0b86604d73a0bbf6`。以下为独立 ABI 和传输编解码统计，不能作为游戏实机通过或票据认证成功的证明。

上游 212 个接口版本；已生成 212 个；声明 6539 个方法，其中 6327 个满足实际 C++ 传输约束；196 个结构、180 个回调支持按字段转换。方法数包含不同接口版本的重复方法。

已按 Git blob 摘要核验固定上游的 Windows 包装源文件，并逐槽比对 212 个虚表，包含重载序号及析构占位。

| 接口族 | 上游版本 | 已生成版本 | 声明方法 | 可编解码方法 |
|---|---:|---:|---:|---:|
| ISteamAppList | 1 | 1 | 5 | 5 |
| ISteamAppTicket | 1 | 1 | 1 | 1 |
| ISteamApps | 9 | 9 | 164 | 164 |
| ISteamBilling | 1 | 1 | 43 | 39 |
| ISteamClient | 17 | 17 | 523 | 508 |
| ISteamController | 7 | 7 | 170 | 170 |
| ISteamFriends | 17 | 17 | 795 | 795 |
| ISteamGameCoordinator | 1 | 1 | 3 | 3 |
| ISteamGameSearch | 1 | 1 | 14 | 14 |
| ISteamGameServer | 12 | 12 | 345 | 345 |
| ISteamGameServerStats | 1 | 1 | 10 | 10 |
| ISteamGameStats | 1 | 1 | 13 | 13 |
| ISteamHTMLSurface | 5 | 5 | 174 | 169 |
| ISteamHTTP | 3 | 3 | 65 | 65 |
| ISteamInput | 5 | 5 | 213 | 208 |
| ISteamInventory | 3 | 3 | 99 | 99 |
| ISteamMasterServerUpdater | 1 | 1 | 14 | 14 |
| ISteamMatchmaking | 9 | 9 | 264 | 264 |
| ISteamMatchmakingServers | 3 | 3 | 51 | 8 |
| ISteamMusic | 1 | 1 | 9 | 9 |
| ISteamMusicRemote | 1 | 1 | 32 | 32 |
| ISteamNetworking | 6 | 6 | 110 | 110 |
| ISteamNetworkingFakeUDPPort | 1 | 1 | 4 | 3 |
| ISteamNetworkingMessages | 1 | 1 | 6 | 5 |
| ISteamNetworkingSockets | 7 | 7 | 266 | 161 |
| ISteamNetworkingSocketsSerialized | 2 | 2 | 16 | 16 |
| ISteamNetworkingUtils | 4 | 4 | 89 | 72 |
| ISteamParentalSettings | 1 | 1 | 6 | 6 |
| ISteamParties | 1 | 1 | 12 | 12 |
| ISteamRemotePlay | 4 | 4 | 50 | 48 |
| ISteamRemoteStorage | 15 | 15 | 609 | 595 |
| ISteamScreenshots | 3 | 3 | 22 | 22 |
| ISteamTimeline | 2 | 2 | 22 | 22 |
| ISteamUGC | 19 | 19 | 1242 | 1242 |
| ISteamUnifiedMessages | 1 | 1 | 5 | 5 |
| ISteamUser | 20 | 20 | 425 | 425 |
| ISteamUserStats | 13 | 13 | 391 | 391 |
| ISteamUtils | 9 | 9 | 247 | 247 |
| ISteamVideo | 3 | 3 | 10 | 10 |

## 当前边界

- `SteamUser023` 已具备独立虚表、新版认证票据身份参数和 Web API 票据回调；远端欧卡仍须使用新候选实测。
- 数据结构按字段转换；Windows 指针、函数地址及 GNU 原始结构内存不能直接跨进程传递。
- 已补齐上游所有接口版本的声明及独立虚表；Windows 同名重载按 MSVC 规则分组倒序，GNU 保留原声明顺序；析构槽位、结构返回缓冲区和 CGameID 参数按各自 ABI 生成。不能将这一点等同于每个方法都可用。
- StringArray 输入已逐字符串复制到服务端本次调用拥有的存储；缓冲区按已审核的方向和长度处理，容量指针支持输入／输出以及空缓冲区查询。
- 未实现的方法明确报错。HTML 像素与网络消息对象所有权、函数回调／自定义信令、多进程启动器生命周期、完整历史回调 ABI 和超过 1 MiB 的存档传输仍需独立实现；逐项缺口见 JSON。
- 官方客户端是否提供某个接口，只有运行时才能确认；源核验通过不等于游戏服务器票据核验通过。

## 重建审计

```sh
python3 scripts/audit_linux_steam_bridge.py --catalog workspace_temp/steam-linux-bridge/generated/abi-catalog.json --upstream-tree <pinned-proton-tree.json> --reference <pinned-proton-cache> --output <audit.json> --markdown docs/linux-steam/STEAMWORKS_COVERAGE.zh-CN.md
```

先运行 `build_linux_steam_bridge.py`，它编译并执行与两端相同的 C++ wire traits。JSON 报告列出每个缺失版本和不可编解码的方法，不能仅按声明存在判断支持。

来源：[Proton lsteamclient](https://github.com/ValveSoftware/Proton/tree/e91ca2be0df2cef4c230cbbc0b86604d73a0bbf6/lsteamclient)、[Steamworks API](https://partner.steamgames.com/doc/sdk/api)、[身份认证与拥有权](https://partner.steamgames.com/doc/features/auth)。
