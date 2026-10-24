# Windows 游戏默认走 HmosDeck

## 问题

`logs-20261024-165831.zip` 中，CS2（730）被映射到 SteamLinuxRuntime_sniper，优先级 100；全局 HmosDeck 默认只有 75。运行时安装在创建 `pressure-vessel-arm64/bin/srt-logger` 符号链接时失败，游戏未进入 Wine。全局默认不能证明游戏已选择 Windows 路径。

## 当前策略

Linux ARM64 Steam 客户端保持原样，Windows 游戏由 OHOS Wine 执行：

- 全局 HmosDeck 默认优先级设为 100，已存在的旧值也会升级。
- 启动 Steam 前，为已安装游戏补全 HmosDeck 的逐游戏映射，优先级为 250，高于 Valve 的原生 Linux 默认 90/100/101。
- 已有 HmosDeck 低优先级映射一起升级，保留其配置参数和更高的现有优先级。
- 低优先级的 `native` / `SteamLinuxRuntime_*` 默认、内置 Proton 选择可迁移；用户明确强制的原生模式（250 及以上）和第三方工具保留。
- 从规范游戏库读取最多 1 MiB 的普通 appmanifest 文件，校验 AppID 和安装目录。Steam Linux Runtime、Proton、FEX、Steamworks 公共组件及声明为工具的条目不加 Windows 游戏映射；无效或链接清单不读取。
- 沿用配置备份、临时文件校验及原子替换。配置只在 Steam 启动前更新，不改写运行中客户端的配置。

只改变兼容工具配置，不清空下载库、不移动或修改游戏、不修改用户存档。Steam 按工具选择决定是否补下 Windows 版本差异文件。

## 生效和验证范围

升级应用后，完整退出并重新打开 Steam，已安装的 CS2 等游戏才会应用逐游戏修正。本次会话刚下载的游戏如仍继承 Linux 默认，应重开 Steam 后再验证；明确手工选择 HmosDeck 也可切换。

测试覆盖全新配置、75→100 升级、CS2 无独立映射、原生默认 100/101、用户明确选择、公共工具排除、重复启动不改写、备份失败保护、原子写入及 Windows Steam 隔离。配套游戏库迁移和诊断导出回归通过。

这修正的是默认启动路径。没有宣称通用 Linux Runtime 符号链接已适配，也没有据此宣称 CS2 的图形、Steam API、联机或反作弊功能已经通过实机验证。
