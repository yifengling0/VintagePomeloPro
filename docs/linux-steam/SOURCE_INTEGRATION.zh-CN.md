# Linux Steam 源码整合与提交范围

2026-10-21，`main_proton_steamarm64`。本分支包含会员、同步和现有 OHOS Wine/FEX/图形后端，以及 Linux ARM64 Steam、Deck 界面与本轮稳定性修复。实际运行的原生 ARM64 链路已整合进主工程，不需要从 `steam-ohos-lab` 动态引用实验源码。

## 源码与构建入口

| 部分 | 主工程位置 | 构建与交付 |
| --- | --- | --- |
| OHOS 宿主、原生 GNU 执行、进程管理、Xwayland 接入 | `entry/src/main/cpp/linux_steam`、`proc`、`wine` | 现有 CMake/NDK；`entry/build-profile.json5` 显式开启 host 与 GNU exec，避免 Release 漏编 |
| GNU 适配、桌面会话、音频、手柄和游戏交接 | `entry/src/main/cpp/linux_steam/gnu_fixture` | `scripts/build_linux_steam_gnu_fixture.py` 与 `package_linux_steam_gnu_fixture.py` |
| SteamAPI PE/GNU 桥 | `entry/src/main/cpp/linux_steam/steam_bridge` | `build_linux_steam_bridge.py`；显式提供 Steamworks SDK 和交叉编译工具链，输出配对清单 |
| Wine 窗口宿主 | `tools/steam-boundary/steam_window_host.c` | `build_linux_steam_window_host.py`；自有 `hmosdeck-steam.exe` 与 `window-host.json` 一起纳入 rawfile 和源码管理 |
| GNU glibc、Wayland、Xwayland、Mesa 及工具适配 | `patches/glibc`、`patches/xwayland`、`patches/mesa` 与 `scripts/build_linux_steam_*.py` | 上游源码、固定包清单与仓库补丁构建；GNU 与 OHOS 动态库保持 ABI 隔离 |
| Wine 会话桥与首初始化 | `patches/wine/0065-kernelbase-linux-steam-session-registry.patch`、`0067-wineboot-single-managed-initialization.patch` | `scripts/build_wine.sh` 登记并验证补丁；现有源码缓存中的修改由补丁保存 |
| 已审核 Wine 载荷增量同步 | `stage_linux_steam_wine_bridge.py`、`stage_wineboot_runtime.py` | 校验基包与 PE 架构，同步 ZIP、manifest、版本戳；不能只复制 DLL 或改版本号 |
| Deck、组件安装、会话与窗口管理 | `entry/src/main/ets/components/hmosdeck`、`service/LinuxSteam*`、`ManagedLinuxSteamBridgeService.ets` | 标准 ArkTS/HAP 编译；购买、会员等公共功能随分支保留 |
| 在线与离线组件交付 | `build_linux_steam_distribution.py`、`build_linux_steam_offline_package.py`；rawfile 中两个 JSON 计划 | GNU 包随 HAP，Valve 客户端从官方源或配套离线包导入；计划、文件大小和哈希配对 |

`host_v1/provenance.json` 保留早期宿主快照的来源记录。原实验仓中的 A64 指令解释器、假 libc 导出与旧原型不是当前原生执行链路的构建依赖，不将它们导入产品工程。

## 子工程与生成物

本次核对已初始化的 `dxvk`、`dxvk-modern`、`libdrm`、`mesa`，工作树均干净，主工程 gitlink 与实际 HEAD 一致。Mesa 保持 `557fa3e781ea6e78fcff3ee759c25f60c3055064`。未初始化的其他子模块不为本次提交临时切换或更新。

Wine 的既有外部增量源码缓存不作为新的子工程依赖；本轮 wineboot 修改完整保存在 0067 补丁中，构建脚本包含与 0011 的叠加校验。GNU Mesa 的适配通过专用补丁应用到隔离构建，不改变 Wine Mesa 子模块。

`wine-data.zip`、Wine manifest/version、内置 GNU ZIP、桥生成目录和安装包沿用现有生成物忽略策略；由构建、staging 与分发脚本重建。自有窗口宿主仅约 86 KiB，作为标准 HAP 必需的固定资源保存，附源码哈希及可重建脚本。Steam 官方程序、账号、游戏库、存档、签名材料和设备日志不入库。

## 打包检查与验收边界

重建时先按需构建上游运行库和自有桥，准备匹配的 Wine/GNU 载荷与分发计划，再进行标准 Release HAP 编译。发布包应执行：

```sh
python3 scripts/audit_builtin_payload.py
python3 scripts/audit_linux_steam_payload.py --hap /path/to/candidate.hap
```

第一项默认检查主工程 Wine 载荷和内置目录，可通过 `--native-compile-commands` 补充原生优化审计；第二项直接检查实际 HAP 中的原生入口、窗口宿主、Wine 四个桥模块、GNU 对端及在线/离线计划配对，防止仅开发缓存可用。Wine 首初始化修复另有生产状态机、生命周期、精确完成判定与 ArkTS 回归。

最新实机包为 v148；首初始化两轮、实际中断重试和正常桌面显示见 [Wine 首初始化报告](WINE_STARTUP_RELIABILITY.zh-CN.md)。Linux Steam 模式切换、仙剑偶发停滞与手机适配的未决项见 [Release 稳定性报告](RELEASE_STABILITY.zh-CN.md)。源码整合与包审计不替代这些兼容性问题的实机验收。
