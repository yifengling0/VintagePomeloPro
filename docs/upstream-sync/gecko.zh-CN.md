# Gecko 默认离线载荷

VPP 默认包含 Wine Gecko 2.47.4 的 x86、x86_64 两套运行时。版本同时与
`mshtml/nsiface.idl`、`appwiz.cpl/addons.c` 配对；Wine 升级后如要求变化，准备阶段即失败，须明确更新锁文件。

采用 WineHQ 官方 tar.xz 的共享目录格式，放入
`share/wine/gecko/wine-gecko-2.47.4-{x86,x86_64}`。Wine 将 OHOS 的 `WINEDATADIR`
转换成 DOS 数据目录后，mshtml 可直接加载其中的 VERSION 和 XUL。
不再额外打入 MSI，也不为 Gecko 启用 appwiz.cpl 或修改 Mono 原有策略。
ARM64EC 的 mshtml 使用 x86_64 Gecko，WOW64 的 mshtml 使用 x86 Gecko；
打包校验检查实际 mshtml 的加载目录字符串和 Gecko PE Machine，不只检查目录名。

`scripts/wine_gecko.lock.json` 固定官方 HTTPS 来源、版本、两包大小/SHA-256，
以及解包后全部 60 个文件的大小与 SHA-256。归档约 81 MiB，共享目录约 207 MiB。
每次构建先核验现有缓存，相同输入复用；有损坏则从已校验归档恢复。
解包拒绝越界路径、重复条目、符号链接和特殊文件。
`assemble.sh` 只接受已就绪缓存；最终运行时 ZIP、HAP、APP 的组件检查都要求两套完整内容。

本地与 CI 走同一默认入口，无需额外开关：

```sh
make wine-gecko NATIVE_ARCH=arm64-v8a
python3 host_tests/wine_gecko_test.py
```

Gecko 提供 Wine MSHTML 功能。Steam 的 CEF 仍是其自带的 Chromium，Gecko 随包不代表
CEF 已改为 ARM 版，也不作为 Steam 界面性能改善的证据。设备功能验收与静态包检查分别记录。
