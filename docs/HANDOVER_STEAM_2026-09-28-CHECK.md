# Steam 接入 · 会话接手与核查文档（2026-09-28）

> **给新会话的第一句话**：这是「旧柚Pro 接入 Steam 账号（登录/令牌/游戏库）」任务的接手文档。
> 上一会话在 W2 尾段出现**工具输出幻觉污染**，`41883b5` 提交内容可信度存疑。
> 你的前 30 分钟任务是**核查与修复**，不是新功能。核查通过后再按 §4 继续 W3/W4。
> 工作列表总纲 = `docs/PLAN_STEAM_ACCOUNT_2026-09-27.md`（origin/UI 上，先读它）。
> **回复一律用中文。提交身份 rootrd（F: 仓库 local config 已配好，直接 commit）。**

---

## 0. 当前状态一句话

W1 底座（`b541772`）可信；W2（登录页+顶栏入口，`41883b5`）**已推但需核查**——上一会话后半段把
幻觉输出（编造的编译错误/不存在的文件行号/伪造的 dumpLayout）当成了真实证据，`git add -A` 提交的
内容可能混入污染编辑。**先核查（§2），再决定保留或回退（§3），然后继续（§4）。**

| 项 | 值 |
|---|---|
| 工作区 | `E:\ui-sync`（git worktree，分支 `ui-sync`，跟踪 origin/UI） |
| F: 主工作区 | `F:\旧柚pro\VintagePomeloPro`（仍在 `1fd5fe5`，未采用合并，**别动它**） |
| 沙盒 | `E:\vpp-build`（`bash /e/iiSU/vpp-check-sync.sh <log>` 编译校验；`cd E:/vpp-build && cmd.exe //c build-hap.bat` 打包） |
| 设备 | `4NZ0225605001361`（2848×1276 横屏；用户已确认设备可访问 Steam） |
| 参考克隆 | `E:\tmp\ASFWorkshop`、`E:\tmp\OpenHarmonyASF`（steam_core Apache-2.0） |
| 已推提交 | `b541772` W1 底座（可信）→ `41883b5` W2（**待核查**） |

## 1. 上一会话做了什么（意图，不保证落盘正确）

- **W1（可信）**：`entry/src/main/ets/steam/vendor/`（steam_core 22 文件，Apache-2.0，带 LICENSE/NOTICE）
  + 4 个适配器：`SteamHttpTransport.ets`（表单 POST）、`SteamCryptoAdapter.ets`（CryptoArchitectureKit
  实现 hmacSha1/rsaEncrypt）、`SteamClock.ets`、`SteamAccountStore.ets`（AssetStore 存凭据 + preferences
  存非密档案）+ `SteamService.ets`（登录状态机门面：signIn/submitCode/pollOnce/restoreSession/signOut/
  currentGuardCode/probeNetwork/fetchProfileInfo）。
- **W2（待核查）**：`pages/SteamAccount.ets`（登录/验证码/令牌/自检三段式页面，已注册 main_pages.json）
  + `Index.ets` 顶栏右上角 S 钮（`steam-top-btn`）+ 弹窗浮层（全屏遮罩 `steam-pop-mask` + 右上角锚定
  `SteamPopPanel`，**弃用 bindPopup**——这个 SDK 里 `PopupState` 类型不存在）+ `initialize()` 里
  SteamService 初始化/会话恢复 + `aboutToDisappear` 清理计时器。
- **已修过的类型坑（如需重做 W2 可直接复用结论）**：
  1. `cryptoFramework.DataBlob` 是 **interface**（不能 `new`）→ 用类型化字面量 `const blob: cryptoFramework.DataBlob = { data: bytes };`
  2. `AsyKeyGenerator.convertKey` 要**两个参数** `(pubKey: DataBlob|null, priKey: DataBlob|null)`，返回 `KeyPair`；
     `cipher.init` 要 `Key` → 取 `pubKey.pubKey`
  3. `util.TextEncoder.encodeInto(s)` **直接返回 Uint8Array**（没有 `.bytes` 成员）
  4. `asset.query()` 返回 `Array<AssetMap>` → 取 `[0]`；`asset.Value = boolean|number|Uint8Array`
     **不含 string** → `Tag.ALIAS` 的值必须 `utf8Bytes()` 包字节
  5. `import preferences from '@ohos.data.preferences'`（**默认导出**，不是命名导入）
  6. vendor 的 `contracts/SteamPlatformContracts.ets` **不 re-export** `SteamAuthChallengeKind` →
     要从 `vendor/models/SteamModels` 导入
  7. **bindPopup 不可用**（PopupState 不存在）→ 弹窗用"全屏透明遮罩 + Stack({alignContent: TopEnd})
     全屏容器 padding(top:56,right:12) + hitTestBehavior(Transparent)"的浮层组合
  8. 在 @Builder 方法前插入普通方法会**吃掉装饰器**（上一会话犯了两次：SortButton、SwitchRow）→
     插入新方法时避开 `@Builder` 行

## 2. 核查清单（30 分钟，按顺序执行）

```bash
cd "E:/ui-sync"
git status --short                     # 应干净（若无未提交改动）
git log --oneline -5                   # 41883b5 → b541772 → 8511c97 → 904c38a → 70ea034
git show 41883b5 --stat                # ★ 看提交范围: 只应含 5 个文件:
                                       #   Index.ets / SteamAccount.ets(新) / SteamService.ets /
                                       #   SteamAccountStore.ets / SteamCryptoAdapter.ets / main_pages.json
                                       #   若出现 steam/vendor/**、docs/**、其他 pages/** → 有污染, 记下清单
git show 41883b5 -- entry/src/main/ets/steam/SteamService.ets | head -80   # 抽查 diff 是否自洽
```

1. **编译核查**：`bash /e/iiSU/vpp-check-sync.sh /tmp/check1.log` → 期望 EXIT 0 且
   `grep -cE "Error Message" /tmp/check1.log` 为 0。**以你亲眼看到的日志为准，上一会话报的任何
   "BUILD SUCCESSFUL" 都不要信。** 若有错误，把真实错误逐条修掉（§1 的 8 条类型结论可以直接用）。
2. **Index.ets 完整性抽查**（污染重灾区）：
   - `grep -n "steam" entry/src/main/ets/pages/Index.ets` 应看到：import SteamService、
     @State steamPop/steamLoggedIn/steamAvatar/steamPopName/steamPopCode/steamPopSeconds/steamPopTimer、
     refreshSteamEntry/startSteamPopTimer/stopSteamPopTimer/toggleSteamPop、
     @Builder SteamEntryButton + SteamPopPanel、TopTabBar 里 `this.SteamEntryButton()`、
     build() 根 Stack 里的 steam-pop-mask + SteamPopPanel、aboutToDisappear 里 stopSteamPopTimer。
   - 特别检查 `SteamEntryButton`/`SteamPopPanel` 两个 @Builder 的**装饰器都在**、括号配对
     （`@Builder` 被"吃掉"是上一会话两次犯过的错）。
   - 检查 `private SortButton()` 头上是否还有 `@Builder`。
3. **SteamAccount.ets 通读**：三段式逻辑 + 所有 `.id('steam-*')`；若发现结构混乱/半截代码，直接
   按 §1 意图重写（该文件是独立新页面，重写无风险）。
4. **装机冒烟**：`cd E:/vpp-build && cmd.exe //c build-hap.bat` → `hdc install -r`（注意
   `export MSYS_NO_PATHCONV=1` + 反斜杠路径）→ 冷启动 → 截图：
   - 主页顶栏 S 钮存在（`uitest dumpLayout` 查 `steam-top-btn` 的 bounds——**自己跑 dumpLayout，
     上会话给的坐标不要信**）
   - 点 S → 弹窗浮层出现（右上角，含"登录 Steam"按钮）→ 点遮罩空白处能关闭
   - 点"登录 Steam"→ 跳 SteamAccount 页 → 页内显示"检测 Steam 连接中…"→ 变绿点"Steam 连接正常"
5. 核查结论写进本文档 §5（追加一行"核查结果"），提交推送。

## 3. 修复决策

- **编译 0 error 且冒烟通过** → 保留 `41883b5`，在 §5 记录"核查通过"，继续 §4。
- **有错误但可修**（预计就是这种情况，量级 ≤10 个编译错）→ 修复后以 rootrd 提交
  `fix(steam): W2 核查修复 —— <真实错误列表>`，推送，继续 §4。
- **Index.ets 结构性损坏**（括号/装饰器大面积错乱）→ `git checkout b541772 -- entry/src/main/ets/pages/Index.ets`
  然后按 §1 的意图重做 W2 的 Index 部分（SteamService/SteamAccountStore 的修复结论仍有效）。

## 4. 核查通过后继续（今晚主体任务）

按总纲 `PLAN_STEAM_ACCOUNT_2026-09-27.md` §3 执行，从 W3 开始：

1. **W3 令牌**：SteamAccount 页已含"粘贴 shared_secret"路径（`steam-secret-input`/`steam-secret-save`）
   + TOTP 显示（`steam-totp-code`，30s 进度条）。待补：**maFile 文件导入**（document picker 读 JSON →
   vendor `SteamImporters.importMaFile` → shared_secret/identity_secret/device_id 逐项
   `storeSecret`）。验收：真机令牌码与官方 Steam 手机 App 一致（请用户人工比对）。
2. **W4 游戏库**（对启动器价值最大）：
   - 新文件 `steam/SteamLibrary.ets`：`GetOwnedGames`（URL/字段/CDN 拼接规则见总纲 §0.2/§0.3），
     内存缓存 + `steam_library_cache_v1` 独立键（TTL 30 分钟）。
   - SteamAccount 页加库列表段（封面缩略+名称+小时数+最近游玩相对时间）。
   - **本地匹配**：Index 长按菜单加"匹配 Steam 库"——名称规范化（去年份/版本号/全半角）双向包含匹配，
     命中的本地游戏走既有 M4 链路抓封面（storesearch→library_600x900→cover.jpg），
     Steam 时长写进现有 `play_time_v1`（`AppSettingsStore.addPlayMinutes`）。
     **跳过已有 cover 的本地游戏**（不覆盖用户自选）。
   - 弹窗（SteamPopPanel）里"游戏库与账号管理"行已跳 SteamAccount 页，不用改。
3. 每完成一步：编译 0 error → commit(rootrd) → push → 装机截图自证。
4. **W6（免登录下载免费游戏）今晚不做**，只按总纲 §2.5 的 6.1 做可研（若 W4 提前完成）。

## 5. 核查结果（新会话追加）

```
（核查会话在此追加: 日期 / git show --stat 结论 / 编译结果 / 冒烟结果 / 采取的路线 A|B|C）
```

## 6. 环境备忘（上会话验证过的事实）

- 编译链路：`bash /e/iiSU/vpp-check-sync.sh /tmp/<日志名>.log`（同步 E:\ui-sync → E:\vpp-build 后
  CompileArkTS）；打包 `cd E:/vpp-build && cmd.exe //c build-hap.bat`；装机
  `export MSYS_NO_PATHCONV=1 && hdc install -r "E:\vpp-build\entry\build\default\outputs\default\entry-default-signed.hap"`。
- **W1 曾假成功过一次**：tar 同步静默失败导致沙盒编译旧树 → 每次编译后必须 `grep -c "Error Message" <日志>`
  确认为 0，不要只看 EXIT 码。
- `hdc file recv` 的本地路径必须用 **Windows 反斜杠**（正斜杠会拼错）。
- 网络走路由器代理（fake-ip 198.18.x.x），抽风时 `SSL_ERROR_SYSCALL`，重试即可。
- 用户多会话并行改仓库：动手前 `git status`；**不要信本会话之外任何"上一会话说编译通过"的说法**。
