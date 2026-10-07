# 历史版 Steam 下载解包兼容修复（2026-10-07）

## 结论与证据边界

用户提供的 `Steam.zip` 中，32 位 Steam 下载解包器缺少 VSZa / Zstd 格式支持。在原生 Windows 和当前 Proton 手机环境中，直接调用该包未经修改的 Steam 解包器，对公开、正确的 Zstd 样本均返回失败、输出 0 字节、分配缓冲 0 字节；同一解包器对 ZIP 和 VZip / LZMA 样本成功，输出内容 SHA1 正确。该缺口可以在不使用 FEX 的 Windows 上复现。

已为这份包的 32／64 位 `steamclient` DLL 增加仅处理 VSZa 的入口。修补版在 Windows 和当前手机环境中通过原 Steam 解密→解压→输出校验函数链路，包含真实公开样本和一个 512 KiB 的确定性加密下载块。原有格式继续通过原解包器。Steam、CEF、启动参数和自动更新设置保持原版。

其他用户 `content_log.txt` 的失败特征与该缺口吻合，且用户确认反馈设备使用的就是此历史包。不过附件未携带实际失败块的解密后格式，不能把 528 条失败都断言为 Zstd；错误密钥、损坏块等其他原因仍需按实际样本区分。**本轮没有完成已登录账号的实际游戏下载，不能宣称目标游戏已下载成功。**

## 输入

- 原 ZIP：`C:/Users/liufeng/Downloads/Steam.zip`，350,790,398 bytes；SHA256 `6fef7f410bdfcb0df852dd72e46aaa17b8d370caacd0f044f655e1febebc2153`，8,064 个条目。
- 原 `steamclient.dll`：19,253,608 bytes，SHA256 `95a7e01a64e20f0726b63cfb63b5240e51c7b15940b52d03e8da4c412ca54669`。
- 原 `steamclient64.dll`：22,645,968 bytes，SHA256 `a2ad498763c4822fd4555c2cba4d4475a2ba450e403a774c1366826ad9451dd1`。
- 其他用户 `content_log.txt`：213,904 bytes，1,200 行，SHA256 `b249449080eceb301f42e629406a464c6bb8c71c609d4c6eceffd60b5788da5d`。这是其他用户设备日志，与当前无线手机上的测试分开。
- 公开样本：SteamKit `Tests/DepotChunkFacts.cs` 与其 `Files/*.bin`；测试密钥是该项目公开 fixture，未使用用户账号密钥。

原 ZIP 的 `Steam.cfg` 明确包含 `BootStrapperInhibitAll=Enable`、`BootStrapperForceSelfUpdate=False`。用户说明选择历史版的原因是较新客户端在 FEX 下不够稳定。本轮保留该选择。

## 内容日志定位

目标 AppID 1942280、depot 1942281。528 次 `Failed unpacking chunk` 对应 159 个不同块；所有记录的 `u:0,b:0`、`c` 为正且 16 字节对齐。80 个相同块跨多个 CDN 失败，各次记录的 `c/r` 长度一致。三个主机分别为 `st.dl.eccdnx.com`、`xz.pphimalayanrt.com`、`dl.steam.clngaa.com`。

目标 depot 鉴权 9 次成功；日志没有 HTTP、TLS、DNS 或 timeout 错误记录。重试始终剩余 159 块，下载 `87805232/161064320`、stage `93323405/245990176` 没有推进。解包失败后 Steam 标坏下载来源，最终呈现 `Content unavailable / No Download Source`。因此优先调查本地解包，而不是依据最终 UI 提示修改 DNS。

日志中的 AppID 7 ownership ticket 拒绝不是目标游戏的鉴权失败证据。`Client version: 0` 也不是可用的客户端真实 build 标识。没有直接把日志未出现网络错误当作所有网络环节都正确的证明。

此前 BCrypt / AES-NI / CRC32C 手机测试仅覆盖基础计算和解密数据，没有调用 Steam 自带解包器。本轮进一步使用用户包的实际代码，补足了这项边界。

## 根因复现

32 位 `steamclient.dll` 的下载入口调用内部解密解包函数 RVA `0x363b90`；压缩格式分发器 RVA `0xa1cb00`。后者识别 gzip、VZa / LZMA、PKZIP、zlib。对于 `VSZa`，第二字节 `S` 不满足 VZa 分支，后续也不满足其他分支，返回格式失败，尚未给输出缓冲分配空间。

64 位对应解密解包函数 RVA `0x3fcea0`、压缩分发器 `0xb9a1d0`，同样缺少该格式。

未经修改的 32 位 DLL 实测：

| 公开样本 | 正确输出长度 | Windows | 当前手机 |
| --- | ---: | --- | --- |
| PKZIP | 544 | 成功，SHA1 一致 | 成功，SHA1 一致 |
| VZip / LZMA | 798 | 成功，SHA1 一致 | 成功，SHA1 一致 |
| VSZa / Zstd | 156 | result=2，output=0，allocated=0 | result=2，output=0，allocated=0 |

公开参考 [SteamKit 增加 Zstd depot 支持的提交](https://github.com/SteamRE/SteamKit/commit/2edc80b96e96dd231df3be996b269aec10144f01) 日期为 2025-05-09。该日期仅表示此库支持的提交日期，不能用来断言服务器切换日期，或保证某个日期之后的所有 Steam build 都支持。

本地另一套 Steam 的官方更新日志显示 installed version `1788652215`，它与用户本轮提供的冻结包不同，不能混用两套版本证据。

## 修复

源码：`scripts/steam_legacy_zstd/`。

1. 构建 `vpsteamzstd32.dll`、`vpsteamzstd64.dll`，静态使用项目已有 BSD 许可 Zstd 解码源码。没有修改 Wine、FEX、Mesa、图形路由或 HAP。
2. `patch_client.py` 只接受上述两份原 DLL 的 SHA256 与机器类型；拒绝其他版本、重复修补、异常入口和不安全 ZIP 路径。
3. 在原 `.text` 已核实为零的尾部空间放入短分发跳转。数据是 `VSZa` 时尾调用 helper；其他格式重放原函数序言，继续原入口。新增导入及 x86 重定位数据位于 RW 数据段；原代码段为 RX，没有 RWX 段，没有对正在翻译的代码做动态热补丁。
4. 新格式检查头尾、CRC 前后一致、单个完整 Zstd frame、输出长度、CRC，以及调用者输出上限。最大输出 128 MiB。损坏数据返回原 Steam 使用的失败码；不伪造解包成功。
5. 输出使用原 CUtlBuffer 分配和 SeekPut 方法，维持已有缓冲追加、长度和最大写入位置。不改变 Steam 账号鉴权、解密和后续内容校验。

原 DLL 签名因二进制修改失效，已清除无效 Authenticode directory 的声明；原证书字节仍保留在 overlay，未将修改版标为 Valve 官方包。构建是针对这份历史包的兼容方案，版本变化需要重新复核 RVA 和 ABI，不能泛化到任意 Steam。

## 验证

| 环境 | 检查数 | 结果 |
| --- | ---: | --- |
| Windows PE32，强制占用原首选基址 | 181 | 0 failures，exit 0 |
| Windows PE64，强制占用原首选基址 | 181 | 0 failures，exit 0 |
| 当前 Proton 手机 PE32 | 180 | 0 failures |
| 当前 Proton 手机 PE64 | 180 | 0 failures |

差一项是 Windows 的显式强制重定位断言。手机记录的实际 DLL 基址也与首选基址不同。手机结果先写入空 TSV 再启动，结果包含 PID 与 UTC 时间，避免误读从宿主传入的旧结果。

覆盖：ZIP / LZMA 原路径，Zstd 新路径和缓冲追加，三种公开样本的原 Steam AES 解密→解压链路与输出 SHA1，512 KiB 加密大块、空块、损坏 footer／CRC／frame、尺寸上限和溢出字段、四线程并发。两个 helper 均以 `-O2 -Wall -Wextra -Werror -static-libgcc` 编译。已核实两个架构的字节补丁可重复生成，错误输入和已修补输入被拒绝。

此处的完整链路指 Steam 内部 chunk 处理函数，不包含实际账号登录、CDN 拉取和最终文件安装。真实用户失败块、实际下载完成、长时间运行和菜单／游戏兼容还需补证。

## 交付物

目录：`F:/VintagePomelo-Workspace/artifacts/steam-legacy-zstd-20261007/`。

- 完整包 `Steam-legacy-zstd-20261007.zip`：332,456,934 bytes，SHA256 `3b56355c7dc025f8e5303ecf9c2ae449379dbb5a51189e4c3e3b8fb85630313f`。
- 覆盖补丁 `Steam-legacy-zstd-overlay-20261007.zip`：17,511,045 bytes，SHA256 `a1f5fce8f9618ce144aef99101491d301e71a4249ea82ded6449db36b34d353b`。
- 各包对应 `.manifest.json` 和 `package-verification.json`。

完整包包含原版代码资产，移除原 config、userdata、appcache、logs、dumps、ssfn 用户状态，添加两个 helper 与说明／BSD 许可。7093 个保留且未修改的条目大小和 CRC 与原包一致；`steam.exe`、`1.bat`、`steam.cfg` 单独逐字节比较一致。完整包与补丁包 CRC 检查通过，相同的四个 DLL 逐字节一致。原 ZIP 未修改。

修补后 DLL SHA256：

- `steamclient.dll`：`9c45de0b9a5fcc67e6bbfb28f54288da0b19f60332a750944dfc58e6aebddecd`
- `steamclient64.dll`：`bb7878cf75b3878d3c365572e0985b69d5850b39dd00e2d3e27e0157246324f9`
- `vpsteamzstd32.dll`：`57c1b0503d131853aa899a896d3a0077b9f62a44ec5545dd6ce58e0b798e0faf`
- `vpsteamzstd64.dll`：`9d561b3033904e240c89e7eab1d2a9585fa6b4ee3cdaf649bd387d01319b8c84`

## 使用与下一轮实际下载验证

现有同版本用户：完全关闭 Steam，备份原两个 steamclient DLL，把覆盖包内四个 DLL 全部放到 `steam.exe` 同级目录，再启动。如 Wine 会话仍持有旧模块，退出 Wine 会话再重启。覆盖补丁不清理现有用户的账号或游戏数据。

新用户：把完整包解压到新目录，用保留的 `1.bat` 登录自己的账号。

下一轮优先复测原 AppID 1942280：记录失败前的基线，重启后恢复同一下载，确认剩余 159 块减少、stage 前进、最终安装完成，以及 content 日志是否仍有解包失败。保留目标游戏鉴权；不要通过关闭校验掩盖错误。如果继续失败，取得同时间的脱敏 content 日志，确认是否仍是输出 0 或变成 CRC／写盘失败，再对该阶段定位。

恢复：关闭 Steam，恢复原两个 steamclient DLL，移除两个 helper，再启动。原包保留，可恢复到完全原版。

## 证据位置

本地：`workspace_temp/steam-download-failure-20261007/`，含脱敏 content 摘要、公开 fixture manifest、未修补 DLL Windows／手机 TSV、修补版四种环境 TSV、反汇编摘要与原包 identity。无线手机命令 ledger 与退出事件在 `workspace_temp/phone-process-liveness-20261007/`，固定目标为本轮指定的手机，没有使用 USB 平板。

仓库可分享证据放 `docs/architecture/evidence/steam-legacy-zstd-20261007/`。只保存必要的脱敏摘要、TSV、测试源码和 manifests；不提交原始 CDN token、账号缓存、用户原 ZIP、Steam 二进制、HAP 或构建目录。本轮源码／报告尚未提交。
