# Steam 账号接入 · 工作列表（2026-09-27 定稿，今晚 23:00 动工）

> **一句话目标**：给旧柚Pro 接入 Steam 账号能力 —— ①密码登录（含 Steam Guard 验证码）②手机令牌 5 位码显示
> ③拉取 Steam 游戏库并与本地游戏目录匹配（批量补全封面 + 时长上卡面）。
> **参考实现**：ASFWorkshop + OpenHarmonyASF（steam_core，Apache-2.0），本地克隆在 `E:\tmp\ASFWorkshop`、`E:\tmp\OpenHarmonyASF`。
> **本工程铁律不变**：只动 `entry/src/main/ets/`；只进 `UI` 分支；数据不落 C 盘；提交身份 rootrd；
> 不加 ohpm 依赖（steam_core 本身零依赖，源码直拷即可）；不提交任何真实账号/密码/token/maFile。

---

## 0. 调研结论（今晚写代码直接引用）

### 0.1 最重要的三个架构判断

1. **登录走 HTTP 通道，不移植 CM/WebSocket 栈。** steam_core 的 `SteamAuthService.ets` 是纯 HTTP 完整实现：
   RSA 公钥 → 密码加密 → BeginAuthSession → 邮件码/令牌码/手机确认 → 轮询拿 token → 刷新。
   唯一不支持的是**扫码登录**（那是 CM WebSocket 专属）→ 扫码列为后续可选阶段，今晚不做。
   好处：不用搬 protobuf 编解码器、WebSocket 传输、zlib 解压三大件，工作量砍掉 2/3。
2. **游戏库不需要申请 Steam Web API Key。** `IPlayerService/GetOwnedGames` 用**登录后的 access_token**
   就能调（query 参数直传），返回全量库 + 时长 + 封面文件名。这与我们 M4 卡住的 SteamGridDB Key 完全解耦。
3. **TOTP 是零依赖纯算法**（HMAC-SHA1 + 30s 窗口 + 26 字符表），steam_core 的 `SteamTotp.ets` 整文件拷走，
   只需宿主提供 `hmacSha1(secret, bytes)`（CryptoArchitectureKit 直调，~30 行适配器）。

### 0.2 端点-凭据对照表（今晚和以后都查这张表）

| 端点 | 需要什么 | 能拿什么 |
|---|---|---|
| `api.steampowered.com/IAuthenticationService/GetPasswordRSAPublicKey/v1` | 无 | RSA 公钥 + timestamp（加密密码用） |
| `…/BeginAuthSessionViaCredentials/v1` | 无 | client_id/request_id/steamid/allowed_confirmations（挑战类型） |
| `…/UpdateAuthSessionWithSteamGuardCode/v1` | 无 | 提交邮件码(type 2)/令牌码(type 3) |
| `…/PollAuthSessionStatus/v1` | client_id+request_id | **refresh_token + access_token**（登录完成） |
| `…/GenerateAccessTokenForAccount/v1` | refresh_token | 新 access_token（会话刷新，可能轮换 refresh） |
| `api.steampowered.com/IPlayerService/GetOwnedGames/v1` | **access_token** | 全量库：appid/name/**playtime(分钟)**/rtime_last_played/img_icon_url/capsule_filename |
| `store.steampowered.com/api/appdetails` | **无（免 key 免登录）** | 名称/头图/简介/开发商/类型（我们 M4 已在用 storesearch+CDN） |
| `steamcommunity.com/profiles/{id}/games/?tab=all` | Web cookie | 库页瓦片（小时级时长，第一屏，非全量）——备选通道 |
| `store.steampowered.com/dynamicstore/userdata/` | Web cookie | 拥有数量（rgc）——对账用 |
| `steamcommunity.com/inventory/{id}/{app}/{ctx}` | Web cookie | 库存物品（后续阶段） |
| CM `CMsgClientGamesPlayed`（5410） | CM 完整登录 | 伪造"游戏运行中"= 挂卡。**不做** |

### 0.3 参考实现文件地图（file:line 都验证过，写代码时对着抄逻辑）

| 参考文件（CORE=`E:\tmp\OpenHarmonyASF\steam_core\src\main\ets`，APP=`E:\tmp\ASFWorkshop\entry\src\main\ets`） | 拿什么 |
|---|---|
| `CORE\auth\SteamAuthService.ets` | HTTP 登录全流程：signIn L145-178、completeChallenge L185-228（含自动 TOTP L196-203）、poll L260-341、refresh L356-410、字段表 L412-435 |
| `CORE\auth\SteamAuthProtocol.ets` | 5 个端点常量 L6-10 + RSA 公钥手写 DER→PEM L51-164 |
| `CORE\security\SteamTotp.ets` | TOTP 全算法：字符表 L3、30s 窗口 L4、小端时间 L26-28、偏移 L33、字符映射 L42-48 |
| `CORE\import\SteamImporters.ets` | maFile 解析 L568-637（字段白名单 L607-614）、ASF bot L83-120 |
| `CORE\import\SteamJson.ets` | ArkTS 安全的类型化 JSON 解析器（无动态键） |
| `CORE\security\SteamJwt.ets` | 从 refresh_token 的 JWT `sub` 离线解析 SteamID64 L11-43 |
| `CORE\contracts\SteamAuthErrors.ets` | 结构化错误分类（validation/transport/timeout/…） |
| `CORE\models\SteamModels.ets` | 模型定义（登录/库相关子集） |
| `CORE\library\SteamOwnedGamesWebApi.ets` | GetOwnedGames URL L40-49、字段解析 L94-127、**图标/竖版封面 CDN 拼接规则 L17-20,L108-115**、分钟→秒 L151-157 |
| `APP\steamAdapters\HarmonyBinaryCryptoAdapter.ets` | CryptoArchitectureKit 适配写法参考（hmacSha1 L14-25 / rsa L27-38 / AES L40-64） |
| `APP\steamAdapters\HarmonySteamAuthTransport.ets` | x-www-form-urlencoded POST 封装样板 L52-107 |
| `APP\ngf_framework\...\KeyStoreManagerFacade.ets` + `SecureCredentialStoreFacade.ets` | AssetStore 用法（`@kit.AssetStoreKit`，`DEVICE_FIRST_UNLOCKED`，alias 规范）L19-26/L39-51/L79-85 |

### 0.4 steam_core 许可与红线

- steam_core = **Apache-2.0**：整文件拷贝需保留原文件头版权注释，并在 vendor 目录放 `LICENSE`（Apache-2.0 全文）
  与 `NOTICE.md`（来源 `github.com/DaLongZhuaZi/OpenHarmonyASF` + 引入 commit + 修改说明）。这不属于构建配置，不违铁律。
- ASFWorkshop 应用层（MIT）只**借鉴架构**，不整抄其页面/服务代码（与我们"只借鉴形态"的原则一致）。
- **绝不**：把真实账号/密码/token/maFile 提交进仓库；把凭据写进普通 preferences 明文。

---

## 1. 目标架构（落地到我们工程）

```
entry/src/main/ets/
├─ steam/                        ← 新增目录（全部新增文件，无 ohpm 依赖）
│  ├─ vendor/                    ← steam_core 拷贝（保留版权头）
│  │  ├─ SteamTotp.ets  SteamJwt.ets  SteamJson.ets  SteamImporters.ets
│  │  ├─ SteamAuthProtocol.ets  SteamAuthService.ets  SteamAuthErrors.ets
│  │  ├─ SteamModels.ets(登录/库子集)  SteamBase64.ets
│  │  ├─ LICENSE  NOTICE.md
│  ├─ SteamHttpTransport.ets     ← POST form 封装（@ohos.net.http，参照 APP 适配器）
│  ├─ SteamCryptoAdapter.ets     ← hmacSha1/rsaEncrypt（CryptoArchitectureKit 直调）
│  ├─ SteamClock.ets             ← Date.now()（v1 不做 NTP 校准）
│  ├─ SteamAccountStore.ets      ← 凭据进 @ohos.security.asset；非密进独立 preferences 键
│  │                                （steam_profile_v1：accountName/steamId64/avatar；沿用我们的独立键模式）
│  └─ SteamService.ets           ← 单例门面：登录状态机 + 订阅通知（极简版 SteamArkStateService）
├─ pages/SteamAccount.ets        ← 新页面，三段式：账号登录 / 令牌 / 游戏库（Tab 切换）
├─ pages/Index.ets               ← 顶栏右上角新增 Steam 头像按钮（与排序/设置并排）+ 弹窗面板；
│                                   长按菜单加"匹配 Steam 库"入口（批量封面/时长）
└─ pages/SystemSettings.ets      ← 不加入口（用户 2026-09-27 指定：入口在主页顶栏，不放设置页）
```

**顶栏增量（用户 2026-09-27 亲自批准，记档防止后续会话误判违冻）**：
顶栏右侧由「排序 + 设置」扩为「**Steam 头像 + 排序 + 设置**」三钮并排。头像钮行为：
- **未登录**：显示灰色 Steam 图标（40vp，同设置钮规格），点击 → 弹窗显示"登录 Steam"按钮 → 跳 SteamAccount 页；
- **已登录**：显示 Steam 头像（拉取 profile 头像，圆角 20），点击 → **小弹窗面板**（顶栏下方右锚定）：
  账号名 + SteamID、**当前 Steam Guard 令牌码**（W3，面板内直接显示，最常用）、
  "游戏库"行（跳 SteamAccount 库段）、"下载"行（W6 预留，先置灰"敬请期待"）、"登出"行。
- 弹窗实现：ArkUI `bindPopup` 自定义 builder 或绝对定位浮层 + 全屏透明遮罩（点外部关闭）——
  沿用我们详情页/底栏的 `hitTestBehavior` 经验；面板挂 id（`steam-pop-*`）供 dumpLayout 验证。
- 顶栏其余结构（时间电池｜搜索框｜排序设置）原样不动。

- 状态通知用**回调订阅 + AppStorage**（沿用我们现有模式，不引 Emitter）。
- v1 **单账号**（数据结构用 accountId=steamid64 预留多账号扩展）。
- 凭据分级：refresh_token/access_token/shared_secret → AssetStore（`steam.<steamid64>.<kind>`）；
  accountName/steamId64/avatar → preferences（非密）。

---

## 2. 工作列表（W0–W5）

### W0 动工前预检（23:00 前，~15 分钟）

| # | 任务 | 验收 |
|---|---|---|
| 0.1 | 设备连通性实测：装机后从应用内（或临时调试页）请求 `https://api.steampowered.com/ISteamWebAPIUtil/GetServerInfo/v1/?format=json` | HTTP 200 且返回 server_time；不通则今晚先解决设备网络（代理/热点），**此项为硬前提** |
| 0.2 | 确认 `E:\tmp\ASFWorkshop`/`E:\tmp\OpenHarmonyASF` 克隆在位（已在 ✓） | 文件可读 |
| 0.3 | 用户准备：一个用于测试的 Steam 账号（**建议小号**）；如已有 Steam 手机令牌，导出一份 maFile 备用（不想导则令牌功能改为"手动粘贴 shared_secret"，二选一） | 用户点头 |
| 0.4 | F: 工作区不阻塞：今晚全部在 `E:\ui-sync` 工作区干活 | — |

### W1 底座：vendor 拷贝 + 三个宿主适配器（~50 分钟）

| # | 任务 | 产出 | 验收 |
|---|---|---|---|
| 1.1 | 拷贝 §0.3 表中 CORE 低耦合文件到 `steam/vendor/`，逐个去掉与未拷贝文件的 import（SteamModels 裁剪登录/库子集） | vendor 目录 + LICENSE + NOTICE.md | `vpp-check-sync.sh` 0 error |
| 1.2 | `SteamHttpTransport.ets`：postForm(url, fields) → string；超时 15s/20s；响应码非 200 抛 SteamAuthError(transport) | 文件 | 单测式调用 GetServerInfo 成功 |
| 1.3 | `SteamCryptoAdapter.ets`：hmacSha1(key,data)→Uint8Array（CryptoArchitectureKit Mac）、rsaEncrypt(spkiPem, data)→Uint8Array | 文件 | 用 vendor 的 SteamAuthProtocol.toPem + 已知公钥走通一次 GetPasswordRSAPublicKey 解析 |
| 1.4 | `SteamClock.ets` + `SteamAccountStore.ets`（AssetStore 写读删 + preferences 非密档） | 文件 | 写读一个假 token 往返成功 |
| 1.5 | 编译校验 + 提交（feat: steam 底座） | — | origin/UI 推送 |

### W2 登录：状态机 + 页面（~100 分钟）

| # | 任务 | 产出 | 验收 |
|---|---|---|---|
| 2.1 | `SteamService.ets`：单例；状态枚举 `Idle→Rsa→Authenticating→AwaitingEmailCode/AwaitingCode/AwaitingConfirm→Polling→LoggedIn→Error`；`signIn(account,password)`、`submitCode(code)`、`poll()`（1s 间隔，最多 10 次×challenge 后 10 次沿用参考实现节奏）、`refresh()`、`signOut()`；成功后 SteamJwt 解析 steamid64、凭据落 AssetStore、刷新得到的 access_token 缓存内存+安全存储 | SteamService.ets | 状态可订阅；每步有 app-log |
| 2.2 | `pages/SteamAccount.ets` 账号段：账号/密码输入、登录按钮（防连点）、挑战分支 UI（邮件码输入 / 令牌码输入 / "请在手机 Steam 上确认"轮询动画）、错误文案映射（SteamAuthErrors.kind→中文）、已登录态显示账号名+头像+登出 | 页面 + router 注册 | 真机：小号密码登录全流程走通一次，`steam_profile_v1` 落值 |
| 2.3 | 会话恢复：启动时（或进 SteamAccount 页时）读 AssetStore refresh_token → `refresh()` → 成功直接 LoggedIn；失败清凭据回 Idle | SteamService.restore() | 杀进程重开，无需重新登录 |
| 2.4 | **主页顶栏右上角 Steam 头像钮**（与排序/设置并排，40vp 同规格）+ 弹窗面板骨架：未登录=灰色图标+登录入口；已登录=头像（profile 头像，回退灰图标）+ 弹窗（账号名/令牌码占位/游戏库行/下载行置灰/登出）；`.id('steam-top-btn')`、面板 `.id('steam-pop-panel')` | Index 顶栏 + 弹窗组件 | dumpLayout 可见；点击弹窗开合正常 |
| 2.5 | 风控文案：登录页脚注"建议使用小号；频繁验证码属 Steam 风控，与客户端无关" | — | 页面可见 |

### W3 令牌：maFile 导入 + TOTP 显示（~60 分钟）

| # | 任务 | 产出 | 验收 |
|---|---|---|---|
| 3.1 | maFile 导入：文件选择器（`@kit.CoreFileKit` document picker）读 JSON → vendor SteamImporters.importMaFile → shared_secret/identity_secret/device_id 入 AssetStore；无 maFile 时支持"粘贴 shared_secret"降级路径 | SteamAccount.ets 令牌段 | 导入一份真 maFile 报告 recognized |
| 3.2 | TOTP 展示：30 秒倒计时环 + 5 位码（大字，`.id('steam-totp-code')`）+ 点击复制（pasteboard）+ 剩余秒条；秒级 tick 用 setInterval，页面隐藏时暂停 | 同上 | 真机码与官方 Steam 手机 App 码一致（用户人工比对）|
| 3.3 | 时间偏移保护：显示"设备时间需准确"提示；预留 `steam_time_offset_v1` 偏移键（v1 不做 NTP） | — | — |

### W4 游戏库：拉取 + 匹配 + 批量补全（~90 分钟，对启动器价值最大）

| # | 任务 | 产出 | 验收 |
|---|---|---|---|
| 4.1 | `SteamLibrary.ets`：GetOwnedGames 拉取（access_token+steamid）、解析为 `{appid,name,playtimeMinutes,lastPlayed,capsuleUrl,iconUrl}`（CDN 拼接规则照 §0.3 参考文件 L17-20）；内存缓存 + `steam_library_cache_v1` 独立键持久化（半小时 TTL） | 文件 | 真机拉到真库（数量与 Steam 网页版一致） |
| 4.2 | 库 UI：游戏库段列表（封面缩略图+名称+小时数+最近游玩相对时间），顶部"同步"按钮；数量对账显示 | 页面 | 截图有真数据 |
| 4.3 | **与本地目录匹配**：名称规范化（去年份/版本号/全半角/大小写）后双向包含匹配；命中的本地游戏批量执行"抓取封面"（复用 M4 链路：storesearch→library_600x900 落 cover.jpg）并把 Steam 时长写入我们现有 `play_time_v1`（`addPlayMinutes(playtimeMinutes)`，以后时长排序/卡面显示直接生效） | Index 长按菜单项"匹配 Steam 库"+ SteamService 桥 | 匹配报告 toast："命中 n / 本地 m"；被命中的本地游戏 rescan 后有封面 |
| 4.4 | 冲突策略：本地已有 cover 的游戏**跳过**（不覆盖用户自选封面），除非长按菜单选"强制覆盖" | — | 自选封面不被冲掉 |

### W5+ 明确不做/后议（写清楚避免今晚跑偏）

- **扫码登录**：需要 CM WebSocket + protobuf 全家桶，收益/成本比低；手机上输密码+验证码够用。后议。
- **挂卡（伪造游戏运行）**：必须 CM 栈 + 长连心跳，且有账号风控风险，与启动器定位无关 → 不做。
- **库存/徽章/好友/消息**：与启动器无关 → 不做。
- **多账号 UI**：数据结构已预留，UI 只在需要时开。

---

## 2.5 Steam 免费游戏下载（W6，2026-09-27 补充立项 · 下阶段）

> 用户需求：像 DepotDownloader（github.com/steamre/DepotDownloader）那样**免登录下载免费游戏**，
> 落到本地游戏库经 Wine 运行；原则同封面抓取——**免登录优先，登录增强**（登录后可下已拥有付费游戏）。

### 已实测/已知事实（2026-09-27 PC 侧核验）

- 匿名 `appdetails`（免 key 免登录）能给出 `is_free` / `platforms.windows` / `packages`——**发现与筛选够用**；
  但**不含 depot 列表**（730 实测无 `depots` 键）→ "下载哪些文件"必须走 **CM 匿名会话**：
  anonymous logon → PICS appinfo（depot 列表）→ GetDepotManifests → CDN auth token → 分块 HTTP 下载。
  这正是 DepotDownloader 的免费游戏路径（`-u anonymous`）。
- steam_core 可复用的只有 **CM 帧层 + 手写 protobuf 原语 + CM 客户端骨架**（登录/心跳）；
  PICS/depot/CDN 下载相关消息（ClientPICS、GetDepotManifests、GetDepotDecryptionKeys、GetCDNAuthToken、
  GetServersForSteamPipe）steam_core **没有**（ASF 不下载游戏文件），需要我们自己补写编解码。
- chunk 解压两种格式：**VZip（zlib 容器）**——`@ohos.zlib` 可解；**LZMA**——ArkTS 无原生实现。
  ⚠️ W6.1 必须先核对目标 depot 实际用的压缩格式与魔数（对照 SteamKit2/DepotDownloader 源码），
  若主流是 LZMA 则面临选型：纯 TS 解码器（慢）/ 引入 cpp 组件（**碰铁律，需用户特批**）/ 只下 VZip depot。

### 工作项（下阶段，预计 3–4 个晚上）

| # | 任务 | 要点 | 验收 |
|---|---|---|---|
| 6.1 | **可研**：匿名 CM logon 打通（复用 steam_core CM 骨架）；核对 chunk 压缩格式与魔数；LZMA 选型结论上报用户 | 独立调试页只显示 logon 成败 + appinfo 抓取样例 | 匿名会话拿到任一免费 app 的 depot 列表 |
| 6.2 | 协议补写：PICS appinfo / GetDepotManifests / GetDepotDecryptionKeys(免费=无键) / GetCDNAuthToken / GetServersForSteamPipe | 全部走 steam_core 的 SteamProtoReader/Writer，逐字段对照 SteamKit2 | 拿到某免费 app 的 manifest 明细 |
| 6.3 | 下载器：CDN 分块并发下载（限 3 并发）、进度通知（AppStorage 给 UI）、断点续传（.part + chunk 校验 CRC/Murmur）、磁盘空间预检 | 下载到 `Download/games/<名称>/tmp` | 断网重试可续；进度条实时 |
| 6.4 | 解包入库：VZip=zlib 解（LZMA 按 6.1 结论）；按 manifest 组装文件树 → `Download/games/<名称>/` → AppCatalogService 扫描自动出现在启动器 | — | 装机后封面流出现该游戏，Wine 点击可启动 |
| 6.5 | **登录增强**：已拥有付费游戏 = CM 完整登录 + license → depot key；UI 上免费游戏标"免费可下"、已拥有标"库内可下" | 与 W2 登录打通（账号页显示下载入口） | 付费已拥有游戏可下载 |
| 6.6 | 红线：只下载**免费内容**与**自有库内容**；**不提供 steam_api 破解/替换**（游戏自带 Steam DRM 在 Wine 下的可玩性不做承诺）；下载行为属 Steam ToS 灰区，批量/滥用风险由用户自担 | 页面免责声明 | — |

**连带收益**：若 6.1/6.2 立项，CM 栈的投入可同时解锁 **扫码登录**（BeginAuthSessionViaQR）——届时把 W5+ 里的"扫码后议"一并评估。

---

## 3. 今晚 23:00 执行顺序（预计 23:00–02:00，含装机验证）

```
23:00  W0 预检（设备网络 0.1 硬门槛，不通先修网络）
23:15  W1 底座四件套 → 编译过 → commit+push
23:55  W2.1 SteamService 状态机（不看 UI，纯逻辑+日志）
00:40  W2.2 登录页 UI → 装机 → 小号登录实测 → 截图自证
01:05  W2.4 顶栏头像钮 + 弹窗面板（登录态头像/令牌码/库/下载占位）→ 装机截图
01:25  W2.3 会话恢复 + 杀进程复测
01:45  W3 令牌（maFile 导入 + TOTP 页，弹窗内同步接入当前码）→ 与官方 App 比对
02:20  W4 至少完成 4.1+4.2（拉库展示）；4.3 匹配若时间不够顺延明晚
```

- 每完成一个 W 包：`vpp-check-sync.sh` 0 error → commit（rootrd）→ push。
- 每次装机后：`uitest dumpLayout` 按 id 验证 + `snapshot_display` 截图。
- 中途 Steam 风控/验证码轰炸：立即停手换时间，不硬刚。
- 若 W4 提前完成：只启动 W6.1 可研（匿名 CM logon 通断 + chunk 压缩格式核对 + LZMA 选型），**不写下载本体**。

## 4. 风险与回退

| 风险 | 影响 | 对策 |
|---|---|---|
| 设备到 `api.steampowered.com` 不通（store 可通不代表 api 可通） | 登录全废 | W0 硬门槛；不通则设备换网/代理后再开工 |
| Steam 登录风控（新设备/IP 触发频繁邮箱码） | 无法登录 | 用小号；间隔重试；绝不连续暴力尝试 |
| refresh_token 轮换（GenerateAccessToken 响应里可能下发新 refresh） | 旧 token 失效 | 每次刷新后若返回新 refresh_token 则覆盖 AssetStore（参考 CM 版 L586 语义，HTTP 版同字段检查） |
| TOTP 时间漂移 | 码不对 | 先比对官方 App；偏差大再启用偏移键 |
| AssetStore API 在此 ROM 的行为差异 | 存储失败 | 回退方案：AES 加密后存独立 preferences（CryptoAdapter 已有 AES-CBC） |
| F: 工作区合并冲突 | 今晚无关 | 全程在 E:/ui-sync 干活 |

## 5. 验收清单（全部打勾才算本阶段完成）

- [ ] 真机密码登录成功（含一次验证码挑战），截图
- [ ] 杀进程重开会话自动恢复，截图
- [ ] 令牌码与官方 Steam App 一致（人工比对），截图
- [ ] 库列表数量与 Steam 网页版一致，截图
- [ ] 至少 3 个本地游戏通过匹配自动获得封面，rescan 后封面流可见，截图
- [ ] 被匹配游戏在排序"时长"下位次符合 Steam 时长
- [ ] 全程无凭据落入 preferences 明文 / 仓库文件
- [ ] origin/UI 推送，提交全部 rootrd

## 6. 开放问题（23:00 前用户拍板）

1. 用哪个 Steam 账号测试（建议小号）？
2. 令牌来源走 maFile 导入还是手动粘贴 shared_secret？（有 Steam 手机令牌的可从 SDA 导出 maFile）
3. 游戏库匹配是"自动批量"还是"逐个确认"？（默认：自动但跳过已有封面）
4. ~~入口位置~~ 已定：主页顶栏右上角，与排序/设置并排，头像+弹窗（2026-09-27 用户指定）。
