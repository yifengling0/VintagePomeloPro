# Steam 与 GNU Proton 离线交付

离线包按组件内容配套，不按应用版本号或日期复制。Steam 官方 ZIP 集合不变时沿用已发布的整包；HAP 内的小型宿主、POSIX、FEX 和图形适配修复由应用管理。官方组件增加或变更时重新生成 Steam 完整包；GNU Proton 上游运行文件变化时交付对应的新包，只有宿主适配变化时沿用原运行包。

旧的 `SteamLinux-ARM64-1792110651-offline.zip` 缺少原生游戏 SDK，不能满足当前安装要求。当前完整包为 `SteamLinux-ARM64-1792110651-native-sdk-offline.zip`。`SteamLinux-native-sdk64-repair.zip` 是单独修复材料，不能作为 Steam 完整包导入。应用仍先校验整个离线 ZIP 的已知 SHA-256，再解压并验证全部内部组件；不会为了兼容任意历史包取消校验，也不会隐式转到网络下载。

## 发布步骤

`make hap`、`make hap-unsigned`、`make app` 和 `make app-unsigned` 经 `scripts/package.sh` 自动执行准备与最终 HAP/App 核验。GitHub Build 工作流缓存精确校验的官方 ZIP，上传 HAP 及配套两包、校验清单和导入说明；Release 工作流在发布前再次核对实际 HAP，缺包、不配套或损坏即失败。只上传清单列出的当前两包，避免把缓存中的历史包混入发布。

本地复用已有官方缓存和交付目录：

```sh
python3 scripts/prepare_release_offline_packages.py \
  --output-dir /mnt/f/VintagePomelo-Workspace/artifacts/steam-offline \
  --downloads /mnt/f/VintagePomelo-Workspace/workspace_temp/linux-proton \
  --archive-cache /mnt/f/VintagePomelo-Workspace/workspace_temp/steam-linux-compat/official-archives \
  --offline
```

不带 `--offline` 时，只为缺失的组件下载清单中固定的 HTTPS 资源，核对大小和 SHA-256。GNU Proton 的导出身份取自导入页面实际使用的 `linux-proton/runtime-catalog.json`，并与发布目录、HAP 内目录、`package/` 文件清单逐一核对。优先复用已发布的精确匹配包，其次复用缓存，缺失时从目录中固定的本仓库 GitHub Release URL 获取。

`linux-proton/manifest.json` 描述应用内完整适配载荷，可能包含更新后的外层宿主文件；它与独立运行包的 ZIP 校验值允许不同，不能直接把该载荷当作用户导入包。当前可导入的是 `GNU-Proton-GE11-11-ARM64-offline.zip`（116676942 字节，SHA-256 `3af1fd2e8ef02ca44e52def036e8401dd5075426abb2be6da9c72928ff9160c4`）。此前误导出的 `GE-Proton11-11-ARM64-97327f3a9e81-offline.zip` 是完整适配载荷，不应分发给用户导入。

用 DevEco／直接 Hvigor 生成上架 App 时，在编译前运行上面的准备命令；编译后执行：

```sh
make verify-release-offline-packages \
  RELEASE_OFFLINE_DIR=/path/to/offline \
  RELEASE_HAP=/path/to/entry-default-unsigned.hap \
  RELEASE_APP=/path/to/store.app
```

检查器同时读取 App 内嵌的 HAP，不解包完整应用。它核对 Steam 外层包大小、SHA 和内部官方组件集合，以及 HAP/App 内的 Steam、Proton 清单；不是仅检查文件名。`release-offline-packages.json` 记录组件集合、两包身份与本轮实际核验的应用路径。已存在的精确匹配大包原地复用。

## 用户导入

Steam 组件页导入完整 Steam ZIP；GNU Proton 组件页导入 GNU Proton ZIP，两者分别选择且无需解压。导入前检查所需空间；不匹配文件提示配套文件或版本，校验不符、缺失内部组件及解压失败分别报告。只有全部官方 ZIP 验证通过才写入下载缓存，后续按原有安装流程启用。Proton 本地 ZIP 在复用已装版本前也必须先通过大小和 SHA-256 校验，防止选错包却误报成功；新装还需逐文件校验后才切换。失败不切换现有版本，不删除游戏、账号和存档。

## 回归

- `python3 host_tests/linux_steam_offline_package_test.py`
- `python3 host_tests/release_offline_packages_test.py`
- `python3 host_tests/linux_runtime_components_test.py`
- `node scripts/run_linux_steam_download_tests.cjs <Harmony SDK TypeScript 路径>`
- `node scripts/run_linux_proton_component_tests.cjs <Harmony SDK TypeScript 路径>`

覆盖首次交付、宿主更新复用、完整适配载荷与导入包不同、官方／Proton 变更、HAP/App 配套门禁、缺档、损坏、离线无网络、空间不足、缓存写入顺序、已装版本的错误输入拒绝以及游戏库保护。真机验收与远端 CI 运行结果仍需分别记录，主机回归不代替设备通过。
