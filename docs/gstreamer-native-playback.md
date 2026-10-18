# OHOS 原生 GStreamer 视频播放修复

2026-10-18；账户分支 `feature/account-cloud-membership-mvp`，基线 `b84e7db1`。

## 问题与修复

1.4.9 使用 ARM64 Proton Wine + FEX，原生 GStreamer 仍从应用数据目录加载。
USB 平板上使用相同 Wine 子进程环境和相同插件字节做对照：数据目录 0/76 插件、0/6 关键元素，
HAP 原生库目录 76/76 插件、6/6 关键元素。底层错误仅返回 `No error information`；
目录放置的回归已经复现，尚不能单凭该错误区分 linker namespace 与可执行映射限制。

- `scripts/gstreamer_plugins.json` 是组包、Wine 加载、包校验共用的唯一插件清单；保留 GStreamer 1.24.4 和 76 个模块。
- `scripts/gstreamer_inventory.py` 验证 ELF 架构及完整清单，生成逐文件 SHA256、内容指纹和 Wine C 头。
- 原生插件进入 HAP 的 `libs/arm64-v8a` 或 `libs/x86_64`；父、子进程共用 `BundleNativeLibsDir()`，通过内部环境变量 `WINEHUA_GST_PLUGIN_DIR` 传给 Wine。
- Box64 继续使用 `bin/x86_64-unix/gstreamer-1.0` 中的客体架构插件及原有扫描路径。
- Wine 的管理补丁 `0066-ohos-native-gstreamer-plugins.patch` 在初始化前关闭原生库目录扫描，初始化后只对清单中的文件调用 `gst_plugin_load_file()`。失败输出文件名、路径和 GError，任一插件或关键元素失败会使媒体初始化失败。
- 注册缓存使用 `registry.<arch>.<inventorySha256>.bin`；指纹同时覆盖清单与插件内容，升级插件后使用新缓存键。同一键中的黑名单也不会阻止显式加载。
- `runtime-components.json` 升至 schema 2；原生插件的 `storage` 为 `native-libs`，`pluginDirectory` 是实际 HAP ZIP 路径。仅校验 wine-data.zip 不代表原生插件通过；最终包必须运行 `check_runtime_components.py --hap`。
- HAP 校验检查插件真实位置、逐文件校验和、架构、内容指纹、Wine 加载器内嵌的同一指纹，以及同一原生库目录中的传递依赖。仅放在 rawfile 中的依赖不能满足原生加载条件。

验收中还复现了独立的 MF/WMV 失败：Proton 的 WMA 转换器优先级高于 libav，
会尝试使用 Steam Fossilize 媒体缓存和 `opusdec`。普通本地游戏没有该缓存。
OHOS 原生路径在没有配置 Steam 媒体转换缓存时跳过注册 Proton 转换元素，使用真实解码器；
显式配置 `STEAM_COMPAT_TRANSCODED_MEDIA_PATH` 或 MEDIACONV 音视频 dump/transcoded 文件时保留转换器。
Box64 和其他平台行为保持原状。

## 构建与验证

使用独立的 Wine 源码和构建目录，执行 `scripts/build_wine.sh --unix-modules winegstreamer`；
构建脚本生成清单头并将清单、生成器、加载辅助头纳入构建身份检查。
候选 HAP 以已发布的 1.4.9 包为基线，仅替换 `winegstreamer.so`、`libentry.so`、`libwine_child.so`，
移动 76 个插件并更新 runtime 清单与 payload 哈希。其余 158 个 HAP 成员和 2282 个 payload 成员逐字节一致。
客户端两个库从发布快照加本次环境修改构建，未混入工作目录内其他功能的未提交改动。
Wine 构建容器以完整源码树 SHA256 记录实际输入；容器挂载内不能独立核验 Git 元数据，日志明确记录该限制。

宿主验证：

```sh
make test-gstreamer-native
python3 host_tests/wine_build_identity_test.py
python3 host_tests/wine_patch_detection_test.py
bash -n scripts/assemble.sh scripts/build_wine.sh
python3 scripts/check_runtime_components.py --hap <final-signed.hap>
```

原生和模拟 ARM64/Box64 的环境表测试分别为 178、180 项，均无失败。
GStreamer 包契约测试使用实际 ELF 共享库样本，覆盖缺失模块、错误架构、错误校验和、缺失加载器指纹、
依赖仅存在 rawfile、Box64 路径及内容变更导致指纹变化。

`scripts/build_media_smoke.py --llvm-mingw <llvm-mingw-root> --out <payload-dir>`
生成 x86_64 Windows 播放探针和 4 秒合成音视频样本。默认在 ARM64 Wine 上经 FEX 执行：

| 路径 | 样本 | 完整播放次数 | 最终安装包结果 |
| --- | --- | ---: | --- |
| DirectShow | MP4 / H.264 / AAC | 2 | PASS |
| DirectShow | ASF / WMV2 / WMA2 | 2 | PASS |
| Media Foundation (MFPlay) | MP4 / H.264 / AAC | 2 | PASS |
| Media Foundation (MFPlay) | ASF / WMV2 / WMA2 | 2 | PASS |

DirectShow 要求 RenderFile S_OK、视频尺寸有效、音频接口有效和 EC_COMPLETE；
MF 要求音视频流均已选中、无错误回调和 PLAYBACK_ENDED。每组重复两次，释放播放对象并正常退出，原子写入结果 JSON。
设备截图确认有视频画面；未进行人工扬声器听测，不能将音频流存在等同于实际外放听感验收。

空注册缓存播放通过。另注入按 GStreamer 1.24.4 二进制格式生成的黑名单样本，覆盖原生目录和旧数据目录的全部插件：
设备 GST_REGISTRY 日志确认成功读取 BLACKLIST 条目，随后显式加载 76/76、创建 6/6 元素并完成播放。
仅有旧指纹缓存的目录也通过。该样本是人为构造的可解析缓存，不是设备旧扫描器写出的缓存；
旧扫描失败的对照测试未实际生成持久化缓存文件。包含预期失败对照的 suite 总状态为 FAIL，需逐项解读。

测试目标为用户指定的 USB 平板；通过 `hdc install -r` 覆盖安装并保留应用数据。
最终候选使用本地 debug profile 签名；`verify-app` 和最终签名 HAP 内容校验均通过，尚非正式签名发布包。

## 尚待验收

- 完整首次安装未测试。尝试隔离干净 Wine prefix 时，发布版 smoke 传 `resetWinePrefix('clean')`，而当前原生接口将参数解释为 container ID，导致未进入播放测试；没有卸载应用或清除用户数据。空缓存验证不能替代首次安装验证。
- 尚未获得反馈游戏名称，需用实际游戏验收过场画面、声音、结束返回和重复播放。
- 未测试 x86_64 设备或实际 Box64 播放；本次覆盖其组包及环境分支的宿主检查。
- MPEG4 兼容选项的迁移差异另行处理，不属于本次通用加载修复。

本次构建、最终安装包、日志、逐项结果和独立补丁位于工作区
`artifacts/video-plugin-native-20261018/`，入口为 `REPORT.md`。
