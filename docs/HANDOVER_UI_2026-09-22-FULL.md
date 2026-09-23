# 旧柚 Pro UI 重组 — 完整接手文档

> 日期：2026-09-22（**2026-09-23 增补**：封面流定稿 + 滑动根因修复，见文末 §11） ｜ 仓库：`F:\旧柚pro\VintagePomeloPro` ｜ 分支：`UI`
> 提交身份：`yifengling0 <yifengling0@users.noreply.github.com>`
> 设备：Pura 80 Ultra（ID: `4NZ0225605001361`）
> 规划蓝图：`docs/UI-REORG-PLAN-2026-09-22.md`

---

## 0. 三十秒摘要

把"应用抽屉"升级为"游戏库管理器"——满屏封面 + 单击直玩 + 游戏元数据。参照 iiSU（逆向评估）、盖世模拟器（满屏大封面形态）、Playnite（游戏数据模型/双模式/三视图架构）。M0-M3 + 沉浸式全屏已完成，下一步是氛围背景层。

---

## 1. 仓库与分支状态

### 1.1 分支
- **当前分支**：`UI`（所有 UI 改动只在 UI 分支提交，永不并入/推送 main）
- **工作区状态**：干净（无未提交改动）
- **UI 分支领先 main**：10 个提交（从 `a488c1f` 到 `1554788`）

### 1.2 提交历史（UI 分支，最新→最旧）
```
1554788 fix(ui): clock/battery text visibility — high contrast color + bold weight
d2bf448 feat(ui): immersive fullscreen — hide system bars, compact top bar, custom clock/battery
7372eef feat(ui): M3 complete — card subtitle, sort modes, SortButton
daf47c6 feat(ui): M2+M3 — GameDetailSheet, background discovery, play time tracking
8731812 feat(ui): M1 default landing restructure — XMB demoted to LIST view, builtin tab demoted to menu
978af1a feat(ui): multi-layout mode UI restructure — Standard/XMB/Classic + visual polish
f33057c feat(model): LayoutMode enum + GlobalSettings.layoutMode + ui_reorg_v1 migration
3d49c45 fix(env): .gitattributes + HdsNavigationAttribute imports + getDesktopRootId declaration
9b05e7f feat(ui): 库页只做横板 + 左右横屏自动旋转
a488c1f feat(ui): 卡面 exe 图标 + 收藏徽标 + 竖版封面墙视图模式
```

### 1.3 diff 统计（main...UI）
22 个文件改动，+2196/−255 行。核心改动集中在 `Index.ets`（+619 行）。

---

## 2. 构建与验证流程

### 2.1 同步+编译验证
```bash
bash /e/iiSU/vpp-check.sh
```
此脚本同步源码到沙盒并验证编译。**注意**：脚本已修改为不覆盖 `build-profile.json5`，签名配置在同步后得以保留。

### 2.2 构建 HAP
```bash
"E:/vpp-build/build-hap.bat"
```
- 沙盒路径：`E:\vpp-build`（纯 ASCII 路径，避开 hvigor 拒绝非 ASCII 的坑）
- 已加 `--no-daemon` 参数修复 Java 守护进程残留问题
- 签名 HAP 产出路径：`E:\vpp-build\entry\build\default\outputs\default\entry-default-signed.hap`
- `build-profile.json5` 中签名配置已就绪

### 2.3 安装到真机
```bash
hdc -t 4NZ0225605001361 install "E:\vpp-build\entry\build\default\outputs\default\entry-default-signed.hap"
```
设备 ID 别带脏字符。

### 2.4 验证循环
每个里程碑装机一次，不做无意义反复。装机后 force-stop + aa start + `snapshot_display` 截图核对。

---

## 3. 已完成的里程碑

### M0 清理与合流 ✅
- 行尾归一 LF + `.gitattributes` 防复发
- 回滚 `wineTextInputPreedit` 1参→3参改动（真实 Linux 构建时 d.ts 由 native 再生成）
- d.ts/import 类环境修复逐条评审（全部保留）
- 3 个分块提交

### M1 默认落地重构 ✅
- XMB 从独立 LayoutMode 降级为 STANDARD 模式内的 LIST 视图（ViewMode.LIST）
- LayoutMode 简化为两个值：STANDARD（默认）和 CLASSIC
- 内建分类从顶栏胶囊降级到右上角菜单按钮
- 默认落地 Windows 分类 + COVER 视图
- `ui_reorg_v1` 一次性迁移（老安装首启落 STANDARD+COVER）
- 紫色渐变背景（浅色/深色双方案）

### M2 详情页 + 列表视图 ✅
- 新建 `GameDetailSheet.ets` 组件（150 行）：覆盖层（背景图 + 封面缩略 + 标题 + 来源 + 上次游玩 + 累计时长 + 启动/收藏/设置按钮）
- 长按卡片→详情页（取代旧 3 键菜单对话框）
- `AppDescriptor` 添加 `backgroundPath` 字段
- `AppCatalogRules` 添加 `BACKGROUND_PRIORITY` 和 `chooseBackgroundFileName()`
- `AppCatalogService` 添加 `findBackground()` 扫描 background.*/bg.*/fanart.* 文件

### M3 信息层 ✅
- `recent_launches_v2`：Map<string, number> 带 `{id: timestamp}`，迁移 v1 数据
- `play_time_v1`：Map<string, number> 会话时长累计
- `AppSettingsStore` 新增方法：`getLastPlayedAt()`、`getPlayMinutes()`、`addPlayMinutes()`
- 卡面副行：COVER 视图显示"上次游玩"替代 exe 路径名（有历史时）
- 排序功能：SortMode 枚举（RECENT/NAME/PLAY_TIME），SortButton 在 TopTabBar 中循环切换

### 沉浸式全屏 + 紧凑布局 ✅
- STANDARD 模式隐藏系统状态栏 + 导航指示条（`setSpecificSystemBarEnabled`）
- PageSurface padding：STANDARD 模式 top=0
- TopTabBar 重写为两行紧凑布局：
  - 第一行 = [时间电量] [分类胶囊] ... [排序] [视图] [内建] [设置]
  - 第二行 = [搜索框]
- 自定义时间+电量显示（30秒刷新，`batteryInfo.batterySOC`）
- 所有按钮尺寸压缩：36→32vp，fontSize 16→14
- contentPadding 压缩：phone 16→12, tablet 28→20, landscape 12→8
- Grid 间距压缩：columnsGap 10→6, rowsGap 10→6
- coverWallHeight 压缩：phone 300→260, tablet 280→250
- STANDARD 模式下抑制 CatalogScroll/CatalogEmptyScroll 中的重复搜索框
- 时间电量文字可见性修复（`usesDarkAppearance() ? '#FFFFFFFF' : '#FF1A1525'` + Bold + fontSize 13）

---

## 4. 关键代码结构

### 4.1 Index.ets（2310 行）— UI 主页面

**状态变量**（`Index.ets:103-168`）：
| 变量 | 类型 | 说明 |
|---|---|---|
| `apps` | `AppDescriptor[]` | 全部已发现应用 |
| `visibleApps` | `AppDescriptor[]` | 当前过滤+排序后的可见应用 |
| `filter` | `CatalogFilter` | 当前分类（默认 `'windows'`） |
| `listFocusIndex` | `number` | LIST 视图聚焦项索引 |
| `detailSheetApp` | `AppDescriptor \| null` | 详情页当前应用 |
| `detailSheetVisible` | `boolean` | 详情页是否显示 |
| `sortMode` | `SortMode` | 排序模式（RECENT/NAME/PLAY_TIME） |
| `clockText` / `batteryText` | `string` | 自定义时间电量显示 |
| `settings` | `GlobalSettings` | 全局设置（含 layoutMode, viewMode） |
| `runningAppIds` | `Set<string>` | 运行中的应用 ID 集合 |
| `favoriteIds` | `Set<string>` | 收藏应用 ID 集合 |
| `builtinMenuVisible` | `boolean` | 内建分类菜单是否展开 |

**关键 @Builder 方法**（行号）：
| 方法 | 行号 | 说明 |
|---|---|---|
| `SideBar()` | 1378 | CLASSIC 模式侧边栏 |
| `AppGrid()` | 1509 | 封面墙 Grid（COVER/GRID/LARGE/COMPACT） |
| `CatalogCard()` | 1529 | 单个卡片（调用 AppCard 组件） |
| `CatalogScroll()` | 1551 | CLASSIC 模式滚动容器 |
| `EmptyLibraryState()` | 1580 | 空状态展示 |
| `LibraryPane()` | 1663 | CLASSIC 模式库面板 |
| `Library()` | 1685 | CLASSIC 模式完整库 |
| `PhoneLibraryNavigation()` | 1832 | CLASSIC 模式手机导航 |
| `TopTabBar()` | 1916 | STANDARD 模式顶栏（两行紧凑） |
| `SortButton()` | 2008 | 排序切换按钮 |
| `ViewModeSwitcher()` | 2032 | COVER/LIST 视图切换按钮 |
| `StandardLayout()` | 2052 | STANDARD 模式主布局 |
| `ListView()` | 2090 | LIST 视图（原 XMB 收敛） |
| `PageSurface()` | 2161 | 页面 Stack（渐变背景 + 内容 + 详情页覆盖层） |

**PageSurface Stack 结构**（`Index.ets:2161-2302`）：
```
Stack({ alignContent: Alignment.Bottom })
├── [1] 用户自定义页面背景（条件：usePageBackground && pageBackgroundUri）
├── [2] Row 内容容器
│   ├── CLASSIC: SideBar + Library 或 PhoneLibraryNavigation
│   └── STANDARD: StandardLayout()
├── [3] DesktopLayer（浮窗模式）
├── [4] WineEngineBlockingOverlay
├── [5] 详情页覆盖层（detailSheetVisible && detailSheetApp）
│   ├── 背景图（backgroundPath 或 coverPath+blur）
│   ├── 暗化遮罩 (#CC000000)
│   └── GameDetailSheet 组件
└── Stack 背景：linearGradient（紫色渐变，浅色/深色双方案）
```

### 4.2 AppCard.ets（446 行）— 卡片组件

- 支持 4 种视图模式：COVER（封面墙）、GRID、COMPACT、LARGE
- COVER 模式：美术全出血 + 底部渐隐 + 标题 + 副行（上次游玩/exe路径） + 徽标（收藏★/运行中●） + 右上设置浮钮
- 图标链优先级：coverPath → exe 内嵌图标（PE .rsrc 提取） → 字母占位
- `lastPlayedText` prop：COVER 视图副行显示"上次游玩"替代 exe 路径名
- 交互：单击=启动，长按=详情页

### 4.3 GameDetailSheet.ets（151 行）— 详情页覆盖层

- Stack 布局：背景层 + 内容层
- 背景层：`backgroundPath`（优先）或 `coverPath` + blur(60) + opacity(0.3/0.4)
- 内容层：封面缩略(120×160) + 标题(24fp) + 来源 + 上次游玩 + 累计时长 + 运行状态
- 操作按钮：启动/关闭（accent色/danger色） + 收藏（金色星） + 设置
- 回调接口：`onLaunch`、`onClose`、`onToggleFavorite`、`onOpenSettings`

### 4.4 AppModels.ets（1444 行）— 数据模型

**枚举定义**：
```typescript
ViewMode: GRID | COVER | COMPACT | LARGE | FILES | LIST
LayoutMode: CLASSIC | STANDARD
SortMode: RECENT | NAME | PLAY_TIME
```

**AppDescriptor 接口**（`AppModels.ets:414-424`）：
```typescript
{
  id: string;
  title: string;
  source: AppSource;        // BUILTIN | DOWNLOAD
  directory: string;
  coverPath: string;
  backgroundPath: string;   // M2 新增
  available: boolean;
  launchTarget: LaunchTarget;
  displayModeOverride: DisplayMode | null;
}
```

### 4.5 AppSettingsStore.ets（627 行）— 设置存储

**存储键**：
| 键 | 说明 |
|---|---|
| `recent_launches_v1` | 旧版：string[]（已迁移） |
| `recent_launches_v2` | 新版：Map<string, number>（id→timestamp） |
| `play_time_v1` | Map<string, number>（id→累计分钟） |
| `ui_reorg_v1` | 一次性 UI 迁移标记 |
| `app_settings_v1` | 单应用设置（兼容追加，不升版本） |

**关键方法**：
- `getLastPlayedAt(appId): number` — 返回上次游玩时间戳
- `getPlayMinutes(appId): number` — 返回累计游玩分钟
- `addPlayMinutes(appId, minutes): Promise<void>` — 累加游玩时长

### 4.6 AppCatalogRules.ets（116 行）— 目录规则

**封面优先级**：`cover.png > cover.jpg > cover.jpeg > cover.webp > folder.png > ...`
**背景优先级**：`background.png > background.jpg > ... > bg.png > ... > fanart.png > ...`
**辅助程序过滤**：unins/uninstall/setup/install/crash/report 等排除

### 4.7 AppCatalogService.ets（767 行）— 目录服务

- `findBackground()`: 扫描游戏目录下的 background.*/bg.*/fanart.* 文件
- `findCover()`: 扫描 cover.*/folder.* 文件
- 所有 `AppDescriptor` 构造点已更新 `backgroundPath` 字段

---

## 5. ArkTS 约束与坑

| 约束 | 说明 |
|---|---|
| **不支持 `Record<string, number>`** | 需用 `Map<string, number>` 替代 |
| **不支持 spread 操作符 `...`** | 需手动构造对象字段 |
| **`saveAppSettings` 只接受 1 个参数** | AppSettings 对象，不是 (appId, settings) 两个参数 |
| **不要修改 native d.ts 声明** | `wineTextInputPreedit` 等，真实 Linux 构建时由 native 重新生成 |
| **hvigor 拒绝非 ASCII 路径** | 项目路径含中文（`旧柚pro`），必须用沙盒 `E:\vpp-build` |
| **`spawn java ENOENT`** | 旧 Java 守护进程残留，build-hap.bat 已加 `--no-daemon` |
| **CRLF 污染** | `.gitattributes` 已配置，但 submodule 中 autocrlf 无效，需 Python 批量转换 |
| **`isPhone()` 在横屏手机上返回 true** | Pura 80 Ultra 横屏时 breakpoint 仍是 sm/md，STANDARD 模式优先于 isPhone() 判断 |
| **`HdsTabsAttribute`/`HdsNavigationAttribute`** | API 26 SDK 下需要显式 import |
| **`AppDescriptor` 属性名** | `title`（不是 displayName）、`coverPath`（不是 iconPath） |
| **`AppSource`** | 只有 `BUILTIN` 和 `DOWNLOAD` 两个值 |

---

## 6. 下一步：氛围背景层（进行中）

### 6.1 设计目标
在 STANDARD 模式下，用最近启动/查看的游戏的封面或背景图做重度模糊全屏铺底，给启动器沉浸感（类似 Steam Big Picture / Playnite 全屏模式）。

### 6.2 实现方案

**新增状态变量**：
```typescript
@State private ambientAppId: string = '';
```
位置：`Index.ets` 状态变量区（约 line 111 附近）

**新增辅助方法**：
```typescript
private getAmbientApp(): AppDescriptor | null {
  if (this.ambientAppId.length === 0) return null;
  for (const app of this.apps) {
    if (app.id === this.ambientAppId) return app;
  }
  return null;
}

private getAmbientBackgroundPath(): string {
  const app = this.getAmbientApp();
  if (!app) return '';
  if (app.backgroundPath && app.backgroundPath.length > 0) return app.backgroundPath;
  if (app.coverPath && app.coverPath.length > 0) return app.coverPath;
  return '';
}
```

**设置 ambientAppId 的时机**：
1. `launch()` 方法（`Index.ets:1126`）— 启动游戏时设置
2. `showAppLongPressMenu()` 方法（`Index.ets:1203`）— 打开详情页时设置

**PageSurface 中插入氛围背景层**：
在 `Index.ets:2172`（用户自定义页面背景 if 块之后）和 `Index.ets:2173`（Row 内容容器之前）之间插入：

```typescript
// 氛围背景 — STANDARD 模式下用最近互动游戏的美术做模糊铺底
if (this.settings.layoutMode === LayoutMode.STANDARD && this.ambientAppId.length > 0) {
  Stack() {
    if (this.getAmbientBackgroundPath().length > 0) {
      Image(this.getAmbientBackgroundPath())
        .width('100%').height('100%')
        .objectFit(ImageFit.Cover)
        .blur(50)
        .opacity(0.3)
    }
  }
  .width('100%').height('100%')
  .hitTestBehavior(HitTestMode.None)
  .visibility(this.libraryChromeVisible() ? Visibility.Visible : Visibility.None)
}
```

**图片来源优先级**：`backgroundPath` > `coverPath` > 无（渐变背景兜底）

### 6.3 注意事项
- 氛围背景层仅限 STANDARD 模式
- 位于渐变背景层（Stack 的 `.linearGradient()`）和内容层（Row）之间
- `hitTestBehavior(HitTestMode.None)` 确保不拦截触摸事件
- 用户自定义页面背景（`usePageBackground`）优先级高于氛围背景
- 重度 blur(50) 使切换不那么突兀；后续可加 animateTo 做交叉淡入

---

## 7. 后续待完成项

### 7.1 封面墙瓦片视觉细节优化（中优先级）
- 更接近 iiSU/盖世风格
- 瓦片底部渐隐参数微调（透明→85%黑，高72-96vp）
- 标题字号/行数限制（16fp ≤2行）
- 徽标带视觉（收藏★金/运行中绿）
- 右上设置浮钮 40×40 常显 40%

### 7.2 ListView 视觉效果改进（中优先级）
- 更像 PlayStation XMB
- 聚焦项放大（封面 96vp），非聚焦 48vp 图标 + 13fp 标题
- 复用 AppCard 资产链（cover→icon→字母），禁止再自绘卡片
- 当前 ListView 在 `Index.ets:2090-2157`，已基本实现但视觉效果可进一步打磨

### 7.3 M4 元数据抓取（低优先级 — 需 API key）
- SteamGridDB（免费 API key；竖版封面/横版背景/图标/横幅）
- IGDB（发行日期/类型/开发商/简介）
- `metadata_cache_v1` 缓存键
- 匹配策略：目录名→规范化→精确→模糊；多结果弹确认
- 前置检查：`module.json5` 的 INTERNET 权限（已有）
- 展示侧零改动原则：抓取只往游戏目录写文件

### 7.4 M5 主题与合集（低优先级）
- 氛围背景自动取色（从封面/背景图提取主色调）
- 主题包（背景+强调色）
- 标签/合集分组（`tags: string[]` 字段已在规划中定义）

---

## 8. 关键文件清单

### 源项目修改文件（F:\旧柚pro\VintagePomeloPro）
| 文件 | 行数 | 说明 |
|---|---|---|
| `entry/src/main/ets/pages/Index.ets` | 2310 | UI 主页面（核心改动文件） |
| `entry/src/main/ets/components/AppCard.ets` | 446 | 卡片组件（4 种视图模式） |
| `entry/src/main/ets/components/GameDetailSheet.ets` | 151 | 详情页覆盖层组件（M2 新建） |
| `entry/src/main/ets/components/SearchBar.ets` | — | 搜索栏（添加 border） |
| `entry/src/main/ets/components/FileBrowserView.ets` | — | 文件浏览器（添加 backgroundPath） |
| `entry/src/main/ets/model/AppModels.ets` | 1444 | 数据模型（枚举 + AppDescriptor） |
| `entry/src/main/ets/model/AppCatalogRules.ets` | 116 | 目录规则（封面/背景优先级） |
| `entry/src/main/ets/service/AppCatalogService.ets` | 767 | 目录服务（findBackground + backgroundPath） |
| `entry/src/main/ets/service/AppSettingsStore.ets` | 627 | 设置存储（recent_v2 + play_time + 迁移） |
| `entry/src/main/ets/pages/AppSettings.ets` | — | 应用设置页 |
| `entry/src/main/ets/pages/HelpCenter.ets` | — | 帮助中心 |
| `entry/src/main/ets/pages/SystemSettings.ets` | — | 系统设置页（layoutMode 同步） |
| `entry/src/main/cpp/types/libentry/Index.d.ts` | — | 类型声明（getDesktopRootId） |
| `entry/src/main/resources/base/element/color.json` | — | 浅色配色（紫色方案） |
| `entry/src/main/resources/dark/element/color.json` | — | 深色配色（深紫色方案） |
| `.gitattributes` | — | 行尾归一规则（M0 新建） |

### 工具/脚本文件
| 文件 | 说明 |
|---|---|
| `E:\iiSU\vpp-check.sh` | 同步+编译验证脚本（不覆盖 build-profile.json5） |
| `E:\vpp-build\build-hap.bat` | 构建 HAP 批处理（已加 --no-daemon） |
| `E:\vpp-build\build-profile.json5` | 签名配置 |

### 关键文档
| 文件 | 说明 |
|---|---|
| `docs/UI-REORG-PLAN-2026-09-22.md` | 完整重构规划文档（M0-M5 路线图） |
| `docs/HANDOVER_UI_2026-09-22.md` | 早期交接文档 |
| `docs/HANDOVER_UI_2026-09-20.md` | 最早交接文档 |
| `docs/HANDOVER_UI_2026-09-22-FULL.md` | **本文档** — 完整接手文档 |

### iiSU 参考
| 文件 | 说明 |
|---|---|
| `E:\iiSU\iiSU-Alpha-7.4.apk` | iiSU Android APK（UI 设计参考） |
| `E:\iiSU\PORTING-ASSESSMENT.md` | iiSU 项目逆向评估文档 |

---

## 9. 规划文档关键决策记录

| 决策 | 理由 |
|---|---|
| XMB 从独立 LayoutMode 降级为 STANDARD 内 LIST 视图 | XMB 本质是"列表视图+纵向导航"，与 STANDARD 不该是平行宇宙；自绘不复用 AppCard 导致视觉资产断裂 |
| LayoutMode 只保留 STANDARD 和 CLASSIC | 简化模式体系，STANDARD 为默认全屏模式，CLASSIC 为工具向经典模式 |
| 内建分类从顶栏胶囊降级到右上角菜单 | 游戏启动器首屏不该出现开发工具 |
| 单击=直玩，长按=详情页 | 盖世式交互；详情页内含启动/关闭/收藏/设置/重新关联/删除 |
| 详情页为覆盖层（Stack 推入） | 保持单页架构，避免路由栈复杂化 |
| `recent_launches_v2` 用 Map<string, number> | ArkTS 不支持 Record<string, number> |
| 紫色渐变背景 | iiSU 风格的双色调参考，浅色/深色双方案 |
| 沉浸式全屏隐藏系统状态栏 | 最大化屏幕空间，自定义时间电量替代 |

---

## 10. 接手检查清单

新接手者请按以下步骤验证：

1. **确认分支**：`git branch --show-current` 应显示 `UI`
2. **确认工作区干净**：`git status` 应无未提交改动
3. **确认提交历史**：`git log --oneline -5` 最新提交应为 `1554788`
4. **确认 git 身份**：`git config user.name` 应为 `yifengling0`
5. **编译验证**：`bash /e/iiSU/vpp-check.sh` 应 0 错误
6. **构建 HAP**：`"E:/vpp-build/build-hap.bat"` 应产出签名 HAP
7. **装机验证**：`hdc -t 4NZ0225605001361 install <hap-path>` 应成功
8. **真机截图核对**：应看到沉浸式全屏 + 紫色渐变 + 自定义时间电量 + 封面墙

---

*文档结束。如有疑问，先读 `docs/UI-REORG-PLAN-2026-09-22.md` 规划蓝图，再读本文档。*

---

## 11. 2026-09-23 增补：封面流定稿与滑动根因（提交 fdf0d01 / cb2aa30）

### 11.1 新铁律（用户定案，永久有效）
1. **封面流永远单行**——横向 Scroll + 单行 Row，Switch/主机式横排；不做多行网格、不做上下滚动。
2. 瓦片规格回到紧凑档：手机高 **160vp**、宽 = 高×0.75 ≈ 120vp；信息条随比例收紧。
3. 搜索栏与卡片之间不留大空白：**Row 必须 `.alignItems(VerticalAlign.Top)`**（Row 默认垂直居中会垫出空白，已踩坑）。

### 11.2 横向滑动失效的根因（排查半日的真凶）
`StandardLayout` 把 `filter === 'windows'` 分支放在最前面，而落地分类就是 windows → **封面流分支从未渲染**，画面一直是 `WindowsRefreshContent` 的竖向 Grid（gridColumns 恰好给 5 列 1fr，造成"已经改好了"的假象）；横向滑动手势作用在竖向滚动容器上自然无效。
**修法**：scanning/empty/LIST/COVER 视图分支全部提前到 filter 分支之前；封面流抽成 `CoverFlow()` builder；windows 分类在封面流外套 `Refresh` 保留下拉刷新。
**教训**：改布局分支顺序时，先确认"当前分类实际走哪个 builder"（用 `uinput` 手势 + `snapshot_display` 前后帧对比做真机验证，别只看截图样式）。

### 11.3 当前实现（`Index.ets`）
- `CoverFlow()`：`Scroll(ScrollDirection.Horizontal)` 包 `Row({space:6})(alignItems Top)`，瓦片定宽，内容宽 = `coverFlowContentWidth(数量)`（≥一屏才可滚）。
- `coverWallHeight()`：phone 160 / xl 150 / 其余 165；`coverTileWidthVp()` = 高 × 0.75。
- 验证方法：`uinput -T -m 2200 600 600 600 400`（左滑）+ 前后 `snapshot_display` 逐字节对比。

### 11.4 真机保活（通宵工作用）
- `hidumper -s PowerManagerService -a '-t'` = keep screen on（`-f` 恢复）；系统超时本身 600s。
- 兜底循环：`E:\iiSU\keep-awake.sh`（每 4 分钟 keep-on + wakeup，nohup 后台）。

---

## 12. 2026-09-23 增补二：全局暗色 + 焦点体系（提交 36e4f2e）

### 12.1 全局暗色系（主机/掌机风格，用户定案）
- **base 与 dark 两套色板统一为近黑中性色**：基座 `#0B0B0F`、卡面 `#1C1C22`、玻璃 `#E61A1A20`、强调色亮紫 `#A78BFA`。两套一致 → **任何系统主题模式下都是暗色**，不会穿帮（这是最稳的做法，比只改 dark 安全）。
- PageSurface 基座渐变改暗色常量（原浅色紫渐变分支删除）。
- 默认 `themeMode` 改为 `DARK`；迁移键 `ui_reorg_v2 → v3`，存量安装一并切暗色。
- 设置页/帮助页等自定义色函数（`groupSurfaceColor`/`topicSurfaceColor`）本就有 dark 分支，随 `usesDarkAppearance()` 自动生效。

### 12.2 P1 氛围背景跟随焦点
- 新增 `@State focusedIndex`（视口中心所在瓦片），`Scroll.onDidScroll` 调用 `updateCoverFlowFocus(xOffset)` 追踪。
- 焦点变化 → `ambientAppId` 同步 → 模糊铺底跟着滑动实时变化（`blur(60)` + `opacity(0.45)` + `#8C0B0B0F` 暗化遮罩保证前景对比度）。
- **`ambientSource()` 修复**：裸路径必须补 `file://` 前缀，否则 Image 静默加载失败（详情页 `backgroundPath` 同款坑一并修复）。
- 分类切换/重扫描走 `resetCoverFlowFocus()` 回第一张，避免沿用上个分类的游戏。

### 12.3 P2 焦点卡放大（标题不动，用户定案）
- `AppCard` 新增 `@Prop focused`：`scale 1.14` + accent 2vp 描边 + 强化投影 + `zIndex` 提升。
- **只做视觉缩放**（scale 不改布局）→ Row 内卡片位置稳定，放大后不被邻卡压住。

### 12.4 焦点计算口径
```
unit   = 瓦片宽(vp) + 间距(6)
center = xOffset + viewportWidth/2 - 3
index  = clamp(floor(center / unit), 0, n-1)
```
调整瓦片尺寸（`coverWallHeight()`）时无需改这里——`unit` 由同一函数推导。

### 12.5 顶栏/底栏（用户确认冻结，勿动）
布局与样式保持 2026-09-22 定稿：顶栏 = 时间电量(12fp text_secondary + 分段电池图标) | 300vp 搜索框居中 | 排序 + 设置(40vp/17fp)；底栏 = 最近/收藏/Game(手柄)/运行中/Home 五元对称玻璃悬浮条。