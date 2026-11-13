# 本地游戏全屏／标题栏问题与焦点修复候选

日期：2026-10-05。基线：feature/main_proton 的 0a877ea3724cf02e248d90c413aac92afbb38fd0，Wine pin cd547f7a0ee9d3d59b3ec47317a7852dedf0443b。对象为平板旧柚 Pro 内用户手动运行的 War3，以及本地 Kingdom Rush Genesis。本文按时间保留各轮候选结论；最终发布范围与待验收项见 [dot 交接说明](dot-performance-handoff-20261005.md)，下文“未提交／未解决”仅描述相应历史阶段。

## 结论和验收边界

确认了宿主键盘焦点的队列顺序缺陷，并完成生产输入路径回归和 OHOS native 构建。候选用于验证 War3 自动最小化是否与该缺陷相关，尚不能宣称游戏已修好。此前 ARM64EC SEH thunk 修复已进入安装包，但用户仍报告 Kingdom 闪退；这次焦点修复不等于解决了该崩溃。

用户确认 War3 没有手动最小化，而是自己变成标题栏。采集期间仅作只读诊断；游戏继续由用户手动启动。本轮不增加 Steam 启动或 DLL 覆盖规则。

## 现场证据

2026-10-04 宿主 PID 10423、war3.exe PID 13389、toplevel 12：

| 时间 | 观察 | 可支持的判断 |
| --- | --- | --- |
| 23:47:47.014 | 全屏 configure 1200×800 | 游戏申请过全屏 |
| 23:47:49.542–49.842 | GL 子面 45 挂在父面 0x5a60d3ec00 下，800×600 SHM 提交／存层成功 | 有图形提交，不能仅用“GL 不支持”解释 |
| 23:47:55.534–55.536 | 点击命中子面 45，随后 keyboard.leave / keyboard.enter | 焦点切换发生在缩小之前；日志未记录 keyboard.leave 的目标 surface |
| 23:47:56.555 | window_geometry=(-32000,-32000 160×31) | Wine 侧已进入典型最小化几何 |
| 23:47:56.590 后 | unset_fullscreen，后续 GL 缓冲变为 1×1 | 小缓冲是客户区缩小后的结果，不应强行放大当作修复 |

证据位于本机 F:\VintagePomelo-Workspace\workspace_temp\crt-bridge-fix-20261004\：fullscreen-first-seconds.txt、war3-window-events.txt、war3-commit-route.txt、war3-frames.txt、fullscreen-current.png。设备标识已掩码。

23:38 的独立 Kingdom 运行中，日志记录 creatorHostPid=6868 创建 steam.exe PID 6914，随后游戏 6868 SIGSEGV；因此那一次 Steam 是由游戏进程拉起。另一次特殊启动器创建 Kingdom PID 11760，其后仍有 SIGSEGV。FEX 的 CallRetStacks guard faults 可由 FEX 正常处理，目前没有足以定位最终崩溃的展开栈。未证实这些 guard faults 是致命原因，也未证实 Kingdom 的画面问题与 War3 有相同根因。

## 修复的代码缺陷

原点击／按键／模态框路径先入队 KBD_LEAVE，再在 NAPI 线程调用 SetKeyboardFocus(new)，最后入队 KBD_ENTER(new)。dispatch 的 InjectKeyboardLeave 从同一 tracker 读取当前 surface，因此可能对新窗口发送 leave，而旧窗口没有收到正确的离开通知。这一顺序可用生产 InputManager 的点击入口复现。

现在 NAPI 仅提交焦点请求，由 Wayland dispatch 线程完成：

1. 验证新目标仍存活，且来自所映射窗口的真实 surface 父链；无归属、断链、循环、销毁资源和无对应 keyboard client 均拒绝，保留有效旧焦点。
2. 同一 owner 的重复请求不发 leave/enter；切换时先向此前实际 enter 的 owner 发 leave，再向新 owner 发 enter，最后记录已发送焦点。
3. 每次点击都保留焦点请求，以正确处理一次 flush 前的 A→B→A；键盘焦点请求排在鼠标按钮按下之前。
4. 指针继续使用命中的子面和原坐标；键盘／IME 使用窗口 owner。PC popup 的直接映射仍可使用它自身的 surface。
5. 键盘发送前验证已聚焦资源存活；keyboard surface 改用 atomic 快照，生命周期 reset 保留。

修改涉及 input_manager.cpp、input_injector.cpp、input_resolver.*、input_state_tracker.*。窗口层序、图形分辨率和 GL readback 没有作为本轮修复手段。

## 验证

命令 python3 host_tests/gpu_scene_input_test.py 的 8 个场景通过，连接生产 compositor、resolver、InputManager、队列和 injector。mock 仅限 Wayland 协议发送、设备 seat、坐标映射和窗口移动／raise 等平台边界：

- 原有 GPU-only 父窗、SHM 菜单、模态框、显示／输入一致性、绑定代次。
- 真实父链解析、嵌套子面、同进程其他窗口、断链／循环／资源登记失配、popup。
- keyboard／IME 目标、旧 owner leave、新 owner enter、重复焦点、键盘 client 隔离、销毁焦点。
- 生产点击入口在 flush 前保持旧焦点、先切焦点再发 button、快速往返、session reset。
- 桌面实际菜单命中仍指向子面，keyboard/IME 指向游戏 owner；桌面按键不抢回 root。

命令 python3 host_tests/gpu_scene_input_test.py --focus-baseline 使用固定基线 0a877ea3 的生产 InputManager／injector，确认旧代码在“flush 前仍应保留旧焦点”的断言失败。输入状态测试 84 项通过。相关 GPU owner、绑定生命周期、Steam GPU 合约和 Wine 构建身份回归通过。没有运行或声称全部测试通过。

OHOS arm64 native entry 构建通过。Wine 复用前按原 GUEST_ARCH=aarch64 检查独立 BUILD_DIR 的完整身份成功；默认 x86_64 配置被 guard 正确拒绝后，恢复记录中的配置，没有跳过检查、覆盖身份或重置缓存。

## 真机复测

安装候选后，由用户手动打开旧柚 Pro，并从原入口独立运行 War3。先等待菜单，再点击内容区、连续操作，观察是否仍自动缩成标题栏。若能够稳定显示，再验证退出／再次进入全屏及切换其他窗口后恢复。Kingdom 使用用户指定的特殊 EXE 单独运行，分别记录“有声音且仍存活但无画面”和“进程 SIGSEGV”。

需对齐新日志 [Input] KbdFocus oldTl=... newTl=... owner=...、窗口几何、fullscreen 状态和图形提交。若仍自动缩小，应继续捕获 Wine 的 keyboard HWND、WM_ACTIVATE／最小化调用链，而非把此候选写成最终根因。

产物和安装记录保存在 workspace_temp/focus-fix-20261005/，包内身份另见 package-identity.json。HAP、设备日志和临时签名脚本不提交；后续提交仅包含源码、回归和本报告。

## 候选包和安装记录

本机交付：F:\VintagePomelo-Workspace\workspace_temp\focus-fix-20261005\VintagePomeloPro-proton26-focus-fix-debug-signed.hap。

- 版本仍为 1.4.5-proton.26-alpha / 1004035，不能仅凭版本号区别前包。
- signed HAP：358356074 字节，SHA256 6aeab5c7635d91190f10e8d81105dbf53fff449a358ed8213f70774350911ebd。
- unsigned HAP：356543064 字节，SHA256 5824b08ab6b99a2ef15826acb5e03d735776911a42d4e2b97ade5e53fee0ec6f。
- 包内 libentry.so：7400e832c58a8705491f003972c1f9819add001d740f4948003add3c9f86c57c，等于本轮 stripped native 产物，包含新增焦点诊断。
- Wine payload：83b17771d9d71237c05e99afe8ff59870dedf96664cccef0e088eb0c75b48004；与前一个 SEH 候选相同。已核对 CRT、FEX 和其余 Wine native 库同前包，隔离本轮宿主焦点改动。

HAP 编译、官方 verify-app 签名核验、runtime library guard 和 runtime component 检查通过。首次签名使用的旧挂载 material 与加密配置不匹配，改用此前本地 debug-sign 材料后签名／核验成功；临时签名源已清理，没有修改原材料。

安装前确认目标包未运行，仅执行 hdc install -r 覆盖升级。HDC 明确返回 install bundle successfully，bm dump 确认目标版本，安装后仍无目标应用进程。没有发送应用／Steam／游戏启动命令，没有停止用户正在运行的其他应用。设备安装库哈希没有直接读取，以本地核验包哈希和安装确认记录为边界。

## 2026-10-05 09:04 复测：仍未解决

用户手动运行后仍自动缩成标题栏／画面不显示，后续确认 War3 未点击就缩小；文件管理器的最小化是用户手动操作，不能作为另一个自动最小化样本。

本轮宿主 PID 44785、war3.exe PID 45851，游戏主窗口 toplevel 7。新日志可确认修复后的 keyboard.enter 使用主窗口 owner：

| 时间 | 新日志 |
| --- | --- |
| 09:03:44.345 | toplevel 7 进入全屏，configure 1200×800 |
| 09:03:46.808 | GL 子面 45 挂在 owner 0x5a62113500 下 |
| 09:03:46.909 | Frozen Throne.exe 启动器 PID 45837 SIGSEGV；war3.exe 仍存活 |
| 09:03:47.086–47.087 | 短暂创建同进程 toplevel 8、全屏后立即销毁；不可当作持续遮挡窗口 |
| 09:03:57.802 | KbdFocus oldTl=4 newTl=7 owner=0x5a62113500；enter 成功 |
| 09:03:58.795 | Wine 提交 (-32000,-32000 160×31) |
| 09:03:58.831 后 | unset_fullscreen；GL 子面随后提交 1×1 |

日志在 09:03:57.800–58.178 也记录了三组鼠标 press/release，用户说明最初未点击就已缩小。两者需用下一轮“启动后不输入”的有界采集对齐，不能仅凭现有相邻时间认定点击或 keyboard.enter 触发最小化。09:04:04.934 的文件管理器点击位于 (572,14.5)，随后最小化，与用户确认的手动操作一致。

当前结论是宿主焦点队列缺陷已通过生产回归复现并修复，但不是已证实的 War3 最终根因，游戏真机验收失败。当前截图 war3-retest-current.png 仍是桌面左上角一条标题栏。

另一个独立适配缺口：当前 Proton winewayland.drv/window.c 不发送 xdg_toplevel.set_minimized，也没有宿主注释所指的 restoring_from_minimize 握手。宿主因此没有进入自己的 minimized 状态，仍绘制 Wine 的 160×31 哨兵几何，并在窗口尚为 fullscreen 时重发 configure。这个缺口可解释最小化后的显示／还原问题；尚不能解释是谁首先请求 War3 最小化。修复前应追踪 Win32 ShowWindow、WM_ACTIVATE／WM_ACTIVATEAPP、foreground 变化及 Wayland configure。

已准备单次窗口跟踪脚本 vp-war3-window-trace-20261005.bat，通过 bundle-aware HDC 传入目标应用 C: 根目录并回读核对完整字节。脚本由用户手动执行，保持原 Frozen Throne.exe 启动器，使用 setlocal 限定 WINEDEBUG／WINEHUA_WINEDEBUG，不更改持久环境、图形参数或 DLL；仅启用 win、msg、waylanddrv 及时间／进程标签。助手没有执行脚本或启动任何程序。诊断阶段不能据此评价帧率。

新增现场证据仍只在本机 workspace_temp/focus-fix-20261005/：retest-hilog.txt、retest-wine-stderr.txt、current-state.txt、war3-paths.txt、war3-retest-current.png、device-commands.json。原始日志、HAP、诊断脚本和签名材料不提交。

## 09:18 诊断启动：白块、停帧及日志桥接缺口

用户执行上述 BAT 后报告左上角一小块白屏、不再继续；旧 main 曾先出现画面、短暂消失、随后全屏。宿主 PID 49911、War3 PID 50825、主窗口 toplevel 8：09:18:49.051 申请全屏 1200×800，09:18:51.417 提交同尺寸几何，09:18:51.583／51.793 只有两次 800×600 存层，随后无新游戏帧。截图 war3-trace-current.png 与用户描述一致。Frozen Throne 启动器 PID 50810 于 09:18:51.507 SIGSEGV，War3 仍存活。这轮尚未记录先前的 160×31 最小化，不能把白块停帧直接等同于最小化。

诊断参数实际没有生效：两个 WineChild 的 final WINEDEBUG 均为 -all。生产 process.c 已从 Windows 子进程环境提取 WINEDEBUG，但 OHOS broker 原调用只转发父进程 Unix environ。BAT 中设置的参数因此丢失。

本轮新增修改：

- 0020 中的实际 Broker sender 接收 Windows 子进程 WINEDEBUG，仅当选择不同于父 Unix 环境时，替换转发的 WINEHUA_WINEDEBUG；不修改父环境，不引入任意 Windows 环境。0028 只修改 process.c 调用，避免跨补丁重复替换 sender 后破坏幂等重放。
- 0029 让实际 WS_MINIMIZE 通知宿主 set_minimized，保留全屏状态。挂起的尺寸 configure 只 ack，不改变最小化哨兵位置；已完成初始配置的窗口收到宿主 0×0 还原通知时，先 ack、释放窗口锁，再发 SC_RESTORE。role 清除重置请求状态。不强制禁止游戏最小化。

broker_startup_contract_test.py 已通过真实 patched C sender、Unix socket／SCM_RIGHTS、生产 Broker framing，新增不同／空／非法诊断选择、替换去重、父环境不变等用例。wayland_minimize_restore_test.py 通过 12 项，重放全部注册 overlay 两次后编译实际 Wine 函数和结构，验证一次最小化、挂起 configure、锁外 SC_RESTORE 重入、正常配置、初始 0×0 不误还原及 role reset；平台调用仅在边界 mock。新增窗口回归已注册 make test。

这些验证只证明两个代码缺口及修复行为，不证明 War3 停帧或 Kingdom 崩溃已经解决。新的独立 BUILD_DIR 为 build-window-state-0a877ea3-20261005，不复用 Wine configure／对象／身份缓存；依赖及 FEX 产物沿用已验证构建。09:46 补读线程列表确认 War3、wined3d_cs、音频等线程仍存在，但 wchan 全部为 0，不能据此推出具体等待点或死锁。

## 新窗口状态候选：构建及安装边界

09:51 完成 fresh Wine、构建身份 guard、assemble、HAP 编译、runtime library guard 和 runtime component 检查。官方 verify-app 返回成功并生成证书链及 profile。签名使用独立临时 profile，没有替换源码中的配置；临时签名材料已清理。

产物位于 F:\VintagePomelo-Workspace\workspace_temp\window-state-20261005\：

- VintagePomeloPro-proton26-window-state-debug-signed.hap：358359377 字节，SHA256 b6edfb8953199a359e1ec88b4599afcd21f9de30a163e61b1ce04e3c32a65fe6。
- VintagePomeloPro-proton26-window-state-unsigned.hap：356549217 字节，SHA256 007393cad934908a496ce4f9835ac96920d6c0aaed8b13adf7a38088d4b9579b。
- 版本仍为 1.4.5-proton.26-alpha / 1004035，测试时以包哈希和 package-identity.json 区分候选。
- Wine payload SHA256 6d6ef4b649da54d6210b4a08a6517d48d65f0f25b2f4aa49aed44e2bac52e056；ntdll.so 和 winewayland.so 已更新。libentry.so 与上一焦点候选一致。

包内 ELF 已核对 ARM64 架构及本轮 stripped native 产物，assemble 的 ntdll／win32u／winewayland 与 fresh Wine 源产物逐字节相等；签名后的 native 和 Wine payload 与未签名输入一致。FEX 两颗 DLL 哈希与上一 CRT 候选一致，真实 zlib 保留。重新构建的 CRT 经 PE 解析验证仍包含正确 ARM64EC SEH fallback，且包内 CRT 与本轮 strip 输出一致。

09:51:50 检查发现旧柚 Pro 宿主 PID 49911 和 Wine 子进程仍运行，因此尚未覆盖安装。已向用户提出手动结束当前会话的协调请求；没有停止会话，没有发送任何 App／Steam／游戏启动命令。游戏真机结果仍待验证，设备安装库哈希未直接读取。

下一轮由用户手动启动旧柚 Pro 并执行 C:\vp-war3-window-trace-20261005.bat，先不输入约 10 秒。先检查两个 WineChild 的 final WINEDEBUG 是否包含诊断通道，再对齐 WM_ACTIVATE／ShowWindow、minimize、configure 和图形提交；如果白块持续且没有最小化，应追踪停帧之前的游戏调用，不能仅靠还原握手认定根因。

用户随后确认已关闭会话。安装前再次确认目标宿主和所有目标 Native 子进程均不存在，执行覆盖升级后 HDC 返回 install bundle successfully；bm dump 核对 com.vintage.pomelopro 的版本 1.4.5-proton.26-alpha / 1004035，安装后目标包仍未运行。安装包 SHA256 为上述 b6edfb89…a65fe6，完整安装审计见本机 window-state-20261005/device-commands.json。状态更新为“新候选已升级安装，等待用户手动启动和验证”，不是游戏验收成功。

## 10:11 新包复测：抓到失活后主动最小化

用户反馈“启动画面显示后消失”，随后确认尝试恢复后未恢复全屏、进程仍在后台。只读采集显示宿主 PID 60595、War3 PID 9027、Frozen Throne 启动器 PID 8996、explorer 文件管理器 PID 7661。主游戏窗口 HWND 0x501a4／toplevel 8；临时 BlockingWindow HWND 0x201bc／toplevel 9。

诊断桥接已在真机生效：BAT 子进程、Frozen Throne 和 War3 的 final WINEDEBUG 都为 -all,+timestamp,+pid,+tid,+win,+msg,+waylanddrv。此次能够读取实际 Win32 窗口行为。

| 时序 | 直接证据 |
| --- | --- |
| 10:11:28.880／29.317 | 主窗口创建并申请全屏 1200×800 |
| 10:11:31.862／32.050 | 主窗口两次收到 800×600 图形存层 |
| Wine 160547.366 | 游戏创建无 owner 的 BlockingWindow，800×600，WS_POPUP／WS_VISIBLE／WS_MAXIMIZE |
| Wine 160547.388 | set_foreground_window(0x201bc)，previous=0x501a4，同线程内切换到临时窗口 |
| Wine 160547.425 | 游戏调用 user_destroy_window(0x201bc) |
| Wine 160547.439 | activate_other_window 选中 0x200c6，set_foreground_window 成功，new_active_thread_id=0144 |
| Wine 160547.448 | 主游戏收到 WM_ACTIVATEAPP(FALSE)，lParam=0144 |
| Wine 160547.505 | 游戏主线程执行 show_window(0x501a4, cmd=6)，即 SW_MINIMIZE |
| Wine 160547.507 | WS_MINIMIZE 生效，客户区为 0×0，窗口位置变为 -32000 哨兵 |
| 10:11:32.184 | 宿主收到 tl_set_minimized(8)，发出 minimized 事件，保留 fullscreen 状态并隐藏窗口 |

线程 0144 可通过同一 stderr 中的 PID=7661 启动段对应到 explorer.so，确认激活切到了文件管理器进程。暂未直接读取 0x200c6 的 class／title，不把它进一步猜成任务栏或桌面窗口。

这次有直接证据证明“应用失活 → 游戏主动最小化 → 宿主隐藏”的路径。新握手没有消除最初的失活：它使已经最小化的游戏不再留下 160×31 标题栏。因此此次画面消失不能直接归为新的 GPU 渲染失败。War3 在采集时仍存活；Frozen Throne 启动器在 10:11:31.828 的 SIGSEGV 与此前相同，不能误报为主游戏退出。

宿主整个游戏启动段没有向 toplevel 8／9 发送 keyboard.enter，已发送焦点仍在文件管理器。没有证据把这次失活归因于点击触发的队列错误。Wine 通用 activate_other_window 在销毁 active popup 时按 owner／Z 序寻找可激活窗口；本次 popup 没有 owner，选择了不同进程窗口。旧参考树也有同一通用选择逻辑，尚不能凭此认定应全局改成优先同进程。后续需核查游戏主窗在选择时的实际 Z 序、可激活状态，以及合成器前台与 Win32 前台之间的同步。

用户尝试“切换至”后补采至 10:16，主游戏仍处于后台。当前日志未出现主 HWND 的 SW_RESTORE／WM_WINE_SHOWWINDOW／SC_RESTORE，也没有宿主 restored／0×0 还原 configure；这轮因此没有直接验证 0029 的还原分支。只能报告未观察到还原请求到达，不能据此把原因归为还原尺寸算法或判定用户未操作。

此轮没有更改运行参数，没有启动／停止应用或游戏，也没有安装新包。证据在本机 focus-fix-20261005/：window-state-retest-current.txt、window-state-retest-hilog.txt、window-state-retest-stderr.txt、window-state-retest-screen.png、window-state-restore-hilog.txt、window-state-restore-stderr.txt。下一个修复应针对临时窗口退出后的前台选择与有效还原入口，避免强制放大 1×1 图形缓冲、禁止所有最小化或无条件抢焦点。

## main 与 Proton 窗口恢复链路对比（2026-10-05）

本轮只比较源码、已有设备日志和生产函数回放，没有修改运行代码、重新构建、安装或发送应用／Steam／游戏启动与停止命令。

### 比较基准

- 宿主 main：origin/main = 5383b7849190c563c0e79007fb9c33da64b326e4。
- main 固定的 thirdparty/wine：1cb1ac93e464fe6db019845f3451f50ef6fc78f4，VERSION 为 Wine 11.10。
- 当前 feature/main_proton：HEAD 0a877ea3724cf02e248d90c413aac92afbb38fd0，Valve Wine pin cd547f7a0ee9d3d59b3ec47317a7852dedf0443b，VERSION 为 Wine 11.0；比较包含当前已安装候选对应的有效 overlay，包括 0029。

最初本地 legacy Wine 工作树为 f6492f848273ae3f4220151c8049e1202669cd49，不能直接当作 main pin。本轮已从 winehua/wine 的 fork remote 获取 1cb1ac93 对象，并用 git show 读取实际固定源码；以下关键结论基于该对象。用户曾成功使用的 main 安装包仍没有精确包哈希／构建身份，不能把源码对比当作同设备、同游戏、同运行配置的严格 A/B。

### 确认的差别与相同部分

| 路径 | main 固定源码 | 当前有效候选 | 判断 |
| --- | --- | --- | --- |
| WS_MINIMIZE 通知宿主 | get_config 记录 minimized，发送 xdg_toplevel.set_minimized，保留 fullscreen | 0029 已补齐，并增加一次请求与 role reset | 原 Proton 迁移确有缺口；当前真机已收到 tl_set_minimized，不再缺这一段 |
| configure 触发 Win32 还原 | minimized 且窗口在 -32000 哨兵位置时，收到 configure 就 ack，然后锁外发送 SC_RESTORE；不限定尺寸 | minimized、已经完成初始 configure、已发 minimize_requested，且新 configure 为 0×0，才发 SC_RESTORE；带尺寸事件仅 ack | 当前条件更严格；这是可复现的恢复行为差别 |
| 真实 WS_MINIMIZE 后的 fullscreen 保留 | 不 unset_fullscreen | 0029 保留 fullscreen | 两边都有相应处理 |
| 临时窗口销毁后的 Win32 前台候选选择 | can_activate_window／activate_other_window | 函数与 main 字节相同 | 不能宣称这是新分支修改过的通用算法，也不能直接改为无条件优先同进程 |
| keyboard.enter／leave | enter 对 managed HWND 发 WM_WAYLAND_SET_FOREGROUND；leave 不同步 Win32 foreground | 两个函数与 main 相同 | 共存的前台同步缺口；不能仅据此解释分支差异 |
| 宿主首帧自动焦点 | TryBeginSessionFirstFrame 仅会话首次生效 | 函数与 main 相同 | 新游戏映射不会因此自动得到一次 enter；这是实际场景需核查的同步路径 |
| WineWindowManager、DesktopLayer、WineWindowAbility、DesktopAbility | main 文件 | 四个文件相对 main 无差异 | 本次差异不来自这四个 ArkTS 文件 |
| 宿主自动恢复帧与全屏尺寸漂移判定 | TryAutoRestoreLocked／HandleCommittedSizeLocked | 两个函数与 main 相同 | 正常尺寸恢复帧可以解除宿主最小化；但不能替代让停在后台的游戏先执行 Win32 还原 |

SetToplevelRestored 两版均发送 0×0 configure，并保留 FULLSCREEN。NotifyToplevelResize 两版均在 minimized 时拒绝普通 resize configure。当前 NAPI setToplevelVisible(true) 比 main 多调用 SetToplevelRestored，但此轮是桌面合成模式、游戏 hasWindow=no；单个 OHOS 子窗口的可见性回调不构成已验证的游戏恢复入口。

### 生产函数对照回放

在本机 workspace_temp/main-window-compare-20261005/compare_restore.py 中，抽取两版实际 wayland_win_data_get_config、wayland_surface_update_state_toplevel、wayland_configure_window 与真实结构，使用已有 host_tests 的协议／Win32 边界 stub。gcc 编译启用 ASan、UBSan；两版各四场景共八次运行通过，无 sanitizer 报告。

| 输入场景 | main 发出 SC_RESTORE 次数 | 当前候选发出 SC_RESTORE 次数 |
| --- | --- | --- |
| 已完成初始配置、最小化后收到 1200×800 configure | 1 | 0 |
| 已完成初始配置、最小化后收到 0×0 configure | 1 | 1 |
| 初始配置尚未完成、已最小化时收到 0×0 configure | 1 | 0 |
| 正常非最小化窗口收到 configure | 0 | 0 |

回放证明的是实际函数的请求差别；send_message 边界在回放中模拟了还原及重入，没有运行真实 War3 或完整 Win32 消息系统，不能据此宣布游戏已恢复全屏。旧版“画面消失后又全屏”的经历与宽松 configure 恢复路径相符，但尚无旧包现场日志证明其确实依赖迟到的非零 configure。

### 与真机日志对应

此前 10:11 现场已证实：主游戏 → BlockingWindow → BlockingWindow 被销毁 → activate_other_window 选 explorer 所在线程 0144 → 主游戏收到 WM_ACTIVATEAPP(FALSE) → 游戏自己 ShowWindow(SW_MINIMIZE)。War3 主进程继续存活。

主游戏 toplevel 8 在 10:11:29.317 收到 fullscreen configure；临时 toplevel 9 在 10:11:32.050 收到 fullscreen configure；10:11:32.184 宿主收到主游戏的 minimized。不能把临时窗口 9 的 configure 错当成主窗口 8 的还原消息。主窗口最小化后，补采未观察到 SC_RESTORE／SW_RESTORE、有效 0×0 restore configure 或新 keyboard.enter。160552.531 的 wayland_configure_window 日志是“requested configure event already handled”，并非已处理有效的还原请求。

因此需区分两段：一段是临时窗口关闭引发前台转移和游戏自最小化；另一段是后续恢复没有形成闭环。此次真机证据不足以证明“只接收 0×0”是唯一阻断，直接恢复 main 的“任何 configure 都还原”也不会凭空产生一个还原请求。

### 修改方向和约束

1. 先把明确的“切回游戏／激活已最小化窗口”接入桌面合成模式的恢复链：有效目标与代次确认 → Win32 完整还原（清 WS_MINIMIZE、恢复 placement、发送相应消息）→ 宿主清 minimized、保留 fullscreen → 同步 Win32 前台及 Wayland seat 焦点。还原与焦点应按目标窗口进行，不能只显示宿主缓存帧或修改客户区尺寸。两侧分别发起的恢复必须防重入，不能循环发送 configure。
2. 核查临时 BlockingWindow 关闭时的真实 Win32 Z 序和最近激活窗口，以及游戏窗口映射／raise 与 compositor seat 的同步。若采用“返回此前窗口”，必须基于明确激活历史或 owner 关系，验证目标仍有效、可激活，且期间没有新的用户切窗；无 owner 的同进程其他窗口不能直接当作父窗。原通用 activate_other_window 函数相同，缺少精确 Z 序证据时不应全局改变它。
3. 保持最小化期间普通 resize 不触发还原。只有证实是一次明确的激活／还原请求，才考虑允许有尺寸的 configure 完成还原；当前 Wine configure 状态解析没有记录 ACTIVATED，宿主普通 resize 也总带 ACTIVATED，不能直接拿该 bit 当作来源判据。需要先建立可识别的激活路径与时序。
4. 回归需要覆盖临时无 owner 窗口销毁、用户主动切到别的应用、正常手动最小化不被旧 resize 拉回、显式恢复保持全屏、GPU-only／SHM 窗口、窗口销毁及代次失效。之后再由用户手动启动做真机确认。

建议优先级为“真实激活／还原入口闭环”和“临时窗口退出时的前台同步”，然后再决定是否扩展 0029 的 configure 兼容性。本轮没有追加一个仅放宽 configure 条件的猜测包。

## 激活返回修复候选（2026-10-05）

在前述源码对比基础上新增 0030-ohos-transient-activation-return.patch。远端 feature/main_proton 再次核对仍为 0a877ea3，没有漏合并新的线上提交。本轮保留既有字体／zlib、FEX、ARM64EC SEH 和 GPU／窗口握手改动。

修复分两段：

1. OHOS Win32 侧在成功的程序前台切换中，只有此前窗口确为全屏、可激活，且新的无 owner popup 与它同一线程时，记录此前完整 HWND。该记录是实际激活历史，不改变 owner 关系，也不按进程猜父窗。popup 退出时，仅当它仍为前台、记录目标仍存活、同线程且可激活，才在普通 Z 序查找之前返回该目标；已有有效 owner 仍优先。用户鼠标／宿主内部切换及其他前台转移取消记录，失效或重用的 HWND 不接受。嵌套 popup 可以逐层返回。记录随窗口属性生命周期释放，没有全局禁止最小化。
2. Wayland 的 WM_WAYLAND_SET_FOREGROUND 是明确的激活请求。目标若最小化，先调用 NtUserShowWindow(SW_RESTORE) 走完整 Win32 还原，再检查目标仍可见、未禁用、已退出最小化，最后设置前台。全过程不持有 win_data 锁，允许 ShowWindow 同步重入或销毁窗口；拒绝还原时不继续激活。普通有尺寸 configure 的处理保持 0029 的最小化门禁。

显示模式切换可能使当前 monitor 尺寸与此前全屏窗口不同，因此全屏资格在记录时确认；返回时重新验证身份和可激活性，不再用变化后的 monitor 尺寸否定这条已记录的激活关系。

### 回归结果

wine_activation_return_test.py 重放全部注册 overlay 两次，从有效 Wine 源抽取实际 can_activate_window、set_foreground_window、activate_other_window、历史函数及 WAYLAND_WindowMessage。Win32 server／窗口查询、Wayland 协议与消息调用仅在平台边界 mock。ASan、UBSan 编译运行通过。

- 新前台回归 14 项通过，其中包含继承的 SHM／生命周期回归。
- 激活历史场景覆盖临时窗口返回、显示模式尺寸变化、用户切窗、鼠标／内部激活、已有 owner、普通窗口、目标禁用／隐藏／最小化、HWND 代次重用、不同线程、前台请求失败、嵌套 popup、失去前台后销毁及矩形查询失败。
- 显式 Wayland 激活覆盖先还原后前台、同步重入、普通窗口、隐藏／禁用、还原时销毁与拒绝还原。
- 使用原固定 Valve 源的实际前台选择函数运行 --activation-baseline，临时窗口退出后应返回游戏的断言失败；修复版本同场景通过。
- wayland_minimize_restore_test.py 的 12 项通过，原有有尺寸 configure 不误还原的约束保留。
- git diff --check 通过。没有声称整个项目测试全部通过。

生产回放证明了候选函数的分支、身份边界和请求顺序；消息边界没有执行真实 War3 的 WM_ACTIVATEAPP、显示模式切换或绘制，真机结果仍待验证。

### 构建及验收安排

新的独立目录 build-activation-return-0a877ea3-20261005 用于 fresh Wine 配置、对象和构建身份。依赖、FEX 与已验证图形库沿用前候选。候选产物和命令审计保存在本机 workspace_temp/activation-return-20261005/。

安装前需用户手动结束当前旧柚 Pro 会话；本轮预检查仍有目标宿主及 War3 9027 存活。助手不停止该会话，不操作 app.hackeris.winehua，不启动应用／Steam／游戏。包完成并核验后才请求用户关闭会话以便覆盖升级。

安装后请用户手动运行 C:\vp-war3-window-trace-20261005.bat：先等待约 10 秒不点击，再测试全屏内容点击及切到其他窗口后恢复。核查 OHOS activation-return record／select、主 HWND 的 WM_ACTIVATEAPP／ShowWindow、minimize／restore configure、keyboard.enter 和持续新画面提交。若没有记录成功，需要继续依据该轮窗口尺寸、显示模式和前台调用判断资格；不能宣称此次候选已修好 War3。

### 激活返回候选构建与安装核验

独立 fresh Wine 构建、缓存身份检查、assemble、ARM64 unsigned HAP 和 runtime closure 检查全部成功。官方 verify-app 已确认签名与摘要有效。HAP 保持版本 1.4.5-proton.26-alpha / 1004035，不同候选以文件哈希识别：

- signed HAP：358356754 字节，SHA256 `c03ccedb1c80afe29b83cbbd763f437cf838416f3f0e2c3ab2dd210c41298b50`。
- unsigned HAP：356552536 字节，SHA256 `d15799f6e20e76d8772b2043486f8878f90736991b4c1c74dfe4cf28971ef760`。
- Wine payload SHA256：`51b270963435436146ab818d12a54a54d9d8e88f32acf51428683686dc66fa55`。
- 包内 win32u.so、winewayland.so、ntdll.so 的所有 ELF 装载节与本轮 fresh 编译输出一致，且存在 activation-return record/select 和 explicit activation restore 标记。
- signed 包的全部原有 archive entry 与 unsigned 输入一致；签名额外添加 .pages.info 和签名块。Wine payload 与 fresh staging zip 逐字节一致。
- libentry.so、libwine_child.so、FEX DLL 哈希与前候选一致；真实宿主及 guest zlib 保留。此次未改宿主运行代码。
- 包内 vcruntime140_1.dll 与 fresh strip 输出逐字节一致，PE 解析确认 ARM64EC SEH fallback 仍在有效代码 RVA 0x15254，保留此前修复。fresh PE 的时间戳会变化，未使用旧 DLL 整体哈希作为相等条件。

用户确认手动关闭后，安装脚本先检查目标包和所有带包名的 native 子进程均已退出，然后覆盖安装，收到明确 install bundle successfully，并通过 bm dump 核对版本；安装后目标进程仍未启动。另一个 app.hackeris.winehua 保持运行，本轮没有操作它。安装时间：2026-10-05T11:11:45.721522+08:00。

C:\vp-war3-window-trace-20261005.bat 已通过 bundle-aware 只读回传与逐字节核验确认保留。应用、Steam、游戏未收到助手的启动或停止命令。当前等待用户手动运行 BAT，先不点击约 10 秒，然后验证点击与切窗恢复；游戏修复结果尚未确认，设备已装载库哈希尚未直接读取。

构建日志、源码身份、包身份、核验脚本和安装命令账本保存在 workspace_temp/activation-return-20261005。回归结果仍为新前台组 14 项、原最小化组 12 项通过，原固定 Valve baseline 预期失败；没有新增全项目总测试通过声明。

## 激活返回候选 V1 真机失败与时序修正

11:11:45 已安装的 c03ccedb 候选未解决用户报告的无游戏窗口。11:16 的同一 War3 诊断 BAT 启动记录仍明确显示：主 HWND 0x501a4 / toplevel 8 → 无 owner 的 BlockingWindow 0x201bc / toplevel 9 → 临时窗口退出 → explorer 0x20144 / 线程 02d4 获得前台 → 游戏 WM_ACTIVATEAPP(FALSE) → SW_MINIMIZE。11:16:10.818 宿主收到 tl_set_minimized(8)，未观察到 activation-return record/select。主游戏 PID 28540 在此后仍有消息轮询并建立音频流；此前能听到音乐与后台运行路径相容。Frozen Throne.exe PID 28527 的 SIGSEGV 是启动器退出，不能等同 War3 主进程退出。当前截图的文件管理器位于 Kingdom Rush Genesis 目录，不能直接把其无窗口也归为同一原因。

读取包公共库目录、其他 UID 进程 maps 被设备权限拒绝，未获得实际装载库哈希。安装包身份和运行时间已对齐，但不把这个限制写成已完成的设备库核验。

### V1 为什么没有记录

V1 在 set_foreground_window 的 server 请求成功后、set_active_window 调用之前检查 NtUserGetForegroundWindow()==新 HWND。真实 server/queue.c 的 set_foreground_window 仅选择 foreground_input；相同输入队列的 active HWND 由随后的 set_active_window 请求写入共享状态。NtUserGetForegroundWindow 读取 input_shm->active，因此此时仍返回旧 HWND。这项检查直接排除了本次有效的同线程切换。

原回放错误地在 set_foreground_window 的 server stub 中立即替换 active HWND，没有覆盖真实队列时序。已修正模型：前台请求选择输入队列；active 请求才更新所属队列的 HWND，并抽取实际 set_active_window 函数执行其调用及回调顺序。使用修正模型与 V1 生产源码，临时窗口场景在“应记录 game HWND”的断言处失败，日志和源码快照已保存到 activation-return-order-20261005。

### V2 修改及验证

0030 overlay 将原活动窗口处理提为内部 set_active_window_with_history，保留 set_active_window 公共接口。只有前台请求的实际 active-HWND 更新成功，才在成功状态写入之后、palette/WM_ACTIVATE 等同步窗口回调之前记录历史。被 CBT hook 拒绝或 server active 请求失败时不记录／清除已有有效历史；直接的普通 set_active_window 调用维持原行为。跨线程前台请求不能创建同线程历史，但仍在异步激活前取消旧关系。

回放加入真实活动窗口处理和每个输入队列的 active 状态；新增实际活动请求失败／hook veto、激活消息回调中目标销毁、回调内用户切到其他窗口等场景。前台组 15 项、最小化组 12 项通过，ASan/UBSan 未报告问题，overlay 两次重放通过，git diff --check 通过。此结论仍是生产函数回放，不是 War3 真机成功。

正在新的 build-activation-return-order-0a877ea3-20261005 目录 fresh 编译 V2，不复用 V1 Wine 配置和对象；FEX、真实 zlib、既有 SEH 和宿主库沿用已核验来源。程序仍由用户手动启动，未向当前会话发送启动或停止命令。V2 尚未安装，待完整包核验后再安排现场升级。

### V2 包核验完成，等待覆盖安装

V2 独立 Wine 构建、有效源码身份检查、assemble、HAP 和 runtime closure 均完成，官方 verify-app 成功：

- signed HAP：358360707 字节，SHA256 `22f85394f0d74f6a2d857c49cd1a9f6b51015060fa2ae926f9f641a544d25963`。
- unsigned HAP：356553971 字节，SHA256 `61e76471e3fcd8a1932e8407b2dbe95042b7442a60c7c24ed16648e1cd0e5c56`。
- 版本仍为 1.4.5-proton.26-alpha / 1004035，应用包为 com.vintage.pomelopro；按包哈希区分 V1 / V2。
- 包内 win32u、Wayland、ntdll 的装载节与本轮 fresh 编译输出一致，运行 zip 与 fresh staging 一致；全部原有 archive entry 在签名前后保持一致。
- 宿主 libentry、libwine_child 与 V1 哈希相同，FEX 保留；真实 zlib 与本轮来源相符。CRT 与 fresh strip 输出一致，ARM64EC SEH fallback 仍为有效 RVA 0x15254。

11:39:01 预检查发现旧柚 Pro 宿主 PID 29731 及 Wine/virgl 子进程仍运行，故未安装 V2。已请求用户手动退出当前游戏和应用，以便覆盖安装；没有发送应用或游戏启动/停止命令。另一个 WineHua 未被操作。V2 真机结果待确认，不能将 27 项回归通过写成游戏已显示。

候选及完整核验资料位于 workspace_temp/activation-return-order-20261005。

### 按用户要求卸载后重装 V2

用户在 V2 覆盖安装完成时进一步要求“删除安装一下，避免有一些未知问题”。本轮已改为卸载 com.vintage.pomelopro（未使用 keep-data）后重新安装同一已核验 V2 HAP，两个命令均返回明确 success，bm dump 再次核对 1.4.5-proton.26-alpha / 1004035。新安装时间 2026-10-05T11:45:22.233853+08:00，包 SHA256 仍为 `22f85394f0d74f6a2d857c49cd1a9f6b51015060fa2ae926f9f641a544d25963`。

卸载前确认目标宿主与 native 子进程已全部退出。已将可读取的注册表、users、ProgramData、proton_shortcuts 和诊断 BAT 保存在本机 private-prefix-user-backup.tar（142104576 字节）；设备与本机 SHA256 相同。tar 未启用 follow-symlinks，未遍历外部驱动或 dosdevices。此备份包含私人用户资料，仅用于本地恢复，不提交／上传，也没有回灌到干净前缀。

11:46:21 只读检查确认旧 .wine 目录与 system.reg 均不存在，目标包没有进程运行；另一个 WineHua 仍在运行且未被操作。用户下载目录中的游戏未收到文件删除命令，其路径此前由进程映射确认位于外部 Download/com.vintage.pomelopro/games；shell 直接读取被权限拒绝，新包需要重新授权／导入后由用户确认可见，不把此前的映射记录当成安装后的文件可见性检查。

已请求用户手动打开新应用并进入 Wine 桌面。待新前缀初始化后，仅放回诊断 BAT；旧注册表和用户配置不恢复。尚未启动应用、Steam 或游戏，干净安装后的 War3 和 Kingdom Rush 结果均未验证。


## 干净重装后的现场：窗口角色丢失与 War3 黑帧（2026-10-05 午间）

V2 干净重装后，新前缀已于 11:49 核对建立，只放回诊断 BAT，未恢复旧注册表。用户手动启动两款游戏后反馈均无画面，并确认没有手动结束游戏。本轮宿主为 PID 40040，UID 已变为 20020279；以下身份均来自新日志，未复用 V1 的 PID/HWND。

- Kingdom Rush Genesis.exe：PID 42226，初始 tl7 在 11:48:27.942 创建，显示中文游戏标题后于 11:48:28.521 释放窗口角色。随后窗口父 wl_surface=21 为 role=none，GL 子 surface=61 为 subsurface，归属 toplevelId=0。进程存活，readbacks 持续递增（HWND 0x40174，1200×800），VirGL 报 exact target missing。不是没有渲染调用，也不能仅由 target missing 推断 GPU 崩溃；宿主没有可映射的顶层窗口。
- 首次 War3：主 PID 42804，tl10，11:49:50.5 起有 800×600 readback 与音频流，11:50:11 主进程退出。用户明确未手动结束。stderr 的 RtlExitUserProcess status=0 与 NCP 的 reason=11 同时存在，不能把 NCP reason 直接当成游戏原始未处理异常栈；确切退出触发原因尚未证明。
- 随后按用户手动运行原窗口诊断 BAT（Frozen Throne.exe 入口），主 PID 44948，HWND 0x50228，tl15。这次持续运行并保持 800×600 全屏，WS_MINIMIZE 未出现，Win32 style=0x96080000，宿主持续收到 subsurface=45 的 SHM 帧，约 22–24 FPS，截图为黑屏。用户确认“运行了，黑屏不动”。没有 activation-return record/select 并不代表此分支失效：本次未出现早前的 BlockingWindow/minimize 激活边，不能把两轮问题混为一谈。

证据位于本机 workspace_temp/focus-fix-20261005/clean-v2-{state,stderr,hilog,screen} 及 clean-v2-war3-{trace-full,live-hilog,live-screen}。设备号和凭据行按既有采集器脱敏；截图与完整原始日志保持本地。

### 0031：补齐纯 GPU 父窗口重新映射

WAYLAND_WindowPosChanged 的 !surface 路径在 GPU client 存在时只保留已有父表面、更新配置。隐藏或窗口重建可以让父表面变为 role-less 或被销毁；随后 ShowWindow/切全屏仍不再有 GDI surface，旧路径不会调用 make_toplevel，因此子表面持续出帧但没有顶层窗口归属。另一个时序缺口是 create_wayland_surface 在 client_attach 之后才写入 data->wayland_surface，首次创建/替换时 attach 从 win_data 查不到新父表面。

0031 让可见的 GPU 顶层窗口复用正常的角色创建/可见性流程，并先发布新父表面再绑定 client。隐藏窗口仍解绑，真实子窗口仍附着自己的根窗口，WS_EX_LAYERED 的属性门禁保留，已有有效顶层角色重复调用不重建。没有放宽跨进程表面身份、不强制还原窗口，也没有改变用户启动方式。保留 0007 状态更新块以支持叠加 overlay 的重复检查。

host_tests/wayland_client_remap_test.py 从实际 overlay 重放结果抽取 WindowPosChanged、父表面创建与角色生命周期函数。旧生产函数在 hidden-show、destroy-show、layered、role-failure 四类断言失败；0031 后六类场景（另含真实 child 与 no-client）全部通过。该组 12 项、最小化组 12 项、前台组 15 项通过，含 ASan/UBSan 和 overlay 二次重放；git diff --check 通过。Win32 查询及协议入口是 mock，未模拟完整游戏，不代表真机验收。

### 0032：War3 黑帧的下一处观测点

新增每 300 次 readback 一次的有界采样（及首次）：记录生成 SHM 前的 RGB/alpha 非零样本数、read buffer、读/绘制 FBO、目标与当前 EGL 读/绘制表面、renderer。不转储像素，不额外调用 glGetError 消费应用错误。用于区分客体生成黑帧和宿主丢失/覆盖内容；尚未宣称 War3 渲染根因已定位。新 vp-war3-render-trace-20261005.bat 只为用户手动的该次启动打开 OpenGL 与错误日志。

独立 BUILD_DIR 为 build-client-remap-0a877ea3-20261005。候选沿用已验证 FEX、宿主、字体、zlib、GPU 多窗口与 SEH 修复；编译、包核验及真机验收结果在完成后补记。


## War3 黑屏：不支持的全屏 FBO / gamma 调用（2026-10-05 12 时段）

用户手动执行 `C:\vp-war3-render-trace-20261005.bat` 后确认仍黑屏。本轮前台包核对为 com.vintage.pomelopro，宿主 PID 49762；旧 stderr 已被新会话重建，不能继续使用上一会话 PID 44948。只读采集保存了启动头段与运行尾段，不把大量诊断输出当作性能基准。

启动日志明确显示实际上下文 `version 2.1`，随后 `fs_hack_setup_gamma_shader` 报 GLSL 3.30 不支持（可用 GLSL 1.10 / 1.20 / ES 1.00）。持续运行段反复出现 `glNamedFramebufferTexture` unsupported、`glBlitFramebuffer` incomplete draw/read buffers，以及 `blit_framebuffer_surface: gamma shader is not initialized`。游戏仍不断调用 OpenGL draw。已找到一条明确失败的呈现路径；这不能排除其他游戏/驱动错误，且不等于设备硬件最高只支持 OpenGL 2.1。

本机证据：`focus-fix-20261005/remap-render-{init,opengl,state}.txt`，对应 SHA256、错误计数和首条样例在 `gl-capability-20261005/opengl-evidence-summary.json`。同时存在 WineD3D buffer Map box 警告，尚未证明与黑屏的因果关系，保留为后续线索。

### 0033：按上下文能力选择全屏转换

Proton 全屏 framebuffer 路径使用 ARB direct state access 的 framebuffer 操作，gamma shader 使用 GLSL 330、UBO 和 sampler。当前实现只获得函数地址，没有保证当前上下文支持这些调用；Mesa 可能返回 unsupported-function stub。`GL_EXT_direct_state_access` 不能替代所需的 `GL_ARB_direct_state_access`。

补丁 `0033-ohos-fshack-context-capability.patch` 在 OHOS 上用实际上下文版本/扩展判断所需能力，并要求 gamma shader 初始化成功。能力未知的首次绑定先使用正常 drawable；能力不足或 shader 失败继续走正常 GPU drawable/readback。支持能力的上下文仍保留 FBO/gamma 转换。能力记录属于每个上下文，切换时向 drawable 选择传入目标上下文，不能用旧的当前上下文；wglShareLists 重建后重新检查初始化。首次输出 `winehua_fshack_caps`，不会每帧重试失败的 shader。

同时避免对 GL 2.1 查询 GL_CONTEXT_PROFILE_MASK，修正内部纹理状态查询为 GL_TEXTURE_BINDING_2D。没有设置伪造 GL 版本，没有全局设置 WINE_DISABLE_FULLSCREEN_HACK，没有改动用户 EXE 或 DLL。低能力回退不提供这套 Wine 高级 gamma/DPI 缩放转换，画面/亮度和输入位置需设备验证。

`host_tests/opengl_fshack_capability_test.py` 抽取生产能力检查、gamma 初始化及 drawable 创建/缓存/更新函数，覆盖 GL 2.1、3.2、3.3+ARB DSA、4.3+ARB DSA、4.5、EXT/ARB 区别、上下文切换、DPI/gamma 变化、缓存 DC、shader 失败与重建后重试。ASan/UBSan（新场景含 LSan）和 overlay 双次重放通过。该组 12 项包含共享基础测试和 5 个新场景，不能算作 12 项独立 GL 场景。最小化组 12 项、前台组 15 项以及 GPU parent remap 组 12 项通过，未声称全项目总测试通过。

0031/0032 的独立 Wine/HAP 构建已完成，但在拿到上述明确错误后，决定合入 0033 再测试，旧 client-remap 候选未安装。新 BUILD_DIR 为 `build-gl-capability-0a877ea3-20261005`，没有复用旧 Wine 配置/对象，没有跳过构建身份检查。新包仍包含 0031/0032。当前正在编译；签名、包核验和真机结果待后续补记。

程序启动和停止仍由用户手动完成。已请用户关闭本次高频诊断会话，待新候选核验完整后再覆盖安装；不卸载、不恢复旧前缀，也不操作另一个 WineHua 应用。


### 0031–0033 联合候选：构建和包核验完成，等待用户关闭会话

独立 Wine 构建、cached identity guard、assemble、HAP、runtime closure、官方 verify-app 均通过。包内 win32u、winewayland、ntdll、opengl32 的全部装载节与 fresh 编译产物一致；opengl32 含 `winehua_fshack_caps`，Wayland 含像素采样标记。全部原始 ZIP entry 在签名前后相同。宿主和 FEX 与 V2 相同，CRT 是本轮 fresh strip 输出且 SEH fallback RVA 为 0x15254，真实 zlib 保留。签名使用与配置匹配的既有本地 debug 材料；临时副本已清理，未记录口令。

- signed HAP：358359202 字节，SHA256 `4d609beb171ce28a4be28ef7e052315b701a775b9660cfda63d3e0cc4d08c69e`。
- unsigned HAP：356554617 字节，SHA256 `54725284f740b0e4abaadc74108da7758a32a61c34070a592a2b29b3c7590e9b`。
- 版本：1.4.5-proton.26-alpha / 1004035，包名 com.vintage.pomelopro；以哈希区分测试包。
- Windows 交付目录：`F:\VintagePomelo-Workspace\workspace_temp\gl-capability-20261005`。

12:35:21 再次检查，目标宿主 PID 49762 及 native 子进程仍运行，未安装。已请用户手动关闭游戏/应用，当前等待回复；未发送任何应用或游戏启动/停止命令。本候选的 War3 和 Kingdom 真机结果尚未验证。

下一轮用 `C:\vp-war3-fbo-check-20261005.bat` 手动复测同一 Frozen Throne.exe 入口；脚本仅开启 error 与基本时间/线程信息，不开启 +opengl/+msg/warn+d3d 高频日志。脚本已 bundle-aware 发送/回传并逐字节核对，SHA256 `afc505ff35da87333638c9ef1ff46455dd16f1e4e2292603fef1cddba9ebe0b5`。旧 +opengl 诊断会话在 12:26 日志已达约 4.7 GB，已提醒用户手动关闭；原始头/尾证据已保存在本机，没有将所有日志上传。

验收顺序：确认新会话出现 gl=2.1 enabled=0；检查 gamma shader unsupported 与 NamedFramebufferTexture 错误是否消失；观察 readback RGB 非零样本及实际游戏图像/输入；然后独立启动 Kingdom，检查父 surface 的 toplevel 归属和可见性。任何一项未完成都不能写成两款游戏已修复。


### 联合候选已覆盖安装，等待用户手动复测

用户确认关闭当前会话后，安装脚本再次核对 com.vintage.pomelopro 及 native 子进程均已退出，验证本机 HAP 完整 SHA256 后执行覆盖安装。系统返回 `install bundle successfully`，随后 bm dump 核对版本 1.4.5-proton.26-alpha / 1004035。安装时间：2026-10-05T12:37:59.048555+08:00；signed SHA256：`4d609beb171ce28a4be28ef7e052315b701a775b9660cfda63d3e0cc4d08c69e`。

安装完成后的进程检查确认目标未启动，没有向应用、Steam 或游戏发送启动/停止命令。新低频诊断 BAT 已在升级后 bundle-aware 回传并与本机内容逐字节核对，路径 `C:\vp-war3-fbo-check-20261005.bat`。已请用户手动运行并反馈画面；本候选游戏结果仍未确认。旧前缀保留，本轮未卸载。


## 联合候选新现场：合法 texture=0 导致 War3 初始化终止

语言切换检查按用户最新指示暂缓，尚未改动语言源码；本轮优先处理游戏。12:44 只读截图显示 War3 启动器弹出 CD-ROM drive error，后台为用户手动运行的旧 render-trace BAT。宿主 PID 58758；三次 War3 启动在此次 stderr 尾段均出现同一断言。最近一次 GL 调用为 `glFramebufferTexture2D(..., texture=0, level=0)`，紧接 `wow64_ext_glFramebufferTexture2D` 的 `!is_wine_reserved_texture` 断言失败。NCP 的 reason=11/60 不是可用的原始崩溃栈；本轮根因依据是明确的 stderr 断言及其前一条真实 GL 调用。

0033 的新标记已在设备日志出现：实际 GL2.1，enabled=0，gamma=0/0。原 GLSL330/gamma/NamedFramebufferTexture 失败路径未在此次新上下文启动尾段重现。但关闭高级 framebuffer 路径暴露了一个旧的纹理保护缺陷：`framebuffer_textures[8]` 未使用项为零，`is_wine_reserved_texture` 直接比较所有项，将默认/解绑编号零判作 Wine 内部已分配的纹理。WineD3D 在能力自检后解绑 FBO 附件即触发断言，游戏未进入正常呈现；启动器随后弹出光盘错误。尚未独立验证所有光盘错误都由此引起，不修改游戏文件、光盘检查或 EXE。

### 0034 修复与回归

`0034-opengl-zero-texture-not-reserved.patch` 在共享纹理判定入口对零返回 FALSE；非零内部纹理继续执行原来的保护。核心和 EXT、native 和 WOW64 的生成 thunk 均复用该入口，不删除断言或改生成代码。此语义适用于默认纹理与解绑附件，不依赖游戏名或伪造 GL 版本。build_wine 注册该 overlay，Makefile 注册生产函数回放测试。

`opengl_reserved_texture_test.py` 五项测试通过：使用固定 Valve 的旧生产判定及实际 WOW64 framebuffer thunk，合法零解绑精确复现断言退出；修复后 native/WOW64 的 core/EXT framebuffer 和 BindTexture 传递零成功，覆盖空、部分和完整内部纹理表；非零应用纹理传递正常，真实内部纹理依然在 thunk 中触发保护；无当前上下文与 overlay 两次重放通过。抽取生产数组声明、参数布局和生成 thunk，平台 context lookup、GL 入口和内部状态边界 mock；ASan/UBSan/LSan 未报告问题。这不是实际游戏渲染验收。

此前 GL 能力/drawable 组 12 项、patch detection 3 项及 Wine 构建身份组通过，git diff --check 通过。未宣称全部项目测试通过。新独立目录为 `build-texture-zero-0a877ea3-20261005`，不复用上一候选 Wine 配置/对象。继承 FEX、宿主图形库、字体/zlib、SEH 与既有多窗口修复。包构建及安装结果待完成后补充。程序启动、关闭仍由用户完成。

本地证据摘要及 SHA256 位于 `workspace_temp/texture-zero-20261005/crash-evidence-summary.json`；完整日志和截图保留在本机 `workspace_temp/focus-fix-20261005/gl-candidate-current-*`，不提交原始用户资料。


### 0034 候选包已核验

新的独立 Wine、构建身份检查、assemble、unsigned HAP、runtime closure 及官方签名核验完成。包内 opengl32 的全部装载节与本轮 fresh 输出一致，opengl32 哈希已与上一候选不同；反汇编确认 is_wine_reserved_texture 在上下文/内部表检查前拒绝将编号零当作内部纹理。保留 0033 能力回退。包内 win32u、Wayland、ntdll 也与 fresh 输出一致，FEX 与宿主主库保留。CRT SEH fallback 经 PE 检查与 fresh strip 输出比对通过，真实 guest/host zlib 保留。

- signed HAP：358359124 字节，SHA256 `7350e9cd0d70bd3e991f17fee587563c03b9ecf9a622118a0c302da0d568d13c`。
- unsigned HAP：356554378 字节，SHA256 `1d471e02dad50c8be2acbb41e24127b9454ba2d6eeacdeb3f8bd9d35db1eaeed`。
- 版本 1.4.5-proton.26-alpha / 1004035，包名 com.vintage.pomelopro，候选按哈希识别。
- 本机交付目录 `F:\VintagePomelo-Workspace\workspace_temp\texture-zero-20261005`。

此时尚未安装，尚未验证 War3 游戏显示。下一步先确认用户已手动结束会话，再覆盖升级；安装后请用户手动运行低频 C:\vp-war3-fbo-check-20261005.bat，核查不再出现 reserved-texture 断言，继续检查 RGB readback 和全屏内容、输入及切窗恢复。Kingdom Rush 需要独立手动复测，不能由 War3 初始化回归推断另一款游戏已修复。日语/繁体切换仍暂缓，本轮未改语言代码。


### 零号纹理修复候选已覆盖安装，等待用户手动复测

用户明确授权“你直接杀死安装就行”后，执行前两次进程快照均确认旧柚 Pro 及 native 子进程已退出，因此无需发送停止命令。安装脚本再次检查进程、验证本机 HAP 完整 SHA256 后执行覆盖安装。系统返回 `install bundle successfully`，随后 bm dump 核对版本 1.4.5-proton.26-alpha / 1004035。安装时间：2026-10-05T13:12:09.861388+08:00；signed SHA256：`7350e9cd0d70bd3e991f17fee587563c03b9ecf9a622118a0c302da0d568d13c`。

安装完成后的进程检查确认目标未启动，没有向应用、Steam 或游戏发送启动命令。新低频诊断 BAT 已在升级后 bundle-aware 回传并与本机内容逐字节核对，路径 `C:\vp-war3-fbo-check-20261005.bat`。下一步由用户手动运行并反馈画面；本候选游戏结果仍未确认。旧前缀保留，本轮未卸载。


## 全屏黑边与多国语言联合修复候选（2026-10-05）

用户已明确确认 7350e9cd 零号纹理候选中《王国保卫战》和 War3 都能正常启动。这个反馈只确认其启动结果；新增的全屏黑边问题和本轮语言切换仍需分别验收。

### 全屏边缘露桌面

只读截图确认 War3 菜单正常显示，但左右保留 Wine 文件管理器和蓝色桌面。现场使用 SnapshotGpuDesktopScene / DrawZeroCopyScene 的 GPU 路径；旧场景只给 Direct fullscreen 加 solidBlack，ZC/SHM fullscreen 没有黑底，EGL 还会跳过无 pixels 的 solidBlack。游戏 800×600 与桌面 1200×800 的比例不同，等比例 fit 留下的区域因此露出下面的窗口。这与提高分辨率无关。

DesktopCompositor 现在在被选中的有效 fullscreen 窗口层序插入整桌面不透明黑底，适用于 SHM / Direct / ZC；上层游戏内容、菜单和弹窗维持原层序。EGL 用 scissor 限定的 opaque clear 真正绘制 solidBlack，既不上传黑底纹理，也不会越过输出 letterbox。没有改分辨率或输入 fit。

两项生产代码回归在修改前均复现缺黑底，修改后 9 个 scene/input case 和 EGL multi-consumer/spatial replay 通过，覆盖左右黑边、上层菜单混合、暂时无 GPU 帧、退出 fullscreen/minimized、黑边吞输入与中心映射。测试包含真实生产层序和绘制循环、模拟 GL/NativeImage 平台边界，不能代替真机图像验收。日志为 scene-before/after.log 与 egl-before/after.log；现场证据保留在本机 focus-fix-20261005/fullscreen-border-current.png 及对应尾段日志。

### 多国语言切换

参考 main 的 5383b784 与语言功能 784851f6，当前 UI、AppSettingsStore、WineEngineService 和独立 EXE 入口已有四语支持；但是当前 LaunchClient 的第 8 参数仍只接受 zh_CN/en_US，导致桌面会话日语/繁体静默回简体。旧 WineEnvService 的保存/加载也只有中英二选一。修复放行 zh_CN/zh_TW/ja_JP/en_US，并在旧服务复用 normalizeWineLanguage；参考 main 恢复 WineLocaleFor，令 LANG/LC_ALL 同源归一化、非法值回简体。保留本分支 launchClient 12 参数布局，没有直接拷贝 main 的不同签名。

新 wine_language_launch_test.py 执行生产 NAPI 语言段、生产 LANG/LC_ALL 生成段及生产设置归一化/保存/恢复逻辑；NAPI 与 preferences 是模拟边界。旧生产代码可复现日语/繁体被改成简体，修复后四种语言、缺省、未知值、参数不足和持久化恢复通过。现有 Valve overlay/字体/locale 4 项回归通过，覆盖 zh_TW LCID 0404/ACP 950、ja_JP 0411/932、zh_CN 0804/936、en_US 0409/1252。没有声称全项目测试通过。

包核验确认本产品实际编译 WineEngineService，而旧 WineEnvService 不在可达字节码中；因此本轮 ArkTS 输出与上一包相同是预期结果。包内 modules.abc 与本轮实际编译输出完全相同，包含四语言模型及 normalizeWineLanguage。有效修复装入新的 native 启动桥接和环境构建代码。旧服务的修复用于维护参考入口，未宣称它是当前产品故障的运行入口。语言切换在重启 Wine 会话后生效，Windows 代码页切换不保证游戏自动更改界面语言。

### 构建、包身份和安装

沿用已验证的 build-texture-zero-0a877ea3-20261005 Wine 输出；cached identity guard、正常 Hvigor/CMake 宿主构建、runtime closure 和官方 verify-app 通过，没有跳过构建身份检查。包内 libentry.so 与当前 strip 输出完全一致、全部 ELF 装载节匹配当前 CMake 链接输出；四个修改的宿主对象已编译并保存 ARM64 反汇编。Wine payload、FEX、CRT、win32u、Wayland、ntdll、opengl32、zlib 和 native 子库与用户已确认能启动游戏的 7350e9cd 包一致。签名前后所有原 ZIP entry 逐项 SHA256 相同。

- signed HAP：358358241 字节，SHA256 `87fc80212983c20375ee5e86057ae82281451440a5a1980330831f938f9cd83d`。
- unsigned HAP：356555290 字节，SHA256 `af23958fb90cfaffe7ac6f573acda8d737b8836ea008c37d85edbe6a692c71ca`。
- 版本 1.4.5-proton.26-alpha / 1004035，包名 com.vintage.pomelopro；同版本候选按哈希区分。
- 本机交付目录 `F:\VintagePomelo-Workspace\workspace_temp\fullscreen-border-20261005`。

安装前目标包及 native 子进程已经退出，未发送 force-stop。系统明确返回 install bundle successfully，bm dump 版本核对通过；安装时间 2026-10-05T13:50:52.963151+08:00。覆盖升级保留前缀，安装后目标未启动；没有向应用、Steam 或游戏发送启动命令，也未操作另一个 WineHua。仅黑边的 b16cdac3 包已被本联合候选取代，未安装。

现有 C:\vp-war3-fbo-check-20261005.bat 已在升级后回传逐字节核对。新 C:\vp-wine-language-check-20261005.bat 与 Win64 EXE 也已 bundle-aware 传入并回读核对，但未执行；EXE 只导入 KERNEL32，手动运行后记录真实 GetACP/GetOEMCP/GetSystemDefaultLCID/GetUserDefaultLCID/GetLocaleInfo 和 LANG/LC_ALL 到 C:\vp-wine-language-result-20261005.txt，不依赖 CRT 或修改游戏文件。

已请用户手动运行 War3 验证全屏黑边、菜单、点击位置、退出全屏；再选择日语、重启 Wine 会话并手动运行语言检测 BAT，日语预期 ACP 932、System/User LCID 0411；繁体同协议预期 ACP 950、0404。当前联合候选的图像及 Windows API 真机结果待确认，不能由离线回归或上一包启动结果代替。本轮未提交或 push。
