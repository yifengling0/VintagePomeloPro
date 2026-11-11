# Linux 游戏组件构建

`python3 scripts/build_linux_game_components.py --verify-only` 是本地和 CI 共用的
生产载荷校验入口；普通 `make ... hap-unsigned` 的 assemble 阶段也会执行。
源码、补丁、ELF 架构或来源收据不匹配就停止，不以旧载荷继续打包。

HAP CI 从源码编译 OHOS 宿主和 ArkTS；GNU 基础 ZIP 是发布后不可变的校验依赖。
HAP 内独立交付的 POSIX、FEX 启动器、Steam supervisor/desktop、GNU Proton shim、
FEX core/GL thunks 和合作式 lsof 全部带源码身份。
CI 校验这些已提交载荷；**HAP CI 不代表每次从源码重编全部 GNU 依赖**。

## 重建适配组件

在 Linux/WSL 中复制配置示例到固定任务目录并填写本机 SDK 路径：

```sh
python3 scripts/build_linux_game_components.py --rebuild \
  --config workspace_temp/hsl-sync/build-config.json
```

示例为 `docs/linux-steam/game-components.config.example.json`。本地绝对路径配置不提交。
输出复用 `workspace_temp/hsl-sync`、`workspace_temp/linux-proton/native-fex-core` 和
`workspace_temp/linux-proton/posix-hap-build`，不另建日期目录。

输入及恢复来源：

| 输入 | 来源与约束 |
| --- | --- |
| OHOS clang、GNU ARM64 13.3 | 显式 SDK 路径；记录 compiler/linker/sysroot/header 内容身份 |
| FEX | `thirdparty/fex` 内固定 `37265b109a4bba9e10241972dfcb3d1fdb8f3f9a` 对象及八个固定子模块；缺失的三个公开归档按脚本固定 SHA-256 下载 |
| FEX 公共修复 | `scripts/patches/fex-valve-2607-game-contract.patch` 及 provenance；保留现有 OHOS guest protection、公开 sigreturn 修复 |
| Linux UAPI | 提供含 linux、asm-generic、aarch64-linux-gnu/asm 的头文件闭包；全量内容记录进收据 |
| GL 编译工具 | `docs/linux-steam/fex-gl-tooling.lock.json` 固定 LLVM15、multilib、X11 DEB；`prepare_linux_fex_tooling.py` 按大小/SHA 校验并有界提取，不安装宿主系统包 |
| X11/XCB 与 GL SDK | `devPrefix` 提供 X11/XCB 头文件，`runtime` 提供匹配 ARM64 libGL 及其链接依赖；沿用 GNU SDK 构建产物，内容身份变化使缓存失效 |
| Mesa GL 头文件 | 本仓库固定 `thirdparty/mesa/include`；无需依赖 HSL 检出路径 |
| lsof 4.95.0 | `https://deb.debian.org/debian/pool/main/l/lsof/lsof_4.95.0.orig.tar.bz2`，脚本固定 SHA `e9faa0fbcc48638c1d1f143e93573ac43b65e76646150f83e24bd8c18786303c` |
| NSI | `scripts/build_linux_proton_network.py` 与 `docs/linux-steam/gnu-proton-network.lock.json`；既有 receipt 必须逐输入校验 |
| Vulkan/软件 GL | 复用 HAP 已校验的既有独立载荷；这次 FEX/GL 重建不修改其字节 |

全新的 GNU SDK 编译仍由已有各组件源码构建脚本负责；此入口没有隐藏下载或从 HSL
目录直接复制二进制。配置中的 SDK 输入均显式，组件收据记录实际使用的内容身份。
更换 SDK 可能产生不同合法载荷；必须重新审查 selector 与载荷整组配对并做设备回归。

FEX 私有源码从固定 Git 对象/归档逐文件校验。缓存目录不能通过 symlink 指向共享源码，
未声明源码修改会拒绝构建。对 AST 类型按稳定名称排序后再做依赖排序；uname 中的构建
时间固定为 FEX revision 的提交时间。BiSheng 不遵循 SOURCE_DATE_EPOCH 的这几处宏
已用明确源码补丁固定。`--force-rebuild` 可以重编 FEX 或四个 GL thunk 审计复现性。
本轮两次完整重编的六个文件 SHA 一致，记录在 `evidence/hsl-sync-build.json`。

GNU Proton shim 的来源收据同时记录原始源和实际转换后的编译源；包装时二者都必须
匹配。不能只刷新清单把旧二进制重新标成新代码。

## 更新与验收

适配器与整组 GL/FEX 一起提交。不可变基础 ZIP 的历史 hash 不修改；通过已批准的
HAP companion source override 引用重编载荷。预检检查新 CMake 输入已被 Git 跟踪。

本地和 CI 使用 `scripts/ci_preflight.py`、同一 Make 入口和最终 HAP 检查。
`verify_ci_release.py` 记录 commit/gitlinks，`prepare_linux_runtime_components.py --hap`
校验最终 GNU 成员，`prepare_release_offline_packages.py` 配对 HAP 和离线包。
签名材料保持外置，公开 CI 交付 unsigned HAP，不移动 `rc-1.5.7`。

主机测试、HAP 检查、签名检查、真机功能与性能结果分别记录；任何一层的通过不能
替代其他层。会员、Steam 组件离线导入、字体导入继续保留原 1.5.7 回归入口。
