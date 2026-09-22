# HANDOVER_UI_2026-09-22.md — VintagePomeloPro UI 布局重构交接文档

> 生成时间：2026-09-22
> 上一份交接：HANDOVER_UI_2026-09-20.md
> 当前分支：UI（本地分支，GitHub 远程无此分支）

---

## 0. 一句话总结

**状态：未完成。** 实现了 iiSU 风格的多布局模式 UI 重构（Standard/XMB/Classic 三模式可切换），修复了全部 10 个编译错误，构建并安装了签名 HAP 到真机（设备 ID: 4NZ0225605001361）共 3 次。最后一次修复了"STANDARD/XMB 模式在手机横屏下不生效"的 isPhone() 判断问题，但用户尚未在真机上确认新 UI 布局是否生效。所有代码改动尚未提交到 git。

---

## 1. 项目基本信息

| 项 | 值 |
|---|---|
| 项目名 | VintagePomeloPro（旧柚Pro） |
| 项目路径（源） | `F:\旧柚pro\VintagePomeloPro` |
| 项目路径（沙盒/构建） | `E:\vpp-build` |
| bundleName | `com.vintage.pomelopro` |
| 设备 ID | `4NZ0225605001361` |
| SDK 版本 | 26.0.0 Beta2（API 26） |
| DevEco Studio 路径 | `C:\Program Files\Huawei\DevEco Studio` |
| JDK 路径 | `C:\Program Files\Huawei\DevEco Studio\jbr` |
| hvigor 缓存路径 | `E:\vpp-hvigor`（避免 C 盘） |
| 签名证书路径 | `C:\Users\Administrator\.ohos\config\default_vpp-build_DT1GaVguJwTyBXZGyxJo8nM_SPvDkYkin2Z6xRn4hNk=.{cer,p12,p7b}` |

---

## 2. 本次会话完成的所有工作（按时间顺序）

### 阶段 A：修复 10 个编译错误

项目在 API 26 SDK 下有 10 个固有的编译错误（非 UI 改动引入）。修复了全部 .10 个：

| # | 错误 | 修复方式 | 文件 |
|---|---|---|---|
| 1-3 | `HdsTabsAttribute`/`HdsNavigationAttribute` 找不到（Index.ets 3 处） | 在 `@kit.UIDesignKit` import 中添加 `HdsTabsAttribute, HdsNavigationAttribute` | `entry/src/main/ets/pages/Index.ets` 第 52-64 行 |
| 4 | `HdsNavigationAttribute` 找不到（SystemSettings.ets） | 同上 | `entry/src/main/ets/pages/SystemSettings.ets` 第 58-64 行 |
| 5 | `HdsNavigationAttribute` 找不到（AppSettings.ets） | 同上 | `entry/src/main/ets/pages/AppSettings.ets` 第 35-41 行 |
| 6 | `HdsNavigationAttribute` 找不到（HelpCenter.ets） | 同上 | `entry/src/main/ets/pages/HelpCenter.ets` 第 13-18 行 |
| 7-8 | `getDesktopRootId` 类型声明缺失（2 处） | 在 Index.d.ts 末尾添加 `export const getDesktopRootId: () => number;` | `entry/src/main/cpp/types/libentry/Index.d.ts` 第 185 行后 |
| 9 | `wineTextInputPreedit` 参数错误（3 参数→1 参数） | `wineTextInputPreedit(text, 0, text.length)` → `wineTextInputPreedit(text)` | `entry/src/main/ets/pages/Index.ets` 第 577 行 |

**编译验证结果**：0 ERROR, 413 WARN（之前基线 10 ERROR / 405 WARN）

### 阶段 B：构建并安装 HAP（第一次）

1. **同步源码到沙盒**：使用 `E:\iiSU\vpp-check.sh` 脚本，将源码从 `F:\旧柚pro\VintagePomeloPro` 同步到 `E:\vpp-build`
2. **构建 HAP**：使用 `E:\vpp-build\build-hap.bat` 脚本运行 `hvigorw assembleApp`
3. **遇到的问题**：
   - `spawn java ENOENT` — Java 不在 PATH 中。解决：在 bat 文件中设置 `JAVA_HOME` 和 `PATH` 指向 DevEco Studio 自带 JDK（`C:\Program Files\Huawei\DevEco Studio\jbr`）
   - HAP 未签名 — `build-profile.json5` 中没有 `signingConfigs`。解决：用户通过 DevEco Studio 自动签名配置
   - 签名 HAP 文件名 — 正确的签名 HAP 是 `entry-default-signed.hap`（不是 `app/entry-default.hap`）
4. **安装成功**：`hdc install entry-default-signed.hap` → `install bundle successfully`

### 阶段 C：用户反馈 — UI 只有颜色变动

用户说"UI 为什么只有颜色变动，这压根不是是我想要的效果"。之前会话（2026-09-20）做的改动主要是渐变紫色背景、卡片样式微调、颜色资源调整，缺少布局结构的重大变化。

用户期望：**整体布局重做**，参考 **iiSU** 项目（`E:\iiSU\iiSU-Alpha-7.4.apk`）的 UI 设计。

### 阶段 D：分析 iiSU UI 设计

从 iiSU APK 中提取了 onboarding 截图并分析：

| 模式 | 描述 |
|---|---|
| Standard | 3×6 全屏网格，无侧边栏/导航栏，极简风格 |
| XMB | 纵向列布局，中间大两侧小（PlayStation XMB 风格） |
| WiiSU | 横向滚动轮播网格，带方向箭头 |
| DS | 上下分屏（主屏+控制屏） |

核心设计原则：全屏沉浸、无 chrome 元素、高对比度 2 色方案、圆角卡片、手柄优先导航。

### 阶段 E：用户确认需求

用户选择：
1. **多模式可切换** — 支持 Standard/XMB/Classic 三种布局模式，用户可以切换
2. **去掉侧边栏** — 分类用顶部标签页或手势切换

### 阶段 F：实现 UI 布局重构

#### F1. 添加 LayoutMode 枚举

**文件**：`entry/src/main/ets/model/AppModels.ets`

```typescript
export enum LayoutMode {
  CLASSIC = 'classic',    // 经典模式: 侧边栏 + 内容区
  STANDARD = 'standard',  // Standard 模式: 全屏网格, 顶部标签分类
  XMB = 'xmb'             // XMB 模式: 纵向列布局, 中间大两侧小
}
```

在 `GlobalSettings` 接口中添加 `layoutMode: LayoutMode` 字段。
在 `createDefaultGlobalSettings()` 中设置默认值 `layoutMode: LayoutMode.STANDARD`。

#### F2. 添加 import 和状态变量

**文件**：`entry/src/main/ets/pages/Index.ets`

- import 中添加 `LayoutMode`
- 状态变量添加 `@State private xmbFocusIndex: number = 0;` 和 `@State private topTabBarVisible: boolean = true;`

#### F3. 新增 @Builder 方法

在 `PageSurface` 之前添加了 4 个新的 @Builder：

1. **`TopTabBar()`** — 顶部标签页（最近/收藏/内建/Windows/运行中）+ 右侧布局切换按钮和设置按钮
2. **`LayoutModeSwitcher()`** — 布局模式切换按钮，点击循环切换 Standard → XMB → Classic
3. **`StandardLayout()`** — Standard 模式布局：TopTabBar + 全屏网格
4. **`XMBLayout()`** — XMB 模式布局：左侧分类图标列 + 右侧纵向列表（中间聚焦项放大）

#### F4. 修改 PageSurface 布局逻辑

**关键修改**：将 `PageSurface` 中的布局切换逻辑从"手机/非手机"改为"layoutMode 优先"：

```
// 修改前（问题代码）：
if (this.isPhone()) {
  this.PhoneLibraryNavigation()    // 手机横屏走这里，不经过 STANDARD/XMB 分支
} else if (this.settings.layoutMode === LayoutMode.CLASSIC) {
  this.SideBar() + this.Library()
} else if (STANDARD) { ... }
} else if (XMB) { ... }

// 修改后（修复代码）：
if (this.settings.layoutMode === LayoutMode.CLASSIC) {
  if (this.isPhone()) {
    this.PhoneLibraryNavigation()  // 只有 CLASSIC 模式才区分手机/平板
  } else {
    this.SideBar() + this.Library()
  }
} else if (STANDARD) { this.StandardLayout() }
} else if (XMB) { this.XMBLayout() }
```

这个修复确保 STANDARD 和 XMB 模式在所有设备上生效（包括横屏手机），不再只限于平板/PC。

#### F5. 修复编译错误

| 错误 | 修复 |
|---|---|
| `AppSource.WINDOWS` 不存在 | 改为 `AppSource.BUILTIN`（AppSource 只有 BUILTIN 和 DOWNLOAD） |
| `app.displayName` 不存在 | 改为 `app.title`（AppDescriptor 的属性是 title） |
| `app.iconPath` 不存在 | 改为 `app.coverPath`（AppDescriptor 的属性是 coverPath） |
| `$r('sys.symbol.app')` 不存在 | 改为 `$r('sys.symbol.house')` |
| `layoutMode` 在多个文件中缺失 | 在 AppSettingsStore.ets 和 SystemSettings.ets 的所有 GlobalSettings 构造处添加 `layoutMode` |

#### F6. 更新 AppSettingsStore.ets

- import 中添加 `LayoutMode`
- 所有构造 `GlobalSettings` 对象的地方添加 `layoutMode: current.layoutMode` 或 `layoutMode: stored.layoutMode ?? defaults.layoutMode`

#### F7. 更新 SystemSettings.ets

- import 中添加 `LayoutMode`
- 所有构造 `GlobalSettings` 对象的地方添加 `layoutMode: this.settings.layoutMode`

### 阶段 G：修改同步脚本

**文件**：`E:\iiSU\vpp-check.sh`

**修改内容**：从 tar 同步列表中移除 `build-profile.json5`（根目录的），避免覆盖沙盒中的签名配置。

```bash
# 修改前：
tar -cf - -C "$SRC" build-profile.json5 entry/build-profile.json5 ...

# 修改后：
tar -cf - -C "$SRC" entry/build-profile.json5 ...
```

**注意**：`entry/build-profile.json5` 仍然同步（它不含签名配置），只有根目录的 `build-profile.json5`（含 signingConfigs）不再被覆盖。

### 阶段 H：构建并安装 HAP（第二次）

1. 运行 `vpp-check.sh` 同步源码 + 编译验证 → 0 ERROR
2. 用户在 DevEco Studio 中重新配置签名（因为之前同步覆盖了签名配置）
3. 在 `build-profile.json5` 的 products 中添加 `"signingConfig": "default"` 引用
4. 运行 `build-hap.bat` 构建 → BUILD SUCCESSFUL
5. 安装 `entry-default-signed.hap` → install bundle successfully
6. 强制重启应用

### 阶段 I：用户反馈横屏没看到变化

用户说"横屏，底部有标签栏（也没改动），左侧本来就没东西"。

**根因**：横屏模式下 `isPhone()` 返回 true，代码走 `PhoneLibraryNavigation()` 分支（底部标签栏），不经过 STANDARD/XMB 分支。

**修复**：修改 PageSurface 的布局逻辑，让 layoutMode 优先于 isPhone() 判断（见 F4）。

### 阶段 J：构建并安装 HAP（第三次）

1. 修改 vpp-check.sh 不再覆盖 build-profile.json5
2. 用户在 DevEco Studio 中重新配置签名
3. 在 build-profile.json5 中添加 `signingConfig: "default"` 到 products
4. 构建 → BUILD SUCCESSFUL（包含 SignHap + SignApp）
5. 安装 → install bundle successfully
6. 强制重启应用

**当前状态**：HAP 已安装到真机，但用户尚未确认是否看到了新的 UI 布局。

---

## 3. 所有修改的文件清单

### 源项目文件（F:\旧柚pro\VintagePomeloPro）

| 文件 | 改动类型 | 改动内容 |
|---|---|---|
| `entry/src/main/ets/pages/Index.ets` | 修复 + 新增 | 1) 添加 LayoutMode import 2) 添加 xmbFocusIndex/topTabBarVisible 状态 3) 新增 TopTabBar/LayoutModeSwitcher/StandardLayout/XMBLayout @Builder 4) 修改 PageSurface 布局逻辑 5) 添加 HdsTabsAttribute/HdsNavigationAttribute import 6) 修复 wineTextInputPreedit 参数 |
| `entry/src/main/ets/pages/AppSettings.ets` | 修复 | 添加 HdsNavigationAttribute import |
| `entry/src/main/ets/pages/HelpCenter.ets` | 修复 | 添加 HdsNavigationAttribute import |
| `entry/src/main/ets/pages/SystemSettings.ets` | 修复 + 新增 | 1) 添加 HdsNavigationAttribute import 2) 添加 LayoutMode import 3) 所有 GlobalSettings 构造处添加 layoutMode |
| `entry/src/main/cpp/types/libentry/Index.d.ts` | 修复 | 添加 `export const getDesktopRootId: () => number;` |
| `entry/src/main/ets/model/AppModels.ets` | 新增 | 1) 添加 LayoutMode 枚举 2) GlobalSettings 添加 layoutMode 字段 3) createDefaultGlobalSettings 添加 layoutMode 默认值 |
| `entry/src/main/ets/service/AppSettingsStore.ets` | 修复 + 新增 | 1) 添加 LayoutMode import 2) 所有 GlobalSettings 构造处添加 layoutMode |

### 工具/脚本文件

| 文件 | 改动类型 | 改动内容 |
|---|---|---|
| `E:\iiSU\vpp-check.sh` | 修改 | 从同步列表中移除根目录 build-profile.json5，保留签名配置 |
| `E:\vpp-build\build-hap.bat` | 新建 | 构建 HAP 的批处理脚本，设置 JAVA_HOME/PATH 等环境变量 |
| `E:\vpp-build\build-hap.ps1` | 新建 | PowerShell 版构建脚本（备用） |
| `E:\vpp-build\build-hap-signed.bat` | 新建 | 同步+恢复签名+构建一体化脚本（备用） |

---

## 4. 构建流程（标准操作步骤）

### 4.1 同步源码 + 编译验证

```bash
bash /e/iiSU/vpp-check.sh
```

此脚本会：
1. 将源码从 `F:\旧柚pro%pro\VintagePomeloPro` 同步到 `E:\vpp-build`（不覆盖 build-profile.json5）
2. 运行 `hvigorw CompileArkTS` 编译验证
3. 输出错误摘要和编译结果

### 4.2 构建 HAP

```bash
"E:/vpp-build/build-hap.bat"
```

此脚本会：
1. 设置 JAVA_HOME、PATH、DEVECO_SDK_HOME 等环境变量
2. 运行 `hvigorw assembleApp` 构建 HAP（含签名）

**前提条件**：
- `E:\vpp-build\build-profile.json5` 中必须有 `signingConfigs` 配置（通过 DevEco Studio 自动签名）
- `build-profile.json5` 的 products 中必须有 `"signingConfig": "default"` 引用

### 4.3 安装 HAP 到真机

```bash
# 检查设备连接
"C:/Program Files/Huawei/DevEco Studio/sdk/default/openharmony/toolchains/hdc.exe" list targets

# 安装（必须从 HAP 所在目录执行，或用 -t 指定设备）
cd /e/vpp-build/entry/build/default/outputs/default
"C:/Program Files/Huawei/DevEco Studio/sdk/default/openharmony/toolchains/hdc.exe" -t 4NZ0225605001361 install "entry-default-signed.hap"

# 强制重启应用
"C:/Program Files/Huawei/DevEco Studio/sdk/default/openharmony/toolchains/hdc.exe" -t 4NZ0225)225605001361 shell aa force-stop com.vintage.pomelopro
"C:/Program Files/Huawei/DevEco Studio/sdk/default/openharmony/toolchains/hdc.exe" -t 4NZ0225605001361 shell aa start -a EntryAbility -b com.vintage.pomelopro
```

### 4.4 签名配置丢失时的恢复

如果 `build-profile.json5` 中的签名配置丢失：
1. 在 DevEco Studio 中打开 `E:\vpp-build` 项目
2. 等待自动签名完成（DevEco Studio 会自动写入 signingConfigs）
3. 手动在 products 中添加 `"signingConfig": "default"` 引用
4. 然后运行 `build-hap.bat` 构建

---

## 5. 关键技术细节和踩坑记录

### 5.1 hvigor 构建

- **任务名**：`assembleApp`（不是 `AssembleApp`）
- **模式**：`--mode project -p product=default`（不加 `--mode module`）
- **Java**：必须设置 `JAVA_HOME=C:\Program Files\Huawei\DevEco Studio\jbr`，否则报 `spawn java ENOENT`
- **签名 HAP**：正确的签名 HAP 文件是 `entry-default-signed.hap`，不是 `app/entry-default.hap`（后者未签名）

### 5.2 签名配置

- 签名证书在 `C:\Users\Administrator\.ohos\config\default_vpp-build_XXX.{cer,p12,p7b}`
- `build-profile.json5` 中需要两部分：`signingConfigs`（证书配置）+ products 中的 `signingConfig: "default"` 引用
- `vpp-check.sh` 已修改为不覆盖 `build-profile.json5`，签名配置在同步后得以保留

### 5.3 isPhone() 判断逻辑

```typescript
private isPhone(): boolean {
  return !isWideBreakpoint(this.breakpoint);
}
```

- `isWideBreakpoint` 在 breakpoint 为 `lg`/`xl` 时返回 true
- 横屏手机（如 Pura 80 Ultra）在横屏时 breakpoint$breakpoint 可能仍是 `sm`/`md`，因此 `isPhone()` 返回 true
- **关键修复**：STANDARD/XMB 模式必须在 `isPhone()` 判断之前生效，否则横屏手机永远走 PhoneLibraryNavigation 分支

### 5.4 AppDescriptor 属性名

| 错误名 | 正确名 | 说明 |
|---|---|---|
| `displayName` | `title` | AppDescriptor 的应用名称属性 |
| `iconPath` | `coverPath` | AppDescriptor 的封面图路径属性 |
| `AppSource.WINDOWS` | `AppSource.BUILTIN` / `AppSource.DOWNLOAD` | AppSource 枚举只有这两个值 |

### 5.5 HDS 组件 import

在 API 26 SDK 下，`HdsTabsAttribute` 和 `HdsNavigationAttribute` 需要显式 import：

```typescript
import {
  HdsNavigation,
  HdsNavigationAttribute,  // 必须显式 import
  HdsTabs,
  HdsTabsAttribute,        // 必须显式 import
} from '@kit.UIDesignKit';
```

### 5.6 iiSU UI 设计参考

- iiSU APK 位置：`E:\iiSU\iiSU-Alpha-7.4.apk`
- iiSU 评估文档：`E:\iiSU\PORTING-ASSESSMENT.md`
- iiSU 有 4 种布局模式：Standard（全屏网格）、XMB（纵向列）、WiiSU（横向轮播）、DS（分屏）
- 核心设计原则：全屏沉浸、无 chrome、高对比度 2 色方案、圆角卡片

---

## 6. 当前 UI 布局模式实现细节

### 6.1 CLASSIC 模式（保留原有布局）

- 非手机：左侧边栏（SideBar）+ 右侧内容区（Library）
- 手机：HdsNavigation + HdsTabs 底部标签栏（PhoneLibraryNavigation）

### 6.2 STANDARD 模式（新实现，默认模式）

- 顶部标签栏（TopTabBar）：最近/收藏/内建/Windows/运行中 + 右侧布局切换按钮和设置按钮
- 全屏网格（AppGrid）：使用原有的 Grid + AppCard 组件
- 无侧边栏，内容区域全宽

### 6.3 XMB 模式（新实现）

- 左侧分类图标列（64vp 宽）：5 个分类图标，选中项有背景色
- 右侧纵向列表（List）：中间聚焦项放大显示（80vp 图标 + 18px 标题），非聚焦项缩小（48vp 图标 + 14px 标题）
- 聚焦项有圆角背景 + 边框 + 详细信息（来源/运行状态）

### 6.4 布局模式切换

- LayoutModeSwitcher 按钮（右上角）：点击循环切换 Standard → XMB → Classic
- 切换后自动保存到 AppSettingsStore（preferences 持久化）

---

## 7. 待完成事项

| 优先级 | 事项 | 说明 |
|---|---|---|
| P0 | **确认 STANDARD 模式在横屏手机上生效** | 最后一次安装修复了 isPhone() 判断问题，但用户尚未确认效果。接手后第一步：安装最新 HAP 并验证 |
| P0 | **根据用户反馈调整 STANDARD 模式视觉风格** | 当前 STANDARD 模式只是把侧边栏换成了顶部标签页，网格和卡片样式没变。需要更明显的视觉变化（如 iiSU 的全屏沉浸式风格、高对比度 2 色方案、无 chrome 元素） |
| P1 | 完善 XMB 模式交互 | 当前 XMB 模式是基础实现，可能需要增加动画效果、手柄导航支持等 |
| P1 | 添加 WiiSU 模式（横向轮播） | iiSU 的第三种布局模式，尚未实现 |
| P2 | 提交 UI 改动到 UI 分支 | 所有改动尚未提交到 git |
| P2 | iiSU 风格的视觉细节优化 | 高对比度 2 色方案、圆角卡片、无 chrome 元素等 |

---

## 8. 快速验证步骤（接手后第一步）

```bash
# 1. 确认源码状态
grep -c 'LayoutMode' "F:/旧柚pro/VintagePomeloPro/entry/src/main/ets/pages/Index.ets"
# 期望输出：>10

# 2. 同步源码 + 编译验证
bash /e/iiSU/vpp-check.sh
# 期望：EXIT: 0, 0 ERROR

# 3. 检查签名配置
grep 'signingConfigs' /e/vpp-build/build-profile.json5
# 期望：有输出（signingConfigs 存在）

# 4. 检查 signingConfig 引用
grep 'signingConfig' /e/vpp-build/build-profile.json5
# 期望：有 "signingConfig": "default" 在 products 中

# 5. 构建 HAP
"E:/vpp-build/build-hap.bat" 2>&1 | grep -E 'BUILD|ERROR|SignApp'
# 期望：BUILD SUCCESSFUL

# 6. 安装到真机
cd /e/vpp-build/entry/build/default/outputs/default
"C:/Program Files/Huawei/DevEco Studio/sdk/default/openharmony/toolchains/hdc.exe" -t 4NZ0225605001361 install "entry-default-signed.hap"
# 期望：install bundle successfully

# 7. 强制重启应用
"C:/Program Files/Huawei/DevEco Studio/sdk/default/openharmony/toolchains/hdc.exe" -t 4NZ0225605001361 shell aa force-stop com.vintage.pomelopro
"C:/Program Files/Huawei/DevEco Studio/sdk/default/openharmony/toolchains/hdc.exe" -t 4NZ0225605001361 shell aa start -a EntryAbility -b com.vintage.pomelopro
```

---

## 9. 关键文件路径速查

| 用途 | 路径 |
|---|---|
| 源项目 | `F:\旧柚pro\VintagePomeloPro` |
| 构建沙盒 | `E:\vpp-build` |
| 同步+编译脚本 | `E:\iiSU\vpp-check.sh` |
| 构建 HAP 脚本 | `E:\vpp-build\build-hap.bat` |
| 签名配置 | `E:\vpp-build\build-profile.json5` |
| iiSU APK | `E:\iiSU\iiSU-Alpha-7.4.apk` |
| iiSU 评估文档 | `E:\iiSU\PORTING-ASSESSMENT.md` |
| 签名证书 | `C:\Users\Administrator\.ohos\config\default_vpp-build_*` |
| DevEco Studio | `C:\Program Files\Huawei\DevEco Studio` |
| JDK | `C:\Program Files\Huawei\DevEco Studio\jbr` |
| SDK | `C:\Program Files\Huawei\DevEco Studio\sdk\default` |
| hvigor 缓存 | `E:\vpp-hvigor` |
| hdc 工具 | `C:\Program Files\Huawei\DevEco Studio\sdk\default\openharmony\toolchains\hdc.exe` |

---

## 10. LayoutMode 枚举值与对应布局

```
LayoutMode.STANDARD (默认)
  → StandardLayout() → TopTabBar + 全屏网格
  → 顶部标签页分类，无侧边栏，右上角布局切换+设置按钮

LayoutMode.XMB
  → XMBLayout() → 左侧分类图标列 + 右侧纵向列表
  → 中间聚焦项放大，类似 PlayStation XMB 界面

LayoutMode.CLASSIC
  → 非手机: SideBar + Library（原有侧边栏布局）
  → 手机: PhoneLibraryNavigation（原有底部标签栏布局）
```

切换方式：点击右上角 LayoutModeSwitcher 按钮，循环切换 Standard → XMB → Classic。