# 华为托管账号与同步服务

2026-10-19 云备份候选版本已收窄为用户设置与输入方案，单份 256 KiB，拒绝游戏库及资源文件；需客户端/账号函数配套发布，尚未上线。见 [范围及迁移说明](CLOUD_USER_SETTINGS_20261019.md)。

2026-10-19 更新：客户端账号同步与通知登记已降频。目标日程为每天北京时间 16:05；两个版本的自动调度均未通过本轮验收，日历表达式仍返回 101212，定时任务保持停用，NORMAL 未开放。见 [降频与调度复核](ACCOUNT_SYNC_FREQUENCY_20261019.md)；实机 Push 证据见 [10 月 17 日记录](RENEWAL_PUSH_VALIDATION_20261017.md)。

不需要自建服务器。云函数执行授权码换会话和会员核验，CloudDB 保存账号与同步记录。存档文件另需私有对象存储及实际传输验证。

## 实际测试资源

| 配置 | 实际值 |
| --- | --- |
| 应用 / 包名 | 旧柚Pro / `com.vintage.pomelopro` |
| App ID / OAuth Client ID | `6917605913989898754`（两个官方字段分别核对） |
| 项目 / 区域 | 旧柚 / `101653523864150887` / CN |
| 云函数 / 实测运行时 | `vp-account-sync` / `$latest` / Node 22.23.2 |
| 测试 CloudDB zone | `PomeloAccountTest` |

官方开发签名平板已完成系统登录、头像昵称授权、云端换码、注销和再次登录。内部会话仅在设备内存保留30分钟；不能用客户端 OpenID 直接访问云数据。

## 云函数协议

入口 `handler.myHandler` 使用单参数 `callback(result)`，不返回 Promise。SDK HTTP 触发器将数据放在 `event.body`。适配器同时接受直接协议和有界 body 包装，外层headers不提供业务身份。

SDK发送 `protocol/method/path/authorization/body`，响应为 `protocol/status/body`，其中body为JSON字符串。事件限768 KiB、业务JSON限512 KiB、响应限1 MiB。旧PC不支持SDK时需配置已验证的HTTPS fallback，目前未配置。失败的修改请求不换通道重发。

安装锁定依赖后运行 `server/value-added/scripts/package-cloud-function.py --clouddb-evidence <报告> --pkix-runtime <暂存运行包> --output <ZIP>`。ZIP含源码、依赖、公开商品目录和报告，不含凭据。真实IAP探针完成后才能加 `--iap-evidence`，不可手造报告。

## 保护环境配置

- `VP_AGC_CREDENTIAL_JSON`：旧AGC“项目设置 → Server SDK → 认证凭据”的项目客户端JSON；新后台服务账号公私钥不能替代。
- `VP_APP_ID`、`VP_PACKAGE_NAME`、`VP_ACCOUNT_CLIENT_ID`：核对的公开应用身份。
- `VP_ACCOUNT_CLIENT_SECRET`：对应Account OAuth Client的Secret；agconnect-services中另一个SDK Client的Secret不能替代。
- `VP_AGC_ZONE=PomeloAccountTest`。
- `VP_CLOUDDB_VERIFICATION_FILE=./config/clouddb-verification.json`，相对服务包根目录解析。
- `VP_IDENTITY_HMAC_SECRET`、`VP_PURCHASE_HMAC_SECRET`：各自至少32字符、高熵、部署时复用。

`PROJECT_CREDENTIAL`是平台保留名，手动添加会导致整次保存失败；使用 `VP_AGC_CREDENTIAL_JSON`。保存后刷新核对真实持久化，再用设备请求验证。秘密不进Git、客户端、截图或普通产物。

## CloudDB权限和真实证据

ValueAddedRecord包含key:String主键非空、scope:String及scopeIndex、json:Text。World/Authenticated/Creator全部无权限；Administrator具有Read/Upsert/Delete，官方Server SDK使用该角色。

停机首次预建一次 `internal:transaction-lock`，内容 `{"revision":0}`；以后不能重置。2026-10-13在线并发、重复订单、回调中断、服务端拒绝回滚、所有者不可变和Docker `--network none`断网闭锁探针全部通过。项目/区域/zone/SDK/依赖锁改变后重做。

## 会员和云存档

会员 `vip_monthly_auto`，type=2，¥5/月自动续费；组ID `055EAC5A781A430B807FEE6DFDC5D173`。 已保存的 `vip_2026_year_end` 为前2期每期¥2、之后¥5；页面读取官方期数和价格，本轮不修改活动规则。签名桥接、优惠catalog及真实沙盒验单报告已部署，详情见 `EXISTING_MEMBER_OFFER_20261013.md`。期限只取官方签名状态；取消续费保留本期，扣费重试不虚构宽限期，撤销不授权。旧type=3月卡规则保留。

云函数没有OpenSSL CLI。`package-pkix-runtime.py`暂存锁定的Alpine OpenSSL3.5.9及musl loader，启动逐文件校验SHA256，保留完整PKIX验链。包内 `config/iap-verification.json` 为真实沙盒查询/验签报告，未显式配置 `VP_IAP_VERIFICATION_FILE` 时读取包内报告，仍验证 fingerprint。沙盒首次取证可开放官方测试收银台，但不授予会员同步；只有匹配的真实报告才开放 profiles/preferences。SANDBOX 报告不能开放 NORMAL。

当前SDK JSON通道拒绝4 MiB二进制存档块，`cloudSaves=false`。必须验证私有存储及实际传输后再开放；账号或CloudDB成功不能代表云存档完成。
