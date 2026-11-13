# 独立 Wine Steam 复用旧柚内置库（2026-10-01）

## 结论和范围

游戏文件可以共用。真机使用 FEX，已将官方 Steam 的 RPG Maker XP 安装目录链接到旧柚内置下载器使用的目录，并由官方 Steam 完整校验：Fully Installed、UpdateResult=0、BytesToDownload=0。正常 DRM 启动还未通过：Steam 确实创建 RPGXP.exe，但它在异常分发中递归并栈溢出，编辑器未出现。文件校验成功不代表游戏运行授权检查成功。

这里的“系统 Steam 库”按用户前文理解为旧柚主界面内置 Steam 库。用户要求暂缓 Wine Steam 的 Wayland / CEF 界面调查；本次没有继续 CEF 模式实验，没有切换到 box。

## 真机文件复用

内置下载器原本发布到 `Download/com.vintage.pomelopro/games/steam/<appid>`，只写 `.steam-game.json`。它没有生成官方 Steam 的 appmanifest，也没有将启动路由到 Steam，因此原来的“下载完成”不能等同于官方客户端“已安装”。

本次测试前，内置下载目录和独立 Steam 均没有 RPG Maker XP。内置密码登录在获取 RSA 公钥时出现传输超时 2300028；没有为此索取密码或复制认证 token。使用已经登录的官方 Wine Steam 命令入口完成下载，再把这些原始文件放入内置下载目录，用于验证文件复用。此测试没有验收 ArkTS CM 下载器本身。

官方返回并实际安装的资料：

- AppID：235900；安装名：RPGXP；启动程序：RPGXP.exe。
- BuildID：1417293；两个 depot：235902 / 235903。
- manifest：1583066630753903782 / 5686894395098078547。
- 完整内容：26915383 bytes；官方保留原始 exe 和安装脚本。

设备最终布局：

```text
Z:\games\steam\235900\                 共享的真实游戏文件，含 .steam-game.json
C:\Program Files (x86)\Steam\steamapps\common\RPGXP
    └─ Wine mklink /D → Z:\games\steam\235900
C:\Program Files (x86)\Steam\steamapps\common\RPGXP.vpp-original
    └─ 原始官方安装备份，保留用于恢复
C:\Program Files (x86)\Steam\steamapps\appmanifest_235900.acf
    └─ 官方 Steam 正常安装产生并维护的记录
```

`Z:\` 映射到当前 app 的 Download 根；规范内置下载路径是 `Z:\games\steam\235900`。初次原型放在已有手动游戏的第二层 games 目录，随后已移到上述规范路径并重建链接。

Windows 目录链接在 Wine 中读取成功；最终仍需以 app 内的 Windows 路径访问，普通 hdc shell 不能据此直接判断 app 的 Download 文件是否存在。没有移动或删除用户原有游戏。

## 无需安装弹窗的官方命令入口

以下命令均由平板中的原始 Wine Steam 执行，使用它已有的正常登录状态：

```text
steam.exe -no-cef-sandbox -console +app_info_print 235900
steam.exe -no-cef-sandbox -console +app_install 235900
steam.exe -no-cef-sandbox -console +app_mark_validation 235900 1 +app_start_validation 235900
steam.exe -no-cef-sandbox -applaunch 235900
```

`app_install` 在 16:43:09（设备日志时间）完成下载、校验和提交；规范共享路径的全量校验在 17:03:35 完成，回到 Fully Installed，没有补下载。17:04:42 官方客户端再次创建 RPGXP.exe，17:04:46 退出，仍未进入编辑器。`app_validate` 不是该客户端的命令；可用命令已通过官方控制台 `find app_` 确认，使用的是上面的 mark/start validation。

账号许可仍需官方 Steam 提供。Community 网页 token、内置下载器的 CM client token、官方客户端的 Steamworks IPC 是不同用途；文件共享不要求搬运 token，也不能把内置网页登录当作 DRM 已成立。后续产品集成应保留真实 depot/build 元数据，让官方客户端确认文件和执行安装脚本，再通过 Steam 启动。

## 启动结果与修复

第一次 `-applaunch` 时，WineChild 把默认 `-no-cef-sandbox` 追加在 AppID 后，Steam 将它误传给 RPGXP.exe。已修正 `apply_steam_client_default_args`：所有自动加入的客户端参数放在 `-applaunch` 之前，检查开关时也不再把游戏参数当作客户端参数。新增 host 回归检查游戏 argv 保持原样、客户端开关位置和数组容量边界。

手动把参数放在正确位置后，官方 Steam 的 gameprocess_log 已记录不带这个多余参数的 RPGXP.exe；进程仍在约 4 秒内退出。原 exe 有 `.bind` 区段，不能假定它不需要 DRM。原始 exe 未修改，也未加入授权绕过。

独立 Steam 启动和共享路径直接启动（Steam 保持登录）均未进入编辑器。共享路径的 `trace+seh` 记录了嵌套异常和 EXCEPTION_STACK_OVERFLOW；当前不足以判断授权检查是否已通过，也不足以把首因归为 Wayland。后续应调查 RPGXP / FEX / WoW64 的首发异常及异常处理递归。

## 证据与构建

本地证据在 `F:/VintagePomelo-Workspace/workspace_temp/pal4-fex-merge-fix`：

- `steam-share-canonical.txt`：规范共享路径、链接后的 exe 读取与内置标记。
- `steam-share-canonical-validation.log`、`steam-share-canonical.acf`：规范共享路径全量校验与无需补下载。
- `steam-share-final-gameprocess.log`：规范路径校验后官方客户端的正常游戏启动记录。
- `steam-reuse-gameflags.log`、`steam-rpgxp-32045.log`：正确参数下的官方启动与栈溢出。
- `wine-stderr-steam-shared.log`：共享路径启动的 SEH 诊断，包含其他进程日志，只用于本地诊断。
- `host-tests-proton8.log`：新增 Steam 参数回归及全量 host 检查。

主仓库分支为 `feature/sync-winehua-proton-3f55395`，本轮参数修复与报告随该分支提交交付。下一调查步骤及本地入口见 [AI 交接](AI_HANDOFF_STEAM_DRM_20261001.md)。参数修复打包为 `1.4.5-proton.8`，全量 host 检查、HAP 构建、debug 签名和 verify-app 均通过；没有改动基线 CEF/V8 策略。真机此轮测试使用已安装的 `proton.7` 和正确排列的显式参数，新包尚未覆盖当前会话。原始账号/认证/下载日志不公开发布。

新包：`VintagePomeloPro-1.4.5-proton.8-debug-signed.hap`，SHA256：

`67c0c618508d951a7f5a061f4d2c3b4f6b05cd57d2b8dfff14dd0ade7d7aa6c5`
