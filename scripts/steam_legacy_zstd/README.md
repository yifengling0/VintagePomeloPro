# 旧版 Steam Zstd 下载兼容补丁 v2（2026-10-07）

此补丁针对用户提供的 `Steam.zip`：保留原 Steam 版本、CEF、`Steam.cfg` 和 `1.bat` 参数，为原有下载解包器增加 `VSZa`（Zstd）格式支持，并修复修改版客户端被启动时的模块签名检查拒绝的问题。ZIP / VZip（LZMA）/ gzip 等格式继续进入原代码。下载鉴权、原有解密、CRC 和下载内容校验仍执行。

v2 已在平板复现网页进程退出和主窗口丢失，暂不作为稳定包推广。前一版四 DLL 包只通过了解包函数测试，Windows 完整启动会提示 “There was a problem with your Steam installation. Please reinstall steam.”；`-noverifyfiles` 无法解决这项 SteamUI 内部检查。

当前 v3 候选仍包含 32／64 位客户端解包支持。仅 64 位解码 DLL 使用精简初始化入口和 Windows 进程堆，移除 MinGW CRT 初始化与 TLS；其余五个 DLL 与 v2 相同。Windows 正常地址／强制重定位各 180／181 项、Proton 平板强制重定位 181 项通过。平板完整客户端已启动、进入大屏，用户反馈本轮启动暂时稳定。尚未完成匹配条件的长时间 A/B；CRT/TLS 是否就是崩溃原因仍未证实。

2026-10-08 补充真机结果：平板用独立诊断覆盖包完成 Purrgatory（AppID 1713610）的真实 CDN 下载，210 个块中 56 个为 VSZa，其余为原解包器支持的格式；最终 `Fully Installed`、`result No Error`，没有解包失败。诊断包的 VSZa 解码逻辑与 v3 相同，普通 v3 的两个解码 DLL 已重建确认字节不变。此结果不代替反馈手机的实际文件身份检查，也不证明所有游戏或长时间启动稳定性。详见 `docs/architecture/steam-download-tablet-validation-20261008.md`。

可用 `VPP_STEAM_NOCRT64=1 bash scripts/steam_legacy_zstd/build.sh /absolute/helper-output` 构建此候选。默认构建保留 v2，便于对照。这个实验不降低下载块 CRC、最终内容校验或启动时 32 位客户端／解码器的精确哈希检查。

## 使用

完整包请解压到新目录，再用原来的 `1.bat` 启动并登录自己的账号。分享包已移除原包的 config、userdata、appcache、logs、dumps 和 ssfn 用户状态，不携带原用户登录信息。

覆盖补丁包用于现有同版本安装（含已应用 v1 的同版本安装）：完全关闭 Steam，备份原 `steamclient.dll`、`steamclient64.dll` 和 `SteamUI.dll`，再把下列六个 DLL 全部覆盖到 `steam.exe` 同级目录，然后重新启动。若 Wine 会话仍有旧 DLL 进程，请退出 Wine 会话再启动。

- `steamclient.dll`
- `steamclient64.dll`
- `vpsteamzstd32.dll`
- `vpsteamzstd64.dll`
- `SteamUI.dll`
- `vpsteamtrust32.dll`

原包 SHA256：`6fef7f410bdfcb0df852dd72e46aaa17b8d370caacd0f044f655e1febebc2153`。

只适用于原 DLL SHA256：

- 32 位：`95a7e01a64e20f0726b63cfb63b5240e51c7b15940b52d03e8da4c412ca54669`
- 64 位：`a2ad498763c4822fd4555c2cba4d4475a2ba450e403a774c1366826ad9451dd1`
- SteamUI：`f08296fb1345e489f97f7d36a2740d899e8a3a3aca0537e300bc9fda0e5e45f1`

恢复方法：关闭 Steam，恢复三个原 DLL，移除两个 `vpsteamzstd*.dll` 和 `vpsteamtrust32.dll`，重新启动。原 ZIP 保留不变。

## 实现和验证范围

新增的导入表位于可写数据区，短跳转位于原有可执行区的已验证零填充空间；没有 RWX 段，没有运行时改写已翻译代码。构建脚本拒绝其他版本及重复打补丁。原 DLL 的数字签名因此次二进制修改失效，包不是 Valve 官方发行版。

SteamUI 仅在加载客户端的三个调用点加入兼容层，共用签名验证函数保持原样。兼容层仅接受 SteamUI 同目录的 `steamclient.dll`，且修改版客户端与 `vpsteamzstd32.dll` 的完整 SHA256 必须与已测试文件一致；其他路径、其他版本、文件损坏或解码器缺失均继续调用原验证器。该例外不会跳过账号、游戏授权、下载块 CRC 或最终文件校验。

兼容层只接受正确的 VSZa 头尾、单个 Zstd frame、输出长度和 CRC。最大输出 128 MiB，并遵守原调用者的尺寸限制。缓冲分配和写入位置通过这套 Steam 的原 CUtlBuffer 方法更新。Zstd 解码器使用项目已有源码，以 BSD 许可分发，许可见同目录 `VPP-ZSTD-LICENSE.txt`。

公开测试样本已覆盖 ZIP、LZMA、Zstd，以及原 Steam 解密→解压→输出校验的完整函数链路；另覆盖 512 KiB 下载块、空块、CRC／frame／footer 损坏、尺寸限制、缓冲追加和并发。解包器在 Windows 32／64 位强制重定位测试及当前 Proton 手机环境通过。v2 的启动兼容层在 Windows 通过 14 项正常／拒绝回归，完整客户端已显示登录窗口，确认加载修改版客户端、Zstd 和启动兼容 DLL。v2 启动层的手机复测、账号登录后的实际下载和长时间运行尚未验证。

## 复现构建

在仓库构建环境运行：

```sh
bash scripts/steam_legacy_zstd/build.sh /absolute/helper-output
```

在安装了 `pefile` 的主机运行：

```sh
python scripts/steam_legacy_zstd/patch_client.py --zip /path/to/Steam.zip \
  --helpers /absolute/helper-output --output /new/path/Steam-legacy-zstd-v2.zip \
  --overlay /new/path/Steam-legacy-zstd-v2-overlay.zip
```

输出必须为新路径，脚本保存原 ZIP 并生成 SHA256 manifest。测试程序源码为 `decoder_regression.c`，fixture 为 SteamKit 公开样本及 host libzstd／OpenSSL 生成的确定性大块，诊断只使用公开测试密钥，不读取账号凭据。

## 下载仍失败时先核对实际文件

更新旧柚 Pro 的 HAP 不会替换用户游戏目录里的 Steam DLL。`content_log.txt` 中的解包错误也不能证明补丁已经加载。可在电脑对实际 Steam 目录做只读身份检查：

```sh
python scripts/steam_legacy_zstd/audit_install.py /path/to/Steam
```

检查只读取六个 DLL，不读取账号、config 或 CDN URL。输出能区分原版缺少 Zstd、完整 v2、完整 v3 候选及混用／缺失／未知文件。目录身份正确之后还应完全退出 Steam／Wine 冷启动，确认进程实际加载这些 DLL；脚本不把磁盘哈希正确当成运行中模块或游戏下载完成的证明。

## 独立解包诊断覆盖包

只有仍失败且六文件已核对的安装需要诊断。普通构建不带记录代码；诊断构建也必须显式设置 `VPP_STEAM_DECODE_DIAG=1` 才写日志。不能单独替换解码 DLL，客户端导入符号与启动哈希守卫必须配套。

1. 对原始 ZIP 中的 `steamclient.dll` 调用 `patch_client.patch(data, 'steamclient.dll', diagnostic=True)`，计算返回文件的 SHA256。
2. 使用上述哈希构建：

   ```sh
   VPP_STEAM_NOCRT64=1 VPP_STEAM_DECODE_DIAGNOSTIC=1 \
   VPP_STEAM_TRUST_CLIENT_SHA256=<诊断版32位客户端的SHA256> \
   bash scripts/steam_legacy_zstd/build.sh /absolute/diagnostic-output
   ```

3. 在 `patch_client.py` 的原有 ZIP 构建命令增加 `--diagnostic`，并指定该诊断 helper 目录，产生独立六 DLL 覆盖包。完全退出 Steam／Wine，备份原六 DLL 后一起覆盖。
4. 通过进程环境传入 `VPP_STEAM_DECODE_DIAG=1` 后冷启动，重试同一下载。日志位于 helper 同目录的 `vp-steam-decode-<PID>-32/64.log`。
5. 采集完成后退出 Steam／Wine，恢复备份的完整六 DLL 并移除诊断环境设置。

日志记录格式、失败阶段、长度、返回值、缓冲状态和当前客户端路径；不记录数据内容、解密密钥、账号或 URL。每个进程最多尝试写入 512 行，打开／写入失败也计入上限。大下载可能超过此上限；没有记录某个失败不等于它没有发生，必须同时检查 `content_log.txt` 的同期新增段。

32／64 位诊断均测试了原格式、VSZa、Steam 加密解包链路、损坏／超限、缓冲追加、并发、强制重定位和记录上限；诊断关闭时无日志。精确启动守卫继续拒绝异目录、未知、损坏或缺失的 client/helper。
