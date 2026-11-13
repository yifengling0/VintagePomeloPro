# HSL 64 位 DXVK 模块闭包移植

基线为 `8c5d45519c777a52506065bd5810f05f1438c305`，目标分支为
`feature/ohos-proton-x11-integration`。HSL 来源、实际工作树文件 SHA-256
及完整目录收据见 [来源记录](hsl-dxvk64-migration.json)。本轮保留既有
WineHua legacy DXVK 1.10.3 ARM64X／i386、Venus precise-shadow 和 WSI，
主要补齐原 GE 运行库与 GNU 启动适配；没有重新引入官方未移植 DXVK DLL。

## 实机定位的两个启动原因

1. 原 GNU 组件只选入 116 个 aarch64-windows 和 637 个 i386-windows
   模块；同一锁定 GE 原包实际有 795／842 个。动态加载的 concrt140、
   媒体、网络等模块不能仅从主程序静态 imports 推导。新增完整模块目录合同，
   同时锁定 44 个 aarch64-unix 文件，与原 PE／Unix ABI 配对。
2. 仅把 WineVulkan PE 放入 C 盘不够。Wine 的 builtin 搜索目录找不到原 PE，
   就无法关联对应 Unix library，扩展枚举返回空，DXVK 报
   `Required Vulkan extension VK_KHR_surface not supported`。补充层还将
   21 个缺失的 Unix 配套 PE 放到原 package 的 builtin 目录，共 21,757,952
   字节；不复制整份第二模块库。所有来源均为同一原 GE 包，不伪造 Vulkan 扩展。

另修正 GNU shim 的可选 syscall 首次匹配：共享汇编新增的 FEX 分支先截获
282／449，导致 GNU Wine 将可选探测送入 OHOS seccomp，设备实际出现
`SIGSYS syscall=449`。GNU 构建单独让 282／374／449 直接返回 ENOSYS，
使用 Wine 的真实回退；原 Linux FEX 与 OHOS Wine 的共享汇编没有改动。
测试编译实际 ARM64 汇编并核对三个最早分支，不只检查源码里是否有回退标签。

## 普通用户安装与更新

正常在线安装、历史离线导入和已安装组件复用都会在 `ensureReady` 中补齐。
Steam／首页手动启动共用该组件准备流程，不需要用户手动复制 DLL 或运行 smoke。

- 历史 GNU Proton／Steam ZIP 原字节、版本和身份保持不变。
- 新 HAP 包含独立 `linux-proton/modules-support`，约 77.5 MB 压缩数据；
  原先缺失模块解压约 593.6 MB。安装时直接移动到唯一共享 GNU prefix；
  只额外保留上述约 21.8 MB 的 Unix 配套原 PE。
- 已有 package／media 原始模块直接复用。先校验压缩包、完整成员、大小与哈希，
  再发布补缺文件。失败时只留下完整模块，下次继续；scratch 在 finally 清理。
- prefix 内已有 DLL、普通文件及链接（含断链）保留；builtin 原件不能从用户
  修改后的 prefix DLL 反向生成。未知 package 配套原件保留并报错。
- 不修改游戏库、注册表、存档或每游戏配置；D3D11／DXGI 继续由既有独立支持层
  成组管理，D3D9 仍走 WineD3D／VirGL。

该层配对 GE-Proton11-11-aarch64，不能据版本名字将其他 GE 的 PE／Unix ABI 混装。
本轮无需重打历史离线包；用户需要更新包含本修复的 HAP。

## 干净构建

`gnu-proton-modules.lock.json` 固定每个原模块的大小、SHA-256 和完整目录收据。
`prepare_linux_proton_module_closure.py` 从已锁 SHA-512 的 GE 原始归档恢复
generated ZIP；缓存命中时复用。下载读取 distribution 的 `source` 字段，
缺缓存路径有 mock 下载回归。ZIP 的未压缩内容受锁，压缩容器哈希由本次构建生成，
不要求不同 zlib 版本输出相同压缩字节。

正常 HAP 准备、包内容核验、CI preflight 和 Node 安装器测试已接入该层。
源码、合同、测试、GNU shim 实际二进制及其来源锁均须入库；generated
modules ZIP 不入 Git，CI 从固定上游输入恢复。

## 验证边界

可重建 64 位诊断：`scripts/build_gnu_proton_d3d11_probe.py`，源码为
`smoke/gnu_proton_d3d11_probe.c`，由 HSL fixture 移植。它是 x86_64 Windows
程序，实际经 GE/FEX 执行 ARM64X 的 EC 视图，不冒称验证了原生 ARM64 主图。
检查六个动态模块的实际路径、Vulkan adapter、BC1～7、DISCARD／NO_OVERWRITE、
读回、缩放及子进程。诊断用 240 帧，让缩放前后各达到 WSI 每 120 帧的提交标记；
保留 120 秒最终保护。包含故意 staging 读回，不能当作性能基准。

普通平板 MLR-AL10 已通过以下基础回归：

- GNU 64 位 D3D11：父子各 240 帧接受／读回，BC1～7、动态缓冲区、缩放与退出通过。
- GNU 32 位 DXVK 1.10.3：确认实际走 x86，GPU 提交与退出通过。
- GNU D3D9 WineD3D／VirGL：1,004 帧，旋转回退计数为 0。
- 原 OHOS Wine 独立冷会话：OpenGL／DNS 双位宽 4 项通过，含 OpenGL 主机图像判定。

首次在同一个诊断宿主中从 GNU 连续测试切回原 Wine，曾出现 GL 生成帧但无显示序列，
后续 AMD64 项超时；独立冷启动后通过。本轮未修复或验收这种混合诊断宿主切换。
GNU smoke 的提交结果也不等于已经验证持续 NativeImage 消费／实际显示帧率。

设备结果与制品哈希见 [smoke 记录](evidence/hsl-dxvk64-module-smoke.json)。
本轮受控 fixture 与基础回归不代表所有游戏、长期运行、全屏切换或性能已经验收。
DXVK 2.6 仍按当前设备限制跳过。
