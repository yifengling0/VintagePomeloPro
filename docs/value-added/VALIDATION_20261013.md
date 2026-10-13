# 账号、咖啡与单份备份验证（2026-10-13）

分支：`feature/account-cloud-membership-mvp`，版本 `1.4.7 / 1004070`。已包含 `feature/main_proton` 的 `e08ca925` 音频修复，2026-10-13 fetch 后确认它是本分支祖先。本次不发布上线。

## 主机回归

- `scripts/run_model_unit_tests.sh` 通过：实际服务配文件系统适配器，覆盖单份备份替换、校验、旧历史迁移、指针提交失败、恢复交换前后中断、删除/重新导入、最后一份备份清理、Steam 排除、账号退出/换号及自动同步；同时覆盖既有输入/鼠标等模型。
- 咖啡 SDK mock 覆盖 1/4/10 份及 8/32/80 元、低 API 数量保护、价格变化、旧订单不重复扣款、取消、核销重试、商品隔离。SDK mock 不代表真实付款。
- 后端 `node --test server/value-added/test/*.test.mjs`：29/29 PASS。包含旧版本清理、额度、冲突和上传失败；尚无真实云部署。
- `python3 -m unittest host_tests.wine_audio_contract_test`：5/5 PASS。
- `git diff --check` 通过。构建、签名、逐成员运行时校验结果随开发包保存在外部 artifacts 目录，不提交 HAP 或签名材料。

## 平板真实验证

官方开发 profile 签名的 HAP 保数据覆盖安装，Account Kit 登录成功；profile 授权实际返回头像和昵称。主机皮肤与经典皮肤都已显示真实头像，最终恢复经典界面。业务云未配置，页面明确区分“华为账号已登录”和“云同步尚未接通”。

咖啡区已显示小杯 ¥8、中杯 ¥32、大杯 ¥80，并接入 `queryProducts/createPurchase/queryPurchases/finishPurchase`。真实商品查询返回 `1001860003`，本地官方 SDK 定义为 `INVALID_PRODUCT`；当前 Pro 应用尚需配置有效商品及沙盒测试账户。未发起、未确认实际付款，不能标记真实支付通过。公开配置见 [ACCOUNT_AND_COFFEE.md](ACCOUNT_AND_COFFEE.md)。

隔离目录 `Z:\games\SaveBackupCheck` 测试了旧三份备份迁移为一份；将 `backup-changed` 恢复到实际存档后经 Wine 读回确认，恢复目录无 `.vp-before-*` / `.vp-restore-*` 残留。开启该测试游戏自动备份，写入 `backup-original` 后停止测试 Wine 环境，实际备份自动替换、旧快照目录删除。测试文件、备份及对应索引已清理，没有修改用户游戏存档。

原始截图、布局、命令账本保存在 `artifacts/account-cloud-dev-1.4.7-20261013/device/`，含用户头像昵称，作为本机证据保留，不提交 Git。

## 包装边界与未覆盖项目

开发包保留已验证音频修复 HAP 的运行时成员，仅使用本次编译的 ArkTS、`libentry.so`、配置与新增资源；基础包 SHA-256 为 `2f8a16da947679ec12c3e1ded9478c4603554cb3ed718906416a0497f7a9b8a4`。最终 `package-verification.json` 记录源码提交、逐源码/成员哈希和签名验证；不将普通源码编译视为重新验证所有游戏兼容性。

仍未完成真实 CloudDB/OBS 部署和跨设备云同步、真实 IAP 付款/退款、手机窄屏真机操作及断电故障实验。本地恢复故障注入和主机云测试不替代上述证据。后端依赖审计沿用 10 月 12 日记录，本轮未升级依赖。
