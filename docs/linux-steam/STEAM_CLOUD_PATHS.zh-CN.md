# Linux Steam 云存档目录适配

本修复参考 HSL 的 `docs/proton-ge.zh-CN.md`“Steam Cloud”说明，将其兼容工具识别和用户目录一致性要求适配到旧柚现有的 GNU／OHOS 双后端。2026-10-27 已在 USB 平板验证 GNU《新仙剑》（AppID 695630）的已有存档下载、启动监视和退出同步。脱敏结果见 [验证记录](evidence/steam-cloud-tablet-20261027.json)。

## 原因与处理

旧柚现有 `hmosdeck_proton`、`hmosdeck_gnu_proton`、`hmosdeck_gnu_proton_gl` 内部名称已经包含 `proton`，满足官方 ARM64 Steam 的 Windows AutoCloud 根目录初始化条件，故不再重命名。已有旧名称 `hmosdeck` 的配置迁移继续沿用原实现，游戏选项和其他兼容工具选择由 Steam 保存。

原先 Cloud 的 `compatdata/<appid>/pfx` 映射固定指向 OHOS `.wine`，而 GNU 游戏使用 `steamohos-native/proton-probe/prefix`。现在读取 Steam 的 `InstallConfigStore/Software/Valve/Steam/CompatToolMapping`：单个游戏选择优先于默认选择，已知 GNU 工具指向 GNU prefix，OHOS 工具指向 OHOS prefix，其他工具和原生游戏的路径不被接管。配置改动后按文件状态刷新缓存。

Steam 安装在鸿蒙公共文档目录时，文件所有者是文档服务，而不是应用 UID。读取配置因此不能用应用 UID 判定；现仅从明确配置的 Steam 根目录读取有大小上限的普通文件，禁止跟随最终文件的软链接，并只识别已知工具。私有路径记录仍校验应用 UID。

| Steam 的旧目录名 | Wine 实际目录 |
| --- | --- |
| `My Documents` | `Documents` |
| `Application Data` | `AppData/Roaming` |
| `Local Settings/Application Data` | `AppData/Local` |

鸿蒙禁止创建普通软链接，故继续使用现有的私有虚拟路径记录，在 Steam 侧解析这些别名。新的元数据目录为 `hmosdeck-cloud-v2-compatdata`；旧元数据保留，不能递归映射整个活跃 prefix。删除虚拟元数据只删除记录，不删除真实存档。

Steam 启动前、GNU prefix 准备时都会检查三类用户目录。先完整检查冲突，再通过不覆盖目标的原子 rename 迁移无冲突条目，不复制存档；同名冲突、自定义根链接和锁争用会明确失败并保留现有数据。

## 交付与构建

小型 `libsteamohos_posix_hap.so` 随 HAP 的 rawfile 交付，包含源码 SHA-256、ARM64 架构、大小和二进制 SHA-256 清单。启动时校验并原子安装到已激活的 GNU runtime，Steam 及其子进程加载此库。GNU 游戏继续使用原有不可变运行库及注册表指定的现代用户目录，避免替换其已经校验的依赖缓存。

GNU／GE ZIP、Steam 二进制、离线包身份、图形和音频运行库保持原校验值。本次修复只需更新 HAP，已安装的同版基础组件和历史离线导入包不需要重打；较早基础组件仍按原有版本检查升级。

`scripts/build_linux_steam_posix.py` 可使用 GNU ARM64 GCC 重建适配库，也可用 `--verify-only` 核验提交的源码／载荷一致性。依赖锁对两份被覆盖的 adapter 源文件作显式声明，并核验配套清单，不放宽其他 GNU 源码的版本检查。CI 已加入目录迁移、VDF 解析、双后端路径 IO、源码／载荷一致性和安装器回归。

## 平板验证范围

- 覆盖安装 Release 代码、调试签名候选包，原有账号、游戏库和 OHOS 存档保留。
- 从原 1.5.0 的组件页就地升级到 1.5.2 基础组件，复用已有官方客户端资源。
- Steam 官方云下载成功，文件实际落入 GNU `Documents/My Games/NewPAL/0.sav`，SHA-256 与原 OHOS 存档相同。
- 通过 Linux Steam 启动 GNU GE-Proton 的《新仙剑》，实际显示主菜单，AutoCloud 监视 1 个存档。
- 从宿主“关闭游戏”发送普通窗口关闭请求，游戏返回 0，未使用强制结束；Steam 执行退出 AutoCloud，识别 1 个文件并跳过未修改存档，完成同步。
- 测试前后 OHOS 原存档与 GNU 下载副本的 SHA-256 均为 `2b4a2f99ffa5063925c1a8dd49e362c12dc3fc52dbd39aaafdaa6ccdc9b8d608`。

本轮不保存新游戏进度，没有验证新写入存档的跨设备上传／下载、远端冲突选择或所有游戏的云存档规则。现有结果证明的是已存在存档的真实下载、正确目录解析、监视及未修改退出同步。
