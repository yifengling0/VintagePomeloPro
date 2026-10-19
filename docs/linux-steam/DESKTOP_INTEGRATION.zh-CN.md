# HmosDeck：原生 Steam 内容接入 Wine 窗口

2026-10-19。实现采用原版 ARM64 Linux Steam、最小 GNU 运行时、rootful Xwayland，以及现有 OHOS Wine/FEX/Proton。没有启动 Windows Steam，也没有使用 QEMU。

## 实现结构

Linux Steam 负责登录、游戏库与下载；Windows 游戏仍交给现有 Wine/Proton/FEX/DXVK/virgl。GNU 和 OHOS 图形库不跨 libc 直接交换对象；GNU Mesa 使用 virpipe 连接现有 virgl 接收端。

`hmosdeck-steam.exe` 是自有 Win32 宿主程序，创建真实 HWND 和任务栏入口。现有 Wayland 合成器验证 Wine 与 Xwayland 的真实客户端身份和窗口代次，将 Xwayland 内容绑定到 HWND 客户区。GPU 场景、软件回退和输入命中使用相同几何；只同步窗口状态，不轮询或复制屏幕像素。

Wine 宿主不再绘制额外标题栏和外框，保留 Steam 原生客户区。私有 X11 supervisor 处理 Steam 的最小化、最大化和全屏请求，与 Wine HWND 状态双向同步。任务栏恢复重新映射原有 X11 窗口，不重新启动客户端。

Steam 原生标题区的 `_NET_WM_MOVERESIZE`，以及按住左键时的旧式 ConfigureRequest 移动请求，经桥接进入现有 Wine compositor move grab。NAPI 仅在按钮仍按住、命中当前宿主、绑定身份存活时接收请求；真正的 grab 在 Wayland 事件线程执行，松开沿用既有释放路径。

实测 ARM64 Frame 客户端的桌面标题没有发出上述两类移动请求，因此另加受限后备：仅对名称为 `Steam` 的标准桌面主窗、非最小化／最大化／全屏状态、宽度至少 800 的中央标题空白区检测左键拖动。按下不移动保持普通点击，移动达到 4 个逻辑像素后只发一次移动请求。菜单、账号、窗口按钮、临时弹窗和 `-gamepadui` 排除；`STEAMOHOS_DISABLE_CAPTION_DRAG=1` 可禁用。现有 50ms supervisor tick 只检测开始拖动，持续位移仍由 compositor 原有输入事件驱动。

## 输入

键盘、触摸和虚拟按键命中 HWND 后交给绑定的 Xwayland surface。Steam 前台使用方向、确认和返回导航；切换 Windows 游戏后恢复原有 DInput 配置。实体手柄仍需实机验证；目前不等于 GNU evdev/SDL 原生手柄直通。

滚轮原先对每个事件强制发送一个 axis_discrete，使 Xwayland 优先采用整格事件并丢失连续量。现在仅对明确绑定的外部客户端发送连续 axis，虚拟滚轮按 10 个 Wayland 单位对应一格转换，物理 AxisEvent 保留连续量。普通 Wine 客户端仍走既有离散滚轮行为。

滚动前同步真实指针位置；透明输入覆盖层使用 windowX/windowY，避免滚到旧光标所在区域。滚轮不生成鼠标按钮事件；零值、无效坐标、模态阻挡和已最小化目标被过滤。

## 当前验证与候选包

- v42 实机确认真实 Wine 窗口、最小化、任务栏恢复及移动同一原生 Steam 会话。
- v43/v44 已完成 ArkTS/native 编译和覆盖安装，不卸载或清除登录、下载及存档。
- 自动回归覆盖 20 组 production scene/input、外部窗口身份与滚轮路由、真实 Xvfb 上的主窗/临时弹窗/最小化/恢复/最大化/全屏/窗口代次，以及 ETS 状态闭环、退出和前台输入方案切换。
- 鼠标回归执行真实 ArkTS 各入口，验证连续量、当前窗口坐标、零值与不产生点击。完整模型测试也已接入新增窗口测试。
- v44 新增标题拖动桥接与所有物理滚轮入口统一单位；实机结果以本轮设备证据为准，不能用编译和 host mocks 代替。
- v45 已完成编译、签名、覆盖安装。`steam-external-92` 实机中央标题拖动产生 `CAPTION_DRAG_FALLBACK`，绑定的 Wine toplevel 5 收到 interactive move，位置从 `(60,35)` 移至约 `(173,144)`，反向拖动至约 `(86,92)`；松开结束 grab。未增加外层标题栏。
- Xvfb 回归通过真实 XTest 左键与位移，覆盖普通点击不移动、拖动只触发一次、菜单区／其他应用名称／全屏排除。物理滚轮实机日志已确认 axis 到达绑定窗口；完整页面滚动体验仍需用户复核。
- 收尾 `steam-external-95` 已重新进入原生 Steam 游戏库，恢复普通窗口后再次拖动成功，当前留在可操作的游戏库页面。期间一次冷启动的系统 NCP 回调注册失败，等待上一会话完全退出后重启恢复；没有为此修改系统进程管理代码。

## GPU 验证边界

现有 GNU Mesa GLX 探针可经 virpipe 渲染并回读正确像素；这只证明基础 GLX 路径。Steam CEF 默认候选使用 `-cef-disable-gpu` 与 `-cef-disable-gpu-compositing`，不能宣称 Chromium 页面已硬件加速。

启动脚本新增 `--cef-gpu`，仅移除禁用参数并启用有界日志，供独立对照。`cefGpuRequested=true` 与 `cefGpuEnabled=false` 明确区分请求与验证。应核对 Chromium 实际 GL/ANGLE 后端、GPU 子进程存活和错误日志，排除 SwiftShader/softpipe 自动回退。

`steam-external-88` 实机 GPU 对照中，基础 GLX 以 `virgl (Maleoon 910)` 完成 18 帧渲染及读回，legacy context 报告 `2.1 Mesa 25.0.1`；这不是设备最高 OpenGL 能力。CEF GPU 进程随后出现 `No configs were generated`、`Could not create a backing OpenGL context`、`EGL_NOT_INITIALIZED`，三次初始化失败后回退。当前不能将 CEF 页面标为硬件加速。

GNU Mesa 的当前构建是 `glx=xlib`、`gallium-drivers=softpipe,virgl`、`egl=disabled`、`gles2=disabled`；CEF 加载的是 Valve 随包 ANGLE。下一步优先打通 **CEF → ANGLE → GNU GLX/Mesa virpipe → OHOS virgl 接收端 → 设备 GPU**，复用已有接收端。先验证 core context、FBConfig 枚举及 X11 visual 匹配，再处理 ANGLE 所需 EGL/GLES 接口与上下文行为；不应只去掉 GPU 禁用参数或伪造能力位。`glx.cpp` 已增加有界配置计数和独立 3.3 core context 探针，保留原先 18 帧像素校验。

`steam-external-93` 的扩展探针正常退出，耗时 507ms：`allConfigs=3`、`visuals32=1`，但按 RGBA／double-buffer／X-renderable 筛选只有 `chosenDouble=1`、`chosen32=0`；扩展声明支持 ARB context，但 3.3 core context 创建失败。这证明当前通路不能直接按完整现代 GLX 使用，不能由此认定设备 GPU 本身不支持。配置匹配与 virgl core-capability 需要进一步定位，尚未证明哪一项是 CEF 的唯一失败原因。参考 ANGLE `DisplayGLX::generateConfigs` 会按 native visual 和颜色／深度／模板等属性过滤配置；应使用 CEF 实际版本与实际请求做下一轮最小复现。

若 GLX 路径需要修订，应仅修改 GNU 专用构建的适配补丁并回归真实 EGL 初始化，不影响 Wine Mesa。若选择 GNU EGL/GLES，则需构建可以经 swrast/virpipe 使用现有接收端的 EGL 后端，验证配置、pbuffer/window surface、共享与线程上下文；不能直接将 OHOS `libEGL` 或 Vulkan ICD 交给 GNU 动态加载器。本轮没有修改 Mesa 子模块，提交仍为 `557fa3e781ea6e78fcff3ee759c25f60c3055064`。

启用条件是 CEF 实际 renderer 指向设备 GPU、GPU 子进程持续存活、页面绘制／滚动／视频与窗口模式切换正常，且没有软件渲染回退。GPU 初始化失败时保持目前可用的软件页面路径。GNU 和 OHOS 的 Vulkan/驱动对象跨 libc 直传需要独立桥接设计，不作为当前 GLX 适配的捷径。

当前 rootful Xwayland 是整张 X11 桌面绑定一个 Wine HWND，并非 rootless 逐 X11 窗口管理。Fusion 独立宿主、完整 SteamAPI/DRM、所有 Steam 模式和实体手柄直通均不能据此视为通过。Steam Cloud 的独立验证结果见本文末尾。

GNU robust mutex 的 fork-owner 恢复探针仍含诊断性显式注册，不能视为正式运行时修复。后续应单独处理并移除该诊断绕行后复验。


## 2026-10-19：对照 Wine GL 3.1 的 GNU 前端修复

Wine 的专项报告 `docs/architecture/opengl31-gpu-srgb-surface-integration-20261009.md`
确实验证了桌面 OpenGL 3.1，不应将其误认成仅 GLES 3.1。GNU Xlib 构建与 Wine 的
DRI/EGL 前端存在差异，因此此前 GNU GLX 的 2.1 结果不是同设备的最终能力上限。

本轮 GNU 专用构建复用 Mesa `0006` socket 事务和 `0007` sRGB GPU storage 补丁，
另加 `patches/mesa/0003-xlib-fbconfig-and-compressed-formats.patch`：

- Xlib context 启用 state tracker 已实现的压缩格式回退。先前缺少 RGTC 扩展，
  即使补齐 sRGB 仍停在 GL 2.1；现在解码到可用 R/RG 纹理后继续由 GPU 采样。
  这可能增加纹理存储与上传成本，并非让设备原生支持 BC 格式。
- GLX 配置选择枚举真实 FBConfig 并按请求过滤，避免单项临时配置在不同查询中
  使用不同 depth/stencil/accum 默认值。保留匹配的原生 visual，包括 32 位 visual。
- 没有设置 GL/GLSL 版本 override；Wine Mesa 库和子模块指针保持原样。

`steam-external-100` 的平板结果：renderer `virgl (Maleoon 910)`，legacy 与 GL 3.0
请求返回 `3.1 Mesa 25.0.1`，独立 GL 3.1 core context 返回 `3.1 (Core Profile)`，
GLSL `1.40`。四种 RGTC 格式均通过真实 shader 采样及像素回读；基础 18 帧 X11
呈现校验保留。匹配的 double-buffer FBConfig 由 1 项变为 3 项，其中 1 项是
32 位 visual。GL 3.3 仍拒绝创建，缺少 depth clamp 与 dual-source blending。
这不是完整 GL CTS 认证，也不是 CEF 所有页面已经完成加速验收。

本轮同一 physical wheel sample 在 ArkUI 出现两次相同 timestamp/value UPDATE，
Xwayland 对应出现两次 scroll。`AxisEventGate` 仅拒绝相同原始事件信息，过滤
END/CANCEL；不同 timestamp 的高频滚动与反向滚动保留。v48 实机 XI2 观察器
确认每次测试滚轮只剩一次 scroll；Xwayland 合成的 button 4/5 是正常 wheel
兼容事件，不能误判为左键点击。页面滚动稳定性仍需连同最终 CEF 模式验证。

新增诊断只采集有界 pointer 和图形能力，不记录键盘、账号或像素。私有截图与
Steam 原始日志不作为公开交付。WinMyDocuments 云同步仍未验收：仅 C/Z 路径
转换不足，Steam 尚未调用它；兼容层标识和存档根目录需要独立、安全地接入。


### 2026-10-19：OpenGL 3.1 与 Cloud 优先级更新

平板会话 101 真实创建 Valve 随包 ANGLE 的 EGL pbuffer ES2 上下文，renderer 为
`ANGLE (Mesa, virgl (Maleoon 910), OpenGL 3.1 (Core Profile) Mesa 25.0.1)`，
像素读回校验通过。此结果证明桌面 GL 3.1 可以支持当前 ANGLE 路径；尚不能替代
CEF 页面自身 GPU compositing／renderer 验收。用户确认 Steam 主界面正常，但
滚轮仍有回弹，重复 UPDATE 去重仅修复了其中一部分。按用户要求先处理 Steam Cloud。

对照 Valve Proton 11 的 `toolmanifest_arm64.vdf`，HmosDeck 补上
`compatmanager_layer_name=proton`。原客户端直接使用
`steamapps/compatdata/<appid>/pfx/drive_c/users/steamuser/My Documents` 等路径，
该路径解析不会调用已有的 getnativepath。兼容元数据改映射至应用私有目录，
鸿蒙禁止应用创建符号链接（实机 EACCES），因此其中仅使用经过验证的私有记录，
在 GNU 的文件接口内解析到当前 OHOS Wine 用户目录与 ProgramData。lstat/readlink
呈现链接语义，unlink/rmdir/递归清理不跟随最终链接，不将整个 pfx 指向真实 Wine 前缀。
真实 Wine 用户目录、Windows Steam 配置和游戏文件均不需要修改。

新增 `linux_steam_cloud_paths_test.py` 验证原存档读取、下载文件原子改名、
目录描述符访问、库根与 AppID 边界、清理 compatdata 后原存档仍保留。
本机测试通过；会话 104 已准备两个应用的路径记录，但 Steam 仍未初始化 Windows Cloud
规则。对未修改的 ARM64 steamclient.so 检查发现：工具注册初始化函数 0x6eb938 使用
工具内部 ID（日志为 Registering tool %s 的字段），在 0x6eb9e0 检查其是否包含 proton；
只有匹配时才填入 /pfx/.../My Documents 等默认路径。因此工具 ID 改为 hmosdeck_proton，
并在启动前备份、迁移原生 Linux Steam 自己的旧工具选择，显示名称与实际 OHOS Wine
启动通道保持原有流程。迁移前备份原配置，备份失败时保留原文件；只处理隔离 Linux 客户端的选择，不改旧 Windows Steam。

v54 / 会话 105 的真实云下载已经通过：原版客户端记录 HTTP Success、
`Download OK for file My Games/NewPAL/0.sav`、`Download complete, result OK`，
随后 `AutoCloud done. Watching 1 files`；此会话没有新的 `Unable to resolve path`。
文件已落在现有 OHOS Wine 的 `Documents/My Games/NewPAL/0.sav`，
142616 字节，SHA-256 为 `6030cd61a8fe7cbabe9a534b08d580314990aa211dfec43a55763ad9ce9da290`。
下载前存档已留私有备份；云端与本地原内容相同，因此下载前后哈希一致。
成功依据是官方客户端真实 HTTP 下载与文件落盘，不能单凭哈希一致认定已完成上传。

本机回归还覆盖真实下载的原子改名、dirfd、旧目录别名、utime/utimes、
O_NOFOLLOW/O_EXCL、原生 libc realpath、配置迁移备份和清理不跟随用户存档目录。
ArkTS/native 编译、签名与覆盖安装完成。没有修改 Steam 或游戏二进制。
正常退出同步与修改存档后的上传仍需单独以官方日志验收；限时套件强停不作为正常退出证据。
目前仅在 Steam 启动前按已安装 appmanifest 准备目录，新下载游戏在首次云同步前
应重启 Steam；同会话安装后自动补目录尚未验证。


### Cloud 验收收尾

会话 106 重启后仍正确解析存档路径并监控到 1 个文件。退出测试中的 Alt+F4
被系统用于关闭桌面承载窗口，游戏仍运行，因此不作为正常退出证据。
会话 107 在研究时限内未进入游戏，普通 WM_CLOSE 诊断未执行；
同样不把超时终止当作正常退出。实际修改存档后的上传往返尚未验收。
会话 108 恢复普通 4 小时有界启动，已移除 launchAppId，不自动启动仙剑。
当前保留 v54 和 v52 两个可安装签名候选，不改既有暂存区或 Mesa 指针。


### 单库启动、正常退出 Cloud 与音频／手柄候选

仙剑 AppID 695630 的 session 114 已进入原版游戏主菜单，用户通过菜单正常退出。
官方 Cloud 日志包含 `Starting sync (up,AC Exit,)`、`Skipping un-modified file`、
`AutoCloud complete` 和 `Upload complete in build list`。退出前后 `0.sav` 均为
142616 字节，SHA-256 为 `6030cd61a8fe7cbabe9a534b08d580314990aa211dfec43a55763ad9ce9da290`。
此项验证正常退出同步和未修改文件的处理，没有创建／覆盖存档，也没有验收修改后的实际上传。

单库的真实安装目录仍为独立 `SteamLinuxARM64/library/steamapps`。修正兼容工具请求中的
install 与 argv[0] 为 canonical 路径，并覆盖 GNU fortified `__realpath_chk`，保留官方
缓冲区越界中止语义。原生 broker 继续校验库目录、PE、nonce 和进程身份；不放宽到
任意文件或 Windows Steam。真实跨 GNU／native broker 测试已经通过。

新增 GNU ALSA playback ioplug 连接现有 OHOS AudioBroker，使用现有共享 PCM ring 和
控制协议；CEF 清理继承 FD 时可通过每会话、同 UID 的私有 bootstrap relay 建立控制通道。
没有将 OHOS 音频对象交给 GNU libc。主机真实 libasound 播放、PCM 顺序、drain、关闭和
错误 ring 回归通过；这仍不能代替实机有声验收。

新增 SDL3 virtual joystick 桥接，使用原版客户端随包的 SDL3 公共 ABI，直接消费现有
ControllerHub 的 WHGP 状态，包含轴、按钮、hat、断连回中和震动回传。Wine 桌面宿主
与独立窗口的 Steam 前台均使用 Hub 输出，避免仍转为 keyboard_legacy。GNU exec
handoff 保留音频／手柄 endpoint 配置，并重建对应 preload；26 组真实执行链回归通过。
原版 SDL ELF 未修改，实体手柄的 Steam 设置页识别与实机声音仍待验收。

正常启动默认不再运行可见的 Xlib 18 帧交替颜色及 GLX 读回测试；Xwayland readiness、
IPC 和接收端存在性检查保留。`--graphics-self-test`／`graphicsSelfTest=1` 可显式运行
原有图形测试。未运行时 `sharedReceiverVerified` 不会被标为 true。

窗口桥接按 ICCCM 区分 `WM_CHANGE_STATE / IconicState` 与客户端主动 Unmap。
大屏界面撤回后重新选择可见主窗，清除旧窗口的全屏与拖动状态；已有最小化窗口仍保留，
原窗口 Map 恢复可取消最小化。真实 Xvfb 已覆盖撤回、替换、重新映射及保留全屏属性，
Steam 实际模式切换仍需实机复验。

手机使用工程现有 `ncp_dispatch` → `Phone_StartNativeChildProcess`，不依赖系统 NCP，
退出读取现有进程注册表的 waitpid 结果；不再等待只属于系统 NCP 的退出回调。
外置资源目录独立校验后不依赖 fork 的 dladdr 必须返回绝对库名。平板继续使用系统 NCP。
手机候选使用独立、全新的 Linux Steam 数据目录，未复制平板登录数据或 Windows Steam。


手机第二层派生只在独立、单线程的 GNU supervisor 选择 libc `_Fork`。原因是第一次
从 App fork 后仍继承 ArkUI／驱动的 pthread_atfork 回调，其中的旧 FD 编号可能已被
Xwayland listener/displayfd 复用。普通 fork 再执行回调会关闭新通道。策略显式随宿主
参数传入，检查 `/proc/self/task` 只有一个线程并确认 libc 提供 `_Fork`；不满足即返回
`fork_policy / ENOTSUP`，不跳过检查或修改主应用、Wine 的 fork 行为。
真实主机回归通过人为注册会关闭指定 FD 的 atfork 回调，验证默认 fork 保持原行为，
专用 `_Fork` 保留传递 FD，父进程的描述符、TLS 和回收结果不变。


### 手机首次启动：执行内存、XKB 与 CEF 共享内存

手机 VYG-AL00 的外置 GNU 链已通过现有手机 fork 后端启动原版 ARM64 Linux Steam，
没有改为 QEMU 或 Windows Steam。首次启动暴露了平板既有私有数据掩盖的三个问题：

- 外置 ELF 的匿名 RW→RX 转换被系统执行内存策略拒绝（EINVAL）。独立 GNU supervisor
  及其外置 loader/glibc 路径参照现有 Wine，在执行权限转换前请求 OHOS 的线程执行内存
  策略（prctl 0x6a6974），随后去掉写权限再执行；不创建 RWX 页、不修改 ArkUI 主进程。
  实机先前拒绝的探针在请求策略后通过。小块探针成功不能替代后续 ELF 范围检查，
  实际 mprotect 失败仍返回地址、长度、保护位和 errno 诊断。
- Xwayland 编译时默认 XKB 路径在新手机上不存在，XKB_CONFIG_ROOT 不会覆盖该编译常量。
  启动时使用标准 -xkbdir 参数指向已校验的外置 XKB 数据，不依赖旧安装残留。
- CEF 直接通过 mkostemp/open 访问 /dev/shm，并不总是调用 shm_open。兼容层把目录本身
  和平面共享内存文件名映射到会话私有 posix-shm，使直接文件访问和 POSIX shm 使用
  同一 inode 命名空间。保留私有目录属主／权限检查及 O_NOFOLLOW；不映射路径穿越
  或其他 /dev 设备路径。真实回归覆盖 mkostemp+CLOEXEC、两种 API 互访、fork 共享映射
  及 unlink 后仍可使用原映射。

phone-011 的 45 秒诊断中，新的 CEF 日志不再出现此前 /dev/shm 权限 FATAL，
网页进程创建窗口并存活至诊断超时。该次结束为测试器 SIGKILL，不能记作客户端自然崩溃。
随后 phone-012 已改回正常 4 小时有界会话，用户确认看到画面，正在手机上独立登录。
用户随后确认实体键盘当时未连接，已用其他输入方式进入登录；本轮没有因此修改键盘路由。

手机首次登录时部署为 v66 签名候选加后续 GNU-only 共享内存修复；HAP 不包含外置 GNU 内容。
后续候选及两者版本、SHA-256 以 phone-deployment-pair.json 配对部署记录为准。
后续普通窗口主界面已由用户确认；声音、实体手柄识别及大屏退出仍需实机验收，不由编译或主机回归代替。


### 手机普通窗口登录与后续诊断

phone-012 以 -gamepadui 启动后用户反馈持续停在登录加载。按用户要求，phone-013
移除 -gamepadui，保留相同的 v66 HAP、外置 GNU release、CEF GPU 请求与 Wine HWND
承载方式，改为普通桌面窗口。用户确认已进入 Steam 主界面／游戏库。手机原生启动
与登录链路已走通，但两轮之间发生了进程重启并复用了登录状态，因此不能仅据此
断言大屏界面是唯一根因；大屏初始化和退出后的再次启动仍待单独对照。

随后按用户要求再次以相同普通窗口配置启动 phone-014，供用户下载游戏并测试启动。
没有安装新包、清理账号数据或更换外置运行时。默认诊断启动工具也改为普通桌面 UI，
大屏仅通过显式 --big-picture 选择，避免普通重启重新切回待查的大屏路径。

源码新增 supervisor 侧每 5 秒更新的 client-live.private.json，包含有界的连接、
CEF、网页助手、控制台和 Cloud 日志尾部，结束时补一次快照。文件留在应用私有目录，
原子替换、权限 0600、拒绝临时文件符号链接，总大小限制 1 MiB；原始日志可能含账号，
不得公开输出。回归覆盖静默子进程的周期采样、超时与正常回收、父线程 TLS 不变、
日志头尾边界、超限保留旧快照以及符号链接不跟随。
该项已随 v67 部署至 phone-015，并已实机取得实时快照；phone-014 的旧 v66 没有该能力。


### 手机文字重绘与游戏启动工具选择

同一 v67 HAP、同一外置 GNU release 下，phone-015 请求 CEF GPU 时，用户反馈文字先空白、
稍后或交互后出现；新网页助手日志记录 12 次 SharedImageManager::ProduceSkia mailbox 不存在。
phone-016 仅关闭 CEF GPU，用户确认文字恢复正常，新日志中对应错误为 0。手机诊断启动助手
默认改为普通窗口、软件 CEF；只有显式 --cef-gpu 才请求 GPU。此调整仅针对 Steam 网页界面，
未关闭游戏的 Wine／DXVK 图形加速。CEF GPU 路径仍待独立修复，不能称为硬件合成验收通过。
平板会话 114/115 当时确实请求过 CEF GPU，不能把手机现象解释为平板一直使用软件 CEF。

手机仙剑 AppID 695630 已完整下载，但新账号配置选择官方 Proton Experimental／Steam Linux
Runtime，外置下载目录不支持其中的 symlink 创建，官方日志返回 Disk write failure。该阶段
没有游戏 broker 请求。v68 在启动原版 Linux 客户端前，通过 VDF 解析将默认兼容工具设置为
hmosdeck_proton；已安装游戏显式选择内置 proton_* 时迁移到 OHOS 工具，其他自定义工具保留。
修改前完整原文备份，备份失败不写配置，重复启动幂等；不修改 Windows Steam 配置。

phone-017 已经过官方 Linux Steam、reaper、hmosdeck 和认证 broker 进入现有 OHOS Wine。
但 NewPAL_Release.exe 在约 2 秒后主动以退出码 1 结束，尚未验收手机游戏运行或 Steam API
认证。未出现游戏错误弹窗。缺少 SDK 分离目录内 d3ddriverquery64.exe 是另一条辅助查询错误，
不能将其当作仙剑 PE 丢失，也不为此放宽 broker 的库目录、PE 或进程身份校验。

新增研究专用的 game-launch-diagnostics.conf，内容恰为 wineLoader=1 时，仅游戏启动附加
DLL／异常诊断；不接收任意环境覆盖。默认关闭。启动结果补充真实 exitCode/exitCodeSource，
非零退出给出提示，未知状态保留未知，不推断认证成功。真实退出和未知状态的主机回归已通过。
