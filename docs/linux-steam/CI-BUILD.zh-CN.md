# Linux Steam / GNU Proton 的干净构建

受支持分支是 `main_proton_steamarm64`。提交到该分支会运行
`.github/workflows/build.yml`，从固定 gitlink 拉取子模块，编译 OHOS Wine、
FEX、Mesa/VirGL、DXVK、OHOS GNU 宿主和 ArkTS，生成 API 23 ARM64 未签名 HAP。
`entry/build-profile.json5` 已提交两个 GNU 宿主 CMake 开关，不依赖本机命令行缓存。
CI 显式安装 ArkTS 依赖；标准打包入口显式选择 Hvigor Release，避免默认 Debug。
快速检查先在不初始化子模块的干净 checkout 上执行主机回归、从公开 URL 获取
并逐文件验证两个 GNU 组件，成功后才开始完整的递归子模块与交叉编译任务。

## GNU 依赖的边界

GNU Steam 运行时和 GNU Proton 使用已验过的预编译依赖。它们没有塞进 Git，
也不是每次 HAP 构建从零编译 glibc/GE Wine/Mesa。
`build-components.lock.json` 记录固定 GitHub dependency release URL、大小、
SHA-256、清单 SHA-256、适配源码 SHA-256 和关键子模块提交。
发布资产包含项目运行时，不包含 Valve Steam 客户端、账号、游戏或存档。
Valve 客户端仍由应用单独下载或由用户导入。

`prepare_linux_runtime_components.py` 首先验证源码、清单和子模块身份，
再复用本地相同包或下载固定资产，逐成员核验后原子安装到 rawfile。
缺失、损坏、源码变化或最终 HAP 漏包均报错，不会静默省略功能。
CI 不需要开发机器的绝对路径、签名材料或增量构建目录。

OHOS Wine 的两份 `lsteamclient.dll` 在标准组装阶段从已提交桥接源码和固定
Proton SDK 头生成，和本次编译的 `kernelbase.dll`、GNU 服务端 SHA 配对，
无需本机后置注入。SDK 头按逐文件 SHA 下载或复用；可用
`LINUX_STEAM_SDK_REFERENCE` 指定已有头文件缓存。增量产物同时校验源码、
编译器和输出哈希；最终门禁拒绝接口桥缺失、错误架构及混合版本。

图形 smoke 测试按实际编译器目标选择 Vulkan 导入库。ARM64 Wine 的 `x64`
槽默认是原生 ARM64，但显式 AMD64 对照使用 x86_64：缺少该架构的 Wine
导入库时，从本次 Wine Vulkan DLL 的导出表生成 AMD64 导入描述符，避免混入
ARM64 实现对象。快速回归覆盖此架构选择，构建脚本变化会使测试载荷重新生成。

## 本地复现

安装与 CI 一致的 HarmonyOS API 23、llvm-mingw 和宿主构建依赖后：

```sh
git clone --recurse-submodules --branch main_proton_steamarm64 \
  https://github.com/yifengling0/VintagePomeloPro.git
cd VintagePomeloPro
make test-ci-release test-linux-runtime-components
python3 scripts/prepare_linux_runtime_components.py
# build-profile.json5 使用工作流中的 unsigned profile，不导入任何私钥。
# 在编译器应用已提交补丁之前，记录干净 checkout 的来源。
python3 scripts/verify_ci_release.py source --expected-sha "$(git rev-parse HEAD)" \
  --output ci-artifacts/ci-source.json
SOURCE_BUILD_MANIFEST="$PWD/ci-artifacts/ci-source.json" \
make NATIVE_ARCH=arm64-v8a WINE_ARCH=aarch64 GUEST_ARCH=aarch64 \
  BUILD_GUEST_GFX=1 BUILD_GUEST_VULKAN=1 BUILD_WINE_MONO=1 hap-unsigned
```

`assemble.sh` 会自动准备 GNU 包；`package.sh` 在 Hvigor 前后分别校验
rawfile 和实际 HAP。CI 另行核验 GNU 宿主 ELF、运行时源码提交、架构、
Wine/FEX/Mesa/DXVK 等 gitlink 及 payload 哈希，并上传小型构建清单。

## 源码重编候选与实机验收的区别

完整 CI 设置 `SOURCE_BUILD_MANIFEST=ci-artifacts/ci-source.json`，其输入必须是
编译前生成的干净源码清单。组装时核对源码提交、所有 gitlink、补丁哈希、
WineD3D 和 Mesa 的实际已应用补丁、两版 DXVK 的已审查源码，再记录编译器
和所有受保护模块的产物身份。HAP 中保留 `source-build-candidate.json`，
明确 `deviceValidated=false`。此模式仍检查原有模块清单、PE/ELF 架构、
纹理修复标记、库依赖闭包、嵌套配对和产物哈希。Release strip 仅允许
非加载元数据变化，实际加载段和节不得变化。

5 个由 Hvigor CMake 编译的受保护库不复用旧产物身份：组装时只允许这份明确
白名单暂缺，首轮 Hvigor 完成后从实际 HAP 补录身份，更新 payload 哈希与
解包版本标记，再增量打包。最终 HAP 校验禁止任何暂缺模块；其他依赖缺失
仍在组装阶段立即失败。

未传 `--source-build-manifest` 的校验仍要求原有已验收二进制锁，不能靠
包内自报字段切换模式。RC 发布另跑这道默认检查；源码重编成功不代表
已经通过游戏或设备验收。新编二进制需回归后再审查更新发布锁。

真实 `libz.so` 在依赖链起点从固定 SHA-256 的 zlib 1.3.1 源码编译，
不会把 SDK 链接桩当成实现库。已有源码包可通过 `ZLIB_SOURCE_ARCHIVE`
指定；默认下载到 `BUILD_DIR/downloads/`，增量复用同时校验输入和产物。

## 修改 GNU 适配源码后

使用已提交的阶段构建脚本重编受影响 GNU 对象，更新组件包及逐文件清单，
先验证实际包，再创建独立 `build-deps-gnu-*` release（不占应用 Latest）。
随后运行并提交：

```sh
python3 scripts/lock_linux_runtime_components.py \
  --release-tag build-deps-gnu-<version-and-identity> --source-commit <verified-source-sha>
python3 scripts/prepare_linux_runtime_components.py --verify-only
```

锁文件不是自动重编器；不要仅更新哈希掩盖未重编的适配对象。
生成器核验指定来源提交确实包含所记录的适配源码及 gitlink，拒绝把本地未提交
的源码写成历史提交的构建身份。
完整 GNU 从零构建编排仍是独立工作，当前发布不得声称已实现该能力。
仅修改 OHOS 宿主或 ArkTS 不需重新发布同一 GNU 二进制，锁覆盖的 GNU 输入
发生变化则必须重新验包。离线导入的客户端包与此构建依赖发布相互独立。
