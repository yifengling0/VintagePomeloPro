# 旧柚 Pro 启动器 UI 移植 — 交接与规划文档（v2，全量重写版）

> 更新：2026-09-20 晚 ｜ 仓库：`F:\旧柚pro\VintagePomeloPro` ｜ 分支：`UI`（基线 `main@73a8e08` = Release 1.4.0）
> **本文档是唯一交接入口**：接手人（人或 AI 会话）开工前必须通读 §0、§1、§2；其余按需查阅。
> ⚠️ 若接手工具读不了含中文的路径（`旧柚pro`），先把本文件复制成 ASCII 路径再喂给它，例如 `E:\handover-ui.md`。

---

## 0. 必读速览（30 秒）

**任务**：在 VintagePomeloPro（旧柚 Pro，Wine-on-HarmonyOS）上**继续设计并开发启动器的新 UI**——盖世模拟器/iiSU 式"游戏主机首页"观感（只借鉴通用形态，不抄任何闭源资源/代码/数据）。

**九条铁律（违反任何一条即为事故）**：
1. **只做横板 UI**：库页永远横屏，左↔右横屏自动旋转，不进竖屏。
2. **只改 UI 代码**：仅 `entry/src/main/ets/` 的 ArkTS 层；`cpp/`、`thirdparty/`、构建脚本不碰。
3. **只进 `UI` 分支，永不并入 `main`**。
4. **不占用 C 盘**（C 盘 92–94% 满；细则见 §1.2）。
5. **优先 WSL 测试**（Ubuntu-24.04 已就位且在 E 盘，方案见 §6）。
6. **全部完成再去真机**：真机是最后一次收尾会话，不是开发循环的一环（终检表见 §8-M5）。
7. **一切贴合项目本身**：先读 §4 项目约定，再设计 UI；禁止引入项目没用过的状态管理/路由/依赖。
8. 提交身份：`yifengling0 <yifengling0@users.noreply.github.com>`（`-c` 注入，见 §9）。
9. 每次改动必过编译校验（Windows 沙盒差分 §5 或 WSL 门禁 §6，后者为准）。

**当前状态**：5 项已完成（卡面 exe 图标 / 收藏徽标 / 封面墙视图 / 横板锁定 / IconExtractor 修复）→ 全在本地 `UI` 分支（2 提交，6 文件 +241/−33），**未推送**（凭据 403，§2.3）；真机未验证。
**⚠️ 异常**：工作区有一处**非交接会话所做**的未提交改动（`Index.ets` 渐变背景），见 §2.4——接手人先处置它再开工。

---

## 1. 铁律细则

### 1.1 用户指令（本会话原话归纳）
横板 UI + 左右横屏自动旋转；只做 UI 部分代码更新不参与（native）；从 `github.com/yifengling0/VintagePomeloPro` 拉最新；全部更新上 `UI` 分支不并入主线；数据不存 C 盘；优先 WSL 测试；全部完成再去真机；一切以贴合项目本身然后开发设计 UI。

### 1.2 磁盘纪律（不占 C 盘）——所有数据的合法落点

| 数据 | 落点 | 状态 |
|---|---|---|
| 项目源码（唯一真源） | `F:\旧柚pro\VintagePomeloPro` | ✅ 已在 |
| Windows 编译沙盒 | `E:\vpp-build`（hvigor 拒绝非 ASCII 路径，见 §5.1） | ✅ 已建 |
| Windows hvigor 缓存 | `E:\vpp-hvigor`（`HVIGOR_USER_HOME`） | ✅ 已建，实测 C 盘零增长 |
| WSL 发行版 vhdx | `E:\WSL\Ubuntu2404` | ✅ 已在 E 盘（勿重装到默认 C:） |
| WSL 内构建树 | `/data/share/wineohos`（WSL fs 内，物理落在 E: 的 vhdx） | 待建，§6.3 |
| 参考素材 | `E:\iiSU\`（iiSU 评估+APK）、`E:\andy88\`（知识库素材，可删） | ✅ |
| 知识库 | `G:\知识库\` | ✅ |

**禁止**：往 `%USERPROFILE%\.hvigor`、`C:\Users\...\AppData\Local\Temp` 长期写构建产物；`wsl --install` 新发行版（默认落 C:）；任何 npm/ohpm 缓存用默认 C: 路径。

### 1.3 测试纪律
开发循环 = **改代码（F:）→ 编译校验 → WSL 门禁（§6）→ 下一个功能**。真机只在 §8-M5 的最终会话出现一次。

---

## 2. 仓库与分支状态

### 2.1 拓扑

```
origin/main = 73a8e08 (Release 1.4.0)
    └── UI = 9b05e7f  feat(ui): 库页只做横板 + 左右横屏自动旋转
            a488c1f  feat(ui): 卡面 exe 图标 + 收藏徽标 + 竖版封面墙视图模式
```

`git diff main..UI --stat`：6 文件 +241/−33（AppCard/AppModels/Index/IconExtractor/DeviceCapabilityPolicy/EntryAbility）。

### 2.2 上游同步（上游推进很快，本次工作期间就发了 9 个提交）

```bash
git fetch origin
git checkout main && git merge --ff-only origin/main   # main 只快进
git checkout UI && git rebase main
```

**已知冲突区**（上次变基实际撞过，都在 `Index.ets`）：① `refreshRunningState` 附近（上游新增 `runningFilterLabel()` 与我们的 `refreshFavoriteIds()` 同位置——两个都留）；② `Header` builder（上游简化成固定 64vp 行、删了手机/md 分支——**取上游结构，只把封面墙按钮插回按钮行**）。`AppCard.ets`/`IconExtractor.ets` 上游近期未动，低风险。
另：上游 1.4.0 重构了断点模型——`isPhone()` = `!isWideBreakpoint()`（<960vp 全走手机壳）；我们的封面墙断点已按新语义对齐。

### 2.3 推送阻塞（待用户处理）

本机 Git 凭据助手缓存账号 **`rootrd`**，对 `yifengling0/VintagePomeloPro` 无写权限（403；`rootrd` 名下无同名仓库，非 fork 问题）。解法三选一：① 换有权限账号（`git credential-manager` 清旧凭据 / `gh auth login`）② 加 `rootrd` 为协作者 ③ 用户在已登录工具里推。解开后：`GIT_TERMINAL_PROMPT=0 git push -u origin UI`。

### 2.4 ⚠️ 工作区未提交改动（非交接会话所做，先处置再开工）

`git status` 出现 ` M entry/src/main/ets/pages/Index.ets`，diff 如下（`Index.ets:1885` 附近，`PageSurface` 根容器）：

```diff
-    .backgroundColor($r('app.color.page_background'))
+    .linearGradient({
+      angle: 135,
+      colors: this.usesDarkAppearance()
+        ? [['#1E1830', 0], ['#2A2040', 0.5], ['#332850', 1]]
+        : [['#F0EBF5', 0], ['#E0D5ED', 0.5], ['#D4C5E8', 1]]
+    })
```

（页面背景纯色 → 明/暗紫调渐变。）**交接会话没有做过这个改动**——来源不明（用户手改或另一 AI 会话）。处置建议：确认作者后，要么 `git checkout -- entry/src/main/ets/pages/Index.ets` 丢弃，要么走正常流程并入 `UI` 分支（方向上与 §7.6 氛围主题一致，但**不得在未确认前盲提交**）。本文档其余部分不依赖此改动。

---

## 3. 已完成改动详解（UI 分支两个提交的内容）

### 3.1 `components/AppCard.ets`（+184，改动最大）

| 位置 | 内容 |
|---|---|
| `:25` `aboutToAppear` | 触发一次性图标提取 `requestIcon(false)` |
| `:30` `requestIcon(coverUnusable)` | 门槛：仅 `AppSource.DOWNLOAD` && 无自定义封面 && `app.available` && exe 非空；**每卡一次，失败不重试**（防每 1.5s 重绘反复解析 PE） |
| `:39` `loadIcon` | 异步 `IconExtractor.extract(exe)`；磁盘缓存 PNG（mtime 校验）→ `@State iconPath` |
| `:50` `hasCover` / `Cover` 内 `onError` | 封面被删/损坏 → `coverFailed` + `requestIcon(true)`；**回退链：封面 → exe 图标 → 首字母**，不留空白卡面 |
| `:172` `ArtPlate` | 图标/字母统一底板；图标 `.interpolation(High)`（32–256px 放大防糊） |
| `:199` `ArtFallback` | 原占位图改造版，内部复用 `ArtPlate` |
| `:336` `CoverWall` | **封面墙瓦片**：美术全出血（竖版）、标题+exe 名压底部渐隐层、徽标左上、设定入口右上浮层钮；`@Prop coverWallHeight` 由页面按断点传入 |
| 徽标 | `favorite`（`收藏`/★ 金）插入两处徽标行 |

测试钩子 `wine-app-card-<id>`、`wine-app-settings-<id>` 全保留（`scripts/verify_*.ps1` 不受影响）。

### 3.2 `model/AppModels.ets`（+2）
`ViewMode` 枚举 + `COVER = 'cover'`（`:47`）。`viewMode` 持久化无白名单校验，加值安全。

### 3.3 `pages/Index.ets`（+59）

| 位置 | 内容 |
|---|---|
| `:112` `favoriteIds: Set<string>` | 与 `runningAppIds` 同构；在 `refreshRunningState()` 一并刷新（覆盖"设置页改收藏后返回"全部时机） |
| `:817` `refreshFavoriteIds()` | 从 `AppSettingsStore` 汇总 |
| `:1010` `coverWallColumns()` | phone 3 列 / xl 6 列 / 其余 4 列 |
| `:1020` `coverWallHeight()` | phone 300 / xl 240 / 其余 280；**必须与列数联动改**（列多格窄就降高度） |
| `:1067` | `phoneGridItemMinHeight` 的 COVER 分支（瓦片高+阴影边距） |
| 视图入口 ×3 | Header 按钮行第 5 钮（`sys.symbol.rectangle_grid_2x1`）、手机溢出菜单（**索引重排 GRID/COVER/COMPACT/LARGE/FILES = 0/1/2/3/4**）、`phoneViewModeIcon()` |
| `:408` `restoreLibraryOrientation` | 退回库页不再先转竖屏（消除闪烁），直接回仅横屏自动旋转 |

### 3.4 `common/DeviceCapabilityPolicy.ets`（+19/−13）
删 `enableSensorRotation`（旧：任意方向含竖屏、仅手机）；新增 `:36` `applyLauncherLandscapeRotation` = `AUTO_ROTATION_LANDSCAPE_RESTRICTED`（SDK 语义：跟随传感器在左/右横屏间旋转，受控制中心旋转开关约束）；`2in1/PC` 跳过（自由缩放窗口）。若要无视系统旋转锁定：改 `AUTO_ROTATION_LANDSCAPE`（一个词，真机试用后定）。

### 3.5 `entryability/EntryAbility.ets`（1 行，`:104`）
主窗口换新策略——平板也从此走横板。

### 3.6 `service/IconExtractor.ets`（+8，`:226`）
修复名字型资源条目（`id & 0x80000000`）被整组跳过 → 部分 exe 提不出图标。实测：`4399启动器` 失败→成功（48×48）；Unity/Tuanjie/UnityHub 等结果逐字节不变；主流产出 256×256 PNG；唯一无解是 Go 程序（无图标资源）→ 字母兜底。命中率 6/7。`FileBrowserView` 同享修复。

### 3.7 刻意不做（防误判遗漏）
默认视图仍 GRID（比例未经真机确认，M1 再改）；卡面无收藏开关（等长按菜单重设计，§7.5）；Wine 桌面方向设置未动（仍只管桌面窗口）；内建程序不做图标提取（有清单封面）。

---

## 4. 项目自身约定速查（"贴合项目本身"——设计前必读）

| 约定 | 内容 | 违反后果 |
|---|---|---|
| 状态管理 | **严格 ArkUI V1**（`@Component/@State/@Prop/@Link/@StorageProp/@Watch/@Builder`）；全项目零 V2 装饰器 | 编译不过 |
| 页面跳转 | 经典 `@ohos.router`（`router.pushUrl`，7 处），无 `NavPathStack` | 风格分裂 |
| HDS 组件 | `HdsNavigation/HdsTabs/hdsMaterial` 是硬依赖，且有 `canIUse('SystemCapability.UIDesign.HDSComponent.Core') && sdkApiVersion >= 23` 门槛 | 低版本崩 |
| 测试钩子 | 几乎所有可点元素带稳定 `.id()`（`library-*`、`wine-app-*`），被 `scripts/verify_*.ps1` 消费 | 验证脚本失效 |
| 设置存储 | `AppSettingsStore` 每个 setter 是**手写全量拷贝** `GlobalSettings`——新增字段要同步改 ~8 处 | 字段静默丢失 |
| 滚动身份 | 每个 filter 一套独立 `Scroller` + 滚动偏移快照/恢复（`scrollOffsets`） | 切分类丢位置 |
| Grid 防盖 | `phoneGridItemMinHeight` 显式预留高度（Grid 对 auto 高卡片测量不足的老坑） | 下一行盖上一行 |
| 断点 | `isWideBreakpoint`（<960vp 手机壳 / lg+ 侧栏）；`isPhoneLandscape()` 按视口宽高比 | 布局错乱 |
| 手机底栏 | HdsTabs 56vp 浮动条，规范在 `docs/bottom-navigation-layout.md` | 溢出/裁剪 |
| 提交风格 | `feat(ui):` / `fix(ui):` 前缀 + 中文描述（见 `git log`） | 历史不一致 |
| 构建规则 | 只改 ArkTS → `make NATIVE_ARCH=arm64-v8a hap` 足够，无需卸载重装（`.claude/rules/build-and-log.md`） | 浪费整机重编 |
| 启动链 | 卡片点击 → `launch()` → 显示模式冲突检查 → 串行化 `enqueueLaunch` —— 新 UI 的任何"启动"入口都必须走这条链 | 并发启动 Wine 会话 |

---

## 5. 验证体系（Windows 侧，改动后立即跑）

### 5.1 为什么需要沙盒
hvigor 拒绝含非 ASCII 的项目路径（`F:\旧柚pro\...` → 错误 `00306003`），junction/符号链接无效（hvigor 解析真实路径）。

### 5.2 一键脚本
```bash
bash /e/iiSU/vpp-check.sh /tmp/本次.log
```
同步（`build-profile.json5`、两个 module 配置、`ets`、`resources`、**`cpp/types`、`entry/oh_modules`**——后两项漏了会出一堆 libentry 假错误）→ 在 `E:\vpp-build` 跑 `CompileArkTS`。环境：DevEco 自带 node/ohpm/hvigor，`HVIGOR_USER_HOME=E:\vpp-hvigor`。

### 5.3 差分基线法
本地 SDK（API 26 Beta）≠ 工程目标（`build-profile.json5.example` 为 `6.1.0(23)`），编译永远有 ~10 个固定环境错误（HDS 属性类型 ×6 / libentry 原生声明 ×2 / 上游 API 签名 `Index.ets:575`）。**判定标准 = 改动前后错误集与警告集逐条一致**（当前基线 ERROR 10 / WARN 405），不是零错误。做法：`grep -oE "Error Message: .*At File"` 两边 sort 后 diff。
**注意：此法只是快速门禁；更强的是 §6 的 WSL 门禁（SDK 版本对齐后应零错误）。**

### 5.4 环境坑速查
| 坑 | 处置 |
|---|---|
| 非 ASCII 路径 `00306003` | 沙盒编译 |
| API 26 版本串 | `build-profile.json5` 写 `"26.0.0"`（≤25 才是 `5.0.0(12)` 括号格式）；仓库里那份是本地校验用的，**无签名、已 gitignore、勿提交** |
| hvigor 守护进程锁目录 | `hvigorw --stop-daemon` 后删 |
| C 盘 | 全部缓存已指 E:（§1.2） |

### 5.5 辅助工具（`E:\iiSU\`）
`pe-icon-probe.js`/`pe-icon-probe-v2.js`（exe 图标命中率探测）、`diag.js`（PE 资源目录 dump）、`PORTING-ASSESSMENT.md` + `extract/`（**iiSU 逆向评估——新 UI 的交互/信息架构参考样板**）。

---

## 6. WSL 构建与测试环境（优先测试通道，重点建设）

### 6.1 现状盘点（已核实）
- 发行版 **Ubuntu-24.04** 已装、WSL2、当前 Stopped（用时自启）。
- **vhdx 已在 `E:\WSL\Ubuntu2404`**（C 盘零占用，✅ 符合磁盘铁律）。
- WSL 内已存在 `/data`（含 `cache/`、`out/`）——与用户真实构建机 `/data/share/wineohos` **同构**，沿用此约定。
- 磁盘余量：vhdx 所在盘可用 931G（稀疏扩展）。
- node/ohpm/hvigor 尚未确认安装（待 §6.3 步骤 2）。

### 6.2 分层目标（按投入从低到高）

| 层 | 内容 | 是否必做 | 产出 |
|---|---|---|---|
| **L1 编译门禁** | WSL 内装 Huawei command-line-tools（Linux 版），SDK 版本**对齐仓库 `build-profile.json5.example`**，跑全量 `CompileArkTS` | **必做，每次改动** | SDK 版本匹配后 HDS/libentry 假错误应消失 → **以"零错误"为准**（比 §5.3 差分法强一级） |
| L2 打包验证 | `assembleHap`（未签名；CI 有 `hap-unsigned` 目标，"Never disguise unsigned output"） | 可选，里程碑节点 | 验证资源/rawfile/打包链 |
| L3 完整 native | `git submodule update --init` + wine/box64/mesa 全链 | **默认不做**（数十 GB + 数小时；留给用户真实构建机） | 可安装 HAP |

### 6.3 建设步骤（命令级）

```bash
# 0) 进入 WSL
wsl -d Ubuntu-24.04

# 1) 目录约定（沿用真实机，物理落在 E: 的 vhdx 内）
sudo mkdir -p /data/share/wineohos && sudo chown -R $USER /data/share

# 2) 安装 Huawei command-line-tools（Linux 版 zip，华为开发者官网下载后放入 WSL）
sudo mkdir -p /opt/harmony && sudo chown $USER /opt/harmony
unzip commandlinetools-linux-*.zip -d /opt/harmony/cli
echo 'export PATH=/opt/harmony/cli/bin:$PATH' >> ~/.bashrc && source ~/.bashrc
hvigorw --version && ohpm -v     # 自检

# 3) 源码：以 F: 仓库为远端，克隆到 ASCII 路径（WSL 内路径无中文 → 不再触发 00306003）
cd /data/share/wineohos
git clone /mnt/f/旧柚pro/VintagePomeloPro VintagePomeloPro   # 本地远端，保留全部历史
cd VintagePomeloPro && git checkout UI

# 4) 构建配置：复制 example（SDK 版本按 example 原样 6.1.0(23)；勿照抄 Windows 沙盒的 26.0.0）
cp build-profile.json5.example build-profile.json5   # L1/L2 不签名，占位即可

# 5) L1 门禁
ohpm install --all
hvigorw --mode module -p module=entry@default -p product=default default@CompileArkTS
#    L2（可选）：hvigorw assembleHap（无 thirdparty 时以 hvigor 直跑为准）
```

### 6.4 Windows ↔ WSL 协作流（F: 是唯一真源）

```
改代码（F:\旧柚pro\...，IDE 在 Windows）
  → bash /e/iiSU/vpp-check.sh …           # 快速差分门禁（秒级）
  → wsl: cd /data/share/wineohos/VintagePomeloPro \
        && git fetch /mnt/f/旧柚pro/VintagePomeloPro && git reset --hard FETCH_HEAD
  → WSL L1 编译门禁（SDK 对齐，零错误为准）
  → git 提交只在 F: 仓库做；WSL 树视为只读镜像
```
Windows 沙盒（§5）和 WSL 门禁（§6）**都要过**；前者快、后者强。

### 6.5 C 盘核查清单（WSL 侧）
vhdx 位置确认在 `E:\WSL\Ubuntu2404`（勿 `wsl --install` 新发行版）；hvigor/ohpm 缓存在 WSL fs 内（物理=E: 的 vhdx）；Windows 侧 `HVIGOR_USER_HOME=E:\vpp-hvigor`。大构建后用 WSL 内 `du -sh ~/.hvigor` + Windows `df /c` 复核。

### 6.6 WSL 阶段的"完成"定义（DoD）——满足才准进真机
1. 全部里程碑（§8 M1–M4）功能实现；
2. WSL L1 **零错误**（或与基线差异可解释）+ L2 HAP 可产出（可选）；
3. 视觉核对：DevEco 预览器（Windows 侧，打开 F: 工程）逐屏截图核对 §7 设计规格；
4. §8 M5 终检表全部条目在文档里"预签"完。

---

## 7. 新 UI 设计方案（目标形态蓝图——在 VintagePomeloPro 上继续设计）

> 全部落在现有工程结构上，遵守 §1 铁律与 §4 约定。

### 7.1 目标与原则
**一句话**：把"应用抽屉"升级成"游戏主机的首页"（盖世/iiSU 式观感，100% 自有实现）。
① 美术优先：封面即界面，文字压渐隐层；② 横板专属（16:9~21:9，左轨+内容区）；③ 零美术兜底链（✅ 已实现）；④ 信息克制（卡面只放 标题/上次游玩/状态徽标）；⑤ 复用现有资产（COVER 瓦片、IconExtractor、favoriteIds、断点体系、`.id()` 钩子、`launch()` 链）。

### 7.2 布局蓝图（横板首页，"方案 B"）

```
┌──────────────────────────────────────────────────────────────┐
│ 顶栏 56–64vp（沉浸）: 旧柚Pro ▏搜索框(常驻) ▏视图切换 ▏设置     │
├────────┬─────────────────────────────────────────────────────┤
│ 导航轨  │  Hero 轮播（高 240–280vp，最近游玩 Top3，5s 自动）     │
│ 72/264 │  ┌─────────────────────────────────────────────────┐ │
│ vp     │  │   大图封面 + 标题 + 上次游玩 + [ ▶ 继续游戏 ]      │ │
│        │  └─────────────────────────────────────────────────┘ │
│ 最近   │  最近游玩 ──────────────────────────────→             │
│ 收藏   │  [封面][封面][封面][封面][封面] →                      │
│ 全部   │  收藏 ────────────────────────────────→               │
│ 内建   │  [封面][封面][封面] →                                  │
│ 运行中 │  全部游戏 ─────────── [⊞ 切换网格视图] →               │
│        │  [封面][封面][封面][封面][封面][封面] →                │
└────────┴─────────────────────────────────────────────────────┘
（窄横屏：导航轨折叠为顶部横向分类条；分类页=现有 COVER 网格原样保留）
```

### 7.3 组件级设计与代码落点

| 设计元素 | 动作 | 文件 | 要点（含 §4 约定） |
|---|---|---|---|
| `HomeHero.ets` | 新建 | `components/` | Swiper(autoplay 5s)+大卡全出血+渐隐层+`▶ 继续游戏`（**必须走现有 `launch()` 冲突检查链**）；无最近记录→欢迎卡 |
| `CoverRow.ets` | 新建 | `components/` | 行标题+横向 Scroll+`ForEach` 复用 `AppCard(COVER, coverWallHeight:220)`；**不另造瓦片** |
| `ViewMode.HOME` | 改 | `AppModels.ets` + `Index` 渲染分支 | 枚举加值（已验证无白名单问题）；沿用 `catalogCardViewMode()` 的 FILES→GRID 同款映射思路 |
| 瓦片信息层 v2 | 改 | `AppCard.CoverWall()` | §7.4 |
| 长按菜单重设计 | 改 | `Index.showAppLongPressMenu()` | §7.5 |
| 氛围背景层 | 改 | `Index.PageSurface()` 背景 Stack | §7.6（先处置 §2.4 那条未提交改动） |
| recent v2 | 改 | `service/AppSettingsStore.ets` | `recent_launches_v1`→v2 带时间戳（**setter 全量拷贝 ~8 处同步**） |

### 7.4 COVER 瓦片信息层 v2 规格
- 渐隐层：180°，透明→85% 黑，高 72–96vp（现 ~110 偏高）；标题 16fp 白 ≤2 行。
- **副行替换**：exe 文件名（对玩家无意义）→ **"上次游玩 3 天前"**（依赖 recent v2；无记录显示目录名）。
- 徽标带（左上，优先级序）：收藏★金 → 运行中绿 → 内建蓝。设置浮钮右上 40×40，常显 40% 透明度。
- 按压/焦点态：保留 scale 0.985；**新增选中态**（2vp accent 边框+轻上浮）——为手柄 dpad 预留（真接手柄是后排项，坑位见知识库 26 号文）。
- 空库引导：行/网格末尾追加虚线"+ 导入游戏"卡（点击=现有导入/扫描入口）。

### 7.5 交互设计
- **长按/右键 → 底部弹层**（HDS sheet 或 `Stack`+`translate`）：启动(或"关闭") / **收藏开关**（就地解决卡面无开关且无误触启动风险）/ 应用设置 / 重新关联(不可用时) / 删除卡片(不可用时) / 取消。消除现 3 键 `promptAction` 溢出。
- 搜索常驻顶栏；Hero"继续"直启；全部新元素带 `.id()`（§4 钩子约定）。

### 7.6 氛围主题（阶段 1，纯 ArkTS）
氛围背景 = 当前 Hero 封面 + `blur(40–60)` + 透明度 0.25–0.35 + 暗化遮罩；轮播切换 500ms `animateTo` 过渡；**用户手动背景（现有 `usePageBackground/pageBackgroundUri` 设置）优先级高于氛围背景**。阶段 2（后排）：主题包预设（背景+强调色）；音乐/视频不承诺。

### 7.7 路线对比
A 渐进打磨（默认视图 COVER + 瓦片 v2）：小工作量/低风险；B 首页化（Hero+横向行）：中工作量/**高观感收益**（盖世/主机感核心）。**A 是 B 的子集，A 先行、B 为目标**。

---

## 8. 实施路线（里程碑；WSL-first / 真机-last）

### M0 — WSL 环境搭建（§6.3 全步骤）
**验收**：WSL 内 `hvigorw --version` 可跑；UI 分支 L1 编译**零错误**（HDS/libentry 假错误应随 SDK 对齐消失；若仍有，记录为该环境基线并走差分法）。

### M1 — 观感地基（A 方案）
默认视图改 `ViewMode.COVER`（`createDefaultGlobalSettings`，仅影响全新安装）+ 瓦片信息层 v2（除"上次游玩"外无新数据依赖）+ 长按菜单重设计（§7.5）。
**验收**：§5+§6 双门禁过；DevEco 预览器逐屏对照 §7.4；长按菜单全项可用、收藏可就地切换。

### M2 — 首页化（B 方案核心）
`ViewMode.HOME` + `HomeHero` + `CoverRow` + `Index` 渲染分支。
**验收**：横板三断点布局正确；Hero 直启成功；无最近记录显示欢迎卡；首页↔分类页切换不丢滚动位置（沿用 `scrollOffsets` 快照）。

### M3 — 氛围主题阶段 1 + 焦点态（§7.6；开工前先处置 §2.4 改动）
**验收**：轮播切换无闪；预览器帧率正常（氛围层为静态图+模糊，不参与每帧绘制）；手动背景优先级正确。

### M4 — 内容侧（含外部依赖项）
- **封面抓取（观感最大杠杆）**：推荐 **SteamGridDB**（免费 API key；备选 TheGamesDB）。**展示侧零改动**——`AppCatalogRules.chooseCoverFileName()` 优先级本就是 `cover.png/jpg/jpeg/webp, folder.*`，抓取服务只需把图写进游戏目录为 `cover.png`。需做：名称匹配（精确→模糊，多结果给确认 UI）、缓存去重、失败回退。先查 `module.json5` 是否已有 `ohos.permission.INTERNET`。法律：按其 ToS、标注来源；**别抓 iiSU 的资源包**。
- recent v2 + 游玩时长（写入点 `AppSessionService.ets:334`；时长需会话结束事件）+ 排序控件（最近/名称/时长）。
- 合集/分组：按 Wine 前缀（`AppSettings.prefixId` 已有）/引擎启发式。

### M5 — 一次性真机会话（**唯一一次真机**，前置=§6.6 DoD 全满足）
1. 构建：优先用户真实 Linux 机（`make NATIVE_ARCH=arm64-v8a hap`，有签名材料与全部缓存）；或本地提供 `.ohos` 签名材料后出签名包。
2. 部署：hdc 用 DevEco 自带（Windows 侧）；流程照 `.claude/rules/build-and-log.md`（只改 ArkTS 无需卸载重装）。
3. **终检表**（合并验证清单 + 各里程碑验收）：
   - 封面墙比例（不裁脸/不空旷；不行调 `coverWallHeight()/coverWallColumns()`，两者联动）
   - 无封面游戏出真实图标；无图标资源（Go 程序）落字母；删 `cover.png` 后回退不空白
   - 收藏徽标/就地开关生效；长按菜单全项
   - 左右横屏自动、竖屏不可达、桌面返回无竖屏闪；桌面内方向仍由"桌面方向"设置管
   - 手机/平板/PC 三态；滚动流畅（图标提取无可感知卡顿）
   - 首页 Hero 直启；氛围背景切换；手柄焦点态（若做）
4. 终检问题回改 → 重走 §5+§6 门禁 → 复测（仅这一阶段允许真机迭代）。

### M6 — 例行
上游发版即变基（§2.2 流程与冲突区）；每次改动双门禁。

---

## 9. 快速上手命令速查

```bash
# ── Windows 快速门禁（秒级）───────────────────────────────
bash /e/iiSU/vpp-check.sh /tmp/check.log        # 差分基线：ERROR 10 / WARN 405

# ── WSL 强门禁（SDK 对齐，零错误为准）─────────────────────
wsl -d Ubuntu-24.04
cd /data/share/wineohos/VintagePomeloPro
git fetch /mnt/f/旧柚pro/VintagePomeloPro && git reset --hard FETCH_HEAD
hvigorw --mode module -p module=entry@default -p product=default default@CompileArkTS

# ── git（只在 F: 仓库提交）────────────────────────────────
git -c user.name='yifengling0' -c user.email='yifengling0@users.noreply.github.com' \
  commit -am "feat(ui): <描述>"
GIT_TERMINAL_PROMPT=0 git push -u origin UI     # 凭据修复后
# 同步上游：
git fetch origin && git checkout main && git merge --ff-only origin/main \
  && git checkout UI && git rebase main

# ── 真机（仅 M5）─────────────────────────────────────────
# 用户 Linux 机: make NATIVE_ARCH=arm64-v8a hap；部署见 .claude/rules/build-and-log.md

# ── 禁止 ─────────────────────────────────────────────────
# ✗ 改 cpp/thirdparty/构建脚本  ✗ UI 并入 main  ✗ 任何数据落 C 盘
# ✗ 未过双门禁就提交            ✗ M5 之前碰真机
```

---

## 10. 关联资料指针

| 资料 | 位置 | 用途 |
|---|---|---|
| iiSU 逆向评估（参考样板） | `E:\iiSU\PORTING-ASSESSMENT.md` + `extract/` | 交互/信息架构参考（XMB 菜单、173 主机元数据模型）；**资源不得复用** |
| iiSU APK | `E:\iiSU\iiSU-Alpha-7.4.apk` | 只读参考 |
| PE 图标探测 | `E:\iiSU\pe-icon-probe*.js`、`diag.js` | 图标命中率验证 |
| 桌面应用移植范式 | `G:\知识库\鸿蒙移植经验库\27-*.md` | native 侧相关（UI-only 下基本用不上） |
| 手柄/黑屏/签名坑 | `G:\知识库\鸿蒙移植经验库\26-*.md` | M3 焦点态、M5 签名 |
| 项目 UI 文档 | `docs/bottom-navigation-layout.md` | 手机底栏规范与验证脚本 |
| 构建与日志 | `.claude/rules/build-and-log.md` | Linux 构建/hdc/hilog 全流程 |
| submodule 流程 | `.claude/rules/submodule-workflow.md` | UI-only 下用不到，勿碰 submodule |
| UI 分支规则（AI 会话自动加载） | `.claude/rules/ui-branch.md` | 本文档的入口指针（防"交接没读取到"） |
