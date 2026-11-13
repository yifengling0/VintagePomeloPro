# Steam 客户端导入与版本选择

Steam 组件页同时提供两种方式：

- Windows Steam：导入原来的 Windows Steam ZIP，安装在 `Z:\Steam`，通过已有 Wine 路径运行。
- Linux ARM64 Steam：保留在线安装和本地组件包导入，使用选定的 C 盘私有位置或 Download 位置。

组件页、经典界面的 Steam 入口及 Deck 的 Steam 操作共用客户端选择服务。
只安装一个版本时直接启动该版本；两个版本都有时，每次点击启动可选择 Windows 或 Linux ARM64。
取消选择不会启动任何客户端。没有安装时进入组件管理。
导入完成停留在管理页，不自动启动 Steam。

Windows 导入复用原有事务与中断恢复逻辑，更新保留已有 steamapps、userdata、config、depotcache 和 ssfn 文件。
Linux 组件的安装、重置、存储位置不影响 Windows Steam；两个版本的游戏库不自动合并或搬移。
普通游戏卡片仍按原来所属客户端启动，不套用客户端版本选择。

当前两种客户端共用 Wine 环境，切换版本前需正常退出另一种客户端及其游戏。
启动器检查正在启动／运行的会话及组件更新状态，不自动结束游戏或关闭客户端。
Windows 的大屏、普通窗口、下载和账号入口发送到 Windows Steam；Linux 入口沿用现有会话通道。

自动验证：`scripts/run_steam_client_selection_tests.cjs` 覆盖未安装、单版本、双版本、取消、重复点击、选择后安装状态变化、会话互斥及启动失败后重试。
`scripts/run_steam_local_unit_tests.cjs` 与 `scripts/run_steam_local_service_tests.cjs` 验证原有 ZIP 更新保留、Windows 启动参数及会话适配。
