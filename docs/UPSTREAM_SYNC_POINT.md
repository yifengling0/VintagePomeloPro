# 上游同步基线标记

## 2026-09-16 WineHua master 增量（fa13f36a..2049af9e）

在产品 `sync/winehua-pad-fusion-inline` 上核对 `winehua/master` @ `2049af9e`。
本段 1 个提交**选择性移植**，不是 Git merge。不上游 Index / WineEnvService / 品牌；wine gitlink 不跟随。

| 上游 | 处置 |
| --- | --- |
| `2049af9e` 全屏直传误判窗口浮层为内容层（含 WINEDEBUG 不再经 `__env` 下发、`SubsurfaceLayer`/`CompositorLayer` 迁 `compositor_layer.h`） | **adapted** → 新增 `SubsurfaceCoversContentRect` 单一判据（覆盖检测与直传扫描共用）+ `compositor/frame/compositor_layer.h`（`DesktopCompositor` 保留同名 `using` 别名）；`wine_env.cpp` 删 `WINEDEBUG=-all` 注入、`wine_child.cpp` 拦截 `__env` 的 `WINEDEBUG` |

产品侧保留：`frame_pipeline` 20260822 黑屏实锤的严格直传几何门（sub 位置与
buffer 尺寸须等于全屏 fit src）、`zc_bridge` 的 `fitChildren` 全屏跳过分支；
不引入上游 `GetZeroCopyContentSizeLocked`（产品无声明无调用方）。

本轮同时新增设置页「Windows 系统语言」日语内核（`ja_JP` / ACP 932），并把
`graphics-stack.lock.yaml` 的 wine 期望值对齐到实际登记指针（此前自 `7d68686c`
起漂移，`make test` 的 graphics-contract-check 一直失败）。详见
[wine-kernel-language.md](wine-kernel-language.md)。

### 子模块指针登记（2026-09-16）

GDI 文本抗锯齿修复（本地 patch `0010`）与上游 `winehua/wine`
`fix/ohos-gdi-text-antialias` 的 `ad7bdd7f092` 是同一改动（同作者、同
`WINEHUA_FONT_AA` 机制），因此改为**正式提交**并入产品 wine 线：

| 项 | 值 |
| --- | --- |
| 产品 wine 线 | `effb47e65cb`（`993993b5` + modal/dnsapi/pread/mprotect/theme/1px + `ad7bdd7` 抗锯齿） |
| 推送分支 | `winehua/wine:sync/winehua-pad-fusion-inline` |
| virglrenderer | `fde243e144d3`（EGL fence 指针缓存），推送分支 `winehua/virglrenderer:sync/winehua-pad-fusion-inline` |
| 删除 | `patches/wine/0010-ohos-font-aa-override.patch`（已被提交取代；`build_wine.sh` 对其余 7 个 patch 仍幂等应用） |

两个 gitlink 已在父仓登记，`graphics-stack.lock.yaml` 同步为可解析指针 ——
新增 clone / `git submodule update` 不再需要本机私有对象。

下次增量从 `2049af9e` 之后开始：`git fetch winehua && git log 2049af9e..winehua/master --oneline`。

## 2026-09-15 WineHua master 增量（ae2cfa63..fa13f36a）

在产品 `sync/winehua-pad-fusion-inline` 上核对 `winehua/master` @ `fa13f36a`。
本段 5 个提交**选择性移植**，不是 Git merge。不上游 Index / WineEnvService / 品牌 / 微信二维码；wine gitlink 不跟随 `3ce65ee4`。

| 上游 | 处置 |
| --- | --- |
| `ea0068fe` 无帧跳过改比上次绘制尺寸 | **adapted** → 产品 `egl_renderer`（保留 `inputFitMutex_` / `skipFrames_` / `renderPaused_`；不抄上游删产品字段） |
| `1d03b063` 补 toplevel `restored` 事件 | **adapted** → `ToplevelEventType::Restored` + `wl_core` / `SetToplevelRestored` 发送点；ArkTS `showWindow` 分支已有 |
| `c76cc795` 更新微信二维码 | **skipped** |
| `6f319a6c` wine gitlink `93c7c58` → `3ce65ee`（最外 1px 圈绘制） | **keep_product** gitlink；wine 工作树 cherry-pick → `3071a32860b`（保留未提交 FONT_AA / mfplat / ntdll locale） |
| `fa13f36a` 桌面未就绪不锁宿主光标 | **adapted** → `BindWaylandRefs` 第四参 `desktopMode`（第三参仍是产品 `InputResolver`） |

下次增量从 `fa13f36a` 之后开始：`git fetch winehua && git log fa13f36a..winehua/master --oneline`。

## 2026-09-13 WineHua master 增量（151d38bf..ae2cfa63）

在产品 `sync/winehua-pad-fusion-inline` 上核对 `winehua/master` @ `ae2cfa63`。
本段 7 个提交**选择性移植**，不是 Git merge。不上游 Index / WineEnvService / 品牌；wine gitlink 不跟随。
本仓 GDI 汉字乱码修复（`WINEHUA_FONT_AA=bitmap` / patch `0010`）一并带入 1.3.8。

| 上游 | 处置 |
| --- | --- |
| `8e986ff` ARGB 改按普通窗口承载 | **adapted** → 首帧一律 `created`；删 `ArgbWindowManager` / `takeWindowMask` NAPI；桌面 blit 不动 |
| `a0cab50` assemble 打 `msstyles` + wine 标题栏主题 `93c7c58` | **adapted** → `scripts/assemble.sh` 增加 `msstyles`；wine 工作树 cherry-pick → `45e32e8d2ab`；**不改**父仓 gitlink |
| `caa9c9c` Fusion modal 相对 owner 居中 + pending 残坑 | **adapted** → `ModalWindowManager.computePosition` / `reposition` + `cancelPendingToplevel` |
| `afa1f12` `applyModePolicy` 后通知订阅者 | **adapted** → `AppStorage` 写 `winehua.desktopMode` / `winehua.presentationMode` |
| `b99a0c2` 相对模式锁定关窗/失败必退出 | **adapted** → `etsLockNotified_` + `ReleaseLockForToplevel` + `clearPointerLockFor` |
| `b863c84` 标题栏「运行中 (N)」 | **adapted** → 产品 `Index`「运行中」导航（不上游 WineEnvService） |
| `ae2cfa6` 窗口 Stack 黑底 | **adapted** → `WineWindow` / `WineWindowAbility` / Fusion 子窗口 |

下次增量从 `ae2cfa63` 之后开始：`git fetch winehua && git log ae2cfa63..winehua/master --oneline`。

## 2026-09-12 WineHua master 增量（03c2384e..151d38bf）

在产品 `sync/winehua-pad-fusion-inline`（叠在 `main` @ `50efd6fb` / rc-1.3.6，工作区已有 1.3.7 平板 fusion）上核对 `winehua/master` @ `151d38bf`。
本段 3 个提交**选择性移植**，不是 Git merge。不上游 Index / WineEnvService / 品牌；**版本号保持 1.3.7 / 1003007**；wine gitlink 不跟随。

| 上游 | 处置 |
| --- | --- |
| `fb8d07bd` virgl gitlink `ba8e4fa8` → `6627c031`（缓存 EGL fence 指针） | **adapted** → 在产品 virgl `670ff196` 上 cherry-pick `6627c031` → 工作树 `fde243e1`；**不改**父仓 virgl gitlink（本地 SHA 不在 winehua/virglrenderer） |
| `68a4c8b2` DXVK legacy 1.10.3 d3d10 链 | **adapted** → 只改 `wine_env.cpp`（legacy 纯 `d3d10/d3d10_1/d3d10core=n`，vkd3d overlay 同步；modern 2.x 仍 `d3d11=n;dxgi=n`；DX9 仍 builtin）。`scripts/assemble.sh` 已打 d3d10 DLL → **covered_by_product** |
| `151d38bf` wine gitlink `08cbdd64` → `f085bc22`（OHOS noexec PE 头 / mprotect 匿名页） | **keep_product** gitlink；wine 子模块 cherry-pick `0b1a274e` + `f085bc22` → 工作树 `9b934b28779`（保留未提交 mfplat MPEG4 handler） |

下次增量从 `151d38bf` 之后开始：`git fetch winehua && git log 151d38bf..winehua/master --oneline`。

## 2026-09-11 WineHua master 增量（5dc2ceb5..03c2384e）

在产品 `sync/winehua-pad-fusion-inline`（叠在 `main` @ `50efd6fb` / rc-1.3.6）上核对 `winehua/master` @ `03c2384e`。
本段 14 个提交**选择性移植**，不是 Git merge。不上游 Index / WineEnvService / 品牌 / 版本号；wine gitlink 不跟随。

| 上游 | 处置 |
| --- | --- |
| docs: `4a161256` `1f8635a6` `d20375a5` `d6c59668` `6a0eefa0` `4dbcd628` `4f923aa3` | **skipped** |
| `6e28f281` + `6736ebd3` + `1d1005c7` + `03c2384e` | **adapted** → `DisplayPolicy::RouteForSubsurface` / InlineClient / 窗口合成持锁 |
| `770e86fb` | **adapted** → `cancelPendingToplevel`（pending 队列改 deque） |
| `348da8db` + WineWindow raise/startMoving | **adapted** → `FusionWindowManager` + 产品 `WineWindowManager` |
| `5363b38d` | **adapted** → `WineEngineService` + `EntryAbility` want（不上游 WineEnvService） |

产品策略：phone 强制虚拟桌面；平板默认虚拟、设置可开多窗口（Fusion subWindow，不 `startAbility`）；PC 仍 Ability 融合。

下次增量从 `03c2384e` 之后开始：`git fetch winehua && git log 03c2384e..winehua/master --oneline`。

## 2026-09-09 WineHua master 增量（46139138..5dc2ceb5）

在产品 `main` @ `b24243a8`（rc-1.3.5）上核对 `winehua/master` @ `5dc2ceb5`。
本段 2 个提交**选择性移植**，不是 Git merge。不上游 Index / WineEnvService / 品牌 / 版本号；wine gitlink 不跟随。

| 上游 | 处置 |
| --- | --- |
| `bc66706e` host 测试 `-I …/cpp/wine` + `env_spec.cpp` 路径 | **skipped**（产品 `host_tests` 已用 `#include "wine/…"`，Makefile 已编 `wine/env_spec.cpp`） |
| `5dc2ceb5` PC 融合嵌套 modal 继承宿主 stage | **adapted** → `ModalWindowManager.ets`（保留 `LogService`；不改 `WineWindowManager` / `Index` / `DesktopAbility`） |

下次增量从 `5dc2ceb5` 之后开始：`git fetch winehua && git log 5dc2ceb5..winehua/master --oneline`。

## 2026-09-05 WineHua master 增量（b0e85c0e..46139138）

在 `sync/winehua-dns-modal-launch-args` 上核对 `winehua/master` @ `46139138`。
本段 6 个提交**选择性移植**，不是 Git merge。不上游 Index / CustomLaunchDialog / Steam Legacy zip / WineEnvService；wine gitlink **不整段快进**（cherry-pick dnsapi musl 与 winewayland modal，保留 schannel CHACHA20 / WHGP / IEEE float32 mix-format）。

| 上游 | 处置 |
| --- | --- |
| `f5c6e5ec` compositor modal 组员化 + 输入拦截 | **adapted** → `ToplevelManager::modalOf_` / `desktop_compositor` / `input_manager` |
| `cfd960ac` PC 融合 `ModalWindowManager` | **adapted** → `ModalWindowManager.ets` + `WineWindowManager`（桌面模式不 `startAbility`） |
| `bd2a5b55` wine gitlink modal `205ba344` | **keep_product** gitlink；wine 子模块 cherry-pick → `21ca0eeab01` |
| `028c07e2` docs(modal) | **skipped** |
| `fdcb54e6` dnsapi smoke 探针 | **adapted** → `smoke/winehua_dns_probe.c` + `assemble.sh` core/all（`dns-api-x64/x86`） |
| `46139138` wine gitlink dnsapi `08cbdd642b9` | **keep_product** gitlink；wine 子模块 cherry-pick → `b74ddbb61d7` |

产品侧另加游戏设定 DX11 / OpenGL 启动参数预设与额外 argv（不在上游本段）。

下次增量从 `46139138` 之后开始：`git fetch winehua && git log 46139138..winehua/master --oneline`。

## 2026-09-04 WineHua master 增量（37f4616d..b0e85c0e）

在 `diag/m5-b3-remaining-pop` 上核对 `winehua/master` @ `b0e85c0e`。
本段 7 个提交**选择性移植**，不是 Git merge。不上游 Index/设置 UI，不切换 wine gitlink（产品线保留 float32 mix-format 与 WHGP v2）。

| 上游 | 处置 |
| --- | --- |
| `91cee3fa` / `b60ab9bb` / `660f2bb8` / `c9c9cefd` Controller Hub v1 | **covered_by_product**（产品已有 Hub + canonical WHGP v2） |
| `12aba3d4` wineserver/wineboot 注入手柄总线 env | **adapted** → `wine_launch.cpp` wineserver spawn（wineboot 两处产品已有） |
| `e538f95a` wine gitlink 到手柄门禁 opt-in | **keep_product**（不丢 IEEE float32 mix format） |
| `b0e85c0e` wayland 协议生成改 `protocols/` | **covered_by_product**（`build_native.sh` 已指向 `cpp/protocols`） |

下次增量从 `b0e85c0e` 之后开始：`git fetch winehua && git log b0e85c0e..winehua/master --oneline`。

## 2026-09-03 WineHua master 增量（61cb4c64..37f4616d）

在 `diag/m5-b3-remaining-pop` 上核对 `winehua/master` @ `37f4616d`。
上次产品 ledger 停在 `61cb4c64`；本段 10 个提交**选择性移植**，不是 Git merge。

| 上游 | 处置 |
| --- | --- |
| `e989cd93` 可靠失效 stale VirGL 产物 | **adapted** → `scripts/build_native.sh` |
| `43772ef4` 进程 env 全量打点 | **adapted** → `spawner.cpp` / `wine_exe.cpp`（保留产品 dxvk/present 字段） |
| `25425184` resize 保留 EGL | **covered_by_product**（`virgl_surface_presenter.cpp` 已有 `retained_egl`） |
| `e71c893d` VirGL queue pacing | **covered_by_product**（`QueuePresentPacingPeriodNs`） |
| `923bc1c4` 引擎就绪无条件播种 C:\\smoke | **covered_by_product**（`syncManagedSmokePayload('engine-ready')`） |
| `8c39a1c2` ArkTS perf.profile 写死 dirty-ring | **not_applicable**（产品默认来自 Native `graphics_profile`） |
| `357db51d` 删除会话级 compatEnvStr | **keep_product**（设置页仍走 `launchClient` 兼容档） |
| `7703adeb` smoke 迁到 `ets/smoke/` | **deferred**（产品已有独立 runner，不搬上游 Index/WineEnvService） |
| `3398ae2d` 上游 Index 四档单选 UI | **not_applicable**（产品 `SystemSettings` 独立布局 + GPU 920 门禁） |
| `37f4616d` 推进 dxvk-modern/virgl gitlink | **not_applicable**（HANDOFF：不因上游 pin 切换子模块） |

下次增量从 `37f4616d` 之后开始：`git fetch winehua && git log 37f4616d..winehua/master --oneline`。

## 2026-08-31 候选同步（尚未提升产品 main 的发布基线）

`codex/sync-master-20260831` 从产品 main `2c043636` 完成了
`d256317e02c83ed81172938c31152ded16393a32..74f2bfe1aba89cbfbc729d1cf658b46f3aea6f80`
的逐项功能适配与 Native 目录对齐。69 个提交的处置以
[来源 ledger](sync/20260831/commits.json) 为准，不能以 Git 祖先关系推断全部采用。
双 ABI 构建已通过，真机验收状态见 [STATUS](sync/20260831/STATUS.md) 和
[DEVICE_RESULTS](sync/20260831/DEVICE_RESULTS.md)。

后续接手先读 [HANDOFF](sync/20260831/HANDOFF.md)，不要重复搬移已完成代码。
本分支下一段上游增量从 `74f2bfe1` 后开始；历史 main 的已核对基线记录保留如下，
等候选验收且产品 main 接受后再提升发布基线。此处没有伪造 merge，也没有 push。

## 原产品 main 的历史记录

> 用途：标明本地产品线已合并到 WineHua 上游的哪个提交，避免重复合并/漏合并。
> 本地镜像分支：`mirror_master`（跟踪 `winehua/master`，只读对照，不 push）。
> 维护命令：`git fetch winehua && git branch -f mirror_master winehua/master`
> 下次同步前先执行：`git log d256317e..mirror_master --oneline`

| 项 | 值 |
| --- | --- |
| 上游仓库 | `https://github.com/winehua/WineHua` |
| 上游分支 | `master` |
| **最后核对的上游 SHA** | `d256317e`（2026-08-27，`winehua/master` 尖端） |
| 功能同步起点（用户指定） | `10a9e6caf33e0147363793947461417dd60a8372`（master 线等价 `189c27c`） |
| 合并方式 | 第 1–5 步已对齐 EnvSpec / 基线 / Profile / Spawner（全部 kind 走 broker）。`d256317e` 三原语名字已挂到 `WineEngineService`（不上游 `WineEnvService` / overlay）。不上游品牌/版本号/CI/README |
| 本地对应分支 | `main`（含 `feature/align-env-spawn-1-4`） |
| 核对日期 | 2026-08-28 |

## 已对齐的上游架构（第 1–5 步 + 三原语命名）

WineHua `master` @ `d256317e` 的 EnvSpec / Profile / SpawnRequest 重构，本仓按第 1–5 步落地（`175ed930` `af871191` `3725dfb5` `d00df45b` + 第 5 步 `f9aaaaed` 适配）。wineserver/wineboot 经 broker 单一通道，`wine_child Main` 截获 wineserver 转入本体。

`startSession` / `stopSession` / `wipeEnvironment` 已作为 `WineEngineService` 上的命名入口；产品仍走 `ensureReady` / `stopAll` / `resetPrefix`。明细见 `docs/private-upstream-sync.md`「2026-08-28 对齐会话生命周期三原语命名」与「2026-08-27 对齐 WineHua master 启动/环境栈第 5 步」。

## 已核对的上游增量（90edaae..d256317e）

从上一基线 `90edaae` 到 `winehua/master` 尖端 `d256317e`。行为修复已在 `feature/host-fps-hud` 吸收；架构第 1–5 步见上一节。

明细见 `docs/private-upstream-sync.md`「2026-08-27 WineHua master 增量（90edaae..d256317e）」。

## 已核对的上游增量（e16d79b..90edaae）

从上一基线 `e16d79b` 到 `winehua/master` 尖端 `90edaae`。采纳 Box64 兼容档位 native 通道（`7cff882` `dcf3906` `90edaae`），跳过上游 `Box64Dynarec.ets` 与版本标签。

明细见 `docs/private-upstream-sync.md`「2026-08-25 WineHua master 增量（e16d79b..90edaae）」。

## 已核对的上游增量（10a9e6c..e16d79b）

从用户指定功能基线 `10a9e6c` 到 `winehua/master` 尖端 `e16d79b`。`98eaca5` 已在上一轮同步；本轮采纳 `189c27c`（光标门禁）、`15c53ee`（resize 强制重绘），跳过 `e16d79b`（CI mono）。

明细见 `docs/private-upstream-sync.md`「2026-08-24 WineHua master 增量（10a9e6c..e16d79b）」。

## 已核对的上游增量（ff76a8f..98eaca5）

从 `ff76a8f`（含）到 `winehua/master` 尖端 `98eaca5`。功能链已 cherry-pick 到
`feature/sync-winehua-ff76a8f`，并叠在 VintagePomeloPro `main` 的 UI 上
（浮窗桌面、PC 沉浸全屏、蓝牙键盘 XComponent 焦点、宿主 IME）。

明细与跳过项见 `docs/private-upstream-sync.md`「2026-08-23 WineHua master 输入/合成器同步」。

## 已核对的上游增量（d9c667e..1036ada，共 5 个提交）

连同先前暂缓的合成器 Layer 重构链（`76a2cd4`→`13cc583`）一并合入，明细见
`docs/private-upstream-sync.md`「2026-08-06 合成器 Layer 重构全链合并」。

## 已核对的上游增量（996aabb..d9c667e，共 11 个提交）

- 采纳（手动移植）：
  - `8fb8488`（wine 内部启动的进程登记到任务列表）→ broker.cpp 全量 AddProcess + ParseProcessName、wine_process.cpp basename 兼容反斜杠（私有 Index 已有 1.5s 轮询，未做 process-updated 推送）。
  - `e5cd7fa`（全屏游戏点击按 zIndex 命中上方窗口 + 菜单被全屏覆盖）→ 私有 input_resolver.cpp 全屏分支前置命中（z-order 高于全屏窗口的 toplevel/subsurface）。
  - `bb617a4` 的 UpsertEnvLine 去重语义（删全部同 key 再追加，避免 WEAKBARRIER 等重复 key）；整体重构（SpawnViaBroker 收敛）与私有启动链路差异大，维持私有实现。
  - `d9c667e`（explorer 登记名 desktop）：私有用 `@engine/explorer` 引擎标记体系，语义不同，未采用。
- 跳过：`7ed8ad2`（dinput_click_probe 已迁移进私有 wine 子模块）、`faf98af`（私有 CI 已装 curl、mono 下载成功）、`d3688e1`（BOX64 __aarch64__ 守卫私有已有）、`c5263a3`/`82ee3f3`/`1dc0283`（docs/清理）、`70abb0b`（上游版本号）。

## 已核对的上游增量（0ed802c..8089968，共 28 个提交）

- 结论：Aug 1-2 的输入/mono/字体/合成阶段修复与本地已有工作**等价**（另一条工作线已同步进上游），**未重复合并**；真正缺失的 **Aug 3 `8089968`**（zwp_relative_pointer_v1 取代 warp 补偿）已 cherry-pick（→ `0e2a86e`）。
- 真正缺失且暂缓：合成器 Layer 重构（阶段 1-4，`76a2cd4` `d5deed7` `6df338a` `c2bd0ee` 等）——大重构，与 1.1.2 修复无关，另行评估。
- 其余为文档/CI/清理类（`088fa6a` `1fba92d` `f817c12` `b40ef56` `a753d15` `8048b95` `1c778af` `8e022b4` `5fb8b86` `f9771a3` `7bd10c8`），按需选择性采纳，不阻塞产品修复。

## 已核对的上游增量（8089968..996aabb，共 5 个提交）

- 已合并：`996aabb`（warm-prefix 显式播种 wineboot boot 事件 + 自动 explorer 走 broker 通道）→ `f2f9cfe`；`build_deps.sh` 同步（BUILD_WINE_MONO 默认启用）。
- 暂缓：合成器 Layer 重构延续 `13cc583`（ToplevelState 封装）、`8ab97c3`（层序/全屏几何对象方法）——与私有 compositor 架构冲突，维持暂缓。
- 跳过：`82ee3f3`（README Contributors）、`c5263a3`（.gitignore，CI 侧）。
- 冲突处理：`.github/workflows/build.yml` 保留私有 CI 环境值（上游 GFX/Vulkan/Mono 全开，私有 workflow 不变）。
