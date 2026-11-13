# 云备份恢复、重复订阅及上线准备复核（2026-10-15）

当前结论：云端备份可被真实平板下载、恢复并在冷启动后完整保留；容量原先显示 0 MB 是小文件取整及未知值混用问题。自动上传最后一步出现的 SERVICE_ERROR 尚未闭环，暂不能正式上线。

## 本轮修复

1. 小于 1 MiB 的备份显示 KB；未知容量显示“云端大小待检查”，空备份显示“云端暂无备份”。容量按账号隔离，切换账号不沿用上一个账号的数值。
2. 进程重启后，若中断上传已被服务端过期清理，abort 返回 CONFIG_NOT_FOUND 不再永久卡住同步。只放行这一错误；网络、权限、会员错误仍保留 pending，账号或前后台变化立即中断。
3. 同账号重新订阅可保留官方 subscription/token/generation，但新的购买 intent 产生新的 developerPayload。旧服务端把 payload hash 当作永久所有者字段，导致 ORDER_OWNER_IMMUTABLE。修复仍验证原始回执与官方最新订单的两个 HMAC 绑定；账号、应用、商品、代次和 token 不得变更。仅另一笔、购买时间严格更新的同账号订单允许轮换绑定。旧查询、旧单回放及同单换绑定均受保护。
4. 客户端失败日志只记录固定操作名；服务端未知异常仅返回固定错误类别或最多 12 位数字 SDK code。异常正文、堆栈、请求内容、账号、token 和回执不进入诊断响应。

## 平板实际验证

- 设备为 USB 连接的 MLR-AL10，应用 com.vintage.pomelopro，版本 1.4.8 / 1004080。保数据覆盖安装开发签名包，没有卸载、清数据或改玩家存档。
- 官方免费 SANDBOX 收银台完成本人验证；第二次订阅暴露旧服务端绑定问题。使用真实官方已验签订单经管理端标准 verify 流程协调，随后平板正常“恢复购买”，SDK finishPurchase 成功，页面恢复 VIP。
- 这次管理端协调没有伪造会员、订单或会话，但也不等于新后端已部署并通过平板自动验单。下一次重新订阅仍需部署本轮修复后复测。
- 恢复测试先正常关闭自动备份，把主题从 system 改为 dark。管理端把同一份 system 云配置发布为下一 revision，模拟另一个客户端的发布，再在平板正常开启备份。
- 平板真实显示冲突，选择“恢复云端”后显示“已恢复应用配置”，主题恢复 system。
- 云端 revision 6，113,569 bytes；页面显示“完整输入方案 8 个 · 云端 110.9 KB”。所有云成员及分块 SHA-256 校验通过。
- 强制停止并冷启动后，保持登录、有效 VIP、自动备份开启、system 主题；本地 manifest 与云端相同。进一步读取真实持久化 preferences，对 settings 的 3 项、input_profiles 的 2 项（包含完整 8 方案）及 library 的 6 项逐项比较，全部一致。
- resources 模块为空，备份中没有字体或背景文件。上述恢复结果不能代替自定义字体和背景文件的真机恢复验收，也没有进行卸载重装恢复。

## 自动上传仍有上线阻断

过期 pending 修复后，平板确实成功自动发布过 revision 3。后续两个独立主题修改均已上传完整分块，但在 finalize 返回 SERVICE_ERROR（22:18:09、22:25:25）。因此不能仅根据页面能恢复或 CloudDB 中有正确数据就认定自动备份稳定。

同一 pending 在管理端只读重放与真实 finalize 均成功；独立真实 CloudDB 的 256 KiB 分块、两次原子发布、去重、读回和删除探针也通过。这缩小了范围，但没有证明云函数实例中的具体异常。全局事务锁竞争仍只是待证假设，本轮没有据此放宽原子性或擅加重试。

云端 revision 的来源必须区分：

| revision | 发布来源 |
| --- | --- |
| 3 | 平板真实自动上传成功 |
| 4 | 管理端按标准 begin/upload/finalize 发布 dark 配置 |
| 5 | 平板上传的 system pending，由管理端标准 finalize 完成 |
| 6 | 管理端复制同一 system 内容为下一版，用于冲突恢复测试 |

管理端成功不计作平板自动上传成功。当前 revision 6 的快照 hash 为 d3c5d0c49933669d16dfd78cc7b2b6c2812701510c1fac78937c6bbd431ea46b。

下一步在正确的旧柚Pro测试项目部署本轮 ZIP，保持 SANDBOX、PomeloAccountTest、现有 HMAC 密钥及真实验证报告不变；复现平板设置修改，取得固定 diagnosticCode，并结合云函数日志确定具体 SDK/代码错误。只在取得根因证据后修复，再要求连续两次平板自动发布、冷启动读回及冲突恢复通过。不能以管理端代提交流程替代验收。

本轮未重新部署云函数。启动新的 Edge 控制台窗口被自动审批拒绝（blocked by policy）；通过 Codex 内置浏览器打开正确项目的请求仅返回 queued，未取得可操作的部署页面。未碰原有另一个“旧柚”应用的上传对话框，没有提交商店发布。

## 编译、回归与评审

- Docker / Node 22 后端 57 个测试通过，覆盖双 HMAC、跨账号、同单换绑定、旧单回放及诊断脱敏；配置集成 26 场景通过；自动同步 38 场景通过；ValueAddedService 现有回归通过；git diff --check 通过。
- 修改的客户端已通过 Docker API 23 ARM64 开发 HAP 和 proRelease / release APP 编译。实际平板测试使用开发签名 HAP，正式 APP 未安装覆盖，避免签名不同需要卸载。
- 正式 APP 外层签名及 release / app_gallery profile 验证通过。同次独立输出的正式签名 HAP 及 profile 也通过。
- 当前 SDK 默认 PackageApp 使用 unsigned HAP 输入后整体签 APP，appWithSignedPkg 默认为 false。因此不声称嵌套 HAP 已独立验签；独立正式 HAP 与 APP 内 HAP 的代码、资源和运行时成员完全相同，差别仅 pack.info 空白及 APP 打包省略 .pages.info。
- wine-data.zip 与此前 DXVK/付费统一包相同，运行时 2353 成员的既有核验仍适用；本轮没有改 Wine/FEX/DXVK 运行时。手机 Direct 按用户要求未测。
- 最终逐项复核：404 恢复只清理当前账号 pending；失败保留重试记录；绑定轮换不放宽账号/代次/token；未知异常不泄漏正文；SANDBOX 不能开启 NORMAL；cloudSaves 仍关闭。

## 交付文件

路径均在 F:/VintagePomelo-Workspace/artifacts/purchase-release-20261015/：

| 文件 | SHA-256 | 大小 |
| --- | --- | --- |
| VintagePomeloPro-1.4.8-1004080-cloud-recovery-dxvk26-dev-signed.hap | 7fb8814bc3a87490001fb4490ed8bd9706650beea0101d7e5a26ddf32f1feb2e | 361878151 |
| VintagePomeloPro-1.4.8-1004080-release-rehearsal-SANDBOX.app | 512de511097c2ae9c3f774a7a8bca8b61fabf3be381d33fc9351b0493f6a64e7 | 272971221 |
| vp-account-sync-recovery-20261015.zip | fb7f8e1ec7c750d89b2957fe04df0583f658350c4455959f568be96475d2161d | 6684012 |

后端 ZIP 已核对源码、ZIP 完整性和 PKIX/真实报告，解包后在 Docker 使用既有受保护凭据做只读 capabilities 验证，configuration=true、sandboxCommerce=true、cloudSaves=false。此项是包级验证，不能代替线上云函数验收。

可核查证据：state-restored-cloud.json、state-restored-cold-cloud.json、restored-persistent-content.json、device/restore-result.json、device/cold-size.json、package-verification.json、release-app-verification.json、function-package-verification.json、backend-final-tests.log。账号/回执/私钥原始材料仅保留于本机私密目录，未提交。

## 正式上线前剩余门禁

1. 部署沙盒后端并解决 finalize SERVICE_ERROR；完成独立的平板自动验单、连续上传和恢复。
2. 验证新订阅、续期、取消保留本期、到期、退款/撤销以及同账号重新订阅。沙盒加速状态不能推出准确续期周期，finishStatus=2 也不是已证实的撤销原因。
3. 生产配置及 CloudDB 与测试环境隔离，固定函数版本并准备回滚。NORMAL 需要匹配 fingerprint 的真实官方验单证据，不能通过改环境或造报告跳过。
4. 真机重装、断电和自定义资源恢复；手机窄屏仍需另验收。
5. 生产商品、自动续费说明、隐私及数据删除文案、退款和服务端订阅通知机制需独立验收。现有实现依赖请求时官方轮询，未验证 webhook 通知闭环。
6. 游戏存档没有上传，cloudSaves=false；正式演练 APP 保留 SANDBOX，只供上线流程准备，不可当作已获准正式上传的包。
