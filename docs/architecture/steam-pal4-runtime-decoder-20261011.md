# Steam 游戏兼容与下载解包分离（2026-10-11）

## 问题与边界

用户提供的原包为 `F:\Steam\Steam`。冻结客户端不升级。32 位游戏需要原版
`steamclient.dll`，64 位游戏需要原版 `steamclient64.dll`；不能以恢复游戏启动为由撤掉
新下载块格式支持，也不能根据操作系统或游戏位数推断下载进程位数。

本轮在同一 Wine/FEX、HAP 和图形设置下对照 PAL4（AppID **681820**）：

- 修改过的 v3 客户端：`steamclient.dll` PROCESS_ATTACH 后，启动器调用
  `NtTerminateProcess(..., exit_code=51)`，随后宿主退出日志才出现 signal 11。
- 两个客户端和 SteamUI 都恢复原版：出现 PAL4 片头、菜单和剧情画面。
- 只恢复 32 位客户端，64 位客户端和 SteamUI 仍为 v3：PAL4 可进入菜单。

因此错误 51 已缩小到修改过的 PE32 客户端造成的兼容回归；尚未证明具体是哪种
签名或完整性校验算法。不能把退出后的 signal 11 当成 FEX 首次异常原因。
旧自动化 AppID 681590 不适用于该游戏，相关命令已纠正。

## 候选方案

磁盘上的两个客户端保持原版。沿用 SteamUI 三个客户端加载调用点，在**原验证器通过后**，
仅在同目录 `steam.exe` 的首次客户端加载阶段，给已加载的 PE32 解包入口增加 VSZa 分支。
其他格式执行原入口完整 prologue。游戏进程加载的磁盘文件和代码没有这个修改。

`vpsteam_runtime32.c` 检查目录、进程名、精确客户端及 release 解码 DLL 的 SHA256；
客户端已加载时拒绝安装，未知文件保留原行为，不在 DllMain 加载或修改模块。
失败回滚入口，恢复页权限，刷新指令缓存并固定模块生命周期。原验证器返回值、
下载鉴权、解密、CRC、输出长度和 Steam 最终文件校验均保留。

默认关闭 `VPP_STEAM_RUNTIME_DIAG`。设为 `1` 时只对 VSZa 记录 PID/TID、位数、输入长度、
序号和原解码返回值；最多尝试写入 512 行，位置为 `logs/vpsteam-runtime-decode.log`。
原 release 解码器字节不变，关闭诊断时直接跳转解码器，没有逐块包装或日志。
安装状态单独记录在 `logs/vpsteam-runtime32.log`。

这是针对精确冻结 PE32 Steam 下载进程的候选，不能用作未知 PE64 下载进程的通用注入器。
64 位游戏的下载内容与其游戏进程加载 64 位客户端是两个需要分别验证的阶段。

## 当前验证

Windows 原解码入口测试：普通地址 184 项、强制重定位 185 项，全部通过。
覆盖 ZIP/LZMA/VSZa、加密块到解包和 SHA1、CRC/尺寸损坏、追加缓冲及并发。
启用日志且强制重定位的上限测试 785 项通过，恰好 512 行。
六项验证器/路径/篡改策略回归通过。每轮磁盘原客户端 SHA256 不变。
此前 BA 构建在平板上另有强制重定位 185 项通过；后续发布身份必须以本节新哈希为准。

平板 MLR-AL10，HAP 1.4.6 / 1004060，仍使用 `2-perf-gpu-auto.bat`，GPU 设置未降级。
已部署新标准构建（`3886be8d...`），Win PID 708 的安装记录为 `install=1`。

### 新候选真实下载

先保留 Purrgatory 原安装目录和 manifest 的相邻备份，再触发干净下载。
2026-10-11 10:15:21–10:15:36 设备时间，Steam 完成 AppID 1713610 / Depot 1713611：

- 计划下载 126,782,064 bytes，写出 192,924,256 bytes，原有三个游戏文件完整提交。
- 56 个 VSZa 调用均来自 Win PID 708，`bits=32`，全部 `result=1`。
- `Fully Installed`、build 22144708、scheduler `No Error`；与同期真实 CDN 请求对应。
- 此游戏 EXE 的 PE machine 为 `0x014c`，不能将它当成 64 位游戏样本。

64 位《山河旅探》EXE 已读取 PE machine `0x8664`。保留 UnityPlayer.dll 原文件和原安装
manifest 后尝试 Steam 验证补下载；到 `Running Update,Validating` 阶段，未取得下载完成。
这次不能给出 64 位游戏补下载通过结论，收尾恢复原文件和测试前 manifest，不保留缺文件状态。
跨进程 Module32 快照返回 error 5，未把磁盘哈希当成游戏已加载模块的证明。
最终关闭诊断、用原 2 BAT 冷启动后，新标准 helper 下 PAL4 通过正确 AppID 进入菜单，
Win PID 1584、父 Steam PID 1840。也打开了读取存档页，但当前 Steam 版显示空槽，
没有声称完成此前独立版本的第二格存档加载。未改动存档。
64 位游戏实际客户端加载和长时间 Steam 稳定性未完成。

64 位测试回收后，UnityPlayer.dll SHA256 为
`099929019221759b77ea705e32a8abb8e190fa3d14959d3eaad9a0b37996fbba`，与测试前一致；
原 manifest 从测试前只读快照恢复，中断状态另存备份。Purrgatory 新下载和原目录备份均保留。
最终从设备活动目录回读 Steam EXE/五 DLL 的哈希，`audit_install.py` 识别为
`runtime32-original-clients-candidate`，与上述标准构建及原版身份相符。

### 独立稳定性现场

主 Steam Win PID 708 保持存活期间，CEF 从 PID 752 变为 1852。约 10:18:41 的
`webhelper.txt` 出现 `SteamClient._internal.RegisterForStyleChanges is not a function`。
这条 JavaScript 错误的原因未确认，不把它直接定为整个重启根因。

另一次重启后的验证期间，10:29:21 首先收到 CEF host PID 40750 的 NCP ipc-death，
随后旧柚宿主 PID 27622 / TID 29459 收到 SIGSEGV。系统产生
`cppcrash-27622-1791685761265.json`。同期有 surface/buffer 回收错误和
`WARP sync failed: surf=... not mapped`。已保存日志，但缺崩溃调用栈，不能仅凭相邻日志
认定 WARP 或 Bufferqueue 是首次故障点，更不能宣称恢复原客户端已解决主窗口稳定性。

## 构建与部署

```sh
VPP_STEAM_RUNTIME32=1 VPP_STEAM_NOCRT64=1 \
  bash scripts/steam_legacy_zstd/build.sh /absolute/new-output
python scripts/steam_legacy_zstd/package_runtime32.py \
  /path/to/original/Steam /absolute/new-output /absolute/new-overlay
```

Python 需要 `pefile`。输出目录必须不存在，打包过程不修改原包。
退出 Steam/Wine 后备份并成组覆盖五个 DLL。保留原 BAT、Steam.cfg、config、userdata、游戏及存档。
回滚同样在退出后恢复整组备份。旧 `vpsteamzstd64.dll` 可以留在目录，但原 64 位客户端不导入它。
`audit_install.py` 检查的是实际完整安装目录（含 steam.exe），不是缺少 EXE 的覆盖包目录。

| 文件 | SHA256 |
| --- | --- |
| steamclient.dll | 95a7e01a64e20f0726b63cfb63b5240e51c7b15940b52d03e8da4c412ca54669 |
| steamclient64.dll | a2ad498763c4822fd4555c2cba4d4475a2ba450e403a774c1366826ad9451dd1 |
| SteamUI.dll | 88004c47d74bf6cd25c2abd191db0eff07d7f5a29cab49b937d6581b73a07600 |
| vpsteamtrust32.dll | 3886be8d6ee354b192552e894c4bd3aa9faae6ff9a9754b03af6a2671ea336a1 |
| vpsteamzstd32.dll | 57c1b0503d131853aa899a896d3a0077b9f62a44ec5545dd6ce58e0b798e0faf |

本地证据：`workspace_temp/pal4-steam-regression-20261010/`。不提交原版/修改版 Steam 二进制、
账号日志、截图或 HAP。本轮已有音频修改保留，PE32/PE64 FLOAT32/S16 共享音频接口测试均通过；
尚无梦幻西游爆音听感回归结论。没有将音频、FEX 或图形路径当作此次 Steam 下载修复的一部分。

## 完整分发包（本轮收尾）

用户明确要求先交付下载修复包，暂不继续处理窗口稳定性。
`package_full_runtime32.py` 从原始 `F:\Steam\Steam` 按资源白名单构建完整目录，
没有整体复制 config/userdata/appcache/logs/dumps/steamapps/steam/games/music，
也未包含 ssfn、浏览器状态、CDN 缓存或本轮调试工具。保留历史版本及原 GPU 2 BAT。

- ZIP：`artifacts/Steam-legacy-download-fix-20261011/Steam-legacy-download-fix-20261011.zip`
- ZIP 大小 332,214,471 bytes；解压文件 6976 个，共 704,949,995 bytes。
- SHA256：`41cafbfba824b0819603d388ddb94dd11169759bf43b260d7069560983e99a9f`。
- ZIP 每个成员均按解压后的字节进行 SHA256 比对，检查无重复成员，原目录哈希前后未变。
- 文本隐私扫描命中的是原版控制器模板 creator ID、客户端静态脚本常量及德语 UI 标签；
  与源目录登录状态中的实际账号 ID/账号名逐项比对，包内无匹配。
- 平板将同一完整目录放到新的 `C:\vp-steam-clean-20261011` 后，用其 2 BAT 启动，
  11:30:39 的 runtime32 日志为 `pid=1640 install=1`，无旧账号状态，登录页完整显示。
  这项是干净目录启动验证，未冒充在新目录重新登录并下载了一遍。

本包不是通用新版 Steam 补丁。真实完整下载、PAL4 和 64 位回归的边界仍按上文记录。
未因本次打包而声明宿主原生崩溃已解决，也未修改 GPU 启动策略。

## 下载后 Purrgatory 的 OpenGL 报错

同一新下载文件用默认参数复现 `Unable to initialize Video driver`。追加
`--video-driver GLES2 --verbose` 后进入就绪画面，经 Enter 到达完整中文主菜单。
游戏自身 `godot.log` 明确记录：

```text
Godot Engine v3.4.4.stable.mono.official.419e713a2
Using GLES2 video driver
OpenGL ES 2.0 Renderer: virgl (Maleoon 910)
```

因此该游戏可以使用自带的 GLES2 渲染器，继续走 GPU；不需要伪报 OpenGL 版本。
`--verbose` 仅用于本轮观察，日常只需 `--video-driver GLES2`。
有 MSAA 上限回落到 4 的警告，但未阻止进入菜单；未完成全剧情/所有特效回归。

重跑 PE32 WGL 探针实际返回 GL 3.1 / GLSL 1.40，3.0/3.1 上下文、着色器及像素通过，
3.2–4.6 请求拒绝。当前扩展列表仍缺 `GL_ARB_depth_clamp` 和
`GL_ARB_blend_func_extended`。Godot 3.4.4 的 Windows GLES3 上下文代码请求 GL 3.3 core，
其 GLES2 fallback 受项目设置控制。不能用此次游戏参数成功声称驱动已支持 3.3。

源码在既有每游戏图形启动预设中新增 `godot3-gles2`，复用现有保存/加载入口，默认不启用。
只生成该游戏 argv，不改 FEX、Direct/Venus 或全局图形能力。
处理已有 `--video-driver` 参数冲突，保留 `--` 后的项目参数；参数级回归已通过。
Steam 游戏应在 Steam 内该游戏的启动选项填写参数，不能给 Steam 主进程选 Godot 预设。
`assembleHap` 中 ArkTS 编译通过（41.820 秒）；随后 `SignHap` 因现有签名材料配置
报 `Unsupported state or unable to authenticate data`。没有把整次构建标为成功，
也没有安装新的 HAP。此限制不影响已交付 Steam ZIP；设备当前可用既有“额外参数”入口。

Godot 4 的 Forward+/Mobile 可以使用 Vulkan。当前 Vulkan Direct/Venus 是可验证方向，
但 DXVK 成功不代表 Godot 4 已完成回归。可按游戏尝试
`--rendering-method mobile --rendering-driver vulkan`；导出模板、所需 features 和特效
决定能否工作。桌面 Compatibility 仍需 OpenGL 3.3，Godot 3 的 GLES2 参数不能复用。

参考：

- https://github.com/godotengine/godot/blob/3.4.4-stable/platform/windows/context_gl_windows.cpp
- https://github.com/godotengine/godot/blob/3.4.4-stable/platform/windows/os_windows.cpp
- https://docs.godotengine.org/en/4.4/tutorials/rendering/renderers.html
- https://docs.godotengine.org/en/4.4/about/system_requirements.html
