# 私有分支上游同步记录

目标分支：`private/wine-engine-app`

基线：WineHua `VintagePomeloMaster` @ `ba7218a`

> **同步基线标记**：最新核对到的上游 SHA 见 [UPSTREAM_SYNC_POINT.md](UPSTREAM_SYNC_POINT.md)（当前为 WineHua `master` @ `b0e85c0e`，本地镜像 `mirror_master`）。下次同步先 `git fetch winehua && git branch -f mirror_master winehua/master && git log b0e85c0e..mirror_master --oneline`，避免重复合并。

### 2026-09-04 对齐 wineserver 手柄总线 env（12aba3d4）

- 分支：产品线 `diag/m5-b3-remaining-pop`（与 `main` @ `cc36a4c7` 同提交再叠本修复）。
- 镜像：`winehua/master` @ `b0e85c0e`。
- 原则：只补 wineserver spawn 的 `AppendWineGamepadEnv`。winebus 由 winedevice 加载、环境继承 wineserver；缺 `WINEHUA_CONTROLLER_HUB` / `WINEHUA_GAMEPAD_*` 时 SDL 总线仍会枚举实体手柄，和 Hub 重复。wineboot 两处产品已注入。不上游 Hub v1 整包、不上游 Index 四档 UI、不跟随 `e538f95a` wine gitlink。
- 验证：未重新打包（用户要求先停打包做同步）。

### 2026-08-28 对齐会话生命周期三原语命名（d256317e 结构层）

- 分支：产品线 `main`（叠在启动/环境栈第 1–5 步之上）。
- 镜像：`winehua/master` @ `d256317e`。
- 原则：只在 `WineEngineService` 上挂齐 `startSession` / `stopSession` / `wipeEnvironment` 三个名字，方便后续上游重构落到同一结构。不上游 `WineEnvService` 类、不引入前台 overlay / `opBusy` / `EngineGuide`、不改 `AppSessionService` / 设置页调用方。
- 映射（行为与改前相同）：

  | 上游原语 | 本仓实现 | 产品入口（保留） |
  | --- | --- | --- |
  | `startSession` | 原 `ensureReady` 体（幂等；首启/二启仍由 native prefix 决定） | `ensureReady` 转发 |
  | `stopSession` | 原 `stopAll` 体（NAPI `stopAll` + 35s drain + 300ms socket settle） | `stopAll` 转发 |
  | `wipeEnvironment` | `resetWinePrefix` + `forceRefreshRuntime`（不清整棵 `WINE_ROOT`） | `resetPrefix` 的清空半段 |

- 组合未改：`restart` = `stopSession` + `startSession`；`resetPrefix` = `stopSession` +（可选文档/注册表备份）+ `wipeEnvironment` +（还原）+ `startSession`。ERROR + 仍有活进程时仍等待、不双开。
- 验证：`make hap`（winehua-dev / vp-build，arm64-v8a，API 23）CompileArkTS + 签名通过。调用方与 UI 未改，未做真机动作回归。

### 2026-08-27 对齐 WineHua master 启动/环境栈第 5 步

- 分支：`feature/align-env-spawn-1-4`（叠在第 1–4 步之上；真机已验证 1–4 可玩后再做本步）。
- 镜像：`winehua/master` @ `d256317e`。
- 原则：按上游 `f9aaaaed` 把 wineserver/wineboot 也改走 broker 单一通道，删除 Spawner 的 NCP 直启与 `libwine_child.so:WineserverMain` 入口。ArkTS 仍是 `WineEngineService` + `launchClient` 9 参；不上游 `WineEnvService` / `Box64Dynarec.ets` / `dxvkBackend`/`wineLang`。
- 已落地（对照上游 SHA，本地适配而非整 commit cherry-pick）：

  | 上游 SHA | 说明 |
  | --- | --- |
  | `f9aaaaed` | Spawner 全部 kind 经 `SpawnViaBroker`；broker 先于 wineserver 启动并尾部追加会话 `WINEPREFIX`；`wine_child Main` 截获 `argv[0]==wineserver` 转入 `RunWineserver`；删除 `WineserverMain` NCP 导出 |

- 私有不变量：`@engine/wineserver` / `@engine/wineboot` 登记仍在 `wine_launch` 拿到 pid 之后覆盖 broker 的 basename 登记；virgl host / 手机 fork 不进 Spawner；Box64 policy 仍在 `AppModels.resolveBox64PresetEnv`；wineboot 进展看门狗与首启重试保留。
- 已知副作用（与上游相同）：wineboot 经 broker 登记会短暂出现在任务列表，随后被 `@engine/wineboot` 覆盖。
- 验证：`make test`；`make hap`（winehua-dev / vp-build，arm64-v8a，API 23）。第 1–4 步真机已过；本步需再做一次冷启动/二启。
- 子模块 gitlink：未跟随上游。

### 2026-08-27 对齐 WineHua master 启动/环境栈第 1–4 步

- 分支：`feature/align-env-spawn-1-4`（从 `feature/host-fps-hud` 切出，含 broker connect 探测、wineserver `Main` 截获、35s drain）。
- 镜像：`winehua/master` @ `d256317e`。
- 原则：按上游 `b04e3410` / `18b6f5a5` / `f3370b05` / `fed07ecf` 落地 EnvSpec / 基线单源 / Profile 管线 / Spawner，行为保持。第 5 步见上一节。ArkTS 仍是 `WineEngineService` + `launchClient` 9 参；不上游 `WineEnvService` / `Box64Dynarec.ets` / `dxvkBackend`/`wineLang`。
- 已落地（对照上游 SHA，本地适配而非整 commit cherry-pick）：

  | 上游 SHA | 本仓提交 | 说明 |
  | --- | --- | --- |
  | `b04e3410` | `175ed930` | EnvSpec 收口 entryParams 序列化；`ohos_broker.c` 只互指、不改 wine gitlink |
  | `18b6f5a5` | `af871191` | `wine_env_baseline.h` 公共键；Box64 出厂表仍用本仓 `SetBox64PerfEnv` |
  | `f3370b05` | `3725dfb5` | `BuildSessionEnv` 单一策略点；私有 host-shadow / `WINEHUA_PERF_PROFILE` overlay 迁入 Profile |
  | `fed07ecf` | `d00df45b` | Spawner：Wineserver/Wineboot → NCP；DesktopShell/WineExe/GuestElf/HostElf → broker |

- **第 5 步**：已在同日后续提交落地，见上一节。
- 私有不变量：`@engine/wineserver` 登记仍在 `wine_launch` 拿到 pid 之后；virgl host / 手机 fork 不进 Spawner；Box64 policy 仍在 `AppModels.resolveBox64PresetEnv`。
- 验证：`make test`（geometry/blit/env_spec/env_baseline）；`make hap`（winehua-dev / vp-build，arm64-v8a，API 23）。**未做真机冷启动回归**（桌面 + 开一个 exe）。
- 子模块 gitlink：未跟随上游。

### 2026-08-27 WineHua master 增量（90edaae..d256317e）

- 分支：`feature/host-fps-hud`（基于 VintagePomeloPro `origin/main` `d73f299f` + 宿主 FPS overlay）。
- 镜像：`winehua/master` @ `d256317e`。
- 原则：不整包吸收 EnvSpec / Profile 管线 / SpawnRequest+Spawner / 会话三原语重命名；只移植有独立行为价值的修复。私有 `launchClient` 仍为 8 参 + `compatEnvStr`；不上游 `WineEnvService` / `Box64Dynarec.ets` / 品牌 / 版本号。
- 已手工移植（上游 SHA → 本地适配）：

  | 上游 SHA | 说明 |
  | --- | --- |
  | `f9aaaaed` 行为子集 | broker 就绪改真实 `connect` 探测 + probe EOF 降 INFO；`IsBrokerWineserverRequest` 跳过 homeDir+binDir 两段路径；`Main` 截获 `argv[0]==wineserver` 交给 `WineserverMain`（不删 NCP `WineserverMain` 入口） |
  | `18b6f5a5` 行为子集 | `WineserverMain` Box64 基线先于 `__env` apply，删除 `BOX64_DYNAREC_*` 二次重放 |
  | `d256317e` 行为子集 | `waitForNativeShutdown` 5s→35s，覆盖 native drain 30s + tsfn 余量 |

- 跳过（架构/文档/已由私有路径覆盖）：
  - `3b589770` `95d03f95` `ade24e0f`（docs）
  - `b04e3410` `18b6f5a5` `f3370b05` `fed07ecf` `f9aaaaed` 的 EnvSpec/Spawner/「全部 wineserver 改走 broker、删除 NCP 直启」
  - `3720e13a`（`Box64Dynarec.ets` 单源化；本仓 `FilterCompatLines` 已用 `BOX64_DYNAREC_` 前缀门）
  - `d256317e` 的 `WineEnvService` 类本身（本仓仍用 `WineEngineService`；三原语名字已于 2026-08-28 挂到该类上，见上一节）
- 子模块 gitlink：未跟随上游。
- 验证：`make hap`（winehua-dev / vp-build，arm64-v8a，API 23）。

### 2026-08-25 WineHua master 增量（e16d79b..90edaae）

- 分支：`feature/sync-winehua-90edaae`（基于当前产品线 HEAD，含 PC 虚拟桌面光标隐藏门禁）。
- 镜像：`mirror_master` 跟踪 `winehua/master` @ `90edaae`。
- 原则：只吸收 Box64 兼容档位 native 通道；不引入上游 `Box64Dynarec.ets` / `WineEnvService` / 品牌 / 版本号 / CI / `dxvkBackend`+`wineLang` 第 9–10 参。
- 已移植（上游 SHA → 本地适配，非整 commit cherry-pick）：

  | 上游 SHA | 说明 |
  | --- | --- |
  | `7cff882` | `compatEnvStr` 通道：wineboot / wineserver / explorer 三路注入 |
  | `dcf3906` | `FilterCompatLines`、automation 跳过、wineserver 二次只重放 `BOX64_DYNAREC_*`、napi 返回码校验 |
  | `90edaae` | 注入调用点 `__aarch64__` 守卫 |

- 私有适配：
  - `launchClient` 本仓仍为 8 参 + 新增第 9 参 `compatEnvStr`（不上游 11 参）
  - 档位表继续用 `resolveBox64PresetEnv`（不含 WEAKBARRIER / VOLATILE_METADATA）
  - `WineEngineService.ensureReady` 把全局 `box64Preset` 编成分号串下发
- 跳过：WineHua `Box64Dynarec.ets`（当时尚未接到设置页，且引用上游 service 名）；`rc-1.0.15` / `rc-1.0.16` 标签；`winehua/main-ui`。
- 子模块 gitlink：未跟随上游。

### 2026-08-24 WineHua master 增量（10a9e6c..e16d79b）

- 分支：`feature/sync-winehua-10a9e6c`（基于 VintagePomeloPro `main` `c13e132`）。
- 镜像：`mirror_master` @ `e16d79b`（`winehua/master` 本地只读跟踪）。
- 原则：保留本仓 UI（浮窗桌面、PC 沉浸、DesktopAbility 模型、虚拟输入、产品身份）；只 overlay 上游输入/渲染功能。`10a9e6c` 与 `189c27c` 同题，只 pick master 线 `189c27c`。
- 已 cherry-pick -x：

  | 上游 SHA | 本地 SHA | 说明 |
  | --- | --- | --- |
  | `189c27c` | `9ed4594` | 光标隐藏/相对模式门禁：展示形态 + desktop root surface；native `toplevelId` 回调 |
  | `15c53ee` | `98647a4` | 虚拟桌面 resize 后 `sizeDirty_` 强制 letterbox 重绘 |

- 跳过：
  - `98eaca5`（changelog CI，上一轮已同步）
  - `e16d79b`（CI `BUILD_WINE_MONO=1`；私有 `vpbuild.sh` 已默认启用）
  - `10a9e6c`（与 `189c27c` 重复，未双 pick）
- 冲突处理（要点）：
  - `pointer_extras`：保留 per-surface `SendRelativeMotion` / `HasRelativePointerForSurface`；叠加 `ApplyHostCursorLock(lock, toplevelId)` + desktop shell `isShell` 跳过冻结
  - `WineWindowManager.ets`：吸收 `canHideCursor` / `applyPointerVisibility`；门禁适配私有 `desktopLauncherVisible` + `desktopAbilityForeground`（不引入上游 VirtualDesktopAbility）
  - `egl_renderer`：采纳 `sizeDirty_` 路径，保留 `skipFrames_` 诊断计数
- 子模块 gitlink：未跟随上游。
- 验证：见合入 `main` 时的 `make test` + `scripts/vpbuild.sh make hap` 记录。

### 2026-08-23 WineHua master 输入/合成器同步（ff76a8f..98eaca5）

- 分支：`feature/sync-winehua-ff76a8f`（基于 VintagePomeloPro `origin/main` `7986595`）。
- 原则：保留本仓 `main` 的 UI 与既有修复（浮窗桌面可恢复、PC 沉浸全屏还原钮、蓝牙键盘走桌面 XComponent、宿主 IME、1.2.8 产品身份）；只 overlay WineHua 输入/合成器功能。冲突文件从不整文件采用上游。
- 已 cherry-pick -x（功能链，上游 SHA → 本地）：

  | 上游 SHA | 本地 SHA | 说明 |
  | --- | --- | --- |
  | `ff76a8f` | `fea424b` | dinput 相对模式：ArkTS `rawDelta` + `OH_WindowManager_LockCursor` |
  | `dd6e11d` | `0bf9d17` | 合成器快照 + 锁外 blit |
  | `6d4eb5a` | `bfbc0d0` | 桌面合成局部化 R + BlitScaled `-O2` |
  | `c4e0e01` | `d3333e3` | 相对灵敏度固定系数 2.5（随后收口到 ArkTS） |
  | `cf63187` | `d543481` | BlitScaled 拆文件 + host 测试 |
  | `98b87ce` | `aeee93b` | 回退 4 采样；`-O2` 挂到 `compositor_blit.cpp` |
  | `9742eba` | `66728dd` | RA2 诊断日志（随后被 `affcb98` 清掉） |
  | `498d873` | `1fa9d7f` | 相对模式冻结死锁：IPC 出 mutex；冻结挂到 relative_pointer |
  | `f5999b6` | `7014887` | 全屏 Toplevel R = fit 后内容区 |
  | `7a805bd` | `21a8948` | 全屏 SHM 直传放宽 |
  | `7d68fa8` | `158666d` | 输入坐标锚定桌面系（直传点击不错位） |
  | `affcb98` | `fefed3e` | 高危 4 项：partial 黑底裁剪、tsfn 原子、LockCursor 工作线程、剩余 0 才解锁 |
  | `110b24f` | `e1eb62c` | `InputDeviceMapper`；scale 收口 ArkTS |
  | `8918fe2` | `400cfdc` | DesktopLayer 补同一套设备校准 |
  | `de57241` | `ed2ff8b` | 桌面中键 BTN_MIDDLE（只改 DesktopLayer） |
  | `794cc9a` | `cba2e57` | 指针链路日志两处修复 |

- 跳过：
  - `13236b5`（Merge #66 VKD3D/DXVK）— 本仓已由 `0533eab` / `bdc895f` 移植
  - merge commits `1d85ec6`、`550afff`
  - `93c00ab`（版本 1.0.7→1.0.13）
  - `06013b3` `b34e16a` `98eaca5`（CI / README / changelog）
- 冲突处理（要点）：
  - `CMakeLists.txt`：保留 `app_log.h` force-include、`libz.so`、`libnative_window_manager.so`；BlitScaled `-O2` 跟 `compositor_blit.cpp`
  - `pointer_extras`：保留按 surface/client 的 `HasRelativePointerForSurface` / `SendRelativeMotion(surface, …)`；冻结与 IPC 按上游迁到 relative_pointer + 工作线程
  - `input_manager`：相对增量用 ArkTS 已缩放的 rawΔ，但仍按 **当前 surface** 判定相对模式，不套全局 `HasRelativePointer()`
  - `DesktopLayer.ets`：叠加 rawDelta 设备校准与中键；保留 `floating` 旁路、`eventGate`、XComponent `wine_desktop_xc` 键盘焦点
  - `DesktopWindow.ets`：整页保持本仓 PC 沉浸还原钮，不引入上游旧页内 onMouse（鼠标在 DesktopLayer）
  - `VirtualDesktopAbility.ets`：不恢复（本仓已删除）
- 子模块 gitlink：未跟随上游 wine/dxvk/vkd3d 指针。
- 验证：`make test`（geometry 52 + blit_scaled 402，0 failures）；`scripts/vpbuild.sh make hap`（winehua-dev，arm64-v8a，API 23）`BUILD SUCCESSFUL`，签名 HAP `entry/build/default/outputs/default/entry-default-signed.hap`。

### 2026-08-09 版本 1.1.7：sRGB、游戏鼠标与虚拟输入方案

- 上游核对：`git fetch origin` 后 WineHua `master` 仍为 `1036ada`，本轮没有新的
  WineHua 主仓提交需要合并。
- VirGL：通用修复提交到 `winehua/virglrenderer`
  `fix/vrend-srgb-write-policy` @ `f49d7da6`；父仓 `a8c9d68` 更新 gitlink。
  策略由首个相关 framebuffer 一次性选择 RGBA preserve 或 XRGB 软件编码，后到
  的无关附件不能反向污染。无游戏、路径、GPU 或设备特判。
- 游戏鼠标：`b8167cf` 统一 overlay window 坐标；empty-input 呈现 subsurface
  穿透父 toplevel；relative-pointer 按 client/surface 路由并在坐标空间切换时
  首帧 rebase；fullscreen 取消/延迟系统窗口 raise。
- 虚拟输入：`7b1c391` 增加通用、全键盘、RPG、射击、动作五套模板；方案弹窗、
  新建命名、重命名/删除、自适应手机/平板布局，以及 Shift/Ctrl 锁定模式。
- 验证：输入模型测试通过；宿主几何 52 项通过；Docker ARM64 API 23 HAP 构建
  成功并覆盖安装；用户真机确认 PAL4、PAL5（含房屋材质）、灰色的果实、游戏内
  鼠标和新输入方案均正常。完整维护边界见
  `PRIVATE_1_1_7_RELEASE_AND_MERGE_MEMO.md`。

### 2026-08-06 合成器 Layer 重构全链合并（d9c667e..1036ada + 先前暂缓链）

- 分支：`feature/20260803-master-sync`（基于 1.1.5 前 `ac788e0`）。
- 背景：`d9c667e` 之后的 5 个提交（`94077be` 方案B 单一 Z 序命中、`2386c5f` 几何收敛、`7a59c00`/`c35ac03` 层序/blit 收敛、`1036ada` 文档）依赖此前两轮「暂缓」的合成器重构链，故本次把整条链按序 cherry-pick -x 合入：

  | 上游 SHA | 私有 SHA | 说明 |
  | --- | --- | --- |
  | `76a2cd4` | `810e632` | 阶段1 Layer 容器（合成/输入同源层序） |
  | `d5deed7` | `2024706` | 阶段2 ZC 入层（GL 画面可被遮挡） |
  | `6df338a` | `9b3a8ed` | 阶段3 PC 窗口内 Layer 收敛 + ZC 状态单一化 |
  | `c2bd0ee` | `ad12797` | 阶段4 全屏目标单一化（fs-pick 纯函数） |
  | `8ab97c3` | `fdf4c5e` | 层序跳过规则与全屏几何收敛到对象方法 |
  | `13cc583` | `fbe84d2` | ToplevelState 完整封装（字段私有 + 语义方法） |
  | `94077be` | `d63e63d` | 方案B 单一 Z 序命中循环统一输入命中 |
  | `2386c5f` | `a1218fe` | 几何收敛 FitMapLayerRect + ResolveRootSize |
  | `7a59c00` | `0443ea0` | ShouldSkipFullscreenCascade 谓词统一连带跳过 |
  | `c35ac03` | `88ff56b` | 像素 blit 收敛 BlitClipAlpha（阶段3a） |
  | `1036ada` | `6fdb30d` | 文档：固化全屏判定两套语义 |

- 冲突处理（2 处）：
  - `94077be`：私有线 2026-08-05 手移的 `e5cd7fa`「前置命中」分支被上游方案B 取代 → `input_resolver.cpp/.h` 直接采用上游版本（校验与上游逐字节一致），删除前置命中块。
  - `7a59c00`：`desktop_compositor.cpp` blitSubsurface 的连带 fullscreen 跳过条件改为统一的 `ShouldSkipFullscreenCascade` 谓词（与输入命中同源）。
- 跳过：链内 docs/checkpoint 提交（`c5e487e` `eaaeaeb` `6aaf6fd` `ea614cd` `4b82709` 及链外纯文档）不重复引入；`996aabb..d9c667e` 段已由上一轮同步覆盖。
- 私有保留：桌面全屏零拷贝、phone in-process VirGL、`@engine/` 核心进程登记、`desktop_root_manager` 语义均未受影响（本链只改合成/输入层内部）。
- 验证：`make hap`（winehua-dev 容器）构建成功，1.1.5/1001005 arm64 HAP 签名完成；`git diff --check` 通过；子模块 gitlink 无变化。

### 2026-08-05 增量同步（996aabb..d9c667e）

- 分支：`feature/20260803-master-sync`（基于 1.1.4 `acaf19e`）。
- 上游增量 11 个提交，采纳 3 项（手动移植）：
  - `8fb8488` → broker.cpp 对全部 SPAWN 请求统一 `AddProcess`（explorer 内双击的 exe 进入任务列表），`ParseProcessName` 兼容 homeDir/binDir/`__winehua_*` 标记段；wine_process.cpp basename 兼容 `\` 反斜杠路径。私有 Index 已有 1.5s 轮询刷新，未做 process-updated 推送节流（后续可加）。
  - `e5cd7fa` → 私有 `input_resolver.cpp` 全屏独占分支前加入前置命中：遍历 z-order 中高于全屏窗口的 toplevel 及其 subsurface（跳过连带 fullscreen 的旧窗口，与渲染侧对齐），修复"全屏游戏时新窗口/菜单显示在上方但点击回到游戏"。
  - `bb617a4` 部分 → `wine_env.cpp` 的 `UpsertEnvLine` 改为"删除全部同 key 再追加"，避免 AppendProductDxvkEnv 覆盖 WEAKBARRIER 等产生重复 key。
- 跳过并记录：`7ed8ad2`（dinput_click_probe 私有 wine 子模块已含）、`faf98af`（私有 CI 已装 curl，mono 下载成功）、`d3688e1`（BOX64 守卫私有已有）、`d9c667e`（私有 `@engine/explorer` 登记体系语义不同）、docs/版本号（`c5263a3` `82ee3f3` `1dc0283` `70abb0b`）。
- 验证：`make hap`（winehua-dev 容器）构建成功，1.1.4/1001004 arm64 HAP 签名完成。

### 2026-08-03 二次合并：warm-prefix explorer 恢复修复

- 上游 `996aabb` → 私有 `f2f9cfe`（cherry-pick -x，仅 CI workflow 冲突且保留私有侧）：
  - 温前缀（prefix 已初始化）时显式用干净 NCP 环境跑 `wineboot --init`，播种 boot 事件，避免 explorer 首客户端触发的 wineboot 卡死导致后续所有 Wine 进程阻塞在 boot-event 等待（与私有线"二次启动无窗口/所有卡片失效"现象同源）；
  - 非桌面模式自动 explorer 改为走 `SpawnWineProgram`（broker 通道），与手动启动路径一致；
  - `scripts/build_deps.sh`：BUILD_WINE_MONO 默认启用（`BUILD_WINE_MONO=0` 跳过），与私有打包约定一致。
- 暂缓/跳过：`13cc583` `8ab97c3`（合成器重构延续，私有 compositor 冲突）、`82ee3f3`（docs）、`c5263a3`（.gitignore）。

### 2026-08-03 三项修复 + 合并 Aug 3 输入提交

- 分支：`feature/20260803-master-sync`（基于 `private/wine-engine-app` `b66318b`）。
- 上游同步：合并 Aug 3 `8089968`（zwp_relative_pointer_v1 取代 warp 补偿，修红警2光标偏移/PAL2点击瞬移）→ 私有 `0e2a86e`，无冲突 cherry-pick（本地输入代码与上游父提交一致）。
- 排除记录：合成器 Layer 重构（阶段 1-4 `76a2cd4` `d5deed7` `6df338a` `c2bd0ee`）与本地私有 compositor 架构（桌面全屏零拷贝、phone in-process VirGL 直连）冲突大、与三项修复无关，维持暂缓；Aug 1-2 输入/mono/字体提交本地已有等价实现，不重复合并。
- 三项修复：
  - `e837ceb` Fix 1：删除 `RunWineExe` 的残留单例复用（进程退出后登记未清 → 二次启动返回死 pid）。对齐 master，每次经 broker 新建；ArkTS `result.reused` 分支恒 false 安全。
  - `10105b5` Fix 2：新增全局设置 `desktopWindowMode`（全屏/切边安全区）。圆角屏全屏遮挡开始菜单；平板默认切边，`DesktopAbility` 按设置应用 `setWindowLayoutFullScreen` + 系统栏显隐，并在窗口再次打开时重应用。
  - `ab4fdab` Fix 3：停止程序改为杀死整棵 wine/box64 进程树（`KillProcessTree`，后代先杀再杀根），并立即触发 Wayland toplevel `destroyed` 事件 + `pid:exited` 状态消息，使 ArkTS 关窗与运行状态即时联动，不依赖断连异步时序。
- 构建验证：目录/模型单测 34 项通过；Docker `winehua-dev` ARM64 Debug HAP（API 23、`com.vintage.pomelopro` 1.1.2/1001002、旧柚Pro、仅 arm64）打包+签名成功；包内 guest-gfx、图形/音频 smoke、wine-mono-11.1.0 msi、dxvk legacy x64/x86 全量 DLL 完整；`hap-sign-tool.jar verify-app` 验签通过。
- 产物：`F:\PomeloWin\artifacts\VintagePomeloPro-1.1.2-master-sync-20260803\旧柚Pro-1.1.2-master-sync.hap`，SHA-256 `021FB252CF69D420CDFCD09CA3F5299950DAB2D95B7F043519A0946695BB8A60`。
- 未覆盖：平板当前离线，未做真机回归（二次运行/切边显示/杀死联动三项仅代码与包级别验证）；合入 `private/wine-engine-app` 并推送 `VintagePomeloPro:main`。

## 2026-07-18 私有基线

- 来源：本地 `VintagePomeloMaster` @ `ba7218a`
- 纳入：1.0.5 配置与品牌图标、guest-gfx 修复、项目构建技能。
- 目的：在不移动现有分支的前提下建立 VintagePomeloPro 私有主工程。
- 验证：原工作区状态/diff、现有分支指针、`origin` 和 `.gitmodules` 均保持不变。

## 上游提交

### 2026-07-18 Wayland 窗口与图形修复

| 上游 SHA | 私有分支 SHA | 范围 | 选择原因 | 合并结果 |
| --- | --- | --- | --- | --- |
| `f5ad791` | `d998e83` | Wayland popup、ArkUI 子窗口 | 修复 PC 模式弹出菜单被窗口边缘裁剪 | 与双显示模式的 `DisplayMode` 导入合并，保留 popup 管理器和单应用模式 |
| `7f6b0ae` | `3548ae8` | Wayland/EGL alpha 合成 | 支持 Pad 端异型窗口透明混合 | 无冲突同步 |
| `4b0c3db` | `8effc60` | PC ARGB 子窗口、窗口 mask、NAPI | 支持分层/异型窗口并保留 popup 回退宿主 | 合并 `sessionId/clientPid` 归属；PC 窗口延迟到首帧分类，桌面/单应用合成继续携带完整会话信息 |
| `2afd8bf` | `133464e` | EGL overlay、Wayland z-order 与输入命中 | 修复桌面模式 zero-copy 内容错误置顶及点击命中 | 无冲突同步 |

- 跳过：品牌、发布、实验性首页和与 Wine 引擎化无关的提交。
- 子模块：未更新 gitlink，未修改 `.gitmodules` 或第三方 URL。
- 静态验证：冲突标记清除，`git diff --check` 通过。
- 构建验证：Docker 中 API 22 ARM64 `assembleHap` 成功，CMake/Ninja、ArkTS 和 HAP 打包均通过；包内仅含 `arm64-v8a` 原生库，并包含 Wine/Box64/guest gfx 运行时。
- 规则验证：目录与模型规则测试 15 项通过，覆盖 EXE 选择、封面优先级、路径规范化、稳定卡片 ID、显示模式和引擎状态转换。
- 签名验证：release HAP 使用 v3 签名块，官方 `hap-sign-tool verify-app` 验证 26 个 ARM64 原生库、证书链和 SHA-256 摘要通过。
- 真机状态：本轮 HDC 无在线目标，因此未执行安装与真机 UI/输入回归；产物已完成构建、release 签名和离线验签。

后续仍仅接受 Wine/Box64、Wayland、图形/音频/输入、HarmonyOS API 或构建运行时修复；每个提交继续在此记录来源 SHA、文件范围、选择原因和验证结果。

### 2026-07-23 PR #34 手机与 TV 运行后端

- 来源：WineHua PR #34，提交 `516c420`、`5c56bfc`、`3a80575`。
- 目标分支：`feature/phone_support`，基于私有 `main` 的 `2d0ceac`。
- 纳入：phone/TV 的 fork NativeChildProcess 后端、phone 横竖屏尺寸同步、TV 设备声明，以及设备能力分流。
- 设备范围：fork 后端仅在 `phone` 和 `tv` 启用；`tablet` 保持原桌面合成路径；`2in1` 和 `pc` 继续使用系统 NCP、Binder 和独立 Wine 窗口。
- 图形调整：未采用 `3a80575` 的 VirGL socketpair Surface relay 和 shm 降级。phone/TV 改为在应用进程的专用线程中运行 VirGL host，并通过窄 C 接口直接绑定现有 `OHNativeWindow`；帧仍提交到 SurfaceQueue/NativeBuffer。保留的 Unix socket 仅承载 x86_64 guest Mesa 与 VirGL/vtest host 之间的命令和资源协议。
- 隔离：Wine/wineserver 仍由 fork shim 在子进程运行；VirGL 不通过 fork shim。未修改任何子模块 URL、gitlink 或第三方源码。
- 静态验证：`git diff --check` 通过；ARM64 和 x86_64 的 `libvirgl_child.so` 均导出五个进程内控制符号，`libentry.so` 均不静态依赖 `libvirgl_child.so`。
- 规则验证：目录与模型规则测试 24 项通过。
- 构建验证：使用 `winehua-dev` 和 Makefile Docker 链路分别完成 API 23 x86_64 与 ARM64 Debug HAP；两者均启用 `BUILD_GUEST_GFX=1`，guest ABI 均为 x86_64。
- 包验证：两个 HAP 均为 `com.vintage.pomelopro` 1.0.8、单一目标 ABI，并包含 guest Mesa/VirGL 环境、`virtio_gpu_dri.so`、图形 smoke 和音频 smoke；官方签名工具验证通过。
- 未覆盖：phone/TV 真机上的 fork Wine、旋转、VirGL 重绘和触摸输入仍需设备回归，不能用 x86 模拟器或 ARM 平板结果替代。

### 2026-07-27 桌面全屏合成器与自动构建

| 上游 SHA | 私有分支 SHA | 范围 | 选择原因 | 合并结果 |
| --- | --- | --- | --- | --- |
| `c00efd8` | `ca10e8c` | Docker 构建配置与签名挂载 | 让自动/容器构建可导入用户配置 | 无冲突同步 |
| `954730e` | `2d5f754` | GitHub Actions 无签名 HAP 构建 | 提供上游自动构建 | 无冲突同步 |
| `a287fe5` | `df8d913` | 合成器模块化与 Wine OHOS 适配 | 是后续全屏渲染与输入修复的基础 | 采用新的桌面合成器；保留私有 NCP shim、应用内 VirGL、手柄桥接与旧柚桌面输入页 |
| `42eb250` | `ac75ed9` | app_id 语义与 ghost desktop-shell 过滤 | 防止 Explorer/桌面壳窗口参与错误命中 | 同步并更新 Wine gitlink |
| `68555ed` | `e352950` | 桌面全屏零拷贝、warp 与 pointer constraints | 修复全屏比例、黑边点击和 dinput 贴边 | 无冲突同步 |
| `ed8dcac` | `e747937` | 全屏前台优先级 | 修复旧窗口连带全屏抢输入 | 无冲突同步 |
| `65cc779` | `9581efd` | Wine shell32 desktop.ini CLSID 回退 | 修复 Explorer 文件夹处理回退 | 子模块无共同线性祖先，已明确切至已审查提交 |

- 跳过：`7d9c7ec` 的 wine32 构建改动已等价存在；`c8e94b3` 会将手机 VirGL 改为 fork/socket relay，违背私有分支保留的应用内直连 Surface 架构；`3331df7` 会把产品版本倒退到 1.0.3。
- 冲突处理：保留旧柚 Pro 的 ArkTS 页面、设备分流和 `ncp_shim`；采用新的桌面合成器、全屏输入/渲染栈和 Wine 适配。
- 验证：目录与模型单元测试 24 项通过；Docker/Makefile ARM64 Debug HAP 构建成功，目标/兼容 API 均为 23，guest ABI 为 x86_64。包内包含 ARM64 的 Box64、entry、wine child、VirGL/Wayland 运行库和 `wine-data.zip`；嵌套运行时包含 guest-gfx 环境、EGL、virtio GPU 驱动及图形/音频 smoke。官方签名工具验证通过。
- 兼容修复：私有 ArkTS 仍调用 `setDisplayScale`，上游合成器已从 `EglRenderer` 移除全局缩放。该 NAPI 导出保留为兼容空操作，实际渲染和输入变换由新合成器测量输出几何统一计算。
- 未覆盖：尚未在物理 ARM 设备上回归零拷贝全屏、pointer warp/constraints 与私有手机应用内 VirGL；不能由本次离线构建替代。

### 2026-08-02 dxvk 1.10.3 合并（版本 1.1.2 / 1001002）

- 来源：WineHua `origin/master` `0ed802c`，按时间序 cherry-pick 36 个提交（dxvk legacy 1.10.3 phase-2、Venus 呈现/阴影上传优化、wine/PE 修复、d3d8 兼容子模块指针、guest-gfx/guest-vulkan 打包等），2 个跳过（`86838e2`、`6d64109`，仅改上游 Index/DesktopWindow 页）。14 个子模块 gitlink 与上游一致（dxvk=`abe71bc` v1.10.3-28；wine/mesa/virglrenderer=d3d8 兼容分支提交）。
- 架构取舍：native 层对齐上游最终版；私有文件保留（`game_controller_bridge.*`、`ncp_shim/*`）；UI 与私有运行时 API 恢复自产品线（test 分支）——`runWineExe`（含 sessionId）、`checkWinePrefix`、`setForkNcpEnabled`、手柄回调等导出保留。
- 关键修复（均为设备实测驱动）：
  - 打包：DXVK Legacy 全量 DLL（d3d9/d3d10core/d3d10/d3d10_1/d3d11/dxgi ×x64/x86）、`bin/Alarm01.wav`、`bin/x86_64-windows` smoke 必须齐备，否则引擎初始化失败；
  - wine-mono 11.1.0 与 `appwiz.cpl` 必须同包（缺失时 wineboot 弹框阻塞前缀初始化，导致音频驱动/图标缓存缺失）；assemble 增加守卫；
  - DXVK 性能：当时启动前使用 `setHostShadowProfile('shadow-precise-dirty-ring-inline-upload-coverage-sort')`，立方体 4 FPS → 84 FPS；当前等价配置已由 Native `GraphicsProfile` resolver 统一生成；
  - games 目录：`bundleManager.getBundleInfoForSelfSync` 动态取包名 + 写探针，杜绝硬编码/假 ready；
  - 标题栏高度：`componentUtils.getRectangleById('HdsTitleBar')` 运行时实测，替代硬编码。
- 验证：ARM64 Debug HAP（API 23、`com.vintage.pomelopro` 1.1.2/1001002、旧柚Pro、仅 arm64、guest-gfx+dxvk+mono 载荷完整，官方签名工具验签通过）；平板上引擎 READY、桌面 100+ FPS、DXVK 立方体 84 FPS、干净安装后音频与 Wine 内 EXE 图标恢复。
- 发布：合入 `private/wine-engine-app` 并显式推送到 `VintagePomeloPro:main`（版本 1.1.2）。

### 2026-08-03/04 真机验证三项修复（平板 API 24）

- 装机方式：因签名不一致先卸载再安装 debug HAP（games 目录在共享 Download，不受影响；Wine prefix 重建）。
- Fix 1（卡片二次启动/唤起）：`created` 事件携带 `sessionId/clientPid` 后，运行中再点卡片正确唤起（`onNewWant` 指向新 toplevel）；杀死后重新启动正常，引擎全程 READY。
- Fix 2（切边/圆角避让，最终方案）：切边=沉浸全屏（隐藏系统栏）+ **只缩左右宽度、高度充满**，边距按设备自动估算（显示短边 5%，夹 40–100px；本机 2560×1600 → 80px）；explorer 启动前预置输出 1200×800，任务栏/开始按钮出生即在正确位置；XComponent 与输入覆盖层（InputOverlay）同步左右内缩，虚拟鼠标/触摸坐标与 surface 对齐（点开始按钮成功弹出开始菜单）。
- Fix 3（杀进程联动+桌面保活）：`KillProcessTree` 杀整树 + 立即触发 toplevel destroyed；wineserver/explorer 登记为 `@engine/` 核心进程，用户程序退出/被杀后注册表保持非空 → 引擎保持 READY、桌面不拆。
- 构建：`scripts/vpbuild.sh` 复用常驻容器 `vp-build`（不再每次 docker run 新建容器）；Docker `winehua-dev` ARM64 Debug HAP 验签通过。
- 产物：`F:\PomeloWin\artifacts\VintagePomeloPro-1.1.2-master-sync-20260803\旧柚Pro-1.1.2-master-sync-automargin.hap`。
- 结论：用户真机确认“好用”，本地提交当前版本（`644f6de`），未推送。

### 2026-08-04 二次 master 更新 + DXVK 方针对齐 + 版本 1.1.3

- 上游合并（996aabb..origin/master 共 6 提交，选 2 合 4 跳过）：
  - `d3688e1` → `fd98cbb`：所有 BOX64 环境变量加 `__aarch64__` 守卫（修复 x86_64 USE_LIBBOX64 导致 broker entryParams 错乱）；私有 `AppendProductDxvkEnv` 内的 BOX64 变量同步加守卫；
  - `7ed8ad2` → `5afcaaf`：dinput_click_probe 迁移到 wine 子模块，wine gitlink `3a69dcad` → `11e59500210`（线性后代，仅新增探针）；已重建 wine；
  - 跳过：`70abb0b`（上游版本号）、`1dc0283`（LGPL 文档）、`82ee3f3`/`c5263a3`（docs/.gitignore）。
- DXVK/VirGL 方针对齐（`1dbed33`）：DXVK override 从全 D3D native 改回 master 的 `d3d11=n;dxgi=n`。原因：Venus/Maleoon 栈上 DXVK 对 DX9/10 兼容性弱于 WineD3D→GL→VirGL，全 D3D 走 DXVK 会破坏 VirGL 驱动游戏；上游评估文档亦确认 Maleoon 910/920 达不到 DXVK 2.x 基线，Modern 仅作独立 profile 待能力门禁。真机回归：D3D11 立方体 dxvk_legacy 80+ FPS 正常。
- 版本：1.1.3（1001003）。
- 产物：`F:\PomeloWin\artifacts\VintagePomeloPro-1.1.3-20260804\`（debug HAP `74F18840…`、release APP `02A6599A…`、release entry HAP `EB159976…`，均验签 Verify success）。

### 2026-08-14 TLS 网络修复 + gstreamer 子模块修补（版本 1.2.1 / 1002001）

- 根因（设备实测）：Box64 0.4.3 dynarec 误译 GnuTLS 的 AES-NI/PCLMULQDQ（AES-GCM 加速路径）与 ChaCha20/Poly1305（SSSE3 路径），HTTPS 响应解密得到乱码（WinHTTP 12152 / 百度 https 400）或 access violation。解释器模式下全部通过（Steam 200 / 百度 200 / msedge 404，与 Windows 基线一致），故非 UA、非环境、非 Wine 代码缺陷。
- 修复（最小改动）：
  - `wine_env.h`：Box64 模拟 cpuid 屏蔽 `BOX64_AES=0`、`BOX64_PCLMULQDQ=0`，GnuTLS/nettle 与 Steam 静态 OpenSSL 1.1.1i 回退纯 C AES-GCM；
  - wine 子模块 `schannel_gnutls.c`：schannel 优先级禁用 `CHACHA20-POLY1305`，强制协商 AES-GCM（与 Windows schannel 一致）；
  - glib `meson.build`（format-security/format-nonliteral 容忍）、pcre2 `config.h.in`（autoconf 重生成）：gstreamer 链 OHOS musl 交叉编译修补。
- 验证：Windows 同源探针 0 failed；ARM64 设备 JIT 全开实测 Steam CDN 200（1.6s）、百度 http/https 200、msedge 404、WinINet 200；Steam 引导更新成功下载 manifest 并进入 336MB 客户端下载，Edge 安装器可下载。仅剩偶发原始 TCP 冷连接超时（网络抖动，重试即通）。
- 合并 core_extract 并推送远端：分支 `core_extract`（基于 `winehua/feature/core-extract` `a0226d8`）两次提交 `edaefff`（TLS）+ `090cf1b`（采用 main 的 GnuTLS/GStreamer 构建链），快进推送到 `winehua/WineHua:feature/core-extract`（`a0226d8..090cf1b`）；wine 子模块 schannel 修复推送到 `winehua/wine:feature/core-extract`（`65b5128..a6a9dac`）。glib/pcre2 保持上游 tag gitlink（glib format-security 由构建期 patch 处理），保证他人 checkout 可解析。排除 Network Test 卡片、自动启动钩子、UI/桌面改动与版本号。
- 版本：1.2.1（1002001）；wine-data 标记 `wine-engine-app-20260814.15-prod`。
- 产物：`F:\PomeloWin\artifacts\VintagePomeloPro-1.2.1-20260814\`（release APP `963d2b55…`、release entry HAP `a1d3c026…`、debug HAP `7fa592ec…`，正式包 verify-app success，SHA384withECDSA）。

### 2026-08-15 选择性同步：PC 适配（compositor/input 修复，版本 1.2.3 之上）

- 上游区间：`winehua/master` 自上次同步点以来的新增提交（fetch 后 `a2b5b36`，`main..winehua/master` 共 201 提交）。本次仅选取**最近的纯 native PC 适配**，全部自动合并成功、未改私有 UI。
- 选用（3 个，按上游顺序 cherry-pick）：
  - `79714a5` → `c5e87d2`：fix(compositor) PC 模式 D3D 游戏全屏画面缩左上角 + 首启自动最小化（`wl_core.cpp`）
  - `55b8d62` → `c1b9eb5`：fix(compositor) 最小化期间不向 Wine 发 configure（`wayland_server.cpp`）
  - `a008a49` → `091c289`：fix(input) host 侧钳制全屏黑边越界坐标，防相对增量差分幽灵位移（`input_resolver/input_manager`）
- 跳过（含原因）：
  - `6081dcf` fix(audio) 来电打断后恢复发声：**已包含**——该修复此前经 PR #58 合入 master，main 已有等价 `f5d807b`，无新内容。
  - `5576283` wine 子模块 → `037984b`（win32u 最小化豁免 present_rect）：**分叉分支**（与当前 `4e8f9f5` 共同祖先 `b7f43d4`），直接切换会**丢失私有仓 secur32 CHACHA20-POLY1305 禁用修复**（4e8f9f5 独有，TLS 1.2.1 验证过）。如需 win32u 修复，应在 wine 子模块内基于 4e8f9f5 单独 cherry-pick，另行评估。
  - `0cbb1aa` fix(files) 文件页自动加载 C: 根目录：依赖上游 "engine service 化" 大规模重构（`prefixReady/curDir/DRIVE_C` 结构），私有 UI 未做该重构，跳过。
- **补充审计（`git cherry -v` patch-id 内容对比）**：`main..winehua/master` 201 提交中，**32 个已在 main**（patch-id 等价，含此前各轮跟进合并的 `46571f0`/`a580efe`/`ed8dcac`/`6c02db6`/`1cc57e2`/`8089968`/`dc6b92f`/`6ab1371`/`4ce959e`/合成阶段2-4/gnutls+gstreamer 链等）。剩余 `+` 未合并项绝大多数为 docs/checkpoint/诊断/重构（engine service 化/几何收敛）/UI/品牌/CI/version，按维护约定不合并。抽查 14 个疑似功能修复候选，13 个本地已有等价实现（`SpawnWineProgram`/CoordTransform/zIndex/app_id/shell desktop/NCP 判活/guest_gfx Upsert 等）；仅 `d9d729b`（wineboot 抑制窗口创建）本地未见，但依赖上游 engine 架构，收益有限，暂不引入。**本地当前状态即合理终态，无需进一步合并。**
- 验证：`make hap` 构建通过（native 重编 + 签名），PC 适配 3 提交编译与行为无回归。
