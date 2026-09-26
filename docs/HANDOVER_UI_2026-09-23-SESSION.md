# 旧柚Pro UI 改造 · 新会话交接（2026-09-23 晚）

> **给下一个会话的第一句话**：这是「把 VintagePomeloPro（旧柚Pro）启动器 UI 改造成游戏主机首页观感」的任务。
> 本文是**当前唯一入口**，读完即可开工；历史细节见 `docs/HANDOVER_UI_2026-09-22-FULL.md`（§1–§15）与蓝图 `docs/UI-REORG-PLAN-2026-09-22.md`。
> 工程目录：`F:\旧柚pro\VintagePomeloPro`（注意是 F 盘、含中文路径）。**回复一律用中文。**

---

## 0. 当前状态一句话

设置页完成沉浸化（顶部宽黑边与底部黑边都已消除）、横屏信息密度收紧、主页底栏配色调成与搜索框同款灰；**UI 分支已首次推送到上游** `yifengling0/VintagePomeloPro`，远端 HEAD `4462993`。

| 项 | 值 |
|---|---|
| 本地分支 | `UI`（跟踪 `origin/UI`） |
| 远端 | `https://github.com/yifengling0/VintagePomeloPro.git` → 分支 `UI`（**已推送，可继续 push**） |
| 本地 HEAD | `4462993`（docs §15） |
| 远端 main | `81f9ac0`（**从未动过**，也不许动） |
| 提交身份 | `git -c user.name='yifengling0' -c user.email='yifengling0@users.noreply.github.com'` |

---

## 1. 铁律（违反即事故，不要协商）

1. **只做横板 UI**：只做横屏（自动左/右横屏），永不进竖屏。
2. **只改 UI 代码**：只动 `entry/src/main/ets/`（ArkTS 层）。**不碰** `cpp/`、`thirdparty/`、`build.sh`、`scripts/`、构建配置。
3. **只在 `UI` 分支提交**，永不并入/推送 main（上游作者也明确说过"推子分支，不要 main"）。
4. **数据不落 C 盘**（系统盘紧张）：沙盒 `E:\vpp-build`、hvigor 缓存 `E:\vpp-hvigor`、脚本与截图在 `E:\iiSU`。
5. **只做单行**：封面流永远单行/环形。
6. **顶栏/底栏结构冻结**（用户"很完美/我很喜欢"）：
   - 顶栏 = 时间(12fp) + 分段电池图标 ｜ 中间 300vp 搜索框 ｜ 右侧 排序 + 设置按钮(40vp/17fp)
   - 底栏 = 五元玻璃悬浮条：最近 · 收藏 · **Game**（手柄图标，底层 value 仍是 `windows`）· 运行中 · **Home**（value 仍是 `builtin`）
   - 冻结的是**结构与内容**；2026-09-23 用户主动要求改过**底栏配色**（见 §3），配色不属于冻结范围。
7. 用户会**并行开多个 AI 会话改同一仓库**：动手前先 `git status`，出现来源不明的改动**不要提交、不要覆盖**，先问。

---

## 2. 环境与验证链路（可直接复制）

### 编译 → 打包 → 装机 → 截图（全在本机 Windows 完成）

```bash
# 1) 编译校验（同步源码到 ASCII 沙盒, hvigor 拒绝中文路径）
bash /e/iiSU/vpp-check.sh /tmp/vpp.log          # 期望: "=== EXIT: 0 ===" 且错误摘要为空

# 2) 签名打包（内含 JAVA_HOME=DevEco jbr）
cd E:/vpp-build && cmd.exe //c "build-hap.bat"   # 期望: BUILD SUCCESSFUL
# 产物: E:\vpp-build\entry\build\default\outputs\default\entry-default-signed.hap

# 3) 装机（注意用 Windows 反斜杠路径, git bash 会吃掉正斜杠）
export MSYS_NO_PATHCONV=1
HDC="C:/Program Files/Huawei/DevEco Studio/sdk/default/openharmony/toolchains/hdc.exe"
"$HDC" install -r "E:\vpp-build\...\entry-default-signed.hap"

# 4) 冷启动 + 截图 + 取回
"$HDC" shell aa force-stop com.vintage.pomelopro
"$HDC" shell aa start -a EntryAbility -b com.vintage.pomelopro
"$HDC" shell snapshot_display -f /data/local/tmp/s.jpeg
"$HDC" file recv /data/local/tmp/s.jpeg "E:\iiSU\shots\xx.jpeg"
```

设备：`4NZ0225605001361`（屏 2848×1276，密度 3.5 → 逻辑 813×365vp）。
保活：`bash E:\iiSU\keep-awake.sh`（每 4 分钟重申不息屏，nohup）。

### 两个关键验证技巧

- **`uitest dumpLayout` 反查控件树**（判断"现在跑的到底是不是新构建"最可靠）：
  ```bash
  "$HDC" shell uitest dumpLayout -p /data/local/tmp/layout.json
  "$HDC" file recv /data/local/tmp/layout.json "E:\iiSU\shots\layout.json"
  # 然后在 json 里按 .id('xxx') 设置的 id 搜索, 能看到 bounds(px)
  ```
- **`uinput` 只能验证点击/长按，验证不了滚动**（注入的滑动手势唤不起本应用的 List 滚动；截图 `cmp` 还会被时间像素骗）。滚动类改动必须让用户用手指验。

---

## 3. 本会话完成的 5 个提交（都已推上游）

| 提交 | 内容 |
|---|---|
| `8fdc197` | **设置页顶栏自绘**：`.hideTitleBar(true)` 隐藏 HDS 标题栏 + 新增 `SettingsTopBar` 自绘紧凑顶栏（返回 32vp 圆钮 + 标题 15fp + 刷新），消除顶部宽黑边 |
| `958fe62` | 底栏底色改用与设置页框同款的半透明玻璃灰（第一版，**用户仍嫌黑**） |
| `3fccb24` | 底栏底色改为 **`app.color.search_background`** 并**去掉背景模糊**，与顶栏搜索框完全同色（**用户认可版**） |
| `ccd26a2` | 设置页横屏密度收紧：`compactDensity()` 一处开关控制卡片内边距 22→14、间距 16/14→10、按钮 48→40、开关行 13→10 |
| `4462993` | 交接文档 §15 |

### 量化证据（`uitest dumpLayout`，屏 2848×1276）

- `settings-top-bar` = `[0,0][2848,140]` → 0–40vp 满宽实栏
- `settings-back-button` = `[49,14][161,126]` → 32vp 圆钮，距左 14vp
- `system-settings-list` = `[840,161][2799,1248]` → 内容起点 46vp（原 56vp+），底边距 8vp（无黑边）
- 返回键 `uinput` 点击实测可回库页；`WMSRotation ... policy has taken effect` 确认横屏方向策略在设置页生效

---

## 4. ⚠️ 重要：一条旧结论已被推翻（不要被历史文档误导）

**旧结论**（`HANDOVER_UI_2026-09-22-FULL.md` 与旧记忆里）："HDS 的 `hideBackButton` + 自绘按钮会被 HDS 内部图层遮挡，按钮不可见，只能把 `HdsNavigation` 换原生 `Navigation`。"

**新事实（2026-09-23 实测）**：遮挡的原因是**标题栏还在**。只要调用
```typescript
.hideTitleBar(true)   // HdsNavigationAttribute.hideTitleBar(hide, animated?)
```
（声明见 `@kit.UIDesignKit` 的 `@hms.hds.hdsBaseComponent.d.ets` 2249 行；同处还有 `hideToolBar`、`hideBackButton`）
整条标题栏隐藏后，**自绘控件放进内容区正常显示**，无需换 `Navigation`。设置页已按此方案改造完成。

**但要注意**：隐藏标题栏会**连带隐藏 HDS 自带的返回键**，必须自绘一个返回键（`router.back()`），否则用户出不去设置页。

---

## 5. UI 层硬限制与已踩坑（累积，别再浪费轮次）

- **ArkTS 只用 V1 状态管理**：`@Component/@State/@Prop/@Link/@StorageProp/@Watch/@Builder`；全工程零 V2 装饰器。
- **`@Builder` 传值参数变化不触发刷新** → 三个空分类共用"空"分支时文案不更新（表现为"点分类没反应"）。修法：`ForEach` 以参数作 key 强制重建。
- **`Row` 里的 `layoutWeight` 只管宽度**，高度必须显式给（否则被内容撑开、溢出被底栏盖住）。
- **全屏容器的空区域会吞掉下层兄弟的点击** → 需要 `hitTestBehavior(HitTestMode.Transparent)`（详情页关闭按钮、底栏悬浮条外层都用这招）。
- **`ForEach` 不重跑 itemGenerator**（key 不变时）→ 3D 变换必须由子组件内部按 `@Prop ringOffset` 自算。
- **`List` 的 scroller 必须在构造里绑定**（`List({ space: 0, scroller: x })`），否则 `currentOffset()` 崩。
- **偏移量守卫要用 `isFinite()`**（`NaN` 会通过 `typeof === 'number'` 检查，进而 `scrollTo({xOffset: NaN})` 触发原生崩溃）。
- **`@Builder` 的 `space` 参数**要放在方法里（如 `Column({ space: this.sectionSpace() })`），依赖 `@State` 才能随窗口变化刷新。
- 裸文件路径渲染必须补 `file://`；`rotate` 要完整参数；不支持 `Record<string,number>`（用 `Map`）、不支持对象 spread。
- **不要改 native 的 `Index.d.ts`**（`wineTextInputPreedit` 是 native 再生成的，改了会破坏真实 Linux 构建）。

### 配色经验（本次教训）

**"换成某处的颜色"要连材质一起对齐**。底栏第一版只换了色值 `#3A202833`（设置页框用的半透明玻璃），但该色**不透明度只有 23%**，背后又是暗色页面，等价于没变，用户仍说"还是黑色"。终版改用 `app.color.search_background`（`#CC1F1F26`，与搜索框、设置页灰行同资源）**并去掉 `backgroundBlurStyle`** —— 搜索框是实心色块，底栏若保留 `COMPONENT_THICK` 模糊会把背后暗色重新叠进来，灰度对不上。

---

## 6. 待办清单

**用户明确提过、尚未做的：**
1. 设置页密度还能再提：右侧注释性长文案（如「程序统一在 Wine 桌面中运行…」两行）是最占地方的部分，砍掉可再省两行 —— **需用户点名删哪些**。
2. 其他页面按设置页同一理念继续改造（用户："我想接着改造其他界面"）。
3. 设置页分类栏滚动 + 跟随高亮，仍需用户**手指验证**（uinput 验不了滚动）。
4. 自动左/右横屏，需用户**实际旋转设备**验证。

**更早的待办（未消失）：**
5. **M4 元数据抓取**：SteamGridDB 链路代码已就位（`service/MetadataScraper.ets` + 设置页 API Key 输入），但用户**暂时无法注册 SteamGridDB**（"稍后开工"），目前只验证到"按钮出现 + 未配置提示"。
6. Hero 精选区、合集/标签分组、卡面信息层（时长上卡面）等深化项。

**与上游作者相关：**
7. 上游作者要求"推子分支、不要 main"。本次只推了 `UI` 分支，**没有开 PR**。若后续要开 PR，目标分支必须先跟作者确认。
8. 远端 `UI` 分支的**早期历史提交**里可能含 cpp 改动（早于本次推送就存在的），作者若在意可让他只看本次 5 个提交。

---

## 7. ⚠️ 工作区污染（下次开工第一件事）

`git status` 当前显示下列**外部未提交改动**（不是 UI 层、不是本会话所改，疑似另一个并行会话正在做构建/cpp 工作）：

```
 M build.sh
 M entry/src/main/cpp/graphics/graphics_broker.cpp
 M entry/src/main/ets/service/WineEngineService.ets
 M scripts/assemble.sh
 M scripts/build_freetype.sh
 M scripts/build_xkbcommon.sh
 M scripts/env.sh
 M thirdparty/gstreamer   (子模块指针)
 M thirdparty/wine        (子模块指针)
```

**本会话全程没有碰过、没有提交、没有推送这些文件**（每个提交都只 `git add` 单个 UI/docs 文件，已逐个核对：5 个提交各只含 1 个文件）。
下次开工先确认这些改动是否已被另一个会话处理掉：**不要 `git add -A`，不要盲提交**。

---

## 8. 上手第一步（照抄即可）

```bash
cd "F:/旧柚pro/VintagePomeloPro"
git status --short            # 先看 §7 的污染是否还在
git log --oneline -6          # 确认 HEAD = 4462993 或更新
bash /e/iiSU/vpp-check.sh /tmp/start.log   # 确认能编过（0 error）
```

然后按用户当次指令做；每次改完走 §2 的链路装机 + 截图自证，**只提交点名范围内的文件**，提交后 `git push`（origin/UI 已配好跟踪）。

### 关键文件

| 文件 | 作用 |
|---|---|
| `entry/src/main/ets/pages/Index.ets` | 主页面：顶栏、底栏、环形封面流、详情页、筛选 |
| `entry/src/main/ets/pages/SystemSettings.ets` | 设置页（本次改造重点）：自绘顶栏、双栏、密度开关 |
| `entry/src/main/ets/components/AppCard.ets` | 封面卡（3D 变换、焦点态） |
| `entry/src/main/ets/components/GameDetailSheet.ets` | 详情页 |
| `entry/src/main/ets/service/MetadataScraper.ets` | SteamGridDB 抓取（M4，待 key） |
| `entry/src/main/ets/service/AppSettingsStore.ets` | 设置持久化（新增字段要同步 ~8 处全量拷贝；独立配置走单独 key） |
| `E:\iiSU\vpp-check.sh` / `E:\iiSU\keep-awake.sh` / `E:\iiSU\wait-and-push.sh` | 编译校验 / 设备保活 / 等权限自动推送 |
| `E:\iiSU\shots\` | 真机截图（`settings-dense.jpeg`、`nav-gray-solid.jpeg` 为本次成果） |
