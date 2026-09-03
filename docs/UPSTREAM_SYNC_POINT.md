# 上游同步基线标记

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
