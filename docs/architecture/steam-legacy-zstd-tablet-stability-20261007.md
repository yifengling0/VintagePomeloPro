# 历史 Steam Zstd v3 候选：64 位解码器初始化对照

日期：2026-10-07。状态：候选测试包，尚未确认崩溃根因，也尚未验证真实游戏下载完成。保留历史 Steam、CEF 和 32／64 位解包支持，不修改 HAP、Wine、FEX 或 GPU 配置。用户已授权提交源码和验证摘要；Steam 二进制、ZIP、HAP 与原始账号日志仅保存在本地。

## 当前平板状态

设备为 MatePad Mini，包 `com.vintage.pomelopro`，实际安装版本 `1.4.5.26 / 1004035`，不是此前生成的 1.4.5.27 发布 HAP。

原包的 `steam.exe` 是 PE32/I386；`bin/cef/cef.win7x64/steamwebhelper.exe` 是 PE64/AMD64。64 位鸿蒙系统仍通过现有兼容运行时执行这两种 Windows 程序。不能依据系统位数移除 PE32 客户端；本候选同时保留 PE64 客户端的 Zstd 支持。

当前目录为 `Z:\games\steam-legacy\Steam`。已恢复两个带解包补丁的 steamclient 和修改版 SteamUI，只将 `vpsteamzstd64.dll` 换为精简初始化候选，另外五个 DLL 与 v2 字节一致。实际目录的六个 DLL 已复制回电脑核对 SHA256，随后完整停止旧 Wine 会话并冷启动。当前不再是“原版 64 位客户端＋补丁 32 位客户端”的隔离组合。

20:29:46 启动 Steam，64 位网页进程随后启动。普通主窗口显示商店，之后进入大屏。用户反馈这轮启动暂时稳定；20:39:55 截图仍显示大屏，Steam 和最初的网页进程均存活，观察窗口约 10 分钟。

测试保持 GPU 开启，参数含 `-cef-in-process-gpu -cef-single-process -no-shared-textures`，不含 `-cef-disable-gpu`。原包的 `1.bat` 本身有禁用 GPU 的参数，不应把直接运行它的结果当成与本轮相同的 GPU 对照。

## 精简初始化如何保留解包

完整 Zstd C 解码源码仍链接进 DLL，导出仍为 `VPDecodeVSZa`，Steam 内原有格式分流、CUtlBuffer 分配和游标更新方式不变。其他压缩格式继续原代码；VSZa 仍检查头尾、单个 frame、尺寸限制、长度及 CRC。Steam 的原 AES 解密和最终输出验证仍执行。

64 位候选的 DLL 入口只返回成功，不运行新增 DLL 的 MinGW CRT/TLS 初始化。算法所需的 malloc/calloc/realloc/free 由 Windows 进程堆提供，内存复制函数由精简 C 实现提供。此解码器没有需要自动初始化的 C++ 全局对象或线程局部算法状态。候选只导入 KERNEL32，TLS 目录为 0；这不会移除 Steam 自身使用的其他运行库。

精简入口用于检验新增 DLL 的加载流程是否影响兼容性。它不是已经确认的崩溃根因修复。

## 验证与边界

| 验证 | 结果 |
| --- | --- |
| Windows 64 位正常地址解包回归 | 180 项，0 失败 |
| Windows 64 位强制重定位回归 | 181 项，0 失败 |
| 当前 Proton 平板 64 位强制重定位回归 | 181 项，0 失败 |
| ZIP／LZMA 原路径、VSZa 新路径 | 通过 |
| 原 Steam AES 解密→解包→SHA1 输出校验 | 通过，使用公开测试样本 |
| 512 KiB 块、追加、空块、损坏输入、并发 | 通过 |
| 当前平板六个 DLL 与候选 ZIP 一致 | 通过 |
| 候选完整包／覆盖包 ZIP CRC | 通过 |
| 登录后的完整游戏下载与最终安装状态 | 尚未验证 |
| 匹配日志级别、页面、操作的长期稳定性 A/B | 尚未完成 |

早期全 v2 在普通窗口操作附近出现网页进程退出，随后新 producer 无法恢复可见主窗口。恢复原版三个核心 DLL 的一轮操作未出现相同退出。仅恢复原版 64 位客户端的一轮，网页进程仍存活，但普通主窗口也曾不可见：窗口枚举显示主窗口 visible=1、iconic=0、foreground=1，宿主记录主画面 producer 无可用绑定。此后大屏恢复。

因此至少要区分“网页进程退出”与“进程存活但 GPU 窗口未合成”。去掉 64 位 CRT/TLS 不能凭本轮短期结果认定同时修复了两者。早期全 v2 开过 +seh，本候选关闭了详细 SEH 跟踪，页面内容、缓存及操作也未严格匹配。原版同样存在可恢复的 FEX CallRetStacks guard fault，不能把此类记录单独当成崩溃根因。

## 构建重复性修正

发现 GNU ld 默认首选映像地址随输出路径变化，重新构建 PE32 解码器会产生不同 SHA256，无法通过现有启动兼容层的精确哈希检查。已在 `scripts/steam_legacy_zstd/build.sh` 固定三个 helper 的首选地址，仍保留 ASLR 和重定位。修正后在新目录重建，三个 helper 均与实际测试文件逐字节一致。

这解决的是重建包的一致性；最初平板六个 v2 DLL 哈希正确，所以不能把它当作最初窗口消失的原因。未来编译器／源码升级仍须重新验证完整哈希，不能跳过启动检查。

候选构建：`VPP_STEAM_NOCRT64=1 bash scripts/steam_legacy_zstd/build.sh /absolute/helper-output`。默认关闭此实验，保留 v2 用于对照；v2 当前不宜作为稳定修复推广。

## 产物与恢复

- 完整包：`Steam-legacy-zstd-v3-candidate-20261007.zip`，332856630 bytes，SHA256 `3ce4d0b2a730acd0864a2cb786370af4d226c3591ac45935b594a7340216202b`。
- 覆盖包：`Steam-legacy-zstd-v3-candidate-overlay-20261007.zip`，23755582 bytes，SHA256 `3b8be6fb6abe1e7073c0ef2255c0923dd78710fef3a4478b86c6fd91ffa7aa27`。
- 新 64 位 helper：SHA256 `e339ce8195a6fdffdd627a1c528fbacc0058fb6d4db7f35e2a2215fecf5c135b`。

覆盖包须在完整退出目标 Steam／Wine 后，将六个 DLL 一起覆盖到 steam.exe 同目录。若已经正确安装 v2，只有 64 位 helper 的二进制有变化。保留自己的 config、userdata 和游戏数据。

恢复 v2：退出目标 Steam／Wine，将备份中的 vpsteamzstd64.dll 恢复。恢复完整原版：恢复原三个核心 DLL。平板 C: 已保留 `vp-steam-original-20261007` 与 `vp-steam-patched-20261007` 两套备份。

## 后续判断

先做同一 GPU 参数、日志级别和操作顺序的 v2／v3 冷启动对照，记录窗口与网页进程状态；若 v3 仍丢窗口，优先对齐 Windows 窗口状态和宿主 producer／parent／generation 绑定，不通过关闭 GPU 或猜测跨窗口路由掩盖问题。再用一个已拥有的游戏验证实际下载完成、内容日志及游戏文件完整性。原始账号日志和截图仅保存在本地。

本轮证据位于 `workspace_temp/steam-tablet-startup-20261007/`，关键文件为 `nocrt64-tablet-regression.tsv`、`nocrt64/windows*.tsv`、`nocrt64/tablet-verified.json`、`patched32-bind.log`、`patched32-windows.txt` 和 `nocrt64-candidate-end-state.txt`。命令均显式选择平板 target，未操作无线手机。

可随提交复核的脱敏摘要位于 `docs/architecture/evidence/steam-legacy-zstd-20261007/`，包含候选的主机／平板回归 TSV、导入表差异、平板六 DLL 哈希与包 manifests。
