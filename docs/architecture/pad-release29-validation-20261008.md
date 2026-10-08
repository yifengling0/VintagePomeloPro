# 1.4.5.29 Pad 自动测试（2026-10-08）

## 结论

覆盖升级、Wineboot、桌面开始菜单、运行对话框和记事本文字输入通过；真正 x86-64 PE32+ 基础程序正常退出。两项 32 位测试已执行功能，但退出阶段收到 SIGSEGV，列为失败，不能宣称 32/64 位全项通过。未运行 Steam、用户游戏或 FPS 对照。

代码已提交到 `feature/main_proton`：`49905e82a925d6ce4b339eec92aa07d372e00ca8`。包含 FEX 原生异常边界、ARM64 回溯保护、CPU DLL 部署、全新 prefix 首启和 ERROR 串行重试，以及对应生产方法回归。构建时基线为 `9465ba60` 加上述修改；本次没有重编或改动发布载荷。补丁、源码和报告已提交，HAP/APP、游戏、签名材料和私有现场未提交。

## 设备与安装

HUAWEI MatePad Mini，型号 `MLR-AL10`，系统 `OpenHarmony-7.0.0.105`，USB 自动操作。包名 `com.vintage.pomelopro`；由 `1.4.5.28 / 1004037` 覆盖升级至 `1.4.5.29 / 1004038`。先 force-stop，确认旧应用 UID 下没有残留进程，再安装；保留原 prefix、已安装程序和游戏目录，没有卸载清空。

使用与未签名 HAP 相同载荷、本地设备材料签名的 HAP，SHA256 `0ba68d855ceb43fe52ded686002e3712f61e2176f31cf252897843c2ab8604e5`。实际 `bm dump` 核对版本正确，`debug=false`，设备调试签名；未安装商店签名 APP，也未上传商店。

Pad 使用 system NCP，Wine 子进程从 nativespawn 启动。FEX 默认和原 x87 设置保留；这轮使用已有 WineD3D 桌面路径，没有进行图形后端性能比较。

## 测试结果

| 项目 | 结果 | 实际证据 |
| --- | --- | --- |
| 版本与升级 | PASS | HDC 安装成功，bm dump 为 1.4.5.29 / 1004038 |
| 首次升级启动 | PASS | wineboot --init 07:48:56.714 至 07:49:01.215，约 4.5 秒，正常退出并进入桌面 |
| 完整退出再启动 | PASS | UID 子进程清空；wineboot --init 07:56:08.326 至 07:56:12.327，约 4.0 秒，复用原 prefix |
| CPU DLL 与 ntdll | PASS | prefix 实际三颗文件与发布载荷 SHA256 一致 |
| Shell COM 注册 | PASS | ExplorerBrowser、ShellWindows 的原生及 WOW6432Node 四项 InprocServer32 均存在 |
| 首次桌面点击 | PASS | 未先打开文件管理器，开始菜单直接出现，运行对话框打开并收到输入 |
| 重启后的输入 | PASS | 开始菜单再次打开；运行 notepad.exe，记事本显示 VP_PAD_INPUT_OK |
| x86-64 基础执行 | PASS | C:\vp-haison2-cpu64-smoke.exe 完成 50,000 次 GetCurrentProcessId 校验，结果文件时间更新，NCP exit=0 |
| PE32 窗口驱动 | FAIL at exit | x86/winehua_win32_driver.exe 完成 BM_CLICK；RtlExitUserProcess status=0，随后 NCP SIGSEGV |
| PE32 CMD 执行 | FAIL at exit | syswow64/cmd.exe 写出 VP_PAD29_X86_CMD_OK；RtlExitUserProcess status=0，随后 NCP SIGSEGV |

三颗实际 DLL 哈希：

```text
libwow64fex.dll  5df18a0d55b1aea696d9b49413897598b63d9600d845e00c7ddca21438434bcf
libarm64ecfex.dll 618b3311f484e4831ce6c6a6b4ffaa255f604490996d414e9b5cb4252626e273
ntdll.dll        e717c66f43e7743f89b4185b075d25ce97544060b6ba61af6414d3b2762e5970
```

另外核对 syswow64 ntdll、kernel32 与解包 runtime 的对应 i386 PE 文件，哈希一致。不是仅以包内文件更新来推断 prefix 已更新。

## 待查异常

两项 PE32 测试均明确进入 FEX WOW64。窗口驱动实际 PE Machine=0x14c、OptionalHeader=0x10b；已发出 BM_CLICK。CMD 已执行 echo 并创建结果文件。两者 Wine stderr 都出现 `RtlExitUserProcess ... status=0x00000000`，随后 NCP 报告进程信号 11。因此范围已缩小到退出/线程清理阶段；具体故障函数和栈尚未确认，不能只凭早期的可恢复 SIGBUS 日志定位崩溃。

对应记录：

```text
07:54:35.605 add pid=31564 name=winehua_win32_driver.exe
07:54:36.962 NCP child pid=31564 terminated signal/reason=11 name=Segmentation fault
07:55:42.914 add pid=31969 name=cmd.exe
07:55:44.092 NCP child pid=31969 terminated signal/reason=11 name=Segmentation fault
```

后续应先用同一最小 PE32 程序复现正常退出，抓最后一次异常与 FEX/WOW64 线程销毁栈，再检查 Wine 线程退出、FEX 清理和 NCP 子进程结束的先后关系。这是调查方向，尚不是根因或已修复结论。本轮没有 28 的同场对照；29 与 28 的 Wine runtime ZIP 相同，不能认定此异常由 29 的 ArkTS 首装修复引入。

首次桌面启动还记录附加 Explorer 进程 PID 29148 的 SIGSEGV；核心桌面和开始菜单继续可操作。完整重启后对应附加 Explorer PID 32933 正常 exit=0，这项仍需独立复现，不能省略。旧 prefix 内 GoogleUpdater 服务两轮均 exit=53，注册表确认存在该第三方服务；不将它误报成 wineserver 启动失败或 FEX DLL 缺失，未删除用户服务。

## 主机回归及边界

本轮重新运行并通过：

```text
host_tests/fex_native_callret_fault_test.py
host_tests/wine_arm64_leaf_unwind_test.py
host_tests/wine_builtin_identity_test.py
scripts/run_wine_managed_cpu_tests.cjs
scripts/run_wine_engine_retry_tests.cjs
host_tests/wine_engine_progress_test.py
```

Docker 的 Git 所有者检查使用本次进程的 safe.directory 参数处理，未修改全局 Git 配置。新增源码文本已统一 LF；补丁文件中的空白上下文保留，生产补丁重放通过。

Pad 本轮保留原 prefix，所以不代替全新 Pad 首装；全新手机首装证据另见 [手机首启报告](phone-first-install-fex-20261008.md)。ERROR 故障重试仅通过生产方法主机回归，未在 Pad 注入失败。没有 Steam 下载、游戏启动、人物完整性、游戏帧率或长时间稳定性结论。

本地现场在 `F:\VintagePomelo-Workspace\workspace_temp\pad-release29-20261008`：命令审计、版本、截图、运行时身份、系统注册表、stderr、hilog 和 test-summary.json。原始日志/注册表仅本地保留；报告引用必要摘要，未公开私人内容。
