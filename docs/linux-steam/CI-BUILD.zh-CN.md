# Linux Steam / GNU Proton 的干净构建

受支持分支是 `main_proton_steamarm64`。提交到该分支会运行
`.github/workflows/build.yml`，从固定 gitlink 拉取子模块，编译 OHOS Wine、
FEX、Mesa/VirGL、DXVK、OHOS GNU 宿主和 ArkTS，生成 API 23 ARM64 未签名 HAP。
`entry/build-profile.json5` 已提交两个 GNU 宿主 CMake 开关，不依赖本机命令行缓存。

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

## 本地复现

安装与 CI 一致的 HarmonyOS API 23、llvm-mingw 和宿主构建依赖后：

```sh
git clone --recurse-submodules --branch main_proton_steamarm64 \
  https://github.com/yifengling0/VintagePomeloPro.git
cd VintagePomeloPro
make test-ci-release test-linux-runtime-components
python3 scripts/prepare_linux_runtime_components.py
# build-profile.json5 使用工作流中的 unsigned profile，不导入任何私钥。
make NATIVE_ARCH=arm64-v8a WINE_ARCH=aarch64 GUEST_ARCH=aarch64 \
  BUILD_GUEST_GFX=1 BUILD_GUEST_VULKAN=1 BUILD_WINE_MONO=1 hap-unsigned
```

`assemble.sh` 会自动准备 GNU 包；`package.sh` 在 Hvigor 前后分别校验
rawfile 和实际 HAP。CI 另行核验 GNU 宿主 ELF、运行时源码提交、架构、
Wine/FEX/Mesa/DXVK 等 gitlink 及 payload 哈希，并上传小型构建清单。

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
完整 GNU 从零构建编排仍是独立工作，当前发布不得声称已实现该能力。
仅修改 OHOS 宿主或 ArkTS 不需重新发布同一 GNU 二进制，锁覆盖的 GNU 输入
发生变化则必须重新验包。离线导入的客户端包与此构建依赖发布相互独立。
