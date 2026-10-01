# 合并后启动回归修复与 FEX 验证（2026-10-01）

分支 `feature/sync-winehua-proton-3f55395`，基线 `1ac61cea`。参考 WineHua `feature/proton-wine-ohos` 的 `3f55395cc4e5ec0dd5e4c71498135b7f80820ce5`，与当前已合并 ref 一致。

## 根因与修复

1. VPP 的十参数 `RunWineExe` 接口接受 workingDirectory，却没有转发。native 的 cwd 推导还把方案③的 argv[0] loader token `wine` 当成程序。修复显式 cwd 转发、跳过 loader token、Want 路径解码，并保留 prefix 内程序的完整 `C:\...` 路径。
2. prefix 被视为 ready 时缺少 ShellWindows / ExplorerBrowser 的 COM 注册。explorer 在 `make_explorer_window` 中释放空的 IShellWindows 指针，是 ARM64 Wine shell 初始化故障；不能由此认定 FEX 崩溃。新增两种位数 Shell COM 检查；不完整 prefix 执行 `wineboot --init --update`，保留已有应用和用户数据。
3. boot event 的旧标记先于注册表落盘；ArkTS 又把 `ready-degraded` 直接当 READY。由 wineboot 在 init/update 完成且 RegFlushKey 后发布原子完成标记，boot event 仅写独立 dispatch 诊断标记。ArkTS 等待真实完成状态。
4. 每个 Wine child 都直接改写 system.reg/user.reg，原实现使用 O_TRUNC。它会与 wineserver 的注册表加载竞争：真机 system.reg 从约 3.83 MB 缩至 846 KB，InprocServer32 从 1238 降为 619，Wow6432Node Classes 消失。迁移现在只在 wineserver 取得会话锁之后、读取 registry 之前运行；普通 child 不再修改 hive。文件读写校验完整 hive，使用临时文件、完整写入、fsync、rename；失败保留原文件。

Wine 侧改动保存于 `0011-wineboot-durable-prefix-completion.patch` 和 `0012-wineserver-locked-registry-migration.patch`，由 `scripts/build_wine.sh` 幂等应用。

FEX 为默认，HODLL32=`libwow64fex.dll`，HODLL64=`libarm64ecfex.dll`。本次没有通过 box 绕过启动问题，也不依赖全局 +seh 日志延迟。原交接附件中“FEX 不处理 SMC”的推论不成立：当前 FEX 源码有 SMC 处理；诊断 fault 不等于未处理异常。

## 验证

- 全量 `make test` 通过，包括新增 prefix registry 回归；模拟 reader 持有旧 inode 时替换约 4 MB hive，旧 reader 仍可完整读取。
- 主仓库和 Wine 编译树 `git diff --check` 通过；debug HAP 构建、签名和 verify-app 通过。
- 真机强制维护约 52 秒完成。system.reg 为 3845422 bytes，InprocServer32=1238，Wow64 Classes=6906。
- 随后两次冷启动均为 3848150 bytes，1238 / 6906 保持；仅普通 wineboot --init，无重复强制更新。explorer 可以打开 Z:。
- PAL4 的菜单和三维木屋场景已实测；用户确认 PAL2、PAL4 测试通过。
- Heaven 4.0 在 FEX + DXVK 1.10.3 上创建 feature level 11_0 设备并进入 Direct3D11 三维场景；用户确认动作正常。Maleoon 910 的现有策略将请求的 modern 后端回退为 legacy，不能把请求值当实际版本。
- Qt 启动页曾完整显示；用户接手验证 Heaven 后要求只继续 Steam，不再扩展 Qt 测试。

## Steam（进行中）

- 用户指定库内 RPG Maker XP；目标是下载、安装、从 Steam 启动以及实际授权检查，保留正常 DRM，不绕过认证。
- 15:58:30 更新完成；自更新原 `cef.win64/vulkan-1.dll` 保持原位，后续后台检查显示 installed 与 manifest 版本相同、Nothing to do。
- 更新所在旧会话出现 CEF D3D11/Vulkan 创建失败和黑登录窗，同时发现渲染服务连接被拒绝；这些证据尚不足以认定 FEX 或某次图形切换为根因。
- 冷启动同一 FEX + DXVK legacy 配置后，CEF Vulkan / D3D11 初始化、700x440 交换链和中文登录页正常，进入 Steam Guard 确认。没有隐藏 Steam DLL，也没有强制软件 / force GPU 参数。
- ProcessorMetrics 子进程仍有 signal 11 记录，必须与主 browser / renderer / gpu-process 分别观察，不能将登录页出现记为全链通过。
- 16:13:22 已登录（SetLoginState: Success - OK），库后台初始化完成；库内容 renderer 仍反复退出，主内容和安装弹窗黑屏，不能记为客户端完整通过。
- 冷会话使用 `WINEHUA_CEF_NO_CRASH_HANDLER=1`；当前基线 Wine 还会由这个开关隐式加入 V8 `--no-opt`。这不是只关闭 crash handler 的纯默认渲染配置；本轮没有保留或发布新的 V8 模式实验。
- 用户要求暂缓 Wayland / CEF 显示调查，优先验证独立 Wine Steam 复用内置下载目录和正常 DRM。命令入口 `-console +app_install 235900` 已完成官方下载安装；共享目录链接及官方全量文件校验通过，无需补下载。
- `-applaunch 235900` 确实创建 RPGXP.exe，但进程异常递归、栈溢出退出，编辑器未出现，DRM **尚未通过**。具体共享方式、限制和证据见 [Steam 共享库验证](STEAM_SHARED_LIBRARY_20261001.md)。

## 产物与证据

当前已安装 `VintagePomeloPro-1.4.5-proton.7-debug-signed.hap`，SHA256：

`d9b57e0f59ff93651fdae9cb4b681993f3931140e46f600c911f122fcd073fa4`

后续 Steam 参数位置修复已构建并验证签名为 `VintagePomeloPro-1.4.5-proton.8-debug-signed.hap`，尚未覆盖真机当前会话，SHA256：

`67c0c618508d951a7f5a061f4d2c3b4f6b05cd57d2b8dfff14dd0ade7d7aa6c5`

本地证据目录：`F:/VintagePomelo-Workspace/workspace_temp/pal4-fex-merge-fix`。包括 `host-tests-final.log`、`locked-repaired-system.reg`、`locked-cold1-system.reg`、`locked-cold2-system.reg`、`Heaven_d3d11.log`、`heaven-scene.png`。Steam 日志与截图只用于本地诊断，分享前需去除账号和认证信息。

本轮修复、测试、补丁和报告随主仓库 feature 分支提交交付。Wine 子模块的改动由上述补丁复现，gitlink 保持不变；原有 dxvk-modern / vkd3d-proton 子模块修改保持原状。下一步与环境入口见 [AI 交接](AI_HANDOFF_STEAM_DRM_20261001.md)。
