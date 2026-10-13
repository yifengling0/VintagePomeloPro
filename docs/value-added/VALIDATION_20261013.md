# 账号、订阅与单份备份验证（2026-10-13）

分支 `feature/account-cloud-membership-mvp`，版本 `1.4.7 / 1004070`。包含 main_proton `e08ca925` 音频修复，本次不正式发布。

## 主机与构建

- 完整 `scripts/run_model_unit_tests.sh` 通过，含 26 项自动同步场景、单份备份/恢复/迁移、Steam 排除、输入和鼠标回归、云档主机环路。
- 会员服务 mock 覆盖登录自动恢复、核验失败保留登录、订单重试、取消、已购恢复；有效自动续费或已取消续费的本期会员均不创建新付款。
- 咖啡 mock 覆盖官方 `applicationId/productType`、分页和重复订单、幂等核销、系统错误后已生成订单的恢复，防止重复付款。
- 后端 46/46 PASS，含官方证书链/JWS、type=2 的 `jwsSubscriptionStatus` 回执、最新订阅状态、取消/撤销/到期、不叠加月份及旧异步快照回滚防护。
- 音频契约 5/5 PASS；assembleApp、开发 profile/HAP 验签和最终包运行时闭包及逐成员哈希检查通过。

## 实云与真机

CloudDB `PomeloAccountTest` 仅服务端 Administrator 权限；首次事务锁、在线并发、重复订单、owner 不可变、事务 rollback 和断网闭锁已通过。证据在外部 `artifacts/huawei-cloud-setup-20261013/clouddb-verification.json`。

官方开发签名、Account Kit 登录、头像昵称、服务端换会话、退出及再次登录已通过。应用标识 `6917605913989898754`、包名 `com.vintage.pomelopro`。

用户完成官方 ¥2/月沙盒订阅的本人验证；收银台明确本次和续期不扣费。实际订单经过完整官方 JWS/证书链验签和最新状态查询，客户端确认订单并恢复 VIP；登录/返回应用恢复既有订阅无需再次购买。真实验单报告为 `artifacts/huawei-cloud-setup-20261013/iap-verification.json`，绑定当前 provider fingerprint。

已部署含该报告的云函数包 `vp-account-sync-subscription-verified-20261013.zip`，SHA256 `7155b87c5a9cecfa2662a96017e1f0389b7879a0d4f40b6a3d48bdc0389282a2`。本次以设备成功验单及实际 CloudDB 写入确认部署生效；未对该最新部署重新导出逐字节核对。SANDBOX commerce/profiles/preferences 可用，不能用该证据开放 NORMAL。

真实设备 journal 与只读 CloudDB 逐项核对：设置及 default/full-keyboard/rpg/fps/action 共 5 个输入方案的 revision、payload SHA256 与内容哈希全部一致。经典皮肤设置 revision=1；切主机皮肤自动上传为 revision=2，5 个方案仍 revision=1；切回经典后再次自动上传。证据为交付目录中的 `cloud-backup-before-change.json`、`cloud-backup-console.json`、`cloud-backup-classic-restored.json`。这证明实际自动上传及云端读回，不等同于真机重装恢复。

两套皮肤的有效 VIP 页面只显示头像昵称、VIP 标识、有效期、权益及订阅管理，不显示价格、优惠、开通或恢复购买区域。取消自动续费但本期仍有效时显示“恢复续订”，只调用官方管理页；服务层拒绝本期重复付款。界面停留期间本期到期后会重新加载价格与开通入口。取消/恢复路径已有主机测试，本轮未主动取消用户现有订阅。

咖啡原未完成订单已实际核销，设备显示“已确认之前的支持订单，感谢你！”，再次查询显示“没有待确认的咖啡订单”。未为此创建新付款。系统设置独立“赞助”默认收起，展开后为 ¥8/¥32/¥80，账号页无咖啡购买区。

## 存档与边界

隔离 `Z:\games\SaveBackupCheck` 已验证三份旧备份迁移为一份、实际恢复及 Wine 读回、运行环境退出后替换最新备份；测试索引/文件已清理，未修改玩家存档。普通非 Steam 游戏只在可靠识别位置后显示开关，未识别不显示灰开关或要求目录选择。最新非 Steam CS1.6 游戏设置真机已确认：未识别存档时显示说明，无灰开关或目录选择；未改玩家设置或存档。

当前 SDK JSON 通道拒绝存档接口，cloudSaves=false。实际 OBS 私有桶和二进制云档上传/下载/恢复未部署；本地备份和主机模拟不称为云档成功。

尚未完成 NORMAL 支付、真实退款/续期操作、正式商品审核、真实重装恢复、手机窄屏真机及断电测试。主机覆盖相应状态/恢复保护，不替代这些实机结果。

## 交付

开发签名及未签名 HAP、校验清单、签名日志、源码/运行时逐成员报告在外部 `artifacts/account-subscription-fix-1.4.7-20261013/`。保留已验证音频基线 SHA256 `2f8a16da947679ec12c3e1ded9478c4603554cb3ed718906416a0497f7a9b8a4` 的运行时；Steam/FEX/PAL4/鼠标和音频未改。凭据、真实账号布局/截图、测试回执与 HAP 不提交 Git。
