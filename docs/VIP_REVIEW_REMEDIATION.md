# VIP 审核整改与开放检查

实施位置：WSL `/home/liufeng/src/vpp-account-cloud`，`feature/account-cloud-membership-mvp`。两套皮肤使用同一个 `AccountSyncSettings`，未改动旧 Windows checkout。新增非续期 SKU `vip_monthly_once`（type 3，CNY 500 分），保留 `vip_monthly_auto`（type 2）和旧订单日历月算法。

## 已实现的用户流程

“系统设置 → 账号与同步 → 开通 VIP”进入购买区；初始不选择商品、不勾选协议。连续包月与单月会员均显示华为报价，前者显示优惠期数、优惠后价格、月周期、扣款和提醒说明。支付按钮带本次金额；价格查询失败提供重试。选择其他商品、换号、刷新报价或报价变化会清除确认；支付前再次查询华为商品并核对账号、报价快照和协议版本。有效会员及尚待核验的本期权益禁止新购。

会员协议和自动续费授权分开勾选；点击协议仅阅读正文。三个协议随应用编译打包，在原生页面中离线阅读，不依赖外部网页。会员权益仅描述应用配置、输入方案备份恢复，游戏存档不上传。

“管理订阅／取消自动续费”和客服在账号区始终显示，不依赖云同步/新购功能开放；无需应用内登录即可打开官方管理弹窗。弹窗使用配置中的订阅组 ID。完整退订路径：**系统设置 → 账号与同步 → 管理订阅／取消自动续费 → 华为订阅详情 → 取消订阅 → 确认**。返回后恢复订单并强制查验官方状态；仅关闭弹窗不会取消。核验关闭显示保留权益的到期日，失败显示“状态待确认”。恢复购买和退订不要求购买协议授权。

购买页、会员详情、三个协议、客服页、赞助购买页统一提供邮箱 **liufengpm@live.cn**、**QQ 群 966307250**，均可直接复制，无需登录。

## 提醒部署

服务端根据官方签名状态中的 `renewalInfo.renewalTime`（毫秒）、`renewalPrice`（分）、`currency` 保存计划。不得用到期日或上次成交价补齐缺失计划。续费时间提前 7 天开始发送，考虑华为可能提前 24 小时扣款，正常情况下至少提前 5 日告知；缺失官方计划、延迟、推送失败均输出固定告警日志并保留重试状态。

使用同一经过审查的云函数代码 ZIP，部署三个函数入口：

| 函数 | 入口 | 触发与访问 |
| --- | --- | --- |
| 账号 API | `handler.myHandler` | 保持现有 SDK/HTTP JSON v1 传输；设备登记、撤销和打开回执必须账号认证；不允许执行定时任务 |
| 续费任务 | `handler.renewalHandler` | 独立 IAM 私有函数，**只配置定时触发器**，不要添加 HTTP/客户端触发；建议 `@every 15m`，超时至少 180 秒 |
| Push V2 回执 | `handler.pushReceiptHandler` | 独立 HTTPS 回调地址；强制华为 HMAC 鉴权，不接受账号 token 代替；在 AGC 配置回调用户名和密钥 |

AGC 定时触发器的“事件值”是开发者定义的 JSON 入参，配置为 `{"triggerType":"TIMER","userData":"与受保护的 VP_RENEWAL_TIMER_SECRET 相同的随机值"}`，随机值至少 32 字符。请勿将实际值提交到仓库或日志。启用触发器并检查首个心跳后再开放自动续费。心跳超过一小时，服务端禁止新开自动续费；单月卡、已有订单核验、退订不受此限制。

CloudDB `ValueAddedRecord` 保留原字段、权限和锁记录，新增复合索引 `scopeKeyIndex(scope,key)`。执行测试区的 `scripts/probe-renewal-storage.mjs` 验证 300 行稳定排序分页；需要明确设置 `VP_PROBE_ALLOW_WRITES=I_APPROVE_TEST_RECORDS`、现有测试区凭据和 `VP_RENEWAL_STORAGE_EVIDENCE_FILE`，脚本不会自动写生产区。索引和分页真实验证完成前不要标记 `paginationVerified`。

受保护配置：

- `VP_PUSH_PROJECT_ID`：与应用所属 AGC 项目一致。
- `VP_PUSH_CREDENTIAL_FILE` 或 `VP_PUSH_CREDENTIAL_JSON`：API Console **推送服务账号密钥**，含 `project_id/key_id/sub_account/private_key`；不是 Android OAuth client secret。NEXT 调用 `/v3/{projectId}/messages:send`，PS256 JWT。
- `VP_PUSH_RECEIPT_ID`：AGC V2 回执配置 ID；申请通知自分类的 `SUBSCRIPTION` 权益及消息回执权益。
- `VP_PUSH_CALLBACK_USER`、`VP_PUSH_CALLBACK_SECRET`：与回执配置一致；新密钥按华为文档先 Base64 解码再 HMAC-SHA256。回调验证五分钟时间窗口、nonce、应用包名及发送 requestId。
- `VP_RENEWAL_TIMER_SECRET`：仅定时函数及其触发器持有。
- `VP_RENEWAL_VERIFICATION_FILE`：经实测记录；`appId/projectId/testedAt` 及 `timerActive/deviceDisplayed/deepLinkOpened/subscriptionCategoryEligible/receiptVerified/paginationVerified` 为真实结果。后述完整发布检查还核对提前时间、退订停发、权限关闭、去重和告警证据。
- `VP_MEMBERSHIP_RELEASE_FILE`：经实测记录，含与商店一致的 `operatorName/appId/environment:NORMAL/oneTimeProductId` 和 `normalPaymentVerified/backupAccepted/supportVerified/cancelInAppVerified/agreementsReadVerified/testedAt`。当前没有这些证据。
- 现有 IAP、账号、CloudDB、PKIX 和对象存储受保护配置仍需保留；勿把密钥打入 ZIP。仅沙盒联调可显式设置 `VP_MEMBERSHIP_SANDBOX_CHECKOUT=enabled` 和 `VP_RENEWAL_SANDBOX_TESTING=enabled`，先部署定时器和回执，再使用真实设备取得提醒验收记录；这些开关只在 `SANDBOX` 官方测试账号环境生效，不能解锁正式备份能力。不要在 NORMAL 环境配置这些开关，沙盒验收不能标记 NORMAL 支付通过。

云函数打包脚本支持 `--membership-release-evidence`、`--renewal-evidence`，将完成的公开验收记录放到 ZIP 的 `config/membership-release.json`、`config/renewal-verification.json`；对应设置上述文件环境变量。现有 `--clouddb-evidence/--iap-evidence/--pkix-runtime` 参数继续保留。无验收记录时可打包整改代码进行沙盒联调，正式新购继续关闭。

设备登记按账号认证，token 单一归属，安装 ID 仅用于设备绑定；换号/退出撤销旧绑定，token 更新使旧 token 退役。华为原生 profileId 绑定进一步隔离账号。应用进程退出后定时任务仍使用持久化计划；发送前重新验证官方签名状态，关闭/撤销不再发。每个订阅续费日期一个周期，每台设备一个发送记录；并发租约和跨进程记录防止重复，已发送的订阅仍每六小时核验以发现下一周期。失败十五分钟后重试。

`acceptedAt/providerRequestId` 仅表示 Push 服务受理；V2 回执保存 `deviceReportedAt/providerDeliveryResult/deliveredAt`，`displayStatus` 在没有用户打开证据时仍为 `UNKNOWN`。点击通知保存 `openedAt/OPENED`；实际屏幕展示必须另外留截图/录像，不能以 HTTP 200 或送达回执替代。瞬时设备错误会安排重试；token 无效、通知关闭、长期不活跃会停发并告警。

在云监控中为 `[RenewalReminders] delivery failed`、`device delivery failed`、`late renewal notice`、`timer failed` 建立告警，并监测 `renewal-timer-checkpoint.lastRunAt` 超过一小时。任务的 `attempts/retryAt/lastFailureAt` 保留失败状态。定时器心跳不是全部任务按时完成的证明，需监控积压及最早待发时间；按用户规模压测和调整触发频率/执行预算。

## 提审前的验收与记录

已按开发者提供的上架登记信息，将 `entry/src/main/resources/rawfile/value_added_config.json` 中运营主体填写为 **刘峰**；应用内协议读取此配置。后续真实验收产生的服务端发布记录中 `operatorName` 须与此一致。NORMAL 支付、自动备份和续费提醒等验收证据尚未齐备，正式新购继续关闭；已有订单恢复、订阅管理、协议和客服仍可用。

运行 `node server/value-added/scripts/check-membership-release.mjs <NORMAL发布记录.json> <提醒验收记录.json> <CloudDB分页记录.json>`。缺失证据返回非零；不得用全 true 的模板代替验收。正式候选包记录版本、SHA256、签名/包名、服务端版本、SKU 配置和逐项截图/录像：

1. 两套皮肤分别验证主动选商品；未勾选不能支付；读协议不勾选；换商品、换账号及价格变化重置确认；首期优惠转标准价说明；价格失败和重试。
2. NORMAL 环境购买单月卡及自动包月，核对实际金额、周期、订单核验/重复回执和权益；单月卡到期不扣款；有效会员隐藏新购。验证配置/输入方案自动备份、重装恢复；现有备份链路未完成验收时不开放承诺服务。
3. 应用内退订，无需离开应用；取消保留本期；仅关闭弹窗不显示取消成功；后端查询失败显示待确认。关闭新购、云同步故障时仍可进入管理；恢复购买不要求协议勾选。
4. 退出应用后的真实通知展示；时区、提前7天与可能提前24小时扣款的计算；金额/币种/期限；点击冷启动和热启动均进入账号与同步的会员详情。取消停发、跨周期与多设备去重、token轮换/换号隔离、失败重试及监控告警。关闭通知后禁止新开自动续费，单月卡仍可选，已有会员提示恢复提醒。
5. 未登录复制邮箱/群号；购买区、会员详情、协议、客服页和赞助页均可访问。

## 三项审核问题的提交说明草稿

以下为代码对应的复测路径。**取得上述正式候选包证据后再提交，不将本文件作为已经验收的证明。**

- **购买信息与协议**：已增加连续包月与不自动续费的单月卡。购买页显著显示本次金额、优惠期数、标准续费金额、周期、扣款时机、提醒方式及取消路径；会员协议和续费授权分别主动勾选，默认不选。复测：系统设置 → 账号与同步 → 开通 VIP → 分别选择商品、阅读协议并检查支付按钮。
- **便捷退订**：账号区常显“管理订阅／取消自动续费”，应用内华为订阅弹窗直接完成取消；正文提供完整路径，取消后核验并保留本期权益。复测：系统设置 → 账号与同步 → 管理订阅／取消自动续费 → 华为订阅详情 → 取消订阅 → 确认 → 返回核验。
- **客服联系方式**：无需登录即可查看、复制邮箱 liufengpm@live.cn 和 QQ 群 966307250；会员购买、详情、协议和赞助页均提供入口。复测：系统设置 → 账号与同步/赞助 → 客服与帮助 → 复制邮箱、复制群号。

## 依据

- [审核指南](https://developer.huawei.com/consumer/cn/doc/doccenter-submission/50104)、[自动续费 FAQ](https://developer.huawei.com/consumer/cn/doc/app/50166)
- [NEXT Push JWT](https://developer.huawei.com/consumer/cn/doc/harmonyos-guides/push-jwt-token)、[v3 请求结构](https://developer.huawei.com/consumer/cn/doc/harmonyos-references/push-scenariozed-api-request-struct)、[请求参数](https://developer.huawei.com/consumer/cn/doc/harmonyos-references/push-scenariozed-api-request-param)
- [消息回执](https://developer.huawei.com/consumer/cn/doc/harmonyos-guides/push-msg-receipt)、[V2 回执接口](https://developer.huawei.com/consumer/cn/doc/harmonyos-references/push-api-msg-receipt)
- [AGC 定时触发器](https://developer.huawei.com/consumer/cn/doc/AppGallery-connect-Guides/agc-cloudfunction-crontrigger-0000001658717864)
