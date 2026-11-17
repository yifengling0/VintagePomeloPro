# Gamescope 移植记录：OHOS 输出与手机适配边界

基线为 VPP `release/2.0.4@043a1cc0`，工作分支为 `feature/gamescope-port`。
HSL 参考提交为 `46fe1daabd26085c617c74033f470ac9e5d912b1`，
Gamescope 源码固定于 `e6674a98118ca7b098b98a3874b8f195ae1bf750`。
原有界面、图形后端默认值、用户配置和正式 2.0.4 包保留。新增仅调试签名可见的
“Gamescope 呈现检查”；完整 Game 页面尚未实现。

## 已迁入和验证

- 10 份上游源码归档锁，总压缩输入 165,486,506 字节。复用 HSL 已有归档，逐项核验大小与 SHA-256，无重复下载。
- HSL 已提交的 12 个补丁，保留顺序、来源与每项源文件应用前后的 SHA-256。
- 单独迁入 HSL 未提交的补丁 0013 和 `HslShmUpload.h`：最多三个可复用 staging slot，GPU 完成后才允许覆盖；每份目标纹理独立持有，保留队列依赖；大图和 DMA-BUF 路线保留同步边界。
- 共用描述符、shader 编译、图像协议与窗口关联源码作为来源记录迁入。`HslBackend.cpp` 的 GNU Xlib WSI 和 `HslImage.cpp` 的私有 GNU Control 传输目前仍是**参考实现**，不能作为 VPP 已接通的 OHOS 输出或资源导入。
- `make gamescope-source` 已在现有 `winehua-dev` 容器及 Linux ext4 工作树上通过 13 项严格补丁重放，关闭 patch fuzz，并核验所有前后文件身份。再次执行核验完整源码清单，拒绝未知改动。
- `make test-gamescope-shm` 通过实际迁入上传 owner 的主机行为测试：120 次复用、4 帧排队压力、5 种分配故障、尺寸变化、同步路线及析构排空。
- `make gamescope-ohos-probe` 使用 OHOS SDK 编译真实 `src/color_helpers.cpp` 和 `src/Utils/Process.cpp`，两者均产出 AArch64 ELF 对象；这不证明完整 Gamescope 已链接，也不证明 fork 等进程操作可在应用沙箱运行。
- 现有 Windows Steam 主机检查和产品源码载荷边界检查通过；现有 WoW64 explicit buffer flush 的 7 项测试通过。后者需指定实际只读 Wine 源码 `/shared/thirdparty/wine-valve`；本地空的子模块目录不能作为该测试输入，未修改共享 Wine 树。

详细来源在 `migration-sources.json`。原始 SHA-256 与 VPP 调整后的 SHA-256 分开保存。
HSL 的设备记录保存为 `hsl-shm-evidence-20261117.json`，只作为迁移依据，不计入 VPP 验收。

## HSL 未提交改动取舍

| 改动 | 本阶段处理 | 原因与边界 |
| --- | --- | --- |
| 有界 SHM staging 复用与队列依赖 | 已吸收并通过 VPP 主机测试 | 通用上传所有权修复；VPP 设备性能和异常退出仍需独立验收 |
| MIT-SHM、CEF GPU 默认值和 child 环境 | 保留合同，未复制 GNU 启动脚本 | 依赖 HSL 私有 GNU SysV IPC；VPP 使用 OHOS Wine 与 Windows Steam，需验证自身跨进程 SHM 能力 |
| OpenGL WoW64 explicit mapped-range flush | 本地已覆盖，无重复移植 | VPP `patches/wine/0035-opengl-wow64-explicit-buffer-flush.patch` 已包含显式范围发布与 unmap 语义，且有现有构建接线和测试 |
| Steam 导航、X11 SHM 诊断脚本 | 作为证据和后续用例参考 | 不引入 VPP 产品运行时或正式包 |
| 构建后存储维护接线 | 沿用各工程现有策略 | HSL 仍由自身任务维护；本轮没有修改或提交 HSL 原工作树 |
| 编辑器状态和临时截图 | 不迁入 | 与运行时修复无关 |

HSL 在审查期间仍有其他任务更新，来源清单记录的是本轮捕获版本；不得把后续工作树状态自动视为本次已审查代码。

## 当前已实现

- `make gamescope-ohos` 完整编译、链接并安装 AArch64 OHOS `libgamescope_core.so`。
  新增 libXdamage 1.1.6、libXtst 1.2.5、libXres 1.2.2、libXmu 1.2.1 中完整五文件的
  libXmuu；来源、大小、SHA-256 固定在 `gamescope-ohos-deps.lock.json`。
  复用共享只读 OHOS 依赖，不加载 GNU 二进制。`native-probe.json` 只是早期探测，
  其“缺失依赖”清单不是当前完整构建状态。
- 平台补丁 0014 条件化 OHOS 后端、脚本/Wayland/输入依赖及 SDK C++ 兼容部分；
  自身线程取消、共享快照和配方重放有主机行为测试。全部 15 项补丁保留前后哈希。
- 平板 core 通过 NCP 运行。父进程通过正式 OHIPC parcel 交接 NativeWindow 和两个
  socket FD，子进程不再根据父进程的 surface ID 查找窗口，不在线上传指针。
  无 context 的启动回调串行管理，超时后迟到回调不能配置下一会话。
- 原生会话控制启动、真实状态、尺寸/可见性及停止；spawn broker 复用 VPP 的
  Xwayland shim 和 FD/参数合同，不引入 GNU fork/exec。异常关闭显示
  `disconnected / ECONNRESET`，协议错误显示 `EPROTO`，不沿用最后一次成功的错误码。
- OHOS 输出使用 `/system/lib64/libvulkan.so`，避开包内供 Wine 使用的 guest Venus
  `libvulkan.so.1`。按真实 surface capabilities 选择 composite alpha；Mini 只提供
  INHERIT，不能强制 OPAQUE。使用逐槽 fence、按交换链图像复用的 present semaphore，
  仅 resize/销毁时排空。WSI clamp 后不因请求尺寸不同而无限重建。
- XKB/Xcursor 使用已有 Wine 资源；不复制键盘、字体、Wine 或游戏数据。
- 调试专用 X11 四色源只在专用 HAP 打包时加入，构建后移出 `entry/libs`，不加入
  生产 CMake target。系统崩溃订阅仅在诊断页面存活期间启用，固定小型本地记录。

## Mini 当前设备证据

Mini / Maleoon 910 已实际启动系统 Vulkan、NativeWindow swapchain、Wayland 和 Xwayland。
最初像素源在第一帧后退出，准确故障为 XWM 线程的 `SIGSYS / syscall 186 (msgget)`：
Mangoapp 的 Linux SysV 消息队列在首帧统计中初始化。补丁 0015 在 OHOS 构建中关闭
该专用 IPC，并保护直接初始化入口；桌面的原有环境开关仍有效。没有处理/吞掉禁止
调用来继续执行，也没有更换游戏渲染后端。定位用的临时信号处理器已删除。

修复后已目视确认四色图及 `SHM_UPLOAD frames=240 allocations=1`，会话保持活动，
点击正常停止收到 `returned / core-returned / 0`，相关子进程退出。
首次点击后立即截图可能仍是黑帧，延迟两秒的实际画面才作为像素证据。
这只证明 X11/SHM → Vulkan 合成 → NativeWindow 基础输出，不能计作 GPU 游戏验收。
早期候选已做五轮真机启停和三次 resize 状态检查，但当时只检查四色，没有几何
判据。随后发现系统 `currentTransform=ROTATE_90` 导致画面比例异常；优先使用
能力中支持的 IDENTITY 后，首次输出恢复 4:3。严格 resize 验证另发现 IPC 窗口的
buffer geometry 保留旧值，正在独立复测显式同步尺寸的修复。不能把早期状态通过
当作尺寸/比例验收。异常客户端、后台恢复和资源增长仍待记录。

主机通过：实际 SHM owner、工作线程、FD transport、7 项配方事务测试、手机/平板进程
路由、五轮真实 socket 会话及异常状态测试，以及直接编译实际 Mangoapp gate/init 的
平台测试。OHOS core、App native、ArkTS 和调试签名打包均通过。

## 手机必须单独适配

手机系统 NCP 不可用。运行时子进程只使用 Ability.onCreate 早期建立的现有 clean
fork server；不可用/死亡时不 late fork、不尝试系统 NCP。路由主机测试不代表手机
NativeBuffer 输出已解决。手机不调用平板专用的 `StartOutputChild` IPC bootstrap。

按用户要求，Game 产品层共用，手机沿现有 App 内合成链适配类似的呈现功能；
不能把独立 Gamescope 可用性当作进入 Game 页面的前置。具体实现边界见
[手机适配方案](phone-support.zh-CN.md)。手机呈现与 FSR/限帧尚未实现或真机验收。

## 尚未完成的交付范围

1. GPU 游戏 NativeBuffer/fence importer、窗口关联与消费退役，覆盖 Direct/Venus/VirGL。
   HSL `HslImage` 的 GNU 私有传输仍为参考；不得跨 VkDevice 使用原始 VkImage。
2. 单一输入通道、焦点/黑边逆映射、相对鼠标、IME、授权剪贴板与多手柄接入。
3. 完整 Game 游戏库、Steam 大屏入口、单游戏覆盖、持久化设置、快捷面板与正常退出。
4. FSR、限帧实效与手机合成链；PAL4、Heaven、DX11、Steam 派发及严格性能对照。
5. 完整生命周期、资源增长和手机真机回归。

调试包与回退包位于固定 `workspace_temp/gamescope-port/`，各有大小和 SHA-256 收据。
现有 Wine 数据 ZIP 及原有 native 载荷（除 App 控制层 libentry）与 PAL4 基线的
成员 CRC/大小对照一致；该检查不等同于逐成员 SHA-256 或完整设备兼容性。
未合并 main、未推送、未上架，也没有替换正式交付包。
