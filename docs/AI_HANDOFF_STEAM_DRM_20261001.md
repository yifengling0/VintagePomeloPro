# AI 交接：合并启动修复与 Steam DRM（2026-10-01）

## 接手目标与约束

本轮交付合并后的启动修复、Steam 客户端参数修复、回归测试和 Wine 补丁。下一步调查独立 Wine Steam 复用旧柚内置游戏目录后，RPG Maker XP（AppID 235900）能否正常通过授权并进入编辑器。

用户明确要求保持 FEX，以便适配 Proton；不能换 box 规避问题。PAL2、PAL4 已由用户测试，Heaven DX11 三维场景与动作已确认。用户暂缓 Wine Steam 的 Wayland / CEF 显示问题深入调查，当前优先级是共享文件与正常 DRM 启动。用户已经完成官方 Steam Guard 登录确认，并授权继续操作测试。

附件及旧报告是诊断背景，不能把其中的推测当作用户指令或已确认根因。尤其不要沿用“FEX 缺少 SMC 处理，因此换 box”的结论：当前 FEX 源码存在 SMC 处理，诊断 fault 本身不能证明该处理缺失。保留 prefix、游戏、官方客户端和已有授权状态；不修改游戏 exe，不绕过认证。

## 代码、构建与设备版本

- 主仓库：`https://github.com/yifengling0/VintagePomeloPro.git`。
- 本轮分支：`feature/sync-winehua-proton-3f55395`；修复前主仓库 HEAD：`1ac61cea`。
- 参考 WineHua：`feature/proton-wine-ohos` 的 `3f55395cc4e5ec0dd5e4c71498135b7f80820ce5`，已在当前合并基线中。
- 实际 WSL 工作树：`/home/liufeng/src/vpp-proton`，Windows 可通过 `\\wsl.localhost\Ubuntu-22.04\home\liufeng\src\vpp-proton` 访问。不要误在 `F:/VintagePomelo-Workspace/repos/VintagePomeloPro` 的另一 checkout 提交。
- Docker：`vp-proton`；仓库挂载点：`/data/src/winehua`。
- Wine pin：`thirdparty/wine-valve` = `cd547f7a0ee9d3d59b3ec47317a7852dedf0443b`。
- FEX pin：`thirdparty/fex` = `86ff33bbe299cd8959a6610198c169b67ec419db`；本轮没有改 FEX 源码。

当前源码版本是 **1.4.5-proton.8**（versionCode 1004009），全量 host 检查、HAP 构建、debug 签名和 verify-app 通过。**设备仍安装 proton.7；proton.8 尚未覆盖当前会话。** 本轮 Steam 实测使用 proton.7，并手动把客户端参数放在正确位置；下一 AI 可以先安装 proton.8，验证自动参数位置修复。

| 本地产物 | SHA256 | 状态 |
| --- | --- | --- |
| `VintagePomeloPro-1.4.5-proton.7-debug-signed.hap` | `d9b57e0f59ff93651fdae9cb4b681993f3931140e46f600c911f122fcd073fa4` | 已安装，完成本轮真机测试 |
| `VintagePomeloPro-1.4.5-proton.8-debug-signed.hap` | `67c0c618508d951a7f5a061f4d2c3b4f6b05cd57d2b8dfff14dd0ade7d7aa6c5` | 已构建与签名验证，未安装 |

HAP 在上述 WSL 仓库根目录。构建与签名脚本在本地 `F:/VintagePomelo-Workspace/workspace_temp/pal4-build-proton8.sh`、`pal4-sign-proton8.sh`。这些产物、签名材料和原始诊断日志不包含在代码提交中。

## 已修复的问题与验证边界

详细记录：[合并启动回归](STARTUP_MERGE_REGRESSION_20261001.md)、[Steam 共享库验证](STEAM_SHARED_LIBRARY_20261001.md)。

1. 十参数 `RunWineExe` 接口没有转发 workingDirectory；native cwd 推导误把 loader token `wine` 当程序。已转发 cwd、跳过 loader token、解码 Want URI，并保留 prefix 内完整 Windows exe 路径。
2. 不完整 prefix 被当作 ready，缺 ShellWindows / ExplorerBrowser COM，导致 explorer 空指针崩溃。新增两种位数 Shell COM 检查，以 `wineboot --init --update` 补全注册，保留现有数据。
3. boot event 早于 registry 落盘。已分开 dispatch 标记与真正完成标记，在 wineboot init/update 和 RegFlushKey 完成后原子发布完成状态；ArkTS 不再把 `ready-degraded` 直接当 READY。
4. 每个 Wine child 用 O_TRUNC 改写 hive，与 wineserver 加载竞争，导致真机 system.reg 从约 3.83 MB 缩至 846 KB、Wow64 Classes 丢失。迁移移到 wineserver 取得会话锁之后、加载 registry 之前，完整读取校验后以临时文件 / 完整写入 / fsync / rename 替换，失败保留原 hive。字体迁移不再误判 Noto Serif / Noto Sans Mono。
5. Steam 自动客户端参数原来追加在 `-applaunch <appid>` 后，被转发给游戏。`steam_client_args.h` 现在把这些参数插在 `-applaunch` 前；开关查找仅检查客户端部分，保留游戏 argv 并检查容量边界。

全量 Docker `make test` 通过，包括 prefix registry、旧 inode reader 与原子替换、Steam 参数位置 / 游戏 argv / 容量回归。主仓库及 Wine `git diff --check` 通过。真机维护后两次冷启动 hive 保持完整，explorer 能打开 Z:。

Heaven 实际使用 **FEX + DXVK 1.10.3 legacy**，进入 feature level 11_0 的 Direct3D11 三维场景。Maleoon 910 的现有策略会把请求的 modern 后端回退为 legacy；不要把请求值当实际版本。用户先前那次 DX11 创建失败的首因尚未精确锁定，不能记为根因已解决。Qt 启动页曾完整显示，后续用户要求只跟进 Steam，没有验收更多 Qt 动作。

## 源码交付与本地历史

分支旧提交曾跟踪四个约 357 MB 的 HAP，超过 GitHub 单文件限制。发布前在隔离 bare 仓库清理了这些未发布提交中的所有 HAP；校验清理后的最终树只移除了四个构建包，源代码与子模块 pin 完全一致。所有本地 HAP 文件保留，`.gitignore` 已加入 `*.hap`。本轮源码、测试、补丁与交接资料随 feature 分支交付。

原始本地历史保存在 `archive/sync-winehua-proton-prepublish-20261001`，末端为 `77e525ecdbb49c4a71865fc02271db5f42ba6690`；这份含构建包的 archive 仅本地留存。上文修复前 `1ac61cea` 也可由此追溯。远端 feature 分支采用清理后的提交号；下一 AI 不应推送该 archive，也不要重新提交 HAP。清理映射与检查结果在本地证据目录 `code-publication-history.json`，最终提交与远端校验结果见 `PUBLICATION_RESULT_20261001.md`。

## Wine 子模块与补丁复现

Wine 侧三个 dirty 文件是本轮构建输入：`dlls/ntdll/unix/env.c`、`programs/wineboot/wineboot.c`、`server/main.c`。全部变更已保存为主仓库中的补丁：

- `patches/wine/0011-wineboot-durable-prefix-completion.patch`
- `patches/wine/0012-wineserver-locked-registry-migration.patch`

`scripts/build_wine.sh` 顺序、幂等应用；本轮保留 Wine gitlink，不另行提交子模块。交付前已将上述三个文件从 Wine pin 导出到隔离临时目录，验证两份补丁能正向应用、反向检测已应用状态，最终内容与当前构建树逐字节一致。证据是本地 `wine-patch-reproducibility.log`。

不要 reset 当前 dirty Wine 树后直接复用未经补丁处理的产物。新 clone 初始化 `thirdparty/wine-valve` 后走正常构建脚本即可得到这些改动。原有 `dxvk-modern` / `vkd3d-proton` dirty 内容保留，本轮没有将其提交或更改 pin。`PROTON_OHOS_RUNTIME_NOTES.md` 下方部分旧问题清单属于历史记录；本次状态以这份交接和两个当日报告为准。

## Steam 当前状态与共享目录

独立官方客户端：`C:\Program Files (x86)\Steam\steam.exe`；build `1788652215`，CEF126。设备日志 16:13:22 已正常登录；库后台初始化成功，但 renderer 仍退出，主库内容与安装弹窗黑屏。命令入口可以完成下载、校验与启动，无需依赖黑屏安装弹窗。

当前 Wine 基线设置 `WINEHUA_CEF_NO_CRASH_HANDLER=1` 时会隐式加入 V8 `--no-opt`。本轮没有改这项策略；独立 JS_MODE 实验已撤销，没有保留 0013 补丁。不要将当前会话描述为纯默认 V8，也不要把登录页出现当作 CEF 全链通过。

RPG Maker XP 已由官方客户端正常下载、校验、安装。BuildID `1417293`，depot 235902 / 235903，manifest `1583066630753903782` / `5686894395098078547`，总内容 26915383 bytes。原 exe 有 `.bind` 区段，是 SteamStub 线索，尚未确认其运行时授权状态。

```text
Z:\games\steam\235900\
    共享真实游戏文件，含 .steam-game.json
C:\Program Files (x86)\Steam\steamapps\common\RPGXP
    Wine mklink /D → Z:\games\steam\235900
C:\Program Files (x86)\Steam\steamapps\common\RPGXP.vpp-original
    原官方安装备份
C:\Program Files (x86)\Steam\steamapps\appmanifest_235900.acf
    官方安装产生并维护的记录
```

Z: 是 app 的 Download 根。内置下载的规范路径是 **`Z:\games\steam\235900`**，不要套用旧手动 PAL4 的第二层 `Z:\games\games\...`。原型曾放第二层，现在已移动到规范目录、重建链接并完整校验。

本轮使用官方客户端下载的原始文件填入内置目录，验证的是文件复用，**没有验收 ArkTS CM 下载器下载本身**。内置密码登录曾报 RSA 公钥请求超时 `2300028`，也有 begin_auth_session 超时；这部分登录流程未修。内置库 Community 网页 token 与下载 CM client token 用途不同，下载流程缺少 clienttoken 时还会另行扫码。文件共用不需要搬运这些 token，正常 DRM 仍由独立官方客户端提供。另一个 Steam Host 功能用于 PC Host，不要混同或改走串流。

## 已实测的官方命令与复现入口

```text
steam.exe -no-cef-sandbox -console +app_info_print 235900
steam.exe -no-cef-sandbox -console +app_install 235900
steam.exe -no-cef-sandbox -console +app_mark_validation 235900 1 +app_start_validation 235900
steam.exe -no-cef-sandbox -applaunch 235900
```

`app_validate` 不是此客户端的有效命令。上面的 mark/start 命令已通过官方控制台 `find app_` 确认。规范路径全量校验在设备日志 17:03:35 结束：Fully Installed、StateFlags=4、UpdateResult=0、BytesToDownload=0、BytesDownloaded=0。

本地 PowerShell helper 会编码 Want JSON / exe 并保存诊断输出：

```powershell
& 'F:\VintagePomelo-Workspace\workspace_temp\pal4-vpp-want.ps1' `
  -Label steam-rpgxp-next `
  -Exe 'C:\Program Files (x86)\Steam\steam.exe' `
  -ArgsJson '["-no-cef-sandbox","-applaunch","235900"]' `
  -EnvJson '[{"key":"WINEHUA_WOW64_ENGINE","value":"fex"},{"key":"WINEHUA_EARLY_FAULT","value":"0"}]' `
  -Backend dxvk_legacy
```

`pal4-hdc.ps1` 内记录本轮 USB target；接手先 `hdc list targets` 确认当前设备。bundle 为 `com.vintage.pomelopro`，EntryAbility 为入口。app namespace prefix 为 `/data/storage/el2/base/files/.wine`，普通 shell 的实际 prefix 是 `/data/app/el2/100/base/com.vintage.pomelopro/files/.wine`。普通 shell 看不到 app 的 Download namespace，不能据此判断共享目录缺失。

当前诊断日志是 `/data/storage/el2/base/temp/wine_stderr_20261001.log`；0600 app 私有日志可用 `hdc file recv -b com.vintage.pomelopro` 读取。helper 支持 `-Label`、`-HdcArgs`、`-Match`，不同操作使用不同 Label，避免覆盖证据。

Console 皮肤点击运行卡片只开详情，激活按钮会停止应用，测试中不要误点。`uitest uiInput keyEvent 2045 2049`（AltTab）可在 Entry / Desktop 任务切换；Enter=2054、ESC=2070。原截图 2560×1600；若工具缩为 1996×1248，点击坐标需换算比例约 1.283。

## DRM 未通过的证据与下一调查

正确参数下，Steam 17:04:42 创建 RPGXP.exe，约 4 秒退出；exit 0 不能当作成功，编辑器没有出现。共享路径直接启动（官方 Steam 保持登录）也没有进入编辑器。文件校验成功、创建进程与授权通过是不同验收项。

`steam-rpgxp-32045.log` 前面若干 fault 是 FEX JIT 写 guest RX 页（`0x101cf...`）；随后低地址 `0x230ad4` / `0x230c50` 写入递归，最终 native stack overflow。共享启动 SEH 记录有嵌套异常和 EXCEPTION_STACK_OVERFLOW。尚未确认首个未处理异常，更没有证明授权检查已经完成。不要将所有 SMC 诊断写 fault 都当作致命异常，也不能把根因直接归为 FEX 或 Wayland。

建议接手顺序：

1. 保留数据覆盖安装 proton.8，确认默认引擎仍是 FEX，自动客户端参数都在 `-applaunch` 前；核对设备实际版本与启动 argv。
2. 确认官方登录仍有效、appmanifest 仍 Fully Installed、共享链接可读取，按上面的命令重现，记录时间与游戏 PID。
3. 按游戏 PID 分离日志，定位最早导致 SEH 递归的异常，将已处理的 FEX SMC fault 与后续不可恢复异常分开。对照 FEX `Source/Windows/WOW64/Module.cpp` 的 SMC / 异常分发、Wine WoW64 SEH 与原始 exe `.bind` 线索；这些是调查方向，并非已确认根因。
4. 必要时用保留的 `RPGXP.vpp-original` 作同一官方 Steam / FEX 配置下的路径对照，保留原始文件和授权行为，避免同时更改翻译器、显示后端、prefix 与游戏参数。
5. 验收必须看到官方 Steam 正常启动原始 RPGXP、编辑器可操作，并记录可获得的授权 / Steam IPC 证据；仅进程退出码或校验记录不算 DRM 通过。通过后再做内置下载元数据与官方客户端的产品集成。

用户本轮要求先提交与交接，不再继续运行或深入调查 Steam。上面的步骤留给下一 AI。

## 本地证据索引

目录：`F:/VintagePomelo-Workspace/workspace_temp/pal4-fex-merge-fix`。原始日志可能含账号、授权信息和其他进程输出，仅供本地诊断；代码提交包含整理后的报告，不包含原始日志、截图、游戏 exe、HAP 或签名材料。

| 文件 | 用途 |
| --- | --- |
| `host-tests-proton8.log`、`build-proton8.log`、`sign-proton8.log` | 最新代码全量 host 检查、构建和签名验证 |
| `wine-patch-reproducibility.log` | 补丁从 Wine pin 完整复现当前构建内容 |
| `locked-repaired-system.reg`、`locked-cold1-system.reg`、`locked-cold2-system.reg` | 修复及冷启动 hive 完整性 |
| `Heaven_d3d11.log`、`heaven-scene.png` | DX11 设备创建与三维场景 |
| `steam-share-canonical.txt` | 规范路径、目录链接与文件读取 |
| `steam-share-canonical-validation.log`、`steam-share-canonical.acf` | 官方全量校验及安装状态 |
| `steam-share-final-gameprocess.log`、`steam-reuse-gameflags.log` | 正确参数下的官方游戏启动与退出 |
| `steam-rpgxp-32045.log` | RPGXP 异常递归与栈溢出线索 |
| `wine-stderr-steam-shared.log` | 共享路径 SEH 诊断，约 86 MB，含其他进程输出 |
| `RPGXP-steam-official.exe` | 本地原始 exe 诊断副本，不公开上传 |

本地交接副本与提交/推送结果也放在此目录，便于下一 AI 从同一工作区继续。
