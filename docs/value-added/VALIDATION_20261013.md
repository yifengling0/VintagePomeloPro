# 账号、订阅与单份备份验证（2026-10-13）

分支 `feature/account-cloud-membership-mvp`，版本 `1.4.7 / 1004070`。包含 main_proton `e08ca925` 音频修复，本次不发布上线。

## 主机通过

- 完整 `scripts/run_model_unit_tests.sh` 通过：账号/换号、SDK 云函数传输、type=2 购买参数与恢复、管理订阅、18 项自动同步场景、单份备份及迁移/恢复/指针失败、Steam 排除、云档主机环路、输入与鼠标回归。
- 后端 `node --test server/value-added/test/*.test.mjs` 45/45 PASS。覆盖完整证书链、未知关键扩展/用途/根/JWS 拒绝、自动续期事实、取消/退款/过期/核验失败、旧异步查询回滚防护和加密回执绑定。
- 音频契约 `python3 -m unittest host_tests.wine_audio_contract_test` 5/5 PASS。
- Coffee SDK mock 覆盖 1/4/10 份、价格/低 API/取消/核销重试/旧订单保护；不是实际付费证据。

## 实云与平板通过

CloudDB `PomeloAccountTest` 仅 Administrator 权限，已创建首次事务锁。实际在线并发首次写、重复订单、owner 不可改、callback abort、服务端事务 rollback、Docker 断网失败闭锁均通过；报告保存在外部 `artifacts/huawei-cloud-setup-20261013/clouddb-verification.json`。

云函数实际 Node 22.23.2，入口采用单参数 callback，支持 HTTP event.body 包装。最新实际导出包与本地业务 ZIP 逐字节一致：SHA256 `71dd89775b985e409c79fef96de33db42eae6df26ce01ab0869ad02f16c72875`。14 项环境配置刷新后逐项 matches=true；不记录配置值。

官方开发 profile 的 appIdentifier 为 `6917605913989898754`，包名 `com.vintage.pomelopro`。平板 Account Kit 登录、profile 头像昵称、服务端授权码换会话、权益查询、退出和再次登录通过；页面实际显示“当前没有有效会员”及“已登录，正在自动检查同步”。两套皮肤已验证头像及共用设置入口。

`buy_coffe` 真实商品查询返回 ¥8，未完成订单查询及产品截图已验证。会员 `vip_monthly_auto` 保存为 type=2、1 月周期、¥5、仅中国大陆，组 ID `055EAC5A781A430B807FEE6DFDC5D173`；保存商品信息不等于提交审核。

官方 SDK `isSandboxActivated` 实际返回 true。订阅携带 quantity=1 时购买返回 401；移除该不适用于自动续订的参数后，实际官方收银台显示 ¥5.00/月，并说明“当前处于沙盒环境，本次订阅及后续续期不产生扣费”。该次购买已取消，尚无购买回执；没有确认任何真实扣费。官方管理订阅页已可打开，之前显示没有订阅。

临时资格测试只允许官方沙盒、精确 CNY5/type2，绑定实际已验证云账号；没有本地授予会员或提前 finish。测试源码已恢复，交付源码不包含探针、管理测试绑定或测试回执持久化。

## 隔离存档已验证

`Z:\games\SaveBackupCheck` 完成三份旧备份迁移为一份、实际恢复并经 Wine 读回、关闭测试 Wine 后自动替换最新备份、旧快照与事务临时目录清理。测试文件、索引及备份已清理，没有改玩家存档。每游戏一份最新备份，Steam 下载游戏不参与。

## 包与未完成项

开发包仅替换本次 ArkTS/libentry/config，保留已验证音频包的其他运行时与资源，基线 SHA256 `2f8a16da947679ec12c3e1ded9478c4603554cb3ed718906416a0497f7a9b8a4`。编译、开发签名验签及逐成员检查记录置于外部 artifacts；源码构建不能替代全部游戏兼容性重测。真实截图含账号信息，仅本机保留，不提交 Git。

未完成：实际订阅回执查询/验签/finish/恢复/续期/退款、付费会员自动同步及跨设备回归、NORMAL 发布验收。未配置 `VP_IAP_VERIFICATION_FILE`，因此 commerce/profiles/preferences 保持关闭。

云函数 JSON 通道拒绝存档接口，cloudSaves=false。实际 OBS 私有桶、二进制传输及云档闭环尚未部署；本地备份与主机模拟不能称为云档成功。手机窄屏真机与断电实验也未完成。

## 现有会员优惠 UI 补测

后台已有 `vip_2026_year_end` 保持不变，不新增日历定价或月份活动。平板实际官方查询返回标准价 ¥5.00/月、前 2 个计费周期每期 ¥2.00；经典与主机皮肤均显示同一规则及禁用购买状态，刷新价格通过，咖啡 ¥8/¥32/¥80 保留。会员工具按钮已合并到右侧，紧凑布局使用纵向卡片并支持换行。最终截图、布局、逐成员哈希与开发签名证据随 HAP 交付。

新增促销签名、未知优惠拒绝和官方金额/期数变化测试均通过，后端总计 45/45。新签名桥接与 catalog 尚未部署；没有真实 ¥2 优惠支付回执。会员与付费同步门禁继续关闭。具体配置边界见 [优惠适配记录](EXISTING_MEMBER_OFFER_20261013.md)。
