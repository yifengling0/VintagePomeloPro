# 1.4.4 更新说明

## Steam 下载

- 修正下载专用扫码授权，通过 Steam Client 的 CM 会话获取下载授权；应用内账号登录继续保留独立流程。
- 修正 PICS 的 `appinfo` 包装层解析，补齐受限内容资料访问令牌及大型资料获取，解决“depot 无可用 manifest”。
- 支持 Steam 返回的中国 CDN 类型和主机，并遵循服务器的 HTTPS 要求。
- 支持加密 manifest 文件名，兼容 Base64 中的换行和空白，解决扫码授权后仍无法完成下载的问题。
- 游戏下载到 `Download/com.vintage.pomelopro/games/steam/<appid>`，完成后加入本地游戏库；暂存目录不显示为游戏，已有目录不会被覆盖。
- 修正目录大小写导致的标题及已下载状态识别失败。
- 登录错误读取 Steam 返回的结果码，改善认证拒绝与请求频繁的提示。

## 版本与验证

- 应用版本：`1.4.4`，版本代码：`1004004`。
- 下载修复已通过 HAP 编译及 Steam 回归检查；平板实测 RPG Maker XP 完成下载并正确识别 `RPGXP.exe`。
- 启动 RPG Maker XP 会拉起 Windows 版 Steam；该客户端的登录窗口仍为白屏，尚未验证 RPG Maker XP 可正常使用。下载成功不代表需要 Steam 客户端认证的程序已能运行。
- Wine 运行时固定在 `1cb1ac93e464fe6db019845f3451f50ef6fc78f4`，本轮没有修改内核及语言传递链路。
- 详细设备结果见 [Steam 下载与启动验证](../steam-download-validation.md)。
