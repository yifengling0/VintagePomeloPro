# HSL DXVK 适配移入 VPP Linux

本次目标分支为 `feature/ohos-proton-x11-integration`，基线为
`cc60f68587baecdc18f696a4c2f87787c7920e88`。来源为 HSL
`fa3dfeec72a0568133383763e77368b8502cf1f8` 及本机工作树中的已校验文件。
逐文件来源与 SHA-256 见 `hsl-dxvk-migration.json`；不能只凭 HSL 提交号还原
其中的工作树内容。VPP 已保存实际补丁、构建脚本、源锁和必要的载荷。

## 接入范围

- GNU Proton 的 D3D11 接入 HSL legacy DXVK 1.10.3 fork，ARM64X（含原生及
  EC 双视图）和 i386 的 `dxgi.dll`、`d3d11.dll` 成组交付。
- GNU Venus 为 WoW64 映射保留 backing FD，导出私有共享映射入口；修正映射
  尾页与整数溢出检查、入口缺失时的初始化和诊断。
- BC1–BC7 的 format、image、sample 能力逐项核对，仅在与逐项能力矛盾时
  降低总的 `textureCompressionBC` 声明，不伪造压缩格式支持。
- Venus 与 OHOS renderer 按连接协商 `precise-shadow-v1`。显式 flush 发布
  CPU 修改，invalidate 接收 GPU 修改，fence 不再全量覆盖尚未发布的 CPU
  写入；未协商的 CEF、Linux 原生及原 Wine 客户端保留原有协议。
- fork 的动态映射 flush、query reset、RGBA8 SNORM RT 仿真、mapped flush
  批处理等开关，仅在所选 GNU DXVK profile 中启用，并随 Wine 子进程保留。
- D3D9 仍用 WineD3D／VirGL；GNU GL 工具显式选择 builtin D3D11/DXGI，
  不误载新安装的 native DXVK DLL。

未纳入 HSL 的 UI、用户前缀策略、rootfs、用户数据及 transfer-only shared
swapchain 原型。VPP 已有的资源发送 FD close 补丁不重复迁入。原 OHOS Wine
的 DXVK 子模块、Deck／经典 UI、音频、字体、RGB、GStreamer、模型和网络修复
没有被替换。DXVK 2.6 仅保留来源锁与构建工具，本次没有启用或真机验收。

## 用户安装与更新

支持层随 HAP 的 `linux-proton/dxvk-support` 交付，由正常组件安装／复用流程
校验并安装到 `proton-probe/dxvk-support`，不要求用户执行调试命令。

Steam 手动启动及首页启动共用 GNU 启动队列，在 Wine 进程启动前准备前缀。
ARM64X 文件安装到 `system32`，i386 文件安装到 `syswow64`，并补齐与原 GE
Unix `winevulkan.so` 配对的 Vulkan PE 文件和 D3D compiler。

先验证整组目标、备份原文件，再写恢复记录、逐文件 fsync／rename、更新
receipt 并提交。下次启动先恢复未完成事务；这属于可恢复更新，不表示跨多文件
的单次原子提交。未知用户 native DLL、符号链接（含断链）及被手动改动的文件
均保留并报错，不强行覆盖。游戏库、用户注册表和存档不在更新清单内。

当前支持层明确配对 GE-Proton11-11 ARM64 的 Unix Vulkan backend，选择其他
不匹配的上游版本时拒绝混装，需为该版本增加对应支持清单，不能假定 ABI 通用。

历史 GE／Steam 离线包及线上组件 ZIP 保持原字节和原身份。本次适配由新版 HAP
独立补充，所以旧离线包安装后也会走同一支持层，不需要只为此变更重打历史包。

## 构建与 CI

- `gnu-dxvk-source.lock.json` 固定 fork、工具链、头文件及 DLL 身份。
  `build-dxvk-inputs.py` 获取校验源码，`build-dxvk-arm64x.py` 与
  `build-dxvk-i386.py` 可分别重建两个位宽；本轮复用 HSL 中与源锁完全一致的
  已构建 DLL，未声称重新编译了 DXVK。
- `build_linux_proton_venus.py` 在私有小型 overlay 中重编全部 23 个 Venus
  编译单元，不覆盖共享 Meson 对象。`mesa-base` 补丁和
  `gnu-venus-base.lock.json` 支持从 VPP 跟踪的 Mesa 源树准备 GNU baseline。
- 既有 Linux 原生 driver 仍对应原构建器，原字节保存为
  `build_linux_proton_venus_legacy.py`，不把旧二进制标成新源码结果。
- `prepare_linux_proton_dxvk.py` 校验载荷、来源、双 PE 视图及 GE backend；
  `prepare_linux_runtime_components.py` 和 HAP 包内容检查已纳入该支持层。
- OHOS renderer 必须应用 0011／0012 后重编并暂存；仅更新 GNU rawfile 不够。
  本轮已重编 renderer，并核对 HAP 中两库与新的暂存字节一致。图形源码锁已更新，
  未把本轮主机结果写成既有真机验收记录。

## 验证范围

已完成的检查：

- GNU Venus、GNU probe shim、GNU Wine WSI、HAP POSIX／FEX companion、
  Linux native WSI companion 和 OHOS renderer 编译。
- Release `assembleHap`，支持层 11 个文件共 26,912,724 字节及新 renderer
  的包内字节核对。
- backing FD、CLOEXEC、双向共享映射、错误路径及 FD 不增长；BC 能力与连接
  隔离；Wine／native WSI、Xvfb 输入与窗口关闭。
- 安装复用、双位宽、整组检查、中断回退、进程重启恢复、FD／pending 清理、
  未知 DLL／断链／journal 拒绝、GE 配对及存档保留。
- GNU ELF 依赖与已交付 runtime 闭包、DXVK PE imports、Vulkan forwarders。
- 既有 Proton 组件／Steam 启动测试、图形契约及运行时组件检查。
- renderer 全部受管补丁的干净应用、重复应用、反向恢复及重放；完整预检
  226 项通过（包含全量 OHOS Native CMake 配置及源码入库检查）。

本轮未操作设备，尚不能认定 VPP 的 DXVK 游戏兼容性、像素正确性和性能已通过。
后续需实机验证 D3D11 双位宽、最小化／恢复／重开、窗口与全屏、D3D9 GL 回归、
GNU GL 与 Linux 原生隔离，并检查是否有闪烁或 GPU 同步回退。DXVK 2.6 按当前
设备限制继续跳过。
