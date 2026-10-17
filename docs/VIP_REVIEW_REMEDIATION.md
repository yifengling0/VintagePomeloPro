# VIP 购买整改与部署验收

实施分支：`feature/account-cloud-membership-mvp`，权威目录 `/home/liufeng/src/vpp-account-cloud`。两套皮肤共用 `AccountSyncSettings`。

## 当前购买流程

仅销售连续包月 `vip_monthly_auto`（type 2）。单月卡 `vip_monthly_once`（type 3）已停售，客户端不查询、不展示该商品，服务端拒绝新的购买 intent；历史回执核验和恢复仍保留，沿用日历月算法。无需为停售商品新建商店 SKU；以后启售时才配置独立商品并完成相应正式支付验收。

系统设置 → 账号与同步 → 开通 VIP：先展示权益和华为实时价格，点击开通后弹出原生居中确认页。当前优惠为前 2 个月每月 ¥2，之后 ¥5/月，以华为当次报价及用户优惠资格为准。确认页显示本次金额、优惠期数、标准续费价、按月周期、扣款时机、提醒和取消入口。

会员、自动续费、取消说明合并在一个离线阅读页面，确认弹窗也能直接展开全文。会员协议、自动续费授权两项独立勾选，初始不选，阅读正文不会授权。换商品、换账号、重新打开确认或报价变化均清空确认。支付前重新查询商品，核对报价快照及账号。有效会员隐藏新购，防止重复收费。预检失败和购买异常均有明显原生弹窗；只有确定尚未调用支付的预检失败才提示“未发起支付，没有扣款”。待核验订单提示刷新或恢复，避免重复付款。

“管理订阅／取消自动续费”、恢复购买、客服入口不依赖新购或云备份是否开放。管理使用 `iap.showManagedSubscriptions` 的应用内弹窗及会员订阅组 ID。完整路径：**系统设置 → 账号与同步 → 管理订阅／取消自动续费 → 华为订阅详情 → 取消订阅 → 确认**。返回后直接强制查询官方状态，不由历史回执恢复阻断；关闭弹窗不能当作退订成功。取消保留本期权益，查询失败显示状态待确认。恢复购买独立处理历史订单，之后核验当前订阅；部分历史恢复失败会明确提示，不能伪装成全部成功。异步结果校验账号和页面生命周期，管理和恢复均不要求购买协议授权。操作结果在会员详情收起时也保持可见；已到期订阅不显示保留本期权益或下一期扣款计划。

运营主体：**刘峰**。客服邮箱 **liufengpm@live.cn**、QQ 群 **966307250**；购买、详情、协议、赞助及客服页均可查看和复制，无需登录。仅承诺已交付的应用配置、输入方案备份恢复；游戏存档不上传。

## 沙盒和正式开放

`VP_MEMBERSHIP_SANDBOX_CHECKOUT=enabled` 仅在官方 provider 的 `SANDBOX` 环境就绪时开放独立的沙盒收银台联调，返回 `sandboxCommerce` 和 `sandboxCheckout`。客户端组件、服务层、服务端 intent 均区分此路径；客户端仍须通过华为 `iap.isSandboxActivated()` 检测。界面明确显示沙盒不会真实扣款，提醒仍待接通。测试开关不会将 `renewalReminders` 标为 true，不能开放 NORMAL 购买或替代正式验收。

正式购买仍需 NORMAL 支付、在售 SKU、实际备份、协议、客服、退订和提醒验收证据。`membershipProductId` 必须与在售自动续费商品匹配；只有单月卡重新启售时才要求 `oneTimeProductId`。不要用沙盒结果填写 NORMAL 证据。

发布检查：

```sh
node server/value-added/scripts/check-membership-release.mjs NORMAL.json renewal.json storage.json
```

缺少记录或记录不一致时检查失败。`NORMAL.json` 包含 `operatorName/appId/environment:NORMAL/membershipProductId/testedAt` 及 `normalPaymentVerified/backupAccepted/supportVerified/cancelInAppVerified/agreementsReadVerified`；可选 `oneTimeProductId` 仅供启售单月卡使用。

## 续费提醒实现

使用官方签名状态中的 `renewalInfo.renewalTime`、`renewalPrice` 和 `currency`，不得从本期到期日或成交价编造下一期计划。提前 7 天安排发送，为可能提前 24 小时扣款及重试留出时间。发送前再次官方核验；取消、撤销、退款后停发。消息包括名称、金额、预计时间、延长一个月及管理/取消入口。

华为通知类别为 **ACCOUNT（账号动态）**。会员续费不属于 SUBSCRIPTION 内容订阅。正式开放要求 `notificationCategory:ACCOUNT`、`accountCategoryEligible:true` 及真实验证记录。2026-10-17 已提交此权益申请，当前审核中；不能把申请提交当作批准。

按订阅周期和设备持久化去重，发送租约防并发重复，失败 15 分钟后重试；已发送订阅仍定期核验下一周期。设备 token 只属于一个账号，换号、退出、权限关闭和 token 更新使旧绑定退役。Push 受理、设备回执、用户打开分别记录，均不能代替屏幕展示截图。

部署入口：

| 功能 | 入口 | 限制 |
| --- | --- | --- |
| 账号与购买 API | `handler.myHandler` | 保留当前 SDK JSON v1 调用；设备登记、撤销及打开回执均须账号认证 |
| 私有续费任务 | `handler.renewalHandler` | 仅定时触发器，无 HTTP 或客户端入口，超时 180 秒 |
| 消息回执 | AGC `handler.pushReceiptHandler`；受支持的公网网关可用 FunctionGraph `handler.handler` | 公网 HTTPS + 华为 HMAC，不能要求账号 token 或 AGC API 客户端令牌 |

**AGC 公网回执入口限制（2026-10-17 实查）**：现项目仅有事件调用函数，HTTP 触发器的认证类型只有 API 客户端鉴权（Client/Server），没有免认证。官方已公告自 **2025-11-04** 起停止新建旧版 HTTP 请求函数，并停止新建 API 网关触发器；旧文档中“免认证/Custom Runtime”的步骤不表示当前账号还能新建。账号 API、购买验证和定时发送继续部署在 AGC；Push 无法自行获得 AGC 客户端 OAuth 令牌，不能直接将该鉴权地址登记为回执 URL，也不能把令牌放进回执地址。

本轮交付采用**云函数代码包**，没有独立服务器或自托管 Docker 部署。`src/push-receipts.mjs` 仅加载 CloudDB、HMAC 和包名，不加载 IAP 私钥。保留 AGC 单参数 callback 入口，供平台支持合适公网入口时接入。另提供独立 FunctionGraph + HTTPS APIG 的适配包，入口 `handler.handler`，使用标准 `callback(error,result)`。该入口仅允许 **POST /push/receipts**，处理网关 Base64 请求体、限制正文 64 KiB、统一请求头并校验 HMAC/时窗/重放；初始化和存储故障返回 503，避免错误地确认已保存回执。

备用云函数打包（不包含支付实现、商品目录、IAP 验证材料或 PKIX 可执行文件；不含任何凭据）：

```sh
python3 server/value-added/scripts/package-cloud-function.py --target functiongraph-receipts --clouddb-evidence /private/clouddb-verification.json --output /artifacts/vp-push-receipts-functiongraph.zip
```

部署前必须有可接收 Push HMAC 的公网 HTTPS 入口。当前 FunctionGraph 控制台仅提供 APIG 专享版入口，需要另行配置网关；本轮未开通或购买网关，也未登记不可用回执 URL。此备用包只是可部署实现，**未完成公网部署验收**，不能据此宣称纯 AGC 回执闭环已接通。

接收端受保护配置：`VP_AGC_CREDENTIAL_JSON` 或文件、`VP_AGC_ZONE`、`VP_CLOUDDB_VERIFICATION_FILE`、`VP_PUSH_CALLBACK_USER`、`VP_PUSH_CALLBACK_SECRET`、`VP_PACKAGE_NAME`。回调密钥为标准 Base64，解码后至少 16 字节，与控制台保持一致。接收端部署且华为回执测试通过后，将回执 ID 填入 API 和定时任务配置。

发送端另需 `VP_PUSH_PROJECT_ID`、`VP_PUSH_CREDENTIAL_JSON` 或文件（项目级 Push 服务账号，PS256 JWT）、`VP_PUSH_RECEIPT_ID`。私有定时器另需至少 32 字符的 `VP_RENEWAL_TIMER_SECRET`。事件值为 `{"triggerType":"TIMER","userData":"与受保护的定时密钥相同"}`，日程 `@every 15m`。密钥不写入仓库、ZIP、日志或验收文档。

仅沙盒提醒联调可设置 `VP_RENEWAL_SANDBOX_TESTING=enabled`；仍需真实凭据、回执 ID 和真实定时运行。正式 sender 另要求 `VP_RENEWAL_VERIFICATION_FILE`，含 `appId/projectId/testedAt/notificationCategory:ACCOUNT` 及 `timerActive/deviceDisplayed/deepLinkOpened/accountCategoryEligible/receiptVerified/paginationVerified/advanceNoticeVerified/cancelStopsVerified/permissionClosedVerified/dedupVerified/retryAlarmVerified` 的真实记录。

CloudDB 保留原对象字段与权限，查询使用 `scopeKeyIndex(scope,key)` 的排序分页。`scripts/probe-renewal-storage.mjs` 仅允许 `PomeloAccountTest`，须显式设置 `VP_PROBE_ALLOW_WRITES=I_APPROVE_TEST_RECORDS` 和证据输出路径；实测 300 行后在 finally 清理隔离记录，不放宽生产删除约束。SDK 初始化失败后可在同一云进程重试，但不同凭据不能复用已有客户端。

监控固定告警 `delivery failed`、`device delivery failed`、`late renewal notice`、`timer failed`，并监测 `renewal-timer-checkpoint.lastRunAt` 超过一小时。心跳只表明任务运行，仍需监控积压、最早待发时间及失败重试。不要手写心跳记录。

## 当前验收边界

- 已通过：后端 77 项测试、客户端服务与购买逻辑测试、ArkTS 编译、调试签名校验；真实 CloudDB 300 行分页和清理；华为官方沙盒检测、最新订单查询及验签；云端沙盒 capability 调用；rc8 平板弹出官方沙盒收银台并完成订阅，初次显示至 2026-11-17、下期 CNY 2，之后加速续期至 2027-02-17、标准价 CNY 5。已在应用内华为弹窗实际取消，官方显示“取消成功”和“2027/2/17 到期后不再扣费”。随后沙盒官方状态已到期，不能用显示的日历日期推断仍有权益。rc9 已验证恢复购买完成，无需重新授权或再次付款；最终 rc11 已安装，管理返回后直接核验并显示“自动续费已关闭，会员已到期”，会员详情收起时结果仍可见。
- 已准备：Push 项目专用密钥、ACCOUNT 权益申请、`vp-renewal-timer` 私有函数及最小配置的回执云函数部署包。
- 未完成：公网回执入口、回执 ID、定时器真实心跳及告警、应用退出后设备通知展示/跳转、真实提醒闭环和 NORMAL 支付验收。定时函数未配置触发器，避免未就绪任务空跑；正式新购保持关闭。

改变商品目录、证书链、支付环境或验证器后须重新执行 `probe-iap.mjs`，旧验签指纹不能直接沿用。部署前以真实受保护配置加载最终 ZIP，验证 capabilities；部署后重复调用并在设备验证。保留原云端配置和代码导出供回滚，禁止用本地过期配置整体覆盖线上环境变量。

最终 rc11 在详情收起状态点击“恢复购买”，实测返回“恢复购买核验完成”，未要求购买协议授权，也未打开收银台。

## 提审证据

按购买信息与协议、应用内退订、客服三项分别整理截图及复测路径。额外覆盖未勾选、报价变化、取消保留本期、关闭管理弹窗未取消、查询失败、重复回执、账号切换、关闭通知和恢复购买。提醒验收需包含应用退出后的实际展示、提前时间、取消停发、跨周期去重、失败重试/告警和通知跳转；沙盒结果不替代 NORMAL 支付或正式自动备份验收。

## 依据

- [审核指南](https://developer.huawei.com/consumer/cn/doc/doccenter-submission/50104)、[自动续费 FAQ](https://developer.huawei.com/consumer/cn/doc/app/50166)
- [NEXT Push JWT](https://developer.huawei.com/consumer/cn/doc/harmonyos-guides/push-jwt-token)、[v3 请求结构](https://developer.huawei.com/consumer/cn/doc/harmonyos-references/push-scenariozed-api-request-struct)
- [消息回执](https://developer.huawei.com/consumer/cn/doc/harmonyos-guides/push-msg-receipt)、[V2 回执](https://developer.huawei.com/consumer/cn/doc/harmonyos-references/push-api-msg-receipt)
- [AGC 停止新建旧版 HTTP 请求函数公告](https://developer.huawei.com/consumer/cn/doc/AppGallery-connect-Guides/develop-func-introduction-0000001512391180)、[停止新建 API 网关触发器公告](https://developer.huawei.com/consumer/cn/doc/AppGallery-connect-Guides/agc-cloudfunction-apigw-trigger-0000001706957933)
- [FunctionGraph APIG 触发器](https://support.huaweicloud.com/usermanual-functiongraph/functiongraph_01_0204.html)
- [AGC 定时触发器](https://developer.huawei.com/consumer/cn/doc/AppGallery-connect-Guides/agc-cloudfunction-crontrigger-0000001658717864)
