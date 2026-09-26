# 上游同步交接（2026-09-26）· UI 分支并入 1.4.2

> 给下一个会话：本次把 `origin/main`（上游 **1.4.2**，`af619d6`）同步进了 `UI` 分支，策略是
> **图标/底层取上游、UI 层保留本地**。结果已推送：`origin/UI = d945e1d`（含合并提交 + 一个适配修复提交）。
> 工具新增：`E:\iiSU\vpp-check-sync.sh`（与 `vpp-check.sh` 相同，只把 SRC 指向隔离工作区 `E:\ui-sync`）。

---

## 1. 先说最重要的三件事

1. **上游自己也实现了一套主机风 UI**（`feat(ui): add optional console skin and game artwork`）：新增
   `ConsoleGameCard.ets`、`ConsoleRingItem.ets`、`GameArtworkService.ets`、`SkinColors.ets`、`ConsoleUiSound.ets`、
   `BuiltinArtwork.ets`、`GameArtworkRules.ets`、`ui_focus.wav`/`ui_select.wav`，并把 `Index.ets`/`SystemSettings.ets`
   大改（Index +1376 行）。按用户指示**保留我们本地 UI**，上游那套 UI 改动在我们的页面上被丢弃，
   但其新增文件（组件/服务）**保留在分支里作为未使用的代码**（编译通过，未来要不要用由用户定）。
   另：上游 `AppSettingsStore` 里已经出现 `sgdb_api_key_v1` 和与我们一致的 `MetadataScraper`/`ScrapeResult`
   （连中文注释都一样）——说明作者那边已经吸收了本分支的 M4 抓取链路。
2. **`AppSettingsStore` 两侧内部字段名不同**（我们 `recentLaunchesV2`/`playTime` vs 上游 `lastPlayedAt`/`playMinutes`），
   公开方法名两侧一致，已统一采用上游内部实现（`lastPlayedAt`/`playMinutes` + `mapToRecord` + `flush`）。
3. **`WineEngineService.ets` 的改动在上游那侧必须取上游**（`wineLang` 参数配合新 native）。F: 工作区里另一个会话
   对该文件的未提交改动与本合并**直接冲突**——见 §5。

## 2. 冲突处理明细（9 个文件 / 104 处）

| 文件 | 处数 | 处理 |
|---|---|---|
| `pages/Index.ets` | 22 | 保留本地（我们的环形封面流主页） |
| `pages/SystemSettings.ets` | 40 | 保留本地（沉浸顶栏 + 密度） |
| `components/GameDetailSheet.ets` | 14 | 保留本地（add/add） |
| `components/AppCard.ets` | 4 | 保留本地（上游对它的改动属"主机皮肤/封面自动抓取"功能，非通用修复） |
| `service/AppSettingsStore.ets` | 12 | 并集：内部实现取上游；`GlobalSettings` 保留我们的 `layoutMode` 并补上游 `uiSkin/consoleCardLayout/consolePalette`；删掉重复的 `getSgdbApiKey/setSgdbApiKey` |
| `service/MetadataScraper.ets` | 6 | 取上游（add/add；`ScrapeResult` 接口两侧一致，仅内部 `download` 签名不同） |
| `model/AppModels.ets` | 3 | 并集：`backgroundPath` 改可选（上游注释：旧缓存兼容）、`GlobalSettings` 四字段全留、默认值用我们的 `viewMode=COVER/layoutMode=STANDARD/themeMode=DARK` |
| `model/AppCatalogRules.ets` | 2 | 并集：背景候选名合并（补上游 `background.bmp`）；`find` 回调补显式 `: boolean` 返回类型 |
| `service/AppCatalogService.ets` | 1 | 保留我们的 `listNum: MAX_COVER_CANDIDATES` 上限（与同文件 `findCover` 一致） |

## 3. 合并后为通过编译做的 5 处修复（第二个提交）

git 的 auto-merge（非冲突区）留下了几处**重复定义**，加上上游新增的必填字段，逐条修掉：

1. `model/AppModels.ets`：`SortMode` 枚举被两侧各加一份 → 删一份（`arkts-no-enum-merging`）。
2. `service/AppCatalogService.ets`：`chooseBackgroundFileName` 在同一 import 里出现两次 → 删一份。
3. `cpp/types/libentry/Index.d.ts`：`getDesktopRootId` 被两侧各加一份（我们加在文件尾、上游加在中部）→ 删尾部那份。
4. `pages/Index.ets:677`：`wineTextInputPreedit(text, 0, text.length)` 改单参。
   **注意这条是我们分支原有的类型错误**：我们自己的 `Index.d.ts` 一直是 `(text: string) => boolean`，
   而 `Index.ets` 传了 3 个参数 —— 之前能编过，是因为**沙盒 `E:\vpp-build` 里有一份手改过的 3 参 `Index.d.ts`**
   （未提交、每次同步 cpp/types 时被覆盖/又补回）。本次同步覆盖掉它后错误才暴露。
   上游 native 实现是 `SendPreedit(text, 0, strlen(text))`，**自己在字节侧算范围**（注释明说不能把 JS UTF-16 下标当字节数），
   所以单参调用语义等价且更正确。铁律"不要改 native 的 `Index.d.ts`"由此得到验证。
5. `pages/SystemSettings.ets` ×3 + `pages/Index.ets` ×1：上游给 `GlobalSettings` 加了 `uiSkin/consoleCardLayout/consolePalette`、
   给 `AppSettings` 加了 `backgroundMode/manualBackgroundPath`，我们在 UI 层的全量拷贝字面量里补上转发。

## 4. 上游这次带进来的东西（用户要的"图标 + 底层"）

- **图标**：`entry/src/main/resources/base/media/{background,foreground,startIcon}.png` + `AppScope/resources/base/media/*`。
- **音效**：`entry/src/main/resources/rawfile/ui_focus.wav`、`ui_select.wav`。
- **底层**：`thirdparty/wine` 子模块指针（含 `0006-ohos-cjk-missing-font-gbk-fallback`、`0008-ohos-musl-c-utf8-zh-cn-locale` 两个补丁）、
  native（`cpp/bridge/napi_init.cpp`、`cpp/wine/wine_env.cpp`、`wine_exe.cpp/h`，新增 `wineLang`）、
  `hvigorfile.ts`（会在项目加载时跑 `scripts/verify_wine_runtime.cjs` 校验）、CI、`Makefile`、`host_tests/`、docs。
- 版本号：`AppScope/app.json5` → versionCode 1004002 / 1.4.2。

## 5. ⚠️ F: 主工作区**尚未**同步（需要用户决定）

本次同步在隔离工作区 `E:\ui-sync`（新分支 `ui-sync`）里完成，**完全没碰 F: 工作区**。原因是 F: 里另一个会话
有未提交改动，其中 `entry/src/main/ets/service/WineEngineService.ets` 与上游改动**同一文件**：

- 直接 `git merge` 会被 git 拒绝（"local changes would be overwritten"）。
- 强行处理就等于**覆盖另一个会话的 WIP**——违反"不要覆盖、先问"的约定，所以没做。

**F: 目前的落后情况**：F: 的 `UI` 仍是 `1fd5fe5`，`origin/UI` 已是 `d945e1d`。恢复/采用的两种路线：

- **路线 A（推荐，等其他会话收工）**：让那个会话把它的 cpp/scripts/`WineEngineService.ets` 改动提交掉（或用户确认不要了），
  然后 `cd F:/旧柚pro/VintagePomeloPro && git fetch origin && git merge --ff-only origin/UI`。
- **路线 B（立刻采用，需要用户点头）**：我先把那个会话的 `WineEngineService.ets` 原样备份到 `E:\iiSU\`（文件 + patch），
  再 `git checkout --` 该文件 → `git merge --ff-only origin/UI` → 把备份的 WIP 覆盖回去（作为新的未提交改动继续留给那个会话）。
  风险：他们的改法是基于旧版文件写的，覆盖回合并后的新文件后可能语义打架，需要那个会话自己收尾。

安全网：`UI` 分支上有标签 **`UI-pre-upstream-sync-20260926`** 指向同步前的 `1fd5fe5`，随时可回退。

## 6. 验证状态（重要）

- ✅ **ArkTS 编译校验通过**：`bash /e/iiSU/vpp-check-sync.sh` → `BUILD SUCCESSFUL`，0 error（只剩 `vp2px` deprecated 之类历史 WARN）。
- ❌ **没有真机验证**：设备当前**未连接**（`hdc list targets` = `[Empty]`），装机/截图链路走不通。
- ❌ **没有打 HAP（刻意不打）**：`E:\vpp-build` 沙盒里的 `libentry.so` 是 2026-09-24 另一个会话构建的，
  **不含上游 native 的 `wineLang`/`getDesktopRootId` 改动**。现在打包会得到"新 ets + 旧 native"的混合体，
  装到机器上有崩的风险，属于会误导人的产物。要真机验证必须先做一次**完整重建**（含上游 native + wine 子模块），
  而这条链路与另一个会话正在做的 `build.sh`/`scripts/` 改造直接相关，建议**协调后再做**。

## 7. 上手第一步

**提交身份自 2026-09-26 起改为 `rootrd`**：已写入 F: 仓库的 local config（`user.name=rootrd`、
`user.email=rootrd@users.noreply.github.com`），所以直接 `git commit` 即可，**不要再带 `-c user.name='yifengling0'`**
（本文件 §1–§6 与更早的交接文档里写的 yifengling0 身份已作废；`rootrd` 同时是 GitHub 上
`yifengling0/VintagePomeloPro` 的 Write 协作者账号，即真正执行 push 的账号）。

```bash
cd "F:/旧柚pro/VintagePomeloPro"
git status --short                      # 确认 §5 的未提交改动还在不在
git log --oneline -3 origin/UI          # 应为 28ff8bf
bash /e/iiSU/vpp-check-sync.sh /tmp/x.log   # 校验合并后代码（源=E:\ui-sync）
```

隔离工作区 `E:\ui-sync`（分支 `ui-sync`）目前保留着，就是合并后的干净树；不需要时可以
`git worktree remove E:/ui-sync` 清掉。
