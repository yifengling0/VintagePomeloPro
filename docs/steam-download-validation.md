# Steam 下载与启动验证（2026-09-30）

本轮平板验证对象为 RPG Maker XP（Steam App ID `235900`，Windows 工具）。客户端下载授权、PICS 内容资料、CDN manifest、文件名及 chunk 解密、解压和文件发布均完成。游戏库刷新后显示正确名称，主程序选为 `RPGXP.exe`。

## 下载修复

- 下载专用扫码通过 CM WebSocket 的 Steam Client 授权流程获取客户端令牌；保持应用内 Community 登录独立。
- CM 未登录请求补齐 Steam realm；登录失败读取 HTTP 响应中的 `x-eresult`，避免把 HTTP 200 的认证拒绝误判为成功。
- PICS 支持 `appinfo → depots` 包装层，并在服务端要求时请求 app 访问令牌。大型内容资料支持服务端返回的 HTTP 地址，限定主机和响应大小。
- CDN 接受 `SteamCache` 与 `CDN` 类型及 Steam 返回的 `steamcontent.com`、`clngaa.com` 主机，并遵循服务器的 HTTPS 要求。
- 加密文件名按 Steam 格式解密。Base64 解码前移除空格、Tab、CR、LF；设备失败样本的编码末尾带 LF，这是最后一处下载阻塞。解密后去掉字符串终止 NUL，其余路径仍由下载写入器校验。
- 暂存文件写入 `games/steam-download-staging`，扫描跳过该目录。完成后发布到 `games/steam/<appid>`，不覆盖已有安装。
- 文档提供方实际返回 `games/Steam/<appid>`；标题和已下载目录识别均兼容大小写。

## 设备结果

- 下载完成路径：`Download/com.vintage.pomelopro/games/steam/235900`。扫描返回的目录名为 `Steam`。
- 本地设置显示“当前主程序：RPGXP.exe”。启动日志确认使用该目录下的 EXE，以及相同目录作为工作目录。
- Steam 游戏详情显示“本地游戏设置”，不再提供重复下载按钮。
- 16:42:28 创建了 RPGXP 启动进程；随后拉起平板已有的 Windows 版 Steam。16:43:20 Wine 窗口事件确认窗口标题为“登录 Steam”。实际窗口内容为白屏，尚未进入 RPG Maker XP 编辑器。

这次证据确认了内容下载和本地识别成功，没有确认程序可正常使用。应用内的下载授权没有建立 Windows Steam 的登录会话；仍依赖 Steam 客户端认证的程序需要另外完成该客户端的兼容性及登录验证。Steam manifest 加密是下载格式，不能据此判断某款程序是否具有运行时 DRM。

## 检查与产物

- `assembleHap` 已通过，日志为工作区外层 `workspace_temp/steam-catalog-case-build.log`。
- 设备安装包：`workspace_temp/steam-download-catalog-fixed-debug-signed.hap`。
- 未签名产物：`entry/build/default/outputs/default/entry-default-unsigned.hap`。
- `scripts/run_steam_integration_unit_tests.cjs` 已通过；新增 PICS 包装层、uint64 manifest ID、含换行的加密文件名、Unicode 文件名及解密失败不部分改写的回归检查。既有缓存、账号隔离、准确匹配、手动封面竞态及登录回归检查同时通过。
- 本轮修改不涉及 Wine 子模块、语言传递、应用图标及卡片布局。

日志和截图保存于工作区外层 `workspace_temp`，不作为仓库提交内容；公开说明只记录结果，不包含账号、令牌及二维码。
