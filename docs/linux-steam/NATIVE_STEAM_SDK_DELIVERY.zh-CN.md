# Linux 原生游戏 Steam SDK 交付遗漏（2026-11-01）

## 已确认的问题

1.5.4 正式 HAP 对应的 Steam 在线安装清单和 11 个官方离线归档中，只有供 ARM64 Steam 使用的 `steamrtarm64/steamclient.so`，没有 x86 原生游戏使用的 `HOME/.steam/sdk64/steamclient.so`。GNU Proton 包也不能补足此项；尖塔二通过 FEX / sniper 运行 Linux 原生版本，不经过 Proton。

此前普通平板能玩的组件目录（本次检查时已改名为 `SteamLinuxARM64-6`）额外存在以下两份文件，其 SHA-256 与缓存的官方 `bins_sdk_linuxarm64.zip.d95d3d94b73855a16840659dffefc7ed6f26c25b` 完全一致：

| 文件 | 字节数 | SHA-256 |
| --- | ---: | --- |
| sdk64/steamclient.so | 51017392 | 6998dceeecf27a32cac3591516c9e20b5c8e9491de7adb7f897f0ef78fb8011c |
| sdk64/crashhandler.so | 1002544 | 8e39a286364eb6ea9eeb2cb8d419e1778ddb0818d50f197c0e819f570c5d47aa |

旧有“标准 HAP 自动交付运行时”的验收没有排除设备预留的这组 SDK 文件。之前离线包审计只验证了清单中已有的文件，不能证明运行依赖齐全。不得继续据此宣称从零安装的尖塔二 Steamworks 路径已经通过。

用户随后确认：手动补齐官方 x86-64 SDK 后，原先报 Steam 初始化失败的游戏可正常运行。这使此次依赖遗漏与用户故障得到实际对照；其他游戏的依赖加载或 IPC 故障仍应分别定位，不能伪造 Steam 初始化成功。

## 通用修正

- `native-sdk.lock.json` 固定官方 ZIP 和其中五个原始 x86/i386 ELF 的大小、架构、SHA-256；归档同时检查 SHA-1。2026-11-01 对官方 URL 的 HEAD 返回 200、Content-Length 37294533。
- 在线组件增加一个 37.3 MB 官方归档，解压后约 106.8 MB。Valve 文件仍由 CDN 或离线包提供，不放进 HAP 的 GNU 运行包，不重编或修改 Valve 库。
- 安装器新增精确 `sdk/sdk32`、`sdk/sdk64` 文件目标，实际落在 `SteamLinuxARM64/client/.steam/sdk32` 和 `sdk64`。不要放到 `client/.steam/steam/sdk64`，也不要让 x86 游戏加载 ARM64 客户端库。
- 初次安装、已有文件按校验值复用、离线修复和清空组件均使用同一目标解析。只处理清单内组件；游戏库、账号、存档、未知文件保留。
- 正常构建分发清单的 CLI 默认必须补 SDK，缺少输入不能发布成功。`linux_steam_native_sdk.py` 也能校验并原地升级已有清单，避免复制或重建大运行时。
- 运行时清单校验必须包含全部五个 32／64 位接口文件。组件页和启动前只检查安全路径、普通文件及大小，发现旧安装缺失 SDK 时明确提示在线修复／导入；不会在打开组件页或启动时隐式下载。点击在线安装或修复会逐文件校验并复用完整的官方组件，仅缺 SDK 时只下载这一个 37.3 MB 归档。
- `build-components.lock.json` 同步固定新 deployment 的 SHA。两个分发脚本和 SDK 交付脚本／锁放入精确白名单 `deliveryInputs`，与未变化的 GNU 原生编译输入分别校验；原生源码漂移仍然拒绝构建，不把此交付修复误报为重新编译 GNU ZIP。CI 和正常 HAP 预检同时校验 SDK 映射、归档身份及 releaseManifest 身份，最终 HAP 再校验实际内嵌清单。
- SDK 改变了官方归档集合，所以完整离线包需要新增这份归档。旧 1.5.4 HAP 和它的旧包仍成对保留；新的完整包只能交给包含新清单的后续 HAP 使用。没有放宽旧应用的整包校验，也没有把残缺旧包标成完整的新包。

## 当前用户的手动确认

无需更新 HAP，可先正常退出游戏、Steam 和旧柚，再将 `SteamLinux-native-sdk64-repair.zip` 解压到当前 **SteamLinuxARM64 根目录**。它只含 `client/.steam/sdk64/steamclient.so` 和 `crashhandler.so`。此包不走应用“组件导入”入口。

保留 Linux Runtime 3.0（sniper），从 Steam 内启动尖塔二。若库仍加载失败，需失败后的完整应用日志，继续分辨缺失依赖、架构或 IPC；补库本身不等于认证已经通过。C 盘组件可从 Wine 文件管理器进入 `C:\SteamLinuxARM64`，不要选择原 Windows Steam。

## 验证边界

手动 ZIP 经逐文件 SHA-256、ELF x86-64 和 ZIP CRC 校验，匹配此前可玩平板的 SDK；用户已确认手动补库有效。主机测试覆盖实际安装器在 Download／C 盘的在线首次安装、缺 SDK 的旧安装提示和增量修复、离线修复、五个 SDK 目标位置、组件重置、相同大小损坏修复、SDK 路径穿越拒绝，及用户数据保留；SDK 清单生成测试覆盖重复运行、损坏输入、错误 ARM64 ELF、缺文件和混合归档／身份拒绝。

29 项主机测试、实际 ArkTS 安装器回归、GNU 依赖预检、Release HAP 增量编译和最终 HAP 组件清单／离线元数据配对校验通过。本轮没有操作平板前台或替换设备运行库，自动从零安装后的实机游戏回归尚未进行；已发布的 1.5.4 正式包未覆盖。GNU/Wine/FEX/图形适配二进制未作本轮功能修改。

## 在线交付补验

2026-11-01 用真实 HTTP GET 检查全部 12 个官方归档，均返回匹配总长度的 206，前 64 KiB（小包为全部内容）与已校验的官方归档一致。SDK 的三条官方 CDN 都通过同样检查，不能只以 HEAD 成功代替下载验收。

随后在主机加载实际 `LinuxSteamComponentDownload.ets`，只将 NetworkKit 传输适配为 curl、fileIo 适配为真实临时文件：从零下载，在 277920 字节主动暂停，再按实际偏移继续，4 MiB 分段下载完整 37294533 字节；总计约 18.2 秒，最终 SHA-256 为 `c23f87048d79f2c05821e6ce203ff89816b350bf0211d864bd15b32bc20b321b`。这次完整下载与续传没有读取离线包或预填 SDK 缓存。临时重复 ZIP 校验后删除，仅保留小型记录。

在线检查同时修正：首条 Valve CDN 返回 403／404／410 时，继续尝试其余两条官方 CDN，保留续传数据，三个来源全部失败才向用户报告。磁盘失败仍立即停止；GitHub 项目组件的 404 不会被改写到 Valve。确定性回归覆盖上述状态和第三线路成功。主机真实网络验收不代表已完成真机 NetworkKit 在线升级测试，也不是所有用户网络的速度承诺。
