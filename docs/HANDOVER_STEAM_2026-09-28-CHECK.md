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
核查会话 2026-09-28 13:40–14:00，路线 A（核查通过，保留 41883b5）。

git show --stat：41883b5 恰含 6 文件（Index.ets / SteamAccount.ets(新) / SteamService.ets /
  SteamAccountStore.ets / SteamCryptoAdapter.ets / main_pages.json），无 vendor/docs/其他 pages 污染。
  4 个小文件 diff 与 §1 的 8 条类型结论逐条对应，自洽。
编译：vpp-check-sync.sh EXIT 0，"Error Message" 计数 0，BUILD SUCCESSFUL；
  另按 §6 教训用 md5 比对了 ui-sync ↔ E:\vpp-build 五个关键文件，全部一致（同步未静默失败）。
Index.ets 抽查：SteamEntryButton/SteamPopPanel/SortButton 三处 @Builder 装饰器都在；
  全文括号配对 balance=0；steam 相关 state/方法/id（steam-top-btn）齐全。
SteamAccount.ets：三段式结构完整，10 个 steam-* id 齐全，括号 balance=0，计时器在
  aboutToDisappear 清理。未发现半截代码。
装机冒烟（4NZ0225605001361，build-hap + hdc install -r + 冷启动）：
  ① steam-top-btn 在布局 [2317,28][2457,168]（顶栏最右）✓
  ② 点 S → 弹窗浮层出现（右上角锚定，"Steam"+说明+"登录 Steam"按钮，steam-pop-mask 存在）✓ 截图 pop2.jpeg
  ③ 点遮罩空白 (700,900) → 弹窗关闭（mask 消失，S 钮仍在）✓
  ④ 点"登录 Steam" → SteamAccount 页（steam-probe/steam-login-button 等 id 在）✓
  ⑤ 页内自检：首两次红点"无法连接 Steam API"（§6 所述代理抽风），重试后绿点"Steam 连接正常" ✓
     截图 acct.jpeg（绿点态）
附带的修复：probeServerInfo 异常原被 catch(_) 吞掉导致无法定位 DNS/SSL/超时，
  已改为 'ERR:'+JSON.stringify(e) 透传，SteamService.probeNetwork 展示真实原因
  （提交 fix(steam)：probe 自检异常透传）。
结论：41883b5 无污染，保留；按 §4 继续 W3/W4。
```

## 7. W3/W4 续作进度（同日晚, 全部已推送 origin/UI）

| 提交 | 内容 | 备注 |
|---|---|---|
| `4a8e934` | probe 自检异常透传（原 catch 吞掉无法定位）+ §5 结论 | 冒烟时两次"无法连接"是 §6 代理抽风，重试即绿 |
| `5aea976` | **W3.1 maFile 导入**：DocumentViewPicker 读文件 → vendor importMaFile → shared_secret/identity_secret/device_id 入 AssetStore；steamid 不一致提示；令牌段加"导入 maFile 文件"按钮（`steam-mafile-import`） | 真机码与官方 App 比对仍需用户人工做 |
| `7f5a127` | **W4.1+4.2**：`steam/SteamLibrary.ets`（GetOwnedGames 复用 vendor Endpoint/Parser；内存+持久缓存 30min TTL；网络失败回退过期缓存）+ 账号页 LibraryCard（LazyForEach/封面缩略/小时数/最近游玩/同步按钮 `steam-lib-sync`，登录后自动首拉） | — |
| `2e57f74` | **W4.3+4.4**：`steam/SteamMatch.ets`（名称规范化双向包含；library_600x900 直链下 cover.jpg，logoUrl 兜底，<2KB 拒收）+ 弹窗"匹配本地游戏（封面/时长）"行（`steam-pop-match`）+ `AppSettingsStore.setPlayMinutes`（绝对值覆盖，防重复匹配累加）+ 已有封面跳过 | **入口偏离计划**：原计划"长按菜单"，实际放 Steam 弹窗面板（详情页组件回调链改动大，且批量操作语义属 Steam 入口）；总纲 §4.3 的"长按菜单"可视为已由该行替代 |

- 待用户人工验收（需真账号）：登录全流程截图、库列表数量对账、"匹配本地游戏"命中报告 toast、令牌码与官方 App 比对、杀进程会话恢复。
- W6 只差可研（§2.5 6.1），未动。

### 7.1 登录失败根因与修复（f74bf79，PC 实测定位）

用户报"登录一直失败，其他客户端正常"。PC 上用 node 复刻 vendor 协议逐端点实测，抓到**两个上游 bug**：

1. **GetPasswordRSAPublicKey 是 GET-only**：POST → 405 + HTML 错误页
   ("This API must be called with a HTTP GET request")，vendor 的 responseObject 拿到 HTML 必炸。
   其余 4 个端点实测均为 POST-only（GET 回 405）。
2. **表单模式拒收 device_details**：BeginAuthSessionViaCredentials POST 带 device_details →
   400 "verify that all required parameters"；字段矩阵实测（仅名字/加 platform/空对象/os_type=0）全部 400，
   **省略该字段即 200**。

修复：`SteamHttpTransport.postForm` 对 405 把表单转 query 用 GET 重发（通用自愈，vendor 不动）；
vendor `beginFields` 删 device_details（NOTICE.md 已记档）。上游 steam_core 的 HTTP 登录链路
从未对真 Steam 端到端验证过（RSA 公钥注释自述 fixture-facing）， ASFWorkshop 参考实现同样带这两个坑。
复现脚本：`E:\tmp\steam-login-repro.js`、`E:\tmp\steam-methods-probe.js`、`E:\tmp\steam-begin-matrix.js`。

### 7.2 QR 扫码登录（c4e3602，替代繁琐的"手机确认"路径）

用户反馈密码登录能到"手机确认"但体验差。**实测推翻总纲 §0.1"扫码登录需 CM WebSocket"的判断**：
`BeginAuthSessionViaQR` 纯 HTTP 表单即返回 client_id/challenge_url(`s.team/q/1/<id>`)/request_id/interval=5s，
轮询 `PollAuthSessionStatus`（与密码流程同端点）等待态 200 空响应，手机确认后响应带 refresh/access_token。

实现（全部宿主侧，vendor 只加一个枚举值 BEGIN_QR_SESSION，NOTICE 记档）：
- `SteamService.beginQrLogin()`（发会话回 challenge_url）/ `pollQrLogin()`（pending/approved/invalid 三态；
  approved 时 JWT sub 解析 steamid，与密码登录同款落库+profile）/ `cancelQrLogin()`。
- SteamAccount 登录卡：密码表单加"扫码登录"按钮 → QR 模式（ArkUI 内置 `QRCode` 组件渲染，5s 轮询，
  失效自动重生成 ≤3 次后回退密码模式）→ 返回密码登录按钮；aboutToDisappear 清理轮询与扫码会话。
- 复现脚本：`E:\tmp\steam-qr-probe.js`。"密钥登录"（refresh_token 粘贴）未做 —— QR 已覆盖该需求场景。

### 7.3 手机确认会话改输令牌 5 位码（fbd5d09）

用户真机反馈：QR 扫码被 Steam 风控标"恶意登录"（风控引擎判断，红线不硬刚）；密码登录只给
"手机确认"选项太繁琐，要求支持密码+令牌验证码直登。排查发现 vendor parseChallenge 对
allowed_confirmations **无条件优先手机确认**，而 Steam 响应通常同时含 TOTP(type 3) 选项；
completeChallenge 对 MOBILE_CONFIRMATION 会话直接无视提交的验证码。

修复三处：vendor parseChallenge 语义靠 completeChallenge 兜底（MOBILE_CONFIRMATION 收到验证码
→ 按 code_type 3 提交，空码仍轮询）；宿主 submitCode 将 NEEDS_CONFIRM 路由到 TWO_FACTOR；
账号页 NEEDS_CONFIRM 分支加"5 位令牌验证码"输入框（steam-confirm-code-input/submit，
与手机确认轮询并行）。NOTICE.md 已记档。

### 7.4 令牌验证码死循环根因（6ce9ca6）

用户实测 fbd5d09 报"一直循环验证令牌"。根因：**keystone 风格错误以 HTTP 200 + `x-error` 响应头
返回**（验证码被拒即如此），vendor 的 SteamAuthHttpResponse 只带 statusCode+body、responseObject
只查状态码 → 拒绝被当成功 → pollAfterChallengeCode 轮询发现会话仍在等 → 界面再要码 → 死循环。
且 Steam 对**未信任新设备**的首次密码登录常只认手机确认（TOTP 被拒属正常风控）。

修复：SteamAuthHttpResponse 加 headers/keystoneError()（vendor，NOTICE 记档）、transport 透传
响应头（POST/GET 两路）、responseObject 先查 x-error、submitCode 失败留在验证界面显示 Steam
真实拒绝原因（不再弹回登录表单循环）。预期：新设备首次登录**手机确认一次后**启动器记住会话
（refresh_token 落 AssetStore），之后启动自动恢复免一切验证。

## 6. 环境备忘（上会话验证过的事实）

- 编译链路：`bash /e/iiSU/vpp-check-sync.sh /tmp/<日志名>.log`（同步 E:\ui-sync → E:\vpp-build 后
  CompileArkTS）；打包 `cd E:/vpp-build && cmd.exe //c build-hap.bat`；装机
  `export MSYS_NO_PATHCONV=1 && hdc install -r "E:\vpp-build\entry\build\default\outputs\default\entry-default-signed.hap"`。
- **W1 曾假成功过一次**：tar 同步静默失败导致沙盒编译旧树 → 每次编译后必须 `grep -c "Error Message" <日志>`
  确认为 0，不要只看 EXIT 码。
- `hdc file recv` 的本地路径必须用 **Windows 反斜杠**（正斜杠会拼错）。
- 网络走路由器代理（fake-ip 198.18.x.x），抽风时 `SSL_ERROR_SYSCALL`，重试即可。
- 用户多会话并行改仓库：动手前 `git status`；**不要信本会话之外任何"上一会话说编译通过"的说法**。

## 8. W6 下载链路（2026-09-29/30 深夜状态：代码全通，待网络验证）

### 8.1 已落地（全部编译 0 error、已推 origin/UI）

| 提交 | 内容 |
|---|---|
| `9bcf1fe` | W6 一阶段：CM 栈补拷（SteamCmClient/AuthClient/DirectoryCodec+4 codec）+ SteamCmWebSocket 宿主传输 + gzip MULTI 解压器 + 内容协议（GetServersForSteamPipe/GetCDNAuthToken/GetDepotManifest/ServiceMethod 解包）+ SteamDownloadService 门面 + **native libsteamdecompress.so**（LzmaDec+zstd 1.5.7，NAPI decompressChunk，独立 so）+ SteamChunkDecoder（VSZa/VZa/裸 LZMA 魔数剥壳） |
| `9f9a47f` | W6 二阶段：详情页下载 UI + SteamDepotWriter 落盘（Download/games/<名>/tmp/<depotId>/）+ setPlayMinutes/getRefreshToken + vpp-check-sync.sh 纳入 cpp 全目录同步（**修 native 源码缺失的假成功，重要**） |
| `3192f9b` | **depot 信息改走 PICS 正道**：无鉴权 appdetails 已不返回 depots（Valve 收紧，PC 实测）；vendor CmClient 加 callProto（EMsg 请求/响应按 proto jobId 配对，路由 5203）；PicsAppInfoRequest 按官方 proto 重写 |
| `77b00f2`/`44940d1`/`33bfb89` | CM 握手三连修：minSupportTlsProtocol 必传（缺则 NETSTACK not found + 30s 超时）、45s guard 兜底 promise 悬挂、**connect resolve(false) 勿当成功** |
| `edc9426`/`231d027`/`5891923`/`1aa3899` | 诊断链：JSON.parse 动态对象 ArkTS 要 Record+Object.keys（直接取属性真机抛 Cannot load property of null）、Error 文案取 .message（stringify 恒为 {}）、堆栈首帧带回页面、4 阶段进度文案 |

### 8.2 唯一剩余卡点（明早从这里继续）

**CM wss 握手在设备网络环境失败**：`wss://cmp1-sea1.steamserver.net/cmsocket/` TLS 握手不通
（connect resolve(false) / on('error') code=200 data=0）。**同网络 PC node 直连握手成功** → 代理放行
wss，是鸿蒙 netstack(lws) 与代理的 TLS/SNI 兼容问题。

**下一步（按优先级）**：
1. 路由器 iStoreOS 给 `*.steamserver.net` 配直连规则（绕代理），设备重试下载；
2. 仍失败 → 手机热点换网验证（区分代理 vs 运营商）；
3. CM 一通则全链自通（PICS→CDN→chunk→解压→落盘代码全部就位）。
4. 测试前先重新登录（会话恢复一直报 access_token refresh http=400，refresh_token 已过期）。

### 8.3 资料索引

- 解压验证脚本：`E:\tmp\vzip-test\`（vzip_test.c + sample.vzip，LzmaDec 解 VZip 逐字节还原）
- 7-Zip-zstd 源码：`E:\tmp\7z-zstd\`（LzmaDec.c 1367 行零依赖；zstd/ 平铺布局，v01-v07 旧格式解码器不需要）
- VZip 布局（实测钉死）：壳头 7B（'VZa'+ts4B）+ props 5B + LZMA 流 + 尾 14B（'zv'+CRC32+原长8B）；VSZa 尾 15B
- 设备上当前构建：含全部上述代码（23:50 装机）
