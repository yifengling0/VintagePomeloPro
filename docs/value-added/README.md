# 账号、会员与自动同步

分支 `feature/account-cloud-membership-mvp`，版本 `1.4.7 / 1004070`。系统设置 → 账号与同步显示账号、自动备份、存档和会员；咖啡位于独立“赞助”，默认收起。两套皮肤共用业务服务，页面沿用系统设置的标题栏与安全区；phone/tv/未知设备隐藏增值入口，直接路由与服务调用同样受设备范围限制。

2026-10-13：官方 ¥2/月沙盒订阅已完成本人验证、服务器验单、最新状态查询与订单确认，真机 VIP 生效。设置及 5 个输入方案自动备份至实际 CloudDB，设备 journal 与云端 revision/内容哈希一致。沙盒会员及设置/方案备份已开放；NORMAL 和云存档尚未验收。详见 [验证记录](VALIDATION_20261013.md) 和 [配置记录](AGC_SETUP.md)。

## 账号与云端

Account Kit 登录校验随机 state，profile 授权返回头像与昵称。基础华为登录独立于云配置；云数据身份必须通过服务端换授权码并读取官方 openID，不能由客户端自行提交 openID 授权。内部会话仅在设备内存持有，服务端存 token 摘要，30 分钟过期；退出立即取消客户端会话并尝试服务端撤销。

主部署为 Cloud Foundation 云函数 `vp-account-sync/$latest`，Node 22，单参数 callback。SDK 协议使用 `protocol/method/path/authorization/body`，业务 JSON 上限 512 KiB、整个事件 768 KiB、响应 1 MiB。应用与服务端都验证格式和账号代次；修改请求失败不切换传输重放。旧 PC 系统缺少 SDK 能力时，需要另行配置已验证 HTTPS fallback，当前未配置。

CloudDB `ValueAddedRecord` 的 key 为主键，scope 为普通索引，json 为 Text。World/Authenticated/Creator 无权限，Administrator 仅用于服务端。首次创建 `internal:transaction-lock`，事务读取并更新该哨兵，保证不存在对象的并发首次插入也有冲突检测；启动及部署不得重置哨兵。实云报告绑定项目、区域、zone、SDK 和完整依赖锁，依赖变更必须重测。

## 连续包月

- 商品 `vip_monthly_auto`，type=2，¥5/月自动续费，组 `055EAC5A781A430B807FEE6DFDC5D173`。
- 沿用后台已保存优惠 `vip_2026_year_end`。页面按官方 SDK 展示 ¥2/月、前 2 个计费周期、之后 ¥5/月自动续费；活动结束不等于所有用户同日恢复标准价。见 [现有优惠适配](EXISTING_MEMBER_OFFER_20261013.md)。
- 服务端验证原始回执 JWS，然后调用官方 subscription/status/query 并验证返回的 group JWS；校验应用、包名、环境、组、代、订阅、token、SKU、P1M 和 CNY500 分；只有官方签名的已知促销 `vip_2026_year_end` 可接受 CNY200 分。
- 到期时间仅采用官方 expiresTime，同一订阅仅保留一个最新快照；重复回执和续订不叠加月份。取消续费保留本期，过期、撤销、扣费重试不虚构有效会员或宽限期。
- 原始绑定回执按账号和订阅作 AAD，经 AES-256-GCM 加密保存在服务端，用于每 5 分钟复查。查询失败不刷新核验时间，旧查询不能覆盖较新状态，旧订单不能复活已撤销订单。
- 续订省略 developerPayload 时，仅相同已绑定所有者/token/代允许用原始保护回执重查；未知订阅不能以持有 token 为由接管。
- 有效 VIP 仅显示权益和管理；取消自动续费保留本期，恢复续订进入官方管理页，服务层再次检查权益以避免重复购买本期。
- 客户端订阅购买省略不适用于 type=2 的 quantity 字段。恢复查询 UNFINISHED + CURRENT_ENTITLEMENT；官方管理订阅页提供取消入口。只有服务器核验并保存权益后才调用 SDK finish。

旧 type=3 的 `calendar-month-v1` 不变：UTC 按日历月计算、月末钳位，多笔购买顺延，退款重新计算。旧商品 `vip_member` 没有改成自动续费，不改变已经出售的期限语义。

咖啡 `buy_coffe` 为独立消耗型商品，单价 ¥8，购买数量 1/4/10，对应小杯/中杯/大杯 ¥8/¥32/¥80；不授予会员。真实商品查询及原未完成订单核销通过；展开赞助时自动恢复旧订单，购买前再次检查，退款仍待验收。详见 [账号与咖啡](ACCOUNT_AND_COFFEE.md)。

## 自动同步

登录、返回前台、本地修改自动触发串行同步；修改合并 700 ms，前台每 30 秒检查云端，网络失败最多 5 分钟退避。设置与输入方案开关按账号保存，不要求逐项上传。页面显示总体状态、每项状态和最后成功时间，未开放、等待、冲突或部分失败不显示全部成功。

设置白名单包含外观、卡片布局、默认输入方案 ID；输入方案为 v6 完整按键、鼠标参数和手柄映射。tablet/desktop 分开，路径、Wine/FEX/图形配置、Steam 文件、账号凭据不上传。默认方案未就绪时整份设置等待；旧云端内建方案不覆盖新模板。

CAS 冲突保留双方，用户选择保留本机或采用云端；采用云端前留本地副本。本机删除的方案不被自动下载复活，也不自动删云副本。退出、过期或换号取消旧账号请求落地，不把旧账号本地数据上传给新账号。云端统一要求有效且近期核验的会员；当前沙盒验单及实际自动备份已通过。真机重装恢复尚未执行，主机恢复场景已覆盖。

## 每游戏一份备份

普通非 Steam 本地游戏自动识别唯一可信存档位置，用户在游戏设置中开启，默认关闭；未知或多候选位置不开放备份，不显示灰开关或要求用户选目录。Steam 管理游戏和旧 Steam marker 排除。游戏移除或路径不可访问时停止备份并保留原备份；重新导入需核对主程序 SHA256，由用户重新开启。

Wine 引擎及 native registry 全部停止后才备份或恢复，不自动停止玩家进程。限定目录扫描、拒绝 symlink/路径穿越/大小写冲突/超额数据；双扫描和复制后校验 path/size/mtime/SHA256。新备份完整后原子提交指针，再清理旧备份，失败保留旧数据；历史版本仅迁移最近有效的一份。

恢复使用暂存、同盘目录交换和 journal；成功清理该事务副本，中断可以回滚。每游戏仅保留最新普通备份，本地总额度 1 GiB；主机故障注入及隔离真机恢复已完成，未做真机断电测试。

## 云存档待完成

现有前后端实现包含显式云存档位绑定、4 MiB 分块、SHA256 校验、CloudDB CAS 发布、OBS 私有对象、单份替换、冲突与清理。每快照上限 256 MiB/1024 文件、单文件 64 MiB；每账号 1 GiB/16 位、最多两项上传任务。

当前 SDK JSON 云函数明确拒绝 `/v1/saves`，capabilities 返回 cloudSaves=false。实际私有对象桶及二进制传输尚未部署和验证，不能把登录、CloudDB 或主机测试称为云存档成功。需要完成私有存储、真实传输、上传/下载/恢复/中断闭环后才能启用。

OBS 适配器仅使用锁定官方 SDK 的本地签名；其网络实现跳过 hostname 校验，所以网络改用默认 TLS 校验的 Node fetch、禁止重定向、限定超时。不授予客户端直接 PUT 权限；上传后读回校验 SHA256，下载仅给 owner 短期 GET。专用桶必须从未启用 versioning，BPA 全开、公共状态 false；探针及每次启动都复查。

写入结果不明时保留额度等待核对，不靠 TTL 假定迟到写入已经消失；发布后先移除引用再删除对象，删除失败继续计入额度并可重试，不误删并发胜者。

## 部署与证据

客户端 rawfile 仅包含公开应用/商品/函数标识，module.metadata 的 client_id 必须匹配。服务端 OAuth secret、IAP 私钥、CloudDB/OBS 凭据及 HMAC 密钥仅保留在受保护部署环境，不能进入 Git、客户端、截图或普通产物。

`package-cloud-function.py` 将锁定依赖、源码、公开目录、CloudDB 报告和完整 OpenSSL PKIX 运行包打为 ZIP；PKIX 启动验证全部成员 SHA256 并运行版本检查。IAP 私钥/根证书通过规范 Base64 配置保留 PEM 换行。真实 IAP probe 报告必须绑定 app/key/root/environment/catalog/PKIX fingerprint，不能手造报告或用 SANDBOX 报告开放 NORMAL。

本轮未改后端依赖锁。AGC SDK override 沿用先前审计，OBS 的 fast-xml-parser 传递依赖告警仍披露；这里只用本地签名，未使用其 XMLBuilder 或网络解析。没有为消除告警盲目跨主版本升级。

主机验证命令：`bash scripts/run_model_unit_tests.sh`、`node --test server/value-added/test/*.test.mjs`、`python3 -m unittest host_tests.wine_audio_contract_test`。完整模型、自动同步、备份、云档主机环路及鼠标回归通过；后端 46/46、音频 5/5 通过。主机 mock 与合成证书不替代真实 IAP、跨设备同步或云档传输。

开发 HAP 保留已验证 1.4.7 音频运行时，逐成员哈希检查；临时沙盒探针从交付源码移除。只提交源码与文档，不提交 HAP、运行包、凭据、真实截图或临时脚本。本次不正式发布，也不提交商品审核。
