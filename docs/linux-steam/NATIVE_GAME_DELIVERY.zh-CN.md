# Linux 原生游戏修复交付与验收

2026-10-31：修复从手工诊断组合接入正常 HAP 安装／升级路径。用户不需要替换游戏入口、修改官方 FEX depot、重新导入 Steam 离线包或打开诊断开关。**自动补丁已实机确认；普通入口的 Banana 完整游戏验收尚未通过：当前设备原有 SQLite 数据库缺表，仍停留在背景。** 不自动删除数据库或清空游戏数据。

## 修复范围

| 内容 | 修复 | 交付位置 |
| --- | --- | --- |
| FEX 信号恢复 | 原样应用公开 issue #5901 diff，恢复 InSyscallInfo，避免后续异步信号使用旧状态 | native-game-support/FEX 与配对 FEXServer |
| FEX guest 内存权限 | guest mprotect 的执行位被宿主以 EACCES/EPERM 拒绝时，仅在 native-FEX guest syscall 层重试去除 host 执行位，VMA 跟踪保留 guest 权限；不修改 ARM64 JIT 分配器 | 本地 MIT 补丁与 companion FEX |
| 可选 NUMA 系统调用 | OHOS seccomp 对五个可选 NUMA 调用触发 SIGSYS 时返回不支持语义 -ENOSYS，保留输出缓冲区与其他上下文 | HAP POSIX companion，限 native-FEX 作用域 |
| Vulkan 资源合同 | 映射／导出内存属性、image/buffer 要求与绑定保持一致；复用已有 NativeBuffer/NativeImage receiver | OHOS 源码补丁 0007/0008、GNU Venus companion |
| BC 纹理 | GPU 转码 BC1/2/3/7→ETC2/EAC，BC4/5→R/RG、BC6→RGBA16F；保留原 blocks 的精确 copy/readback | 仅 native-FEX WSI 启用 |
| CEF 内存驻留 | softpipe 按需零页与有效位图，避免为尚未用到的纹理缓存触碰全部像素页 | GNU software GL companion、Mesa 补丁 0011 |
| 执行权限预检 | X_OK 与实际用户态 ELF/解释器交接共享同一受控命名空间；不把任意数据或外置程序伪装为可执行 | HAP POSIX companion |
| 保存／安装失败日志 | 常见 EACCES/EPERM/EROFS/ENOSPC/EDQUOT 有界记录真实 errno | 每进程最多 32 条，不伪造写入成功 |

没有将 BC 层接入原 OHOS Wine WSI，也没有用 CPU 帧回读规避 GPU 问题。ETC2/EAC 是有损再编码；并非所有 BC 格式都等比例省内存。BC4/5/6 backing 可能更大，完整限制见 native_bc README。存档失败日志增强不等于所有游戏的存档权限问题都已解决。

## 用户安装与升级

`ensureLinuxSteamPosixSupport()` 在 Steam 启动前安装 POSIX、native WSI 和四个 `native-game-support` 组件。它使用随 HAP 携带的 manifest 逐文件核对大小、ARM64 ELF 与 SHA-256，通过 `.pending` 校验后原子发布；已有正确文件直接复用，缺失／损坏文件重装，符号链接或不安全目录拒绝。

四个组件共 23,568,408 bytes。HAP 自带这些修复，不需要为了本次补丁更新两个不可变 GNU ZIP，原历史离线包继续由现有导入器处理。游戏库、账号、存档与 Steam 下载的官方 FEX 文件不被该安装步骤覆盖。不能将这句话理解为任意来源或任意未来格式的 ZIP 都受支持。

FEX selector 同时校验已知官方 core 与本次 HAP 配对 core/server。仅当官方 SHA-256 为 `f9cbb7a70e804b9930fed02fd9d1349d49d7808faace703063a9b4944bc3568e` 时启用补丁版；未知／新版本保留该 depot 的官方版本及 thunks，日志解释回退原因。当前锁定的是已验证 depot core，不代表已验证任意未来 thunk ABI。无 OpenSSL 新依赖，选择器使用最小 GNU 运行时既有 libgcrypt。

实际启用日志：

```text
STEAMOHOS_NATIVE_FEX core=hap-matched reason=matched patch=sigreturn-5901+ohos-guest-vma
```

安装器校验成功和命令 ACK 不是游戏启动成功。正式入口还必须核对新 Player.log、新 PID、实际画面与输入。

## 实机结果与未完成项

- 普通平板安装标准 Release 构建的调试签名 HAP，四个修复组件自动落地，身份全部匹配；原游戏启动器及官方 FEX SHA 未变。
- 普通 Steam 会话实际选择补丁 core，同源 FEXServer 成功启动；768 次 BC upload、10440 帧标记、Vulkan allocation 峰值 2,403,707,648 bytes、分配失败 0、呈现 pixel_readbacks=0。进程持续 4 分 31 秒，本轮未见 fatal，随后测试程序主动结束宿主。这不是全游戏最大内存或长期稳定性保证。
- 原有数据库报 `SQLiteException: no such table: CollectionItem`，画面停在背景；没有点击及正常菜单退出验收。不清理原数据库，也不以更换私有目录隐式重置用户存档。本轮尚未做“新外置 SQLite 数据库 vs 私有数据库”严格对照，不能据此认定整个外置文件系统损坏。
- 之前独立诊断组合使用私有 XDG 配置、不同启动协议与临时诊断入口，持续 483.4 秒，完整 UI、约 60 FPS，点击 64→65；到时由 runner 停止。该结果说明组合可行，不能冒充普通安装已完成同样验收。
- 没有对手机、Edge、i386 原生游戏或所有 Unity/Godot 游戏作通用兼容保证。正常退出／重新启动及跨设备云存档仍需后续验证。

## 构建与 CI 闭环

`prepare_linux_native_repairs.py --verify-only` 校验四个 ELF、manifest、源码锁、上游 patch 来源和 FEX selector 的配对 SHA，避免换工具链后悄悄回退原 core；HAP 检查还要求文件和 NOTICE 完整一致。源码、修复补丁、重建器、预编译 companion、来源清单均随提交保存，CI 不依赖本机临时目录。

OHOS receiver 由正常 `scripts/build_native.sh`／virglrenderer patch 流程重建；GNU 大运行时继续从固定 SHA 的构建依赖恢复，不重新打包官方 depot。原 Wine、DXVK、GStreamer、字体和 RGB 相关补丁没有被这些 companion 替换。

复用已有构建输出：

```sh
python3 scripts/prepare_linux_native_repairs.py \
  --fex-build workspace_temp/linux-proton/native-fex-core \
  --venus /path/to/verified/libvulkan_virtio.so \
  --software-gl /path/to/verified/libGL.so.1.5.0
python3 scripts/prepare_linux_native_repairs.py --verify-only
python3 scripts/build_linux_steam_posix.py --verify-only
python3 scripts/build_linux_steam_native_graphics.py --verify-only
python3 scripts/prepare_linux_runtime_components.py --verify-only
python3 scripts/prepare_linux_runtime_components.py --hap path/to/final.hap
```

FEX 复建参数见 [sigreturn 说明](FEX_SIGRETURN.zh-CN.md)。native WSI 重建需 GNU ARM64 编译器、已有 Mesa/Wayland 依赖和 glslangValidator；其生成器、输入 Vulkan 头、SPIR-V 及结构尺寸来源均受 manifest 记录约束。

回归覆盖：真实 GNU ELF 执行预检、FEX 路由与 libgcrypt SHA、未知 depot 回退、安装缓存／缺失／损坏／SHA 拒绝、不可变 ZIP 和 HAP 校验、Deck 双 Steam 隔离、compositor ASan/UBSan。完整设备结果见[脱敏证据](evidence/native-game-delivery.json)；早期失败证据保留，原始账号和存档日志不入仓库。
