# 手动启动旧 GDI 游戏的文字碎点修复（2026-10-05）

本文保留该候选的调试与安装记录。最终发布范围和仍待确认的游戏验收见 [dot 交接说明](dot-performance-handoff-20261005.md)。

用户报告“端午补丁版”游戏的文字形成碎点，与 main 当年梦幻群侠传/GGE 修复的现象一致。当前只读截图停在应用设置页；采样时游戏已退出，没有把旧日志尾段当成本次游戏字体调用证明。

## 历史修复与当前差异

main 提交 `24bbc2b30f8ae7bf858da8e9b46c44eac49bb06f`（2026-09-12）记录了根因：GBK 经 cp936 正确转成 Unicode，但 GGE/XYQ 把 GDI 灰度字形读成 16bpp 后以 `>0x7ffe` 二值化，大量灰色笔画被丢掉，形成碎点。历史修复为游戏进程设置 `WINEHUA_FONT_AA=bitmap`，让 win32u 使用 `GGO_BITMAP`，桌面保留默认灰度；不是修改代码页或强制重映射日韩字符。历史提交 `23284f9b` 整理了曾被误判为编码问题的补丁。

该 Wine reader 已在本分支的 `56bded0e`（2026-10-02）移植为 `0014-win32u-font-aa-override.patch`。刚安装的 87fc8021 包的 win32u SHA256 为 `da3ef6c53566b01127d82582564324cdbcafcac781efbac564073995efe7fc7f`，包含 WINEHUA_FONT_AA 及对应 trace 标记，构建身份也登记 0014。因此当前没有遗漏 Wine reader，不能仅重复复制同一补丁解决问题。

当前 WineEngineService.launchExecutable 对非 explorer 程序注入 bitmap；从 Wine 文件管理器/CreateProcess 启动的程序经 NCP broker 创建，绕过该 ArkTS 入口，若继承环境未含该值，已有 reader 返回 0，继续使用默认灰度。源代码生产阶段回放确认了这处启动覆盖缺口；由于现场游戏已经退出，本次游戏实际 getenv 值尚未读取。

## 本轮移植补齐

在 wine_child.cpp 的真实子进程入口中，先应用 entryParams 的环境覆写，再补默认字体参数，随后才加载 Wine。沿用 main 的非抗锯齿游戏文字策略：已有 WINEHUA_FONT_AA 保持调用方设置；未设置时，普通 EXE/BAT 游戏进程默认 bitmap。识别带/不带 wine loader、Windows/Unix 路径、引号和大小写。explorer、wineboot、wineserver 及 services/winedevice/rpcss/plugplay/svchost/conhost/winemenubuilder 辅助进程保留缺省灰度。没有按“端午”或中文 EXE 名硬编码，也没有改游戏文件、分辨率、语言设置或 Wine 的既有字体/代码页逻辑。

作用范围是非桌面/引导进程的传统 GDI 字体；其默认点阵观感与 main 的游戏启动策略一致。调用方需要灰度时可显式传 WINEHUA_FONT_AA=gray。当前补齐作用于 native 子进程已接收的环境覆写，不把 Windows BAT 修改的任意环境变量全量导出到宿主。

## 验证与包身份

wine_process_font_aa_test.py 抽取旧提交和当前生产启动环境阶段、实际 env 覆写函数、路径处理及已移植 Valve ohos_font_aa_override。修改前手动游戏缺 bitmap，reader 返回 0；修改后手动 EXE/BAT 的 reader 返回 GGO_BITMAP=1。显式 bitmap/gray/空值/未知值优先级、桌面和引导辅助进程均通过。平台边界为 entryParams 输入和日志，未模拟字形截图，也未宣称全项目测试通过。

本轮未修改 Wine 源或 overlay，使用同一已核验 Wine 构建且 cached identity guard 通过；正常 Hvigor/CMake 和 HAP 构建、runtime closure、官方 verify-app 通过。包内新 libwine_child.so 与本轮 strip 输出完全一致，全部 ELF 装载节匹配实际 CMake 输出，ARM64 Main 反汇编包含新增字体默认函数调用。Wine payload、win32u、Wayland、opengl32、FEX、CRT、zlib、宿主 libentry 和 ArkTS 与前一包完全相同，保留 War3/王国启动、全屏黑边、日语/繁体及多窗口修复。签名前后所有原 ZIP entry 的 SHA256 相同。

- signed HAP：358356835 字节，SHA256 `1f608116a45c1313597534c2b182374d5b743801935ba4bccf1dd76cd402bbf8`。
- unsigned HAP：356556698 字节，SHA256 `d0e8307eb92d8d842629eb9025fec9fa70a3725068b1c15d013e723b26ff6141`。
- 版本 1.4.5-proton.26-alpha / 1004035，包名 com.vintage.pomelopro；按哈希识别同版本测试包。
- 交付目录 `F:\VintagePomelo-Workspace\workspace_temp\font-aa-manual-20261005`。

## 设备结果

按用户此前授权 force-stop 目标包并核对 native 子进程全部退出，随后覆盖安装。系统返回 install bundle successfully，bm dump 核对版本通过；安装时间 2026-10-05T14:34:39.456669+08:00。安装后目标未启动，未向应用、Steam 或游戏发送启动命令，未操作另一个 WineHua，也未卸载或清空用户前缀。

已请用户按原入口手动启动“端午补丁版”，检查对话、聊天、菜单和提示文字。本轮真实游戏字体恢复仍待反馈；不能以构建、字体模式回放或 main 的旧真机结果替代验收。新会话可用低频 hilog 标记 `[WineChild] GDI font AA=bitmap (game process default)` 核对路径；不默认开启 +font 全量日志。本轮未提交或 push。

安装后的只读核查未发现目标应用运行，也尚未出现新字体默认标记。缓冲中的旧会话日志显示 14:13–14:17 的启动桥接已选择 zh_TW，说明上一轮繁体值已传过 native 白名单，但不能证明 GetACP 的真机结果；原历史游戏按 GBK/cp936 诊断，复测这个简体补丁版宜选择简体内核、重启会话后再启动。此次未替用户更改语言偏好。
