# 旧柚 Pro 启动器 UI 重组 — 完整重构规划（v1）

> 日期：2026-09-22 ｜ 仓库：`F:\旧柚pro\VintagePomeloPro` ｜ 分支：`UI`
> **定位**：UI 重组的唯一蓝图，取代 09-20 交接文档 §7 设计方案与 09-22 交接中的零散计划。
> 参照系：**iiSU**（逆向评估，`E:\iiSU\PORTING-ASSESSMENT.md`）、**盖世模拟器**（形态对标，闭源只取形态）、**Playnite**（`github.com/JosefNemec/Playnite`，开源架构范式，本次新增研究）。
> 用户锚点（原话）："初衷是在旧柚 Pro 上借 iiSU 做 UI 重组，但我什么也没看到改变" / "当前 UI 不适合移动端，压根不像游戏启动器（和盖世对比）"。
> ⚠️ 本文只规划不动工。当前工作区已存在的未提交改动在 §10 如实登记（含本次规划前已做的 4 处小改，均未构建、未装机）。

---

## 0. 三十秒摘要

**重构本质**：把"应用抽屉"升级为"游戏库管理器"。Playnite 给的最大启示是——**先有游戏数据模型，才有游戏启动器的视觉**。三个参照物的交集就是北极星：**满屏封面 + 单击直玩 + 游戏元数据**。

四步走：① 清理合流（两条平行工作线收拢）→ ② 默认落地重构（开屏即封面墙，立即可见）→ ③ 详情页 + 信息层（上次游玩/时长/列表视图）→ ④ 元数据抓取（封面/背景/资料自动补齐）。

---

## 1. 现状诊断：为什么"什么也没看到改变"

### 1.1 真机实测证据（2026-09-22，Pura 80 Ultra 横屏截图）

启动应用截图分析结论：当前渲染的是 **XMB 模式**（左列分类 + 聚焦列表）——新布局代码**确实在跑**，但观感是"工具型应用启动器"：全部条目是占位图标（无任何美术），停留在"内建"分类（DXVK 测试、IE、注册表编辑器这类开发工具）。

### 1.2 因果链（四条，全部已核实）

| # | 根因 | 证据 |
|---|---|---|
| 1 | **默认落地面错**：应用打开落在 `'builtin'`（开发工具分类），不是游戏 | `Index.ets:105` `filter: CatalogFilter = 'builtin'` |
| 2 | **视觉资产没接进新布局**：XMB 自绘列表不复用 `AppCard` → 09-20 做的 exe 图标提取、封面墙（COVER）、收藏徽标**全部不可见**；封面墙也非默认视图（默认 GRID） | `XMBLayout()` 自建渲染；`AppModels.ets:1095` `viewMode: ViewMode.GRID` |
| 3 | **没有游戏库信息层**：无封面来源、无背景图、无上次游玩/时长、无详情页——"游戏启动器"的实质内容缺失 | `AppDescriptor` 仅 title/coverPath/available/launchTarget |
| 4 | **两线工作未合流**：09-20 线（保守增量，已提交 2 commit）与 09-21/22 线（布局骨架，未提交）互不知情，各自平行 | `git log`（HEAD=9b05e7f）+ 工作区 11 文件未提交 |

### 1.3 工程侧待清理（详见 `docs/HANDOVER_UI_2026-09-20.md` §2.4/§5，此处只列结论）

- **CRLF 行尾污染**：11 文件被整体改写 LF→CRLF，表面 diff +8769/−8575，真实改动仅 +381/−187；不归一就提交 = blame 全毁、rebase 灾难。
- **`wineTextInputPreedit` 3 参改 1 参**：真实 Linux 构建时 d.ts 由 native 再生成为 3 参声明 → **正式构建必炸**，必须回滚。
- **沙盒变第二工程**：签名配置/构建脚本全长在 `E:\vpp-build`，与 F: 真源结构性分叉。

---

## 2. 三个参照物：各取什么，各舍什么

### 2.1 iiSU（已有完整逆向评估）

| 取 | 舍 |
|---|---|
| 多布局模式思路（Standard 网格 / XMB 纵列 / DS 分屏） | **桌面替换器形态**（鸿蒙不开放，永不可能） |
| 全屏沉浸、无 chrome、高对比双色调 | 主题市场/付费墙/Shopii 等在线服务 |
| 手柄优先导航 | **闭源资源一概不碰**（美术/代码/数据都不抄） |
| RetroAchievements 集成思路（后排） | |

### 2.2 盖世模拟器（闭源，只取形态）

取：**移动端触屏手感**——满屏大封面网格、极简单击直玩、极简顶栏、为横屏拇指区设计。它证明了一件事：移动端游戏启动器的全部秘密就是"封面够大、层级够浅"。

### 2.3 Playnite（开源，架构范式——本次核心增量）

> 来源标注：仓库元数据已核实（C# / WPF / 14,048★ / 官网 playnite.link / 描述 "Video game library manager with support for wide range of 3rd party libraries and game emulation support, providing one unified interface for your games."）；下述功能细节来自其公开产品文档知识（本机网络对 GitHub 正文被重置，未能拉取 README 全文，落地前建议再核对一次官方文档）。

| 取（六项，按对本项目的价值排序） | 对应到旧柚 Pro |
|---|---|
| ① **游戏是一等公民的数据模型**：Name + 四资产（Cover / Background / Icon / Logo）+ Platform / Genre / ReleaseDate / Developer + **Playtime / LastActivity** + IsInstalled / Favorite / Hidden | 我们的 `AppDescriptor` 只有 title/coverPath/available——**差距就是 §4 的路线图** |
| ② **双模式分离**：Desktop Mode（键鼠、信息密度）与 **Fullscreen Mode**（10-foot 手柄/TV 界面，独立主题域） | 映射为 CLASSIC（工具向）vs STANDARD（默认、盖世式）；证明全屏模式应做成**默认且独立换肤**的一等模式 |
| ③ **三视图体系**：Grid（封面）/ List（信息）/ Details（详情面板）——一个库的三种看法，不是三个平行产品 | 现有 LayoutMode 三选一改为"全屏模式下可切 Grid/List 视图 + 详情页覆盖层"（见 §3.1） |
| ④ **元数据抓取架构**：IGDB 自动匹配 + 手动编辑 + 插件源 | 映射为 SteamGridDB（资产）+ IGDB（资料）组合，见 §7 |
| ⑤ **筛选面板**：平台/类型/年份/时长/安装状态多维筛选 | 阶段后排（P3），先做好分类胶囊 |
| ⑥ **每模式独立主题系统** | 氛围背景/主题包（M5） |

| 舍 | 理由 |
|---|---|
| Steam/GOG/EA/Epic 商店账号同步 | 鸿蒙无对应生态 |
| XAML 主题引擎 / .NET 插件 SDK | 用 ArkTS 资源体系即可，不过度工程 |
| Windows 桌面端信息密度布局 | 我们是移动端横屏，不是 PC |

**三者交集（北极星）**：满屏封面 + 单击直玩 + 游戏元数据。

---

## 3. 目标形态定义

### 3.1 双模式 + 三视图（Playnite 式收敛）

**关键决策：XMB 从"独立 LayoutMode"降级为"全屏模式里的列表视图"。**
理由：XMB 本质是"列表视图 + 纵向导航"，与 STANDARD 不该是平行宇宙；现有 `XMBLayout()` 自绘不复用 `AppCard`（视觉资产断裂的直接原因），维护两套卡片渲染是反模式。收敛后：

```
LayoutMode（保留两值）
├── STANDARD（默认，全屏模式，盖世式）
│     ├── 视图: GRID 封面墙（默认）⇄ LIST 列表（XMB 重写，复用 AppCard 资产链）
│     └── 详情页 GameDetailSheet（覆盖层，任何视图下长按进入）
└── CLASSIC（经典模式，工具向：侧栏/底栏 + 四视图，现状保留）
```

### 3.2 游戏卡片三资产（Playnite 数据模型的最小裁剪）

| 资产 | 用途 | 来源优先级 |
|---|---|---|
| **cover（竖版）** | 封面墙主视觉 | 用户手放 `cover.*` → 抓取 → （无则降级） |
| **icon（方形）** | 列表视图小图、无封面兜底 | exe 内嵌图标提取（09-20 已实现）→ 抓取 |
| **background（横版）** | 详情页主视觉、氛围背景 | 抓取 → 用户手放 → 封面放大+blur 兜底 |

### 3.3 布局蓝图（横板）

**全屏模式（默认落地）：**
```
┌────────────────────────────────────────────────────────────┐
│ [最近][游戏][收藏][运行中] ┄ ┄ ┄ ┄ ┄ [搜索] [视图切换] [☰] │  顶栏 48vp 分类胶囊
├────────────────────────────────────────────────────────────┤
│                                                            │
│   ┌────┐  ┌────┐  ┌────┐  ┌────┐                          │
│   │封面│  │封面│  │封面│  │封面│      满屏封面墙（单击=直玩， │
│   │墙 │  │墙 │  │墙 │  │墙 │      长按=详情）              │
│   └────┘  └────┘  └────┘  └────┘                          │
│   ┌────┐  ┌────┐  ┌────┐  ┌────┐                          │
│   └────┘  └────┘  └────┘  └────┘                          │
└────────────────────────────────────────────────────────────┘
```
（"内建"分类从顶栏胶囊里降级进 ☰ 菜单——游戏启动器首屏不该出现开发工具。）

**详情页（覆盖层，长按/右键进入）：**
```
┌────────────────────────────────────────────────────────────┐
│           背景图（横版）blur 20% + 暗化遮罩                    │
│   ┌──────┐  标题（24fp）                                    │
│   │封面  │  平台 · 上次游玩 3 天前 · 累计 12.5 小时            │
│   │缩略  │  [ ▶ 启动游戏 ]   [♡ 收藏]  [⚙ 设置]              │
│   └──────┘                                                  │
└────────────────────────────────────────────────────────────┘
```

---

## 4. 信息架构与数据模型重构（本次规划的最大增量）

### 4.1 字段扩展（Playnite 模型 → 旧柚模型）

| 新字段 | 挂载点 | 类型/默认 | 里程碑 |
|---|---|---|---|
| `backgroundPath` | `AppDescriptor` + `AppSettings.customBackground` | string，扫描发现 `background.*`（`AppCatalogRules` 加一行，同 `cover.*` 模式） | M2 |
| `lastPlayedAt` | 内存会话层（`recent_launches_v2`） | number（epoch ms） | M3 |
| `totalPlayMinutes` | `AppSettings` | number，会话结束累加 | M3 |
| `platform` | `AppDescriptor` | 恒 `'Windows'`（未来 other-engine 扩展） | M2 |
| `tags: string[]` | `AppSettings` | 用户标签（合集用） | M5 |

> 存储纪律：`AppSettingsStore` 的 setter 全量拷贝模式——每加字段同步 ~8 处构造点（项目既有约定，09-21/22 线加 `layoutMode` 时已验证此流程）。

### 4.2 存储键升级

| 键 | 变化 |
|---|---|
| `recent_launches_v1` → **`recent_launches_v2`** | `string[]` → `{id, at}[]`；读 v1 迁移（无时间戳的按 0 处理）；上限 20→50 |
| `app_settings_v1` | 兼容追加（`??` 默认），不升版本 |
| **`metadata_cache_v1`（新）** | 抓取结果缓存（游戏 id → 资产路径 + 资料 + 来源 + 时间），M4 |
| `ui_reorg_v1`（已写入代码，未提交） | 一次性 UI 迁移标记（§10） |

### 4.3 详情页交互定案

- **单击卡片 = 直接启动**（盖世式；沿用 `launch()` 冲突检查链，禁止旁路）；
- **长按/右键 = 详情页**（取代现在长按弹 3 键菜单的旧交互——详情页内含 启动/关闭/收藏/应用设置/重新关联/删除，一次性解决菜单溢出问题）；
- 详情页为**覆盖层**（`Stack` 推入 + 背景模糊），不做独立 router 页（保持单页架构，避免路由栈复杂化）。

---

## 5. 视觉规格（细化到参数）

| 项 | 规格 |
|---|---|
| 封面墙瓦片 | 沿用 COVER 瓦片：美术全出血、底部渐隐（透明→85% 黑，高 72–96vp）、标题 16fp ≤2 行、副行"上次游玩 3 天前"（M3 后替换 exe 路径名）、徽标带（收藏★金/运行中绿）、右上设置浮钮 40×40 常显 40% |
| 列表视图（XMB 重写） | 复用 `AppCard` 资产链（cover→icon→字母）；聚焦项放大（封面 96vp），非聚焦 48vp 图标 + 13fp 标题；**禁止再自绘卡片** |
| 顶栏分类胶囊 | 48vp 高，选中态 accent 填充白字；游戏相关分类在前（最近/游戏/收藏/运行中），内建收进 ☰ |
| 氛围背景 | 详情页背景 / Hero 封面 → `blur(40–60)` + 透明度 0.25–0.35 + 暗化遮罩；切换 500ms `animateTo`；用户手动背景优先级更高（现有设置项） |
| **移动端适配专项**（用户点名） | 触控目标 ≥48vp；瓦片最小宽 160vp；横屏拇指区——分类在顶、详情"启动"钮在右下可达区；最小字号 13fp；dpad 焦点态（2vp accent 边框）为手柄预留 |

---

## 6. 组件与代码落点（映射表）

| 组件/改动 | 动作 | 文件 | 依赖 |
|---|---|---|---|
| 封面墙瓦片 + 图标链 + 徽标 | **已实现**（09-20 已提交） | `components/AppCard.ets` | — |
| 默认落地（STANDARD+COVER+游戏分类+迁移） | **代码已就绪未提交**（§10） | `AppModels/Index/AppSettingsStore` | M0 合流后提交 |
| `GameDetailSheet.ets` | 新建 | `components/` | backgroundPath、recent v2（M3 完整） |
| 列表视图重写 | 改造 `XMBLayout` → 全屏模式内 LIST 视图 | `pages/Index.ets` | 复用 AppCard |
| 顶栏分类重排（内建降级） | 改造 `TopTabBar` | `pages/Index.ets` | — |
| 氛围背景层 | 改造 `PageSurface` 背景 Stack | `pages/Index.ets` | 详情页/背景资产 |
| `background.*` 发现 | `AppCatalogRules` 加一行 | 同 cover.* 模式 | — |
| 抓取服务 | 新建 `service/MetadataScraper.ets` | SteamGridDB/IGDB | `ohos.permission.INTERNET`（先查 `module.json5`） |
| 设置页增项（布局/视图/主题入口） | 改造 `SystemSettings` | | |

---

## 7. 美术与元数据来源（M4，观感的最大杠杆）

- **资产**：SteamGridDB（免费 API key；竖版封面/横版背景/图标/横幅，Windows 游戏覆盖最全）。
- **资料**：IGDB（发行日期/类型/开发商/简介——即 Playnite 的主数据源）。
- **展示侧零改动原则**：抓取只往游戏目录写文件——`cover.png`（现有 `chooseCoverFileName` 已自动发现）、`background.png`（§6 加一行）；元数据进 `metadata_cache_v1`。
- 匹配策略：目录名→规范化→精确→模糊；多结果弹确认（复用详情页 UI）。
- 前置检查：`module.json5` 的 `INTERNET` 权限；法律：按各自 ToS、标注来源，**不抄 iiSU 资源包**。

---

## 8. 里程碑（每步含验收与验证循环）

> 验证循环（已打通，替代旧"WSL 优先"单一路线）：`vpp-check.sh` 编译 → `E:\vpp-build\build-hap.bat` 签名构建（注意：先 `--stop-daemon` 清无 java 的旧守护进程）→ `hdc -t 4NZ0225605001361 install entry-default-signed.hap`（**设备 ID 别带脏字符**）→ force-stop + aa start → `snapshot_display` 截图核对。每个里程碑装机一次，不做无意义反复。

| 里程碑 | 内容 | 验收 |
|---|---|---|
| **M0 清理与合流**（先行，必做） | ① 行尾归一 LF（+`.gitattributes` 防复发）② 回滚 `wineTextInputPreedit` 1 参改动 ③ d.ts/import 类"环境修复"逐条评审 ④ 两线改动分块提交（真实 diff +381/−187 + 已就绪 4 处小改） | `git diff -w --stat` 与 `--stat` 一致；编译 0 错误；真实构建链不受影响 |
| **M1 默认落地重构** | 全屏模式默认 + 封面墙默认 + 落地"游戏"分类 + 一次性迁移（§10 代码）+ 顶栏重排（内建降级进 ☰） | **真机开屏即满屏游戏封面**（截图核对） |
| **M2 详情页 + 列表视图** | `GameDetailSheet` v1（背景图+元信息+启动+收藏）+ XMB 重写为 LIST 视图（复用 AppCard）+ `background.*` 发现 | 长按进详情、单击直玩；列表视图有真实封面/图标 |
| **M3 信息层** | `recent_launches_v2`（时间戳）+ 会话时长累计 + 卡面副行"上次游玩" + 排序（最近/名称/时长） | 卡面可见"3 天前 · 12.5 小时" |
| **M4 元数据抓取** | SteamGridDB 资产 → IGDB 资料 + 首次配置向导 + 缓存 | 无封面游戏一键补齐封面/背景/资料 |
| **M5 主题与合集** | 氛围背景自动取色/主题包（背景+强调色）+ 标签/合集分组 | 主题切换即时生效 |
| 例行 | 上游 rebase（已知冲突区：`Index.ets` Header/refreshRunningState 附近） | 双门禁通过 |

---

## 9. 风险与待决

| 风险 | 缓解 |
|---|---|
| 两线合流冲突（09-20 已提交 vs 09-21/22 未提交） | M0 先行；XMB 降级决策落地后，未提交的 XMB 代码只保留可复用部分 |
| 游戏目录名 ≠ 游戏名（抓取匹配失败） | 目录名/`customTitle`/exe 元数据三路匹配 + 人工确认兜底 |
| 手柄焦点导航（dpad 扰动输入层，知识库 26 号坑） | M5 才接手柄；焦点态视觉先行、事件接管后排 |
| CRLF 再次混入 | `.gitattributes`（`*.ets text eol=lf` 等）进仓库 |
| 签名配置只在沙盒 | `build-hap` 脚本与签名流程文档化回迁仓库 `scripts/`（证书本身不入库） |
| 真实 Linux 构建被本地"环境修复"污染 | M0 回滚清单执行；此后环境差异一律留在沙盒层 |

---

## 10. 当前工作区未提交改动登记（截至本文撰写）

**A. 09-21/22 会话的改动（11 文件，真实 +381/−187，含 CRLF 噪音）**：LayoutMode 三模式骨架、TopTabBar/XMB、环境错误"修复"（d.ts/imports/wineTextInputPreedit）、颜色资源调整、渐变背景。处置：M0 分块评审提交，`wineTextInputPreedit` 必回滚。

**B. 本次规划前已做的 4 处小改（M1 内容，编译 0 错误，未构建、未装机——构建在你喊停时中止）**：
1. `AppModels.ets` 默认 `viewMode: GRID → COVER`
2. `Index.ets:105` 默认 `filter: 'builtin' → 'windows'`
3. `Index.ets:118` `phoneNavIndex: PHONE_NAV_BUILTIN → PHONE_NAV_WINDOWS`
4. `AppSettingsStore.ets` 新增 `ui_reorg_v1` 一次性迁移（老安装首启落 STANDARD+COVER）

去留由你定：保留则并入 M1 提交；不要则 `git checkout -- <文件>` 逐个还原（注意别连 09-21/22 的改动一起还原，B 与 A 在同几个文件里，建议由我在开工时按 hunk 拆分）。

---

## 附录 A：Playnite 参照笔记

- 仓库：`github.com/JosefNemec/Playnite`（已核实：C# / WPF / 14,048★ / 官网 playnite.link / 定位 "Video game library manager … one unified interface for your games"）。
- 全屏/桌面双模式、Grid/List/Details 三视图、IGDB 元数据、筛选面板、双模式主题系统——来自产品公开文档知识；**落地 M4/M5 前建议再核对官方文档一次**（本机网络当时受限）。
- 对本方案的三条核心启发已内化进 §2.3/§3/§4：数据模型先于视觉、全屏模式是一等模式、三视图收敛而非平行模式。

## 附录 B：既有资产清单

| 资产 | 状态 |
|---|---|
| 封面墙瓦片/exe 图标链/收藏徽标/横板旋转 | 已提交（`a488c1f` + `9b05e7f`） |
| LayoutMode 骨架/TopTabBar/XMB | 未提交（M0 评审） |
| 本机签名构建→装机→截图 验证循环 | 已打通（含 java 守护进程坑、设备 ID 正确值） |
| iiSU 逆向评估 + APK | `E:\iiSU\`（只读参考） |
| 知识库相关坑位 | `G:\知识库\鸿蒙移植经验库\` 26 号（手柄/黑屏）、27 号（ELF/musl） |
