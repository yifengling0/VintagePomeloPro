# 历史 Steam Zstd 测试包 v2：启动检查修复

日期：2026-10-07。此版保留用户提供的历史 Steam 客户端、CEF、启动参数及禁止自动更新配置，不修改 Wine、FEX、图形路径或 HAP。

## 修复与 Windows 结果

用户反馈 Windows 启动提示 “There was a problem with your Steam installation. Please reinstall steam.”。已复现：补丁版 steamclient 触发错误，原 DLL 不触发；`-noverifyfiles -norepairfiles` 无效。

错误来自 SteamUI 的 `CSteamGamesUIModule::LoadSteamClient`，其 `InternalAPI_Init_Internal` 在模块加载前检查 Valve 文件签名。修改 steamclient 后签名不再有效，之前只验证了解包函数调用，漏验完整客户端启动。

v2 修改精确版本的 `SteamUI.dll`，将客户端加载函数中的三个签名检查调用点（RVA `1a3d74`、`1a3dce`、`1a3e63`）转给 `vpsteamtrust32.dll`。兼容层只允许 SteamUI 同目录的修改版 `steamclient.dll`，且客户端与 `vpsteamzstd32.dll` 的完整 SHA256 均须匹配已验证文件。其他版本、其他路径、损坏文件、缺失解码器继续调用原检查。共用签名验证函数 RVA `74ed80` 的代码保持原样。

没有修改账号登录、游戏授权、下载块解密、CRC 或下载内容校验。新增导入和重定位在 RW 数据段，小型 thunk 位于原 RX 段已验证的零填充空间；没有 RWX 段或运行时修改翻译代码。

Windows 14 项正常／拒绝回归全部通过，覆盖相对／绝对路径、大小写、相同文件不同目录、未知模块、客户端和解码器损坏、解码器缺失及恢复。补丁生成可重复，未知输入和重复修补被拒绝。

Windows 完整启动已显示“登录 Steam”窗口。进程 42156 实际加载本测试目录的修改版 SteamUI、steamclient、vpsteamtrust32 和 vpsteamzstd32；登录窗口由同目录 CEF 子进程 31688 显示，窗口范围为 `(608,320)-(1313,760)`。原有 `E:\Game\stream\steam.exe` 进程 16052 保持运行，本次用 `-master_ipc_name_override vpp_zstd_test_20261007` 隔离测试实例。

本机安装目录：`C:\Users\liufeng\Downloads\Steam\Steam`。原三个 DLL 的备份：`F:\VintagePomelo-Workspace\workspace_temp\steam-download-failure-20261007\windows-original-backup-20261007-190128`。

## 文件与交付

请发布 **v2**，替代先前的四 DLL 包。

| 文件 | 作用 |
| --- | --- |
| steamclient.dll | 原 32 位下载解包器增加 VSZa 分发 |
| steamclient64.dll | 原 64 位下载解包器增加 VSZa 分发 |
| vpsteamzstd32.dll | 新增 32 位 Zstd 解码器 |
| vpsteamzstd64.dll | 新增 64 位 Zstd 解码器 |
| SteamUI.dll | 客户端加载的三个调用点接入 SHA256 兼容检查 |
| vpsteamtrust32.dll | 对已验证客户端／解码器文件提供固定哈希例外 |

完整包：`Steam-legacy-zstd-v2-20261007.zip`，332,885,091 bytes，SHA256 `5199545b2ffd04b9e6582798ef5c4c42e1dfdd943e69872f5f5ab69fbd8787cd`。

覆盖包：`Steam-legacy-zstd-v2-overlay-20261007.zip`，23,784,043 bytes，SHA256 `b25e88542d310fd0f12e8882fe68ab159ad004dc132efb4cac79981cb0f93121`。

各包 manifest 记录全部新增／修改 DLL 的身份；`package-verification-v2.json` 记录包 CRC、两包六 DLL 与实际安装文件一致、未修改成员及隐私状态移除检查。`steam.exe`、原 `1.bat`、`steam.cfg` 逐字节保持不变。

完整包解压到新目录，用原 `1.bat` 启动并登录自己的账号。覆盖包须先完全关闭目标 Steam／Wine，备份原三个 DLL，再将六个 DLL 全部覆盖到 `steam.exe` 同级目录；仅支持用户提供的精确历史版本，也可用于该版本已安装 v1 的目录。不清理已有用户账号或游戏数据。原 DLL 的签名因此次修改失效，此包是非官方兼容测试包。

## 如何验证解包和真实下载

已有解包验证：Windows 32／64 位各 181 项，当前 Proton 手机 32／64 位各 180 项，均无失败。使用这套 Steam 的原 AES 解密→解包函数处理公开 ZIP／LZMA／Zstd 样本，核对最终 SHA1，另覆盖大块、空块、追加、损坏输入和并发。v2 没有修改这四个解包 DLL。

真实下载验证必须登录后进行，优先复测原 AppID 1942280、depot 1942281。此前日志始终剩余 159 块：

1. 记录开始测试的时间，在 v2 中恢复原下载；无需先删除已下载内容。
2. 确认下载／stage 持续推进，越过原卡点，剩余失败块减少，最终显示已安装。
3. 查看 Steam 目录 `logs/content_log.txt` 的**本次新增记录**，确认没有新的 `Failed unpacking chunk`，并出现目标应用安装完成／Fully Installed 的状态。
4. 在 Steam 游戏属性执行“验证游戏文件完整性”。完成后尝试启动游戏，分别记录下载成功和游戏运行结果。

只看到瞬时下载速度、只通过 DLL 样本或只启动客户端均不能宣称游戏下载已解决。如果仍失败，请保存同一时间的 content 日志，分享前移除 CDN URL token 和账号信息；区分解包输出 0、CRC、网络或写盘错误。

本次没有登录账号或完成实际游戏下载；v2 启动层的手机复测和长时间运行也尚未完成。此前手机解包函数回归不等于 v2 手机完整启动验证。

## 源码与证据

权威源码目录：`/home/liufeng/src/vpp-proton/scripts/steam_legacy_zstd/`。新增 `vpsteamtrust.c`、`patch_startup.py`、`trust_regression.c`，更新 `build.sh`、`patch_client.py` 和 README。Zstd 解码源码保持不变。

完整构建时，如果编译器或 Zstd 源变化导致解码 DLL 的 SHA256 改变，须重新完成解包回归并更新固定哈希；启动层会拒绝未经确认的新文件组合。

本地证据：`workspace_temp/steam-download-failure-20261007/windows-client-*-start.json`、`windows-v2-ui-snapshot.json`、`windows-startup-trust-regression.tsv`、`windows-install-check-xrefs.txt` 及 v2 manifests。原始账号数据和 Steam 二进制不提交仓库。本轮没有 commit／push。
