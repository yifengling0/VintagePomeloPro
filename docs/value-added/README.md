# 账号、会员与同步首版

## 范围与当前状态

本分支独立实现，未复制其他仓库源码。已有免费游戏、Wine、Steam、手机分支及 phone/tablet/PC 的打包配置不变。账号与云服务页面只在 tablet、pc、2in1 显示；phone/tv/未知类型隐藏入口，直接路由和服务调用同样阻止触发新增网络功能。隐藏不代表停止支持手机。

已实现：

- 华为授权码 → 服务端换码、读取可信 openID → 30 分钟内部会话；设备仅内存持有会话。
- 非自动续订月卡（IAP type=3）及独立支持开发商品（type=0）。商店查询真实价格，后端查询最新订单、验证证书/JWS，原子保存订单与会员事实后才允许客户端 finish。
- 完整 InputProfile v6 方案同步；平板/桌面独立版本；CAS 冲突不覆盖；采用云端前本地备份。列表只返回轻量名称/版本索引，完整方案按需下载，避免合法大方案撑爆列表。
- 本地非 Steam 游戏自动识别唯一可信的存档目录，在每款游戏设置中单独开启（默认关闭）；运行环境退出后制作校验快照，内容未变不重复保存，恢复前保留保护备份。未知或多候选位置不开启备份。
- 云存档完整前后端代码：显式跨设备存档位绑定、有界分块上传、OBS 私有对象、校验后 CloudDB CAS 发布、历史版本、下载为本地快照、取消与安全清理。

尚未接通真实账号/AGC/IAP 配置、实云并发探针或沙盒付费/退款验收。UI 与本地备份真机验证记录随测试包交付，不能代替真实云同步验收。默认显示 `NOT_CONFIGURED`，没有模拟成功。云存档代码已实现，但未取得真实 OBS/后端验收报告，默认仍不开放；本地自动备份已实现；云存档仍保留显式上传和恢复确认，未宣称自动跨设备恢复。自动续订 type=2 端侧类型可配置，但本版服务端目录明确拒绝该类型，不能误收款。

## 设置与输入方案的自动同步

系统设置 → 账号与同步 → 自动同步。输入方案与设置开关按账号保存，默认开启；无需逐个上传。登录、返回前台和本地修改触发串行队列，修改合并 700 ms，前台每 30 秒检查云端，网络失败最长 5 分钟退避重试，后台停止轮询。

页面显示整体状态、每项状态和最近成功同步时间；部分内容未开放、等待方案、冲突或其他账号内容未上传时，不显示全部同步成功。关闭某一项仅停止其自动传输；离线使用不受影响。退出、过期或换号取消旧请求落地，新账号不会自动收到旧账号的本地数据。

设置只同步外观、卡片布局与桌面默认输入方案 ID；输入方案同步 v6 按键、鼠标速度和手柄映射。设备路径、Wine/FEX/图形参数、Steam 文件、账号凭据不上传；tablet 和 desktop 分开。云端默认方案尚未在本机就绪时，整份设置等待，不局部应用或推进版本基线。旧云端内建方案不自动覆盖新模板。

双方同时修改时 CAS 保留双方，显示“保留本机／采用云端”；采用云端前保存本地副本。本机删除方案不被自动下载复活，也不自动删除云端副本。

自动队列使用真实 ArkTS 服务转译的主机 mock 测试，覆盖首次、变更合并、云更新、冲突、离线、开关、换号、退出、前后台、删除、落地期间修改、默认方案依赖、旧模板和损坏基线。它证明队列行为，不证明真实华为账号、云函数、会员或跨设备网络已接通。

## 月卡规则

当前规则 ID 为 `calendar-month-v1`：服务器以已验证 purchaseTime 为起点，UTC 加一个日历月，目标月无对应日则钳位到月末，保留时分秒和毫秒。例如 1 月 31 日到 2 月月末。它是本应用的明确商品策略，不是 IAP 回传到期日；IAP 非续订订单没有 expiresTime。

相同订单只存一份事实。不同未撤销订单按 purchaseTime、orderId 排序，从 `max(购买时间, 上一段到期时间)` 继续加月。退款/撤销后重新计算；退款/撤销不可被较晚到达的 purchased/expired 结果复活。规则版本随订单持久化，后续不得改变已售 SKU 的期限语义。若决定固定 30 天，应新增独立规则/SKU 并补测试，不能默改已售月卡。

支持开发商品不授予会员。只支持 quantity=1，不会把消费商品当订阅。商品 ID、价格均未预设；发布前需在 AGC 和商品说明中明确规则及当地价格。

## 客户端配置

编辑 `entry/src/main/resources/rawfile/value_added_config.json`，只填公开信息：

- `apiBaseUrl`：没有尾斜杠的 HTTPS 服务地址，可带固定路径前缀，不允许 query/用户名/密码。
- `accountClientId`：本 HarmonyOS 应用的 OAuth Client ID。
- `membershipProductId`：AGC 的 type=3 SKU。
- `membershipProductType`：3。
- `coffeeProductId`：AGC 的 type=0 SKU。

按华为官方要求在 `entry/src/main/module.json5` 的 `module.metadata` 加 `client_id`，值必须与上述公开 ID 一致。现有 `deviceTypes` 不得删改。包名 `com.vintage.pomelopro`、AGC 应用、签名指纹必须对应。客户端初始化会核对 metadata，缺失/不匹配即 NOT_CONFIGURED；不要往客户端填 client secret、IAP 私钥或 AGC Server SDK 凭据。

入口统一为系统设置的“账号与同步”，沿用设置页的 HDS 沉浸式标题栏、安全区与皮肤。账号状态、输入方案、游戏存档和会员通过分组列表管理；原 AccountCloud 路由转到该设置项。平台隐藏、未配置和本地快照互相独立：符合设备范围时，本地快照不需要登录、会员或云配置。

## 后端运行

目录：`server/value-added`。Node.js >=22、系统 OpenSSL 3、HTTPS 反向代理、AGC CloudDB 是必要运行条件。当前提供独立 Node HTTP 服务入口，尚未验证 AGC 云函数具体运行时/HTTP 事件适配；不能把已开通云函数理解为直接可部署本包。`npm ci --ignore-scripts` 安装已锁定的官方 `@agconnect/database-server@1.0.7` / `common-server@1.1.0`，OBS 使用 `esdk-obs-nodejs@3.26.2`。没有 SQL 或新增 KMS 依赖。

该官方 SDK 的 common-server 声明旧 axios `^0.21.1` 和 uuid `^8.3.0`。首次 `npm audit` 报 3 high、1 moderate（包含传递依赖汇总）；本新增后端独立 override 并锁定 axios `0.34.0`、uuid `11.1.1`，不改变原 HarmonyOS 工程依赖。会员首片的 audit 为 0；加入 OBS SDK 后当前 audit 为 2 moderate（同一传递依赖及 SDK 汇总），不是 0。相关公告包括 [axios 原型污染](https://github.com/advisories/GHSA-x97p-jq2g-jp4f)、[axios SSRF/凭据风险](https://github.com/advisories/GHSA-jr5f-v2jv-69x6)、[uuid buffer bounds](https://github.com/advisories/GHSA-w5hq-g745-h8pq)。SDK 使用 uuid.v1，不是公告中的 v3/v5/v6，但仍需保持依赖风险可见。

OBS SDK 引入 `fast-xml-parser` 的 [XMLBuilder 注释/CDATA 注入公告](https://github.com/advisories/GHSA-gh4j-gqv2-49f6)。本适配器只使用 SDK 本地签名函数，未走其 XMLBuilder/网络解析路径；仍披露依赖风险，没有为清空 audit 盲目跨主版本 override。

实际 SDK 3.26.2 的 HTTP 实现跳过 hostname 校验，且预签 PUT 不签 Content-Length。因此本版**不使用 SDK 的网络方法**，也不给客户端 PUT URL；只调用本地 `createSignedUrlSync`，网络全部由默认 TLS/hostname 校验的 Node fetch 发出，禁止重定向并带超时。真实存储验收未通过时不启用。

`npm run test:sdk` 用真正的 pinned AGC SDK 对本地 HTTPS mock 验证 TLS、认证/requestId、JSON body、响应/错误、超时、查询与事务序列化，不只是 import。这不能替代实云探针。CloudDB fingerprint 包含 package-lock 摘要，任何锁变动都会要求重新取得实云证据。

后端只监听 127.0.0.1。反向代理必须提供 HTTPS，限制请求大小、每 IP/账号速率、超时，禁止记录 Authorization 和请求/响应正文。本服务另有整体 300 请求/分钟的防误用限制；这是首版安全上限，不是多租户公平限流。

环境变量（真实值只进入部署 secret/config，不提交到 Git）：

- `VP_ACCOUNT_CLIENT_ID`、`VP_ACCOUNT_CLIENT_SECRET`
- `VP_APP_ID`、`VP_PACKAGE_NAME`
- `VP_IDENTITY_HMAC_SECRET`、`VP_PURCHASE_HMAC_SECRET`：分别至少 32 字符、高熵、保持稳定。更换身份 HMAC 会改变内部 userId，须先制定迁移，不可随部署随机重置。
- `VP_AGC_CREDENTIAL_FILE`：AGC Server SDK 受保护凭据文件；`VP_AGC_ZONE`：指定 CloudDB zone。
- `VP_CLOUDDB_VERIFICATION_FILE`：同项目/region/zone/SDK/完整依赖锁的实测探针报告。
- `VP_PRODUCT_CATALOG`：从 `catalog.example.json` 配置真实 SKU。`prices` 为币种到允许分值列表，例如一个实际 CNY 价格以整数“分”录入；示例不含实际定价。须保留曾出售的合法价格以便历史订单恢复。
- `VP_IAP_ISSUER_ID`、`VP_IAP_KEY_ID`、`VP_IAP_PRIVATE_KEY_FILE`
- `VP_IAP_ROOT_CA_FILE`：华为官方 Huawei CBG Root CA G2 证书。
- `VP_IAP_ENVIRONMENT`：严格 `SANDBOX` / `NORMAL`。
- `VP_IAP_VERIFICATION_FILE`：对应 app、key、root、environment、catalog 的真实查询/验签探针报告。
- 云档另需 `VP_OBS_ENDPOINT`（区域 HTTPS OBS endpoint）、`VP_OBS_BUCKET`、`VP_OBS_CREDENTIAL_FILE`、`VP_OBS_VERIFICATION_FILE`。凭据文件字段 `accessKeyId`、`secretAccessKey`，可选 `securityToken`；只允许部署安全环境存放，不进入聊天或客户端。
- 可选 `PORT`，默认 8080。

`node src/main.mjs` 启动。无配置可以启动闭锁 API，`GET /v1/capabilities` 真实返回不可用；配置/报告矛盾直接拒绝启动。测试里的 MemoryStore 和 fake provider 不被生产入口导入。

### CloudDB 对象类型与权限

在授权的 AGC zone 创建对象类型 `ValueAddedRecord`：

| 字段 | 类型 | 约束 |
| --- | --- | --- |
| key | String | 主键，非空，最长 200 |
| scope | String | 非空，按 scope 建普通索引 |
| json | Text | 非空，服务端限制为 512 KiB |

所有客户端角色禁止读写，仅后端 Server SDK 使用此 zone。不要因为客户端集成 AGC 而开放该表。

停机、首次配置时预建唯一哨兵行：key=`internal:transaction-lock`，scope=`internal`，json=`{"revision":0}`。仅首次创建，不可启动时 upsert，不可在上线后覆盖/重置。

SDK 事务只验证读到的对象版本，空查询不能保护“首次插入”。本版每次事务均读取并修改预建哨兵，串行化首次订单绑定和首次配置写；业务写入延迟到所有读取之后，回调允许 SDK 重跑。哨兵是首版吞吐瓶颈，扩容前需重新设计分片锁并重复并发证明，不能直接移除。

### 实云事务上线门禁（本轮未执行）

先停掉应用服务，在获准的预上线 zone 执行。脚本会写少量 `probe:` 测试行，不删除现有数据，也不会创建云服务。

1. 配置上述 AGC 环境、报告路径，明确设置 `VP_PROBE_ALLOW_WRITES=I_APPROVE_TEST_RECORDS`。
2. `npm run probe:clouddb`：用首轮双读 barrier 确认同 base 真正并发；仅一份 head 成功；同单双请求只一份 grant；owner 不能覆盖；callback abort 无写。
3. 同轮还构造一个服务器不认识的测试对象类型，确保事务实际发往 CloudDB 后拒绝；验证同事务合法写与哨兵均未部分提交。这与“回调内 throw”分开记录，不能相互替代。SDK 内部 transport 变化会使该探针失败，禁止跳过。
4. 在线检查只产出 `.pending`，不会启用购买。按运行环境方式暂时阻断 AGC 网络，再执行 `npm run probe:clouddb -- --expect-disconnected`，确认查询及事务失败闭锁，才产出最终报告。
5. 恢复网络，启动服务；服务再次确认相同 fingerprint 和哨兵存在。任何探针失败都保持购买/配置写关闭。

本机 MemoryStore 并发测试不是上述实云证明。

### IAP 验单与沙盒门禁（本轮未执行）

官方中国站实时查单：`https://iap.cloud.huawei.com/order/harmony/v1/application/order/status/query`。JWT 的 digest 来自发送的同一 JSON 字符串；header/payload 按华为 ES256 规范签名。

客户端 PurchaseData JWS 和最新查询 JWS 都必须验证：固定外置根证书、精确 3 张 x5c、证书时效、OpenSSL 3 PKIX 路径/关键扩展/用途、leaf 非关键官方 OID、leaf P-256 digitalSignature、JWS ES256。应用、环境、SKU、订单/token、数量、价格/币种、服务端 owner binding 全部复查。服务器不调用 shipped/confirm；仅客户端 finish。

有授权沙盒测试订单后，以该 `PurchaseData` 原始字符串写入受保护临时文件，并配置 `VP_IAP_PROBE_RECEIPT_FILE`；执行 `node scripts/probe-iap.mjs`。脚本只查单/验签，不付费、不授权益、不 finish，不把 receipt 写入报告或日志。沙盒报告不能用于 NORMAL 环境，不能手造报告绕过门禁。

本轮未完成 NORMAL 发布验收；官方账户/商品审核、付款/退款/重装/换设备与网络中断真机矩阵完成前不得开启正式销售。自动续订后端仍未实现，不能把 type=2 塞进目录。

## 云存档协议与上线门禁

- 云端只收到显式存档位 UUID/名称、文件相对路径/大小/SHA256/偏移和连续分块清单。安装路径、appId、本机 selectedPath 不上传。绑定保存在设备私有目录，并以当前内部账号隔离；另一设备在已可靠识别存档的非 Steam 游戏设置中核对对应云存档位；不要求浏览或手输文件目录。
- 单快照最多 256 MiB、1024 文件、单文件 64 MiB；以无压缩连续字节打包为最多 64 个 4 MiB 块，没有 zip 解压路径或解压炸弹。
- 首版每账号 1 GiB（已发布历史 + 上传预留）、16 个存档位、每位 3 个版本、最多 2 个待处理上传。达到历史上限时要求显式删除不需要的云版本，绝不默默淘汰用户存档。空存档位当前不删除，16 位上限需提前规划。
- CloudDB 同一事务预留容量、取得上传 lease、发布版本并比较 slot revision。跨实例最多 4 个活动块/每用户最多 2 个；进程额外最多 2 个接收请求。请求头不合法或超过 4 MiB 在读取 body 前拒绝，流读取仍按实际字节硬限量。
- 当前采用**有界后端代理上传**。服务端验证块精确长度和 SHA256 后，将同一份已验证 bytes 上传到从未给客户端写权限的私有 key，并重新 GET 校验实际存储内容。重复块同 key、同 bytes，不增加对象或权益；不把 ETag/客户端 metadata 当 SHA256。
- 所有块确认持久后，CloudDB 原子 CAS 发布清单；同 base 冲突保留本地快照和云端胜者。页面要求检查/重新选择版本，不做最后写入覆盖。
- 下载只给 owner 的已发布版本签短期 GET（60 秒），每块即时刷新 URL。账号/会员在签发时复核；已签发 URL 在剩余有效期内无法即时撤销。下载后重新验证块和文件 SHA256，生成本地快照，用户再按原有备份/journal 流程明确恢复。
- 取消分为 receiving / writing / stored。只有尚未发起 PUT 的过期 receiving lease 可以安全清理；写入已开始但结果不明（包括进程中断）标记 writing/uncertain，**不以 TTL 假定写入永远不会迟到，不释放配额、不自动删对象**。显示需要管理员核对；这是保守限制，不是后台自动恢复承诺。后续需在停止相关写入实例并核对 OBS 审计/对象后制定人工收敛操作，本版未提供危险的强制释放按钮。
- 正常取消/冲突可清理仅属于本次 job 的未引用对象；删除已发布版本先取消引用，再删对象，全部确认后才释放容量。失败保留待清理索引，可重试，永不误删并发胜者。用户删除前有明确永久删除确认。
- 本版对象 adapter 不携带 VersionId，因此仅支持从未启用 versioning 的专用私有桶；Enabled 和 Suspended 都拒绝。禁止绕过 gate 把带历史版本的桶接入后声称释放实际容量。

在获准的隔离测试桶/运行环境准备好之后：

1. 核对实际云函数/网关是否允许至少 4 MiB 原始二进制 body（如事件 base64 化，需计算约 4/3 编码开销）、Node >=22、OpenSSL 3、30 秒接收/最多 120 秒端侧等待，以及全链路请求/内存/计费上限。**这项未验证前不能部署直接开放上传**；可以再把协议分块上限调整得更小，不要求购买新套餐。
2. 私有桶、部署身份最小权限只覆盖本服务指定前缀的 Put/Get/Delete（需只读 GetBucketVersioning/GetBucketPublicAccessBlock/GetBucketPublicStatus 权限用于启动检查）。禁止用户直接 List/Put/Delete 桶。凭据由用户在部署安全环境配置。
3. 设置 `VP_PROBE_ALLOW_WRITES=I_APPROVE_TEST_RECORDS`，执行 `npm run probe:obs`。只写约几十字节随机 probe 对象，在真实 `saves/随机合成身份/随机job/0` 命名空间实测匿名读拒绝（不是只测 probe 前缀）、错误 checksum 返回 400、不改原对象、实际读回 SHA256、删除后 404、versioning disabled、四项 BPA 全开启、官方桶公共状态为 false；每次启动还会只读复核 BPA/公共状态/versioning，不用旧报告永久证明权限；报告绑定 bucket/endpoint/身份/SDK/依赖锁，旧报告不解锁新配置。
4. 同时需要已有 CloudDB 实云事务报告和 IAP 对应环境报告。缺任一，`cloudSaves=false`。
5. 真机验证后台断网、服务端超时、跨设备绑定错误、退款/过期、磁盘不足、短写/中断恢复后，才可讨论开放给真实用户。

当前没有实际运行这些云探针，没有创建桶、开通套餐、上传用户存档或部署后端。逐步需要用户提供/操作的清单见 [部署验收清单](DEPLOYMENT_CHECKLIST.md)。

## 隐私、安全与已知限制

- 身份来自换码后的官方 openID，不信客户端头像/昵称；既不保存 provider access/refresh token，也不记录 code/receipt。订单只留 token 摘要，finish token 每次来自最新验单结果。
- 内部会话以随机 token 的摘要落库，30 分钟过期；退出在设备立即清空并尝试服务端撤销。撤销离线失败仍受 TTL 限制。
- 本版没有 IAP 通知/webhook，也没有持久 receipt。权益超过 5 分钟未重新核验即关闭增值使用，页面要求“恢复购买”。type=3 恢复使用 ALL 分页逐单查最新状态，重算整个相关月卡链。退款获知有最多 5 分钟窗口，不能宣称实时撤销。
- 证书未做 CRL/OCSP 联网撤销检测。固定信任根、完整 PKIX 及证书时效不能替代撤销检查；正式上线安全验收需明确处理策略。
- 每账号最多 256 个会员订单事实，每设备类最多 256 个方案，超限拒绝；会员订单达到上限时 intent 提前阻止进入收银台。生产长期运营前需审计式归档策略。
- 单个 profile 256 KiB、512 控件/128 手柄映射。只同步当前 v6 白名单；旧三套 elements、偏好全集、token、图片、路径都不进入同步 payload。
- 云端覆盖前的本地方案备份存于应用私有目录，复制云端为新方案不隐式解决原冲突。
- 本地传输临时目录仅由服务生成；成功导入只保留 files/manifest，不永久保留第二份分块。失败尝试自清，存储异常时可手动“清理本地传输临时文件”，不删除游戏和已发布快照。
- 本地存档仅使用 catalog 已知安装根，有界检查最多 96 个目录、3 层深度和每层 256 项；唯一可信候选才允许开启。拒绝程序、资源包、媒体、符号链接和超限文件。不猜全前缀/AppData、注册表或 Steam Cloud。Steam 管理的游戏和旧 Steam marker 均排除。
- 本地存档要求 Wine 引擎 STOPPED 且 native registry 无 running 项；归属不明一律拒绝，不会 stopAll。
- 双扫描和复制后复核 path/size/mtime/hash；拒绝 symlink、目录穿越、大小写碰撞；最多 1024 文件，单文件 64 MiB，总计 256 MiB。恢复校验快照 hash、先做当前副本，再同文件系统目录交换；保留 `.vp-before-*` 原树，不自动删除用户数据。
- 已移除或暂不可访问的游戏停止自动备份并保留历史；重新导入后核对主程序 SHA256，并由用户重新开启。主程序改变暂停恢复。本地历史总量限 1 GiB，达到上限后暂停新增，不默删旧版本。
- 异常/崩溃留下 journal 时先“处理恢复中断”，缺失当前目录且存在原树则回滚；两者均存在时保留两份。临时/历史副本暂不自动清理，磁盘空间需留足。没有真实设备文件系统/断电测试，不应将主机 mock 测试当作该保证。
- 后端一次性 attempt/session/probe 记录尚无定时清理；上线前制定 TTL 清理/配额运维，不能以开放客户端权限清理。

## 验证命令

- `TOOL_HOME=<官方工具目录> MODEL_TEST_DIR=<隔离输出目录> bash scripts/run_model_unit_tests.sh`
- `cd server/value-added && npm ci --ignore-scripts && npm run test:sdk`（真实 SDK 对本地 HTTPS mock，非实云）
- `cd server/value-added && npm test`（串行运行，包含运行时生成且销毁的合成证书，需 OpenSSL 3）
- `git diff --check`

模型/服务测试在主机执行，不能代替真机与实云。当前自动同步 18 项场景和后端 28 项测试通过，完整 ArkTS assembleApp 通过；开发签名 HAP 已验签并保数据安装。发行测试包复用已验证 1.4.7 原有运行时字节，仅替换本轮编译的 ArkTS 和应用桥接库，并生成逐文件 SHA256 记录。最终安装、UI/本地存档与源码提交对应关系见随包验证记录。未部署后端、未支付、未验证真实账号或云同步。

## 官方依据

- [Account 授权码与 profile](https://developer.huawei.com/consumer/cn/doc/HarmonyOS-Guides/account-get-avatar-nickname)
- [Account 换码](https://developer.huawei.com/consumer/cn/doc/harmonyos-references/account-api-obtain-user-token)
- [Account getInfo](https://developer.huawei.com/consumer/cn/doc/harmonyos-references/account-api-get-user-info-get-nickname-and-avatar)
- [IAP 应用身份配置](https://developer.huawei.com/consumer/cn/doc/harmonyos-guides-v5/iap-config-app-identity-info-V5)
- [IAP 最新订单](https://developer.huawei.com/consumer/cn/doc/harmonyos-references/iap-query-order-status)
- [IAP JWT](https://developer.huawei.com/consumer/cn/doc/harmonyos-references/iap-jwt-description)
- [IAP 验签与根证书](https://developer.huawei.com/consumer/cn/doc/harmonyos-references/iap-verifying-signature)
- [IAP 数据类型](https://developer.huawei.com/consumer/cn/doc/harmonyos-references/iap-data-model)
- [CloudDB 官方 Node SDK](https://www.npmjs.com/package/@agconnect/database-server)
- [File.path realpath 实现](https://github.com/openharmony/filemanagement_file_api/blob/master/interfaces/kits/js/src/mod_fs/class_file/file_n_exporter.cpp)

- [OBS 预签 URL 官方接口](https://support.huaweicloud.com/sdk-nodejs-devg-obs/obs_29_0701.html)
- [OBS 内容 SHA256 校验](https://support.huaweicloud.com/sdk-nodejs-devg-obs/obs_29_1713.html)

- [OBS BPA 配置读取](https://support.huaweicloud.com/intl/en-us/api-obs/obs_04_0175.html)
- [OBS 桶公共状态与权限](https://support.huaweicloud.com/api-obs/obs_04_0111.html)
