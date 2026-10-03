# WineHua Proton-OHOS 运行须知与已知问题

> 维护口径：本文汇总"跑得起来需要知道的事"与"当前还没解决的事"，供后续接手/回归时直接对照。
> 证据与逐轮细节见 `docs/steam-win64/` 下的交接与复核文档；本文只保留结论与操作要点。
> 最近更新：2026-10-01（VPP 合并后的 prefix/cwd 启动回归修复，保持 FEX）
> 本轮修复与真机验收见 [合并启动回归记录](STARTUP_MERGE_REGRESSION_20261001.md)，下一调查入口见 [AI 交接](AI_HANDOFF_STEAM_DRM_20261001.md)。下方旧问题清单属于历史记录，本轮状态以当日报告为准。

## 1. Proton 注意事项（native / Wine 侧）

### 1.1 当前 Wine 构建基线与补丁

- 构建实际使用 `scripts/env.sh` 的 `WINE_SRC`，默认已注册子模块 `thirdparty/wine-valve`，当前 pin 为 `cd547f7a0ee9d3d59b3ec47317a7852dedf0443b`。
- `.gitmodules` 同时注册 `thirdparty/wine` 与 `thirdparty/wine-valve`，默认构建使用后者；不要误改另一棵树。
- 本轮 Wine 改动保存为主仓库 `0011` / `0012` 补丁，由 `scripts/build_wine.sh` 幂等应用，gitlink 保持不变。补丁已验证能从 pin 完整复现当前构建内容。

### 1.2 默认遵守 PE 的大地址空间标志

- Proton 原来的 `need_override_large_address_aware()` 在没有环境覆盖时默认返回 true，
  将没有声明 LAA 的 32 位程序也放进 >2 GB 地址空间。
- 实测《仙剑奇侠传四》PAL4 未声明 LAA，LAA=1 时像素复制循环跨界读到未提交区域，LAA=0 才能跑到场景。
- 2026-10-03 通用修复：`0016-ntdll-honor-pe-large-address-aware-default.patch` 默认遵守
  主映像的 `IMAGE_FILE_LARGE_ADDRESS_AWARE` 标志，移除 child 中的 PAL4 文件名特判。
  非 LAA 的 WoW64 程序默认使用 2 GB 范围，声明 LAA 的程序仍使用 4 GB 范围；
  `WINE_LARGE_ADDRESS_AWARE=1` 仍可显式强制开启，0 表示遵守 PE 标志，不会清除 PE 自身声明的 LAA。
- 判定在 Wine 映像层完成，适用于直启、重命名的 launcher 和 Steam 创建的子进程。
  真机复测已能从 Steam 启动 PAL4，用户确认画面、帧率和声音可用。
- 曾依赖 Proton 强制默认获得 >2 GB 空间的非 LAA 程序需要显式设置
  `WINE_LARGE_ADDRESS_AWARE=1`。64 位原生程序的地址空间分支不变。

### 1.3 必须由 `scripts/build_wine.sh` 应用的补丁（幂等）

| 补丁 | 作用 |
| --- | --- |
| `patches/wine/0001-win32u-repair-external-font-registration.patch` | 外部字体注册修复；Steam 中文字体依赖它，不要为了排查白屏整体回滚 |
| `patches/wine/0002-ntdll-ohos-signal-safe-stack-read.patch` | 信号处理器用 `process_vm_readv` 读栈；FEX 的 SP 可能正好落在 guard 页，直接读会二次 SIGSEGV |
| `patches/wine/0011-wineboot-durable-prefix-completion.patch` | init/update 与 RegFlushKey 完成后原子发布 prefix 完成标记 |
| `patches/wine/0012-wineserver-locked-registry-migration.patch` | 会话锁内、加载 registry 之前迁移字体/代码页，避免 child 与 server 竞争 hive |

### 1.4 ntdll unix 侧改动是"一整块"，不要切碎回滚

`unix/ohos_virtual.[ch]`、`unix/sync.c`、`unix/thread.c`、`unix/virtual.c`、`unix/env.c`、`unix/loader.c`、
`unix/server.c`、`unix/signal_arm64.c`、`unix/unix_private.h`、`win32u/font.c`、
`programs/winehua_platform_process_smoke/main.c` 属于 32/64 位运行时适配 + 诊断设施。
它们一起构成"已知可跑"的状态，单点回退会引入更难定位的故障。

### 1.5 图形侧两条硬约束

- **opengl32 draw buffer**：单缓冲恢复必须用 `glDrawBuffer`，多缓冲才用 `glDrawBuffers`。
  混用会在 virgl GL 2.1 上报 `INVALID_ENUM`（白屏/黑屏类症状的常见根因之一）。
- **Vulkan loader**：应用目录里的 AMD64 `vulkan-1.dll`（Steam 自带 CEF 那份）被 ARM64X DXVK 导入时返回
  `STATUS_NOT_SUPPORTED`，loader 必须在这种状态下**继续搜索默认路径**，否则找不到后面兼容的系统 DLL。
  修的是加载选择，不改 Steam 原 DLL 字节。

## 2. 32 位后端：FEX 为默认（2026-09-22 起），box64 为可选回退

### 2.1 选择规则

[wine_child.cpp](../entry/src/main/cpp/proc/wine_child.cpp) 的 `select_wow64_backend()`：

| 场景 | HODLL | 说明 |
| --- | --- | --- |
| 32 位进程（默认） | `libwow64fex.dll` | FEX WoW64 路径 |
| 32 位进程 + `WINEHUA_WOW64_ENGINE=box` | `wowbox64.dll` | 显式回退（旧行为） |
| 64 位进程 | `HODLL64=libarm64ecfex.dll` | FEX 内核，固定 |

`steamapps\common` 路径不再强制 box64，只打日志 `[WineChild] steamapps game on FEX base (exe=...)`。

### 2.2 为什么换：box64 在当前树上是"要么慢、要么卡"

| 配置 | PAL4 表现 |
| --- | --- |
| box64，`BIGBLOCK=3`（出厂/ master 默认：`CALLRET=2 / FORWARD=1024`） | **卡死在加载**：readbacks 冻结、约 2 核空转 |
| box64，`BIGBLOCK=0`（唯一能跑通的档） | 能进场景，但只有 **24.8 FPS**（用户对照正常值 70+） |
| FEX（默认） | 加载阶段 **48 FPS**，进入场景正常；用户实测"明显流畅"；同版本下 32 位 steamapp 也能出标题画面 |

box64 侧遗留问题：v33 给 `dynarec_native_pass.c` 的跨页 guard / `ninst>=inst_max` 两条提前 `break`
补了 `need_after |= X_PEND`（修好白屏），但 `BIGBLOCK>=stopblock` 的**合并块**路径没有同样保护，
这就是默认档卡死的候选根因，未结案。

### 2.3 FEX 侧已知故障形状

32 位游戏在 FEX 下反复出现同一形状的 host 异常：

```text
[early-fault] #11 pid=28780 tid=28780 sig=11 code=2 addr=0x140430ff0 pc=0x7ff5bdfcac lr=0x7ff5bdfb94 sp=0x1001ff2e0
```

- `sig=11 code=2` = `SEGV_ACCERR`（页存在但权限不允许），地址末三位固定 `0xff0`（页内最后 16 字节）。
- PC/LR 落在 `<unmapped>`（FEX 的匿名 JIT 映射），不在 ELF 模块内。
- **目前判为疑似良性**：PAL4 与 `Game.exe` 出这些 fault 后都继续运行数十秒。判读必须结合其后是否有
  `code=c0000005` / `dispatch_exception`，不能只看 `[early-fault]`。
- 注意 `[early-fault]` 只在 `WINEHUA_EARLY_FAULT=1`（默认）打印，且**先于** Wine SEH。

### 2.4 FEX 二进制要对齐（重要）

以下是 2026-09-21 的历史快照，不代表本轮产物。该时间点存在三个不同 SHA：设备上的 `libarm64ecfex.dll` 是 **v21 返回缓存绕过诊断版**
（`a9b75e5e…`），容器 `build/fex-ec/Bin` strip 后是 `a42eb333…`，文档记载的"原版"是 `8aba586e…`。
把 FEX 当基座做性能/稳定性结论前，先确定并部署**非诊断版**，否则结论会混入诊断补丁本身的行为。

## 3. Steam 启动

### 3.1 启动方式

统一走 Want（`tools/steam-rwx/launch_want.py`），它总是发送带 argc 的 encoded JSON argv/env：

```text
winehua.mode=game
winehua.game_path=<percent-encoded exe>
winehua.game_argc / winehua.game_args_json
winehua.d3d_env_json=[{key,value},…]
```

- **陷阱**：`winehua.game_argN` / `d3d_env_valueN` 是字面字符串、**不做 URI 解码**；
  传 `%2Fc` 会把 `%2Fc` 原样交给 Wine。用 `launch_want.py` 或 `game_arg_encN`。
- 环境变量白名单在 [GameHook.ets](../entry/src/main/ets/game/GameHook.ets)：
  未知 key 会被丢弃并打 `ignoring invalid D3D environment key`。可调键含
  `BOX64_DYNAREC*`、`WINE_LARGE_ADDRESS_AWARE`、`WINEHUA_WOW64_ENGINE` 以及 `WINEHUA_*` 诊断开关。

### 3.2 CEF 必需参数与冲突项

- `-no-cef-sandbox` 无条件注入：缺了会停在 `Startup - webhelper launched pid`，登录窗口永不出现。
- `-cef-force-gpu` 是可选项：它与 WineHua 注入的 `--disable-gpu*` 直接冲突，会造成"半 GPU 状态"。
- 相关诊断开关（`WINEHUA_CEF_*`）只在排查时开；`WINEHUA_CEF_LOGGING=1` 自己会制造 renderer 崩溃，
  评估稳定性时必须关。

### 3.3 DLL 与自更新

- 不再每次隐藏 Steam 校验的 `vulkan-1.dll`；只在 **Steam bootstrap** 时把缺失的 `.winehua-shadow`
  备份恢复原位，否则每次隐藏都会触发重新下载。
- `WINEDLLOVERRIDES` 保留 `vulkan-1=b`、`d3d11=n;dxgi=n`（由 native 端拼接）。
- Steam 客户端进程树（含 `bin\cef\steamwebhelper.exe`、`gldriverquery*` 等相对路径拉起的工具）走 FEX。

### 3.4 验收口径

四项同时满足才算 Steam 回归通过：原 `vulkan-1.dll` 保持原位、自检不反复下载、
CEF GPU 不进入崩溃循环、界面**实际操作有响应**。只看截图出现界面不算通过。

### 3.5 已知问题

- FEX 下 CEF 的 renderer / ProcessorMetrics 会在一段时间后 `signal 11`；历史排查（V8 反优化栈帧检查、
  `--jitless` / `--no-opt` 对照、browser/renderer 退出次序）见
  [FEX_NATIVE_FAULT_REVIEW_20260921.md](steam-win64/FEX_NATIVE_FAULT_REVIEW_20260921.md)，
  **尚未结案**。

## 4. 游戏运行与显示链

### 4.1 分层

```text
Windows D3D  →  wined3d 或 DXVK  →  OpenGL/Vulkan(guest Mesa virtio/virgl)
             →  vtest socket      →  virglrenderer →  host EGL/GLES
显示：Wine Wayland wl_surface → 内嵌合成器 → GraphicsBroker 取帧 → EglRenderer → XComponent
```

### 4.2 帧率读数（不要靠肉眼判断）

- proton.24 起，`/data/.../files/.wine/drive_c/windows/temp/winehua_display_fps.txt.<toplevelId>`：
  `序号 fps toplevel 单调时钟微秒`，由 `perf_utils.cpp::PublishDisplayedFps()` 每秒刷新。
  HUD 只接受目标窗口的近期样本；旧的单一文件不能用于本版本帧率判断（见 §8）。
- `hilog -T WL_EGL` 的 `[GL-PERF]`：`take/upload/swap/total` 的 50/95/99/max 分位。
  `total` 分位远小于帧周期时，瓶颈在 guest 侧而不是显示链。

### 4.3 已知显示问题

| 问题 | 现象 | 现状 |
| --- | --- | --- |
| 子面归属（"Wayland 对不齐"类） | 子面的**直接父级**不一定带 xdg 角色（Wine GL drawable 挂在窗口 surface 下，中间层是普通 surface），只看直接父级会把游戏帧整段丢掉 → 游戏在跑但窗口黑屏/空白 | 已修：`wl_core.cpp::UpdateSubsurfaceOnCommit()` 沿父链向上找最近的 toplevel（PAL2 1280x800 toplevel serial 停在 2、真实帧都在 640x480 subsurface 的场景） |
| 坐标空间不一致 | 同一 `TakeToplevelFrame` 有时返回桌面合成帧、有时返回 800x600 直传帧，调用方按尺寸猜空间 → letterbox 锚错、点击偏移（红警 2 主菜单点不中） | 设计层收口见 [COMPOSITOR_REFACTOR_PLAN.md](COMPOSITOR_REFACTOR_PLAN.md)；弹窗偏移公式 4 处收口仍未完成 |
| DPR / fit 缩放 | 缩放后点击位置偏移 | 需要时按 `COMPOSITOR_REFACTOR_STATUS.md` 的 fit 映射核对 |
| PAL2 | GL 输入、blit 后图像完整且方向正确，屏幕仍是黑底白条 | 后续 Wayland/合成呈现问题，**未结案**（用户允许暂缓） |
| PAL4 | 白屏 | 已修（见 §2.2） |
| Heaven（D3D11） | 直接启动缺参数 → 表现异常；需与 launcher 相同的数据/脚本参数 | 参数组合已跑通；封装脚本 `tools/steam-rwx/launch_heaven.py` 已加入仓库 |

### 4.4 输入与截图

- `uitest uiInput click/keyEvent` **不能驱动 Wine 窗口**：点击会落在 WineHua 自己的 ArkTS 界面
  （实测会把 App 主界面切到前台）。游戏内输入要人工/外设操作。
- 截图用 `uitest screenCap -p /data/local/tmp/x.png` 再 `hdc file recv`；`snapshot_display` 只接受 `.jpeg`。

## 5. 设备与调试注意

- hdc target 会变：本轮为 USB 序列号 `5KPBB25818203996`，旧文档里的 `192.168.180.71:36875` 已不可达。
  所有设备命令走 `tools/steam-rwx/device_command.py`（带本地命令账本）。
- 宿主（`app.hackeris.winehua`）重启会**重开 stderr 日志**：按进程/时间切分日志，别把上一轮的
  `readbacks` 当成新游戏出帧。设备重启可用 `uptime` 区分（本轮 host 重启时设备 uptime 未变）。
- 数据分区 453 GB，本轮仅剩约 58 GB；`/data/local/tmp` 里堆积的历史 HAP 需要时再清。
- `WINEHUA_DIAG_QUIET=1` 关掉反复 SMC 日志；`trace+seh` 会打印大量正常 unwind/setjmp，不等于致命错误。
- 诊断用的字节流拷贝必须 `copy /b`；文本 copy 遇 `0x1a` 会截断 EXE。

## 6. 当前未结清单（按优先级）

1. box64 回退路径在 `BIGBLOCK>=2` 下 PAL4 卡死的根因（合并块 `X_PEND` 覆盖 / SMC 页保护陈旧 JIT）。
2. FEX 二进制对齐到非诊断版，再复测 32 位游戏与 CEF 崩溃。
3. `addr=…0xff0` 的 `SEGV_ACCERR` 是否良性（对照后续 `code=c0000005` 与帧率变化）。
4. Steam 全链路回归（§3.4 四项口径），含从 Steam 启动 32 位游戏。
5. PAL2 的 Wayland/合成呈现（黑底白条）。
6. 合成器坐标空间收口（`PresentedFrame` 契约、popup 偏移公式 4 份）。

## 7. 提交、子模块与快照约定（2026-09-22 更新）

| 仓库/子模块 | 远端位置 | 当前 pin | 说明 |
| --- | --- | --- | --- |
| `WineHua`（主仓库） | `feature/proton-wine-ohos` | — | 入口层 32 位基座改动、白名单、subsurface 父链修复、文档与诊断工具 |
| `thirdparty/box64` | 分支 `ohos-wow64-smc-fs` | `e970ee6f2` | 跨页停块 `X_PEND` + JIT 非 SMC 故障交回 epilog |
| `thirdparty/wine-valve`（**已正式登记为 submodule**） | `winehua/wine.git` 分支 `ohos-port-steam-win64` | `008bf8ac796` | ntdll 故障路由/SMC、信号安全栈读取、字体、opengl32、vulkan-1 回退、smoke 扩展 |
| `thirdparty/dxvk` | `winehua/dxvk.git` 分支 `feature/arm64-legacy` | `7c5cc47f` | 新增 WineHua present 观测（`DXVK_WINEHUA_PERF_DIAGNOSIS`、`WineHuaPresentImage` 时间线） |

历史快照分支仍保留：`ohos-port-snapshot-20260919` / `ohos-port-snapshot-20260922`
（根提交快照，仅作存档，不再作为构建输入）。

### Wine 仓库的两个坑

1. 本地 `thirdparty/wine-valve` 目前仍是 `thirdparty/wine` 仓库的 **linked worktree**
   （`thirdparty/wine-valve/.git` 指向 `thirdparty/wine/.git/worktrees/wine-valve`），
   两处共享对象库；新 clone 则按 submodule 正常独立检出。二者内容必须一致（都以
   `ohos-port-steam-win64` 的 pin 为准，本地已切到该分支）。
2. 远端 `origin/ohos-port` 与本地历史 **没有共同祖先**（`no merge base`），不能直接 push、
   **不要强推**；要提交改动，请把当前树叠成 `ohos-port-steam-win64` 上的一次提交后快进推送
   （本次即 `008bf8ac796`）。

### 本地曾出现的两个子模块故障（已修）

- `thirdparty/dxvk`：`.git/modules/thirdparty/dxvk` 丢失，导致该子模块任何 git 操作都报
  `fatal: not a git repository`（`git status` 需要 `--ignore-submodules=all`）。修法：备份目录 →
  删除目录 → `git submodule update --init thirdparty/dxvk`。修复时发现本地有 3 个未提交的
  present 观测改动，已作为 `7c5cc47f` 推到 `feature/arm64-legacy` 并更新 pin——即"本地构建用的
  dxvk 与仓库记录不一致"这个隐患已消除。
- `thirdparty/wine-proton`、`build/` 下若干目录为历史遗留产物，不参与当前构建。

### WSL 网络（推送/拉取前必看）

- 全局 `git config http.proxy = http://127.0.0.1:8080` 在 **WSL 内不生效**（Clash 监听在 Windows 侧）。
  WSL 里要用网关地址：`git -c http.proxy=http://172.17.80.1:8080 <cmd>`。
- 直连 `https://github.com` 取 ref 可以，但大 pack 上传会超时；推送统一走上面的网关代理。
- 本机已配置 `url.https://github.com/.insteadOf git@github.com:`，所以 `git@github.com:` 会被改写成 https。

## 8. 共用呈现服务与 FPS（proton.24，2026-10-03）

Steam CEF 与 GL 游戏共用 `--no-fork --multi-clients` vtest 命令线程。
呈现目标未绑定时，服务端立即返回：GL 为 `kPresentNoTarget`，Venus 为
`-EAGAIN`。Venus 沿用 Guest 的 8 次指数退避；GL 沿用 readback，后续
消费者 Attach 发布 ready 后可转入零拷贝。服务端不等待窗口目标，避免
一个后台窗口拖住其他进程的 GL/Vulkan 提交。目标条目仍参与 Query，
因此取消等待不等于放弃后续绑定。

零拷贝消费者按每个 `ZeroCopySurfaceInfo.vulkan` 选择目标类型，并把该
类型显式传给 Attach。全局 D3D/Vulkan 模式只描述会话设置，不能用于
过滤同一 Steam 会话内的 GL 子游戏。

FPS 文件为 `winehua_display_fps.txt.<toplevelId>`，原子发布
`sequence fps toplevelId sampled_monotonic_us`。HUD 只读所请求窗口，拒绝
超过 2.5 秒或时钟重置后的样本；旧的无时间戳文件不再作为 HUD 回退。
此数值是相应窗口成功交换的显示次数，仍需区别于游戏内部的渲染次数。

`host_tests/shared_present_dispatch_test.py` 编译执行实际 PresenterManager
代码，以替代 GPU/窗口目标验证连续后台 miss 下前台提交、晚到 Attach、
GL/Vulkan 类型切换和 Detach。`--baseline --baseline-ref 56bded0e` 使用修复前的实现复现
2.5 秒服务阻塞。`host_tests/displayed_fps_test.cpp` 验证多窗口并发发布、
样本新鲜度及错误 ID；两者已接入 `make test`。宿主测试不替代设备端
Steam/PAL4 场景性能与实际 NativeImage 帧验证。

## 9. proton.25 发布记录（2026-10-03）

版本为 `1.4.5-proton.25-alpha`，versionCode `1004034`，保留 proton.24 的
共用呈现服务与 FPS 修复，以及通用 PE 执行节 EOF 补零、PE LAA 默认策略。
设备上用户确认 Steam 启动仙剑四后帧率可用，并在音频排查后确认该版本有声音；
未取得同场景量化 FPS，未确认此前暂时无声的根因，没有为此改动音频后端。

内置音频测试入口改为包内实际存在的 `C:\smoke\x64\winehua_audio_smoke.exe`，
生成 3 秒确定性测试音并写入 `C:\smoke\results\builtin-audio-smoke.json`，
不再依赖未打包的 Alarm01 媒体文件。资源 catalog 与 fallback catalog 保持一致。
在仙剑四运行期间，x86 音频自检通过，宿主读完 144000 帧，用户确认能听到测试音。
