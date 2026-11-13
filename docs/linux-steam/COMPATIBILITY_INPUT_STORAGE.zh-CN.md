# Linux Steam：输入、目录与安装位置

## 本轮修改

- Wayland 键盘按“key → modifiers”顺序投递，避免 Xwayland 重复翻转 Caps Lock。左右修饰键分别跟踪，锁定键的重复按下不重复切换。
- Steamworks 接口桥补充 `ISteamApps::GetAppInstallDir`、`ISteamAppList::GetAppInstallDir`、`ISteamUser::GetUserDataFolder` 的有界字符串返回。目录来自官方 Steam 对象，解析 GNU 别名后转换成实际 C:/Z: 路径；未安装、无映射或容量不足时按接口语义失败，不伪造认证或所有权。
- 启动时分别映射程序、工作目录和独立的绝对路径参数。开关、URL 和已有 Windows 参数保持原义；路径必须属于真实磁盘边界。
- 组件页选择“C 盘 · 应用私有”或“Download · 公共下载目录”。新装默认私有，已有公共安装保留原位置；在线安装和本地导入都写入当前选定位置。两处独立，不自动搬移或删除账号、游戏、存档。运行或安装时禁止切换。
- 私有位置为 `.wine/drive_c/SteamLinuxARM64`。重置 Wine 原地保留这个确切目录，不跟随符号链接；卸载应用仍会删除私有数据。
- 更新 HAP 后，官方资源集合未变时，启动前可离线更新配套 GNU 适配器。只复用清单内大小和 SHA-256 均匹配的已有官方组件，完成后再激活。损坏时保留旧激活版本并要求显式修复，不自动联网兜底。
- 官方档案集合不变时继续使用已有离线导入包；外层包仍按 HAP 固定 SHA-256 验证，每个官方档案仍按当前清单验证。

## 下载连接

维护页新增可撤销的 HTTP/1.1 选项，默认使用 Steam 原策略。下次启动只在 `steam_dev.cfg` 中维护本应用标记块：

```
// BEGIN HMOSDECK DOWNLOAD CONNECTION
@nClientDownloadEnableHTTP2PlatformLinux 0
// END HMOSDECK DOWNLOAD CONNECTION
```

关闭选项仅移除该块，保留用户其他配置。标记损坏、符号链接或配置过大时拒绝改写，提示后继续使用原配置启动。未修改 DNS、下载地区、用户限速或认证规则。

这属于下载连接对照选项：客户端二进制包含对应控制项，但尚缺同一网络、同一游戏的 HTTP/2 与 HTTP/1.1 吞吐对照，不能据此宣称已解决下载慢。

## 验证与边界

宿主回归覆盖左右 Shift、Caps Lock 重复事件、C/Z 边界、中文与空格路径、目录返回容量、私有库重置保留、离线导入、位置往返、配置撤销以及适配器升级失败保留旧版本。Release ArkTS/NDK 构建与桥接组件配对检查通过。

目录不可写的具体故障游戏暂不在可用设备中，补齐目录接口不代表所有游戏已通过，也不代表 Steamworks 各历史版本和认证流程已经完整覆盖。需要该游戏的实际错误目录与一次启动日志才能确认剩余原因。
