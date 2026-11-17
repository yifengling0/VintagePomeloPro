# Gamescope 移植记录：OHOS 输出与手机适配边界

基线为 VPP `release/2.0.4@043a1cc0`，工作分支为 `feature/gamescope-port`。
HSL 参考提交为 `46fe1daabd26085c617c74033f470ac9e5d912b1`，
Gamescope 源码固定于 `e6674a98118ca7b098b98a3874b8f195ae1bf750`。
原有界面、图形后端默认值、用户配置和正式 2.0.4 包保留。已新增独立 Game 游戏库、
快捷面板和 VPP 自有合成器的 Game 呈现；仅调试签名可见的“Gamescope 呈现检查”
继续用于独立 core 验证。真实 GPU 游戏尚未经过 Gamescope core，FSR 尚未接入。

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
早期候选只检查四色和状态，未包含几何判据。随后定位并修复两项问题：

- 系统 `currentTransform=ROTATE_90` 导致重复变换；在实际支持时使用 IDENTITY。
- IPC 窗口的 buffer geometry 保留旧值；resize 时显式同步生产者尺寸，再查询 WSI。

修复后的同一 core 已通过带像素/几何判据的五轮启停和三次 resize。最终包
`b107f69d` 又独立完成一轮启停和三次 resize：实际交换链为
`2446×1231 → 1835×1231 → 1223×1231 → 2446×1231`，800×600 内容的
截图比例分别约 1.33496、1.33496、1.33370、1.33496，偏差低于 0.13%。
每次停止收到 `returned / core-returned / 0`，未见相关子进程残留。
普通启动的进程列表只有 App，没有 Gamescope 探测子进程。
这些仍只验证 X11/SHM 基础输出；异常客户端、后台恢复和资源增长仍待记录。

主机通过：实际 SHM owner、工作线程、FD transport、7 项配方事务测试、手机/平板进程
路由、五轮真实 socket 会话及异常状态测试，以及直接编译实际 Mangoapp gate/init 的
平台测试。OHOS core、App native、ArkTS 和调试签名打包均通过。

## 手机必须单独适配

手机系统 NCP 不可用。运行时子进程只使用 Ability.onCreate 早期建立的现有 clean
fork server；不可用/死亡时不 late fork、不尝试系统 NCP。路由主机测试不代表手机
NativeBuffer 输出已解决。手机不调用平板专用的 `StartOutputChild` IPC bootstrap。

按用户要求，Game 产品层共用，手机沿现有 App 内合成链适配类似的呈现功能；
不能把独立 Gamescope 可用性当作进入 Game 页面的前置。具体实现边界见
[手机适配方案](phone-support.zh-CN.md)。共用 Game 页面、App 内缩放和限帧接线已实现；
手机真机、限帧实际效果与 FSR 尚未验收。

## Game 产品层与客户区呈现

- Game 入口复用 Deck 的数据与卡片，提供全部、最近、Steam、本地及搜索；进入页面不启动 Wine/Steam。
- 全局和单游戏设置在启动前解析，单游戏可跟随、开启、关闭。保持原图形后端，已有 Windows 会话设置冲突时拒绝热切换。
- 快捷面板支持返回游戏、保留会话回库、性能浮层、下次会话的线性/最近邻/整数缩放及默认/30/60 FPS、正常退出。面板键盘 Down/Up 和手柄沿已有输入所有权管理，不向 Wine 重复投递。
- `2ca3d728` 修复 Game 画面包含标题栏的问题：从协议关系识别唯一存活 InlineClient，以真实客户区尺寸和偏移计算缩放、层位置与输入逆映射，不猜边框宽度；含糊窗口保持安全整窗回退。保留游戏分辨率、Wine DPI 和窗口状态。
- Mini 的 PAL4 主菜单和存档页已确认去标题栏、800×600 客户区等比放大、4:3 双侧黑边；点选存档和读取确认框的坐标正确，中文正常。该路径是现有 Wayland/VPP 合成器，X11 的 Game 专用裁切尚未接通。
- `d74507f1` 的面板键盘隔离已在 Mini 验证：Down+Enter 返回游戏库，保留原会话，未将两键传给 Wine。
- `042600eb` 新增退出监测：仅在 native 呈现接收后开始，真实进程及最后窗口均退出才返回；查询失败、残留子进程和其他前台窗口不能触发返回。Steam 复用原客户端；游戏库恢复非输入框焦点，避免搜索键盘自动弹出。行为测试通过；Mini 已验证保留会话回库、从运行中恢复和 PAL4 菜单退出后自动返回 Game（保留此前子页面）。Steam 返回只有主机测试，尚未真机验收。
- 已接入帧调度与生产者节拍设置，但尚无完整同条件游戏 3×60 秒测量；不能把宿主上屏计数视为游戏 FPS。
- 载入 PAL4 存档时仍见既有 Zink 白地面/几何异常；与本次标题栏修复分开记录，未将其标为已解决。

本轮主机通过 `test-game-mode-models`、`test-game-presentation` 和 `test-compositor-migration`
（92 项，零失败）。完整 suite 中旧 Linux Steam 接口测试，以及缺少展开 Wine 子模块的
手柄 replay 尚未全过，不以本轮定向测试代替完整验收。

## 尚未完成的交付范围

1. GPU 游戏 NativeBuffer/fence importer、窗口关联与消费退役，覆盖 Direct/Venus/VirGL。
   HSL `HslImage` 的 GNU 私有传输仍为参考；不得跨 VkDevice 使用原始 VkImage。
2. X11 Game 最终裁切与对应输入；扩大相对鼠标、IME、授权剪贴板与实体多手柄回归。
3. Game 库与快捷面板的完整生命周期、Steam 派发/退出返回及单游戏覆盖真机验收。
4. FSR、限帧实效与手机真机；PAL4、Heaven、DX11 及严格性能对照。
5. 完整生命周期、资源增长和手机真机回归。

调试包与回退包位于固定 `workspace_temp/gamescope-port/`，各有大小和 SHA-256 收据。
当前代码包对应 `042600ebd491827293a05835b0199bf3e2c4bc51`，调试 HAP 为
633,843,538 字节，SHA-256：
`289143c1cba350f76f144a9647d7198b7b6e6bd7423ed7a29baf330fc01a9cfe`。
所有与 PAL4 调试基线共有的包成员均做 SHA-256 对照，没有成员删除；Wine 数据
ZIP 和原有 native 载荷（除 App 控制层 libentry）一致。新增七个 native 成员。
Gamescope core 经 Hvigor 去符号后，所有 ELF allocated section 与已链接 core 相同。
嵌入的 `application-build.json` 与实际本地源码收据一致，既有 dirty DXVK overlay
保留并记录，没有更改 gitlink。该检查不等同于完整设备兼容性。
未合并 main、未推送、未上架，也没有替换正式交付包。
