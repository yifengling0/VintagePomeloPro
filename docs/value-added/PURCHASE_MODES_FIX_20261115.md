# 包月与买断并存修复候选（2026-11-15）

用户已自行更新云函数，配套 r17 已覆盖安装 Mini 并启动；随后用户确认“这次可以了”。这是用户反馈的购买链路验收，本轮未独立执行付款，也未查看账户或订单。上架整理继续移除客户端诊断日志，不改变已验收的购买流程。

## 定位

云端真实只读调用 vp-account-sync /v1/capabilities 返回 commerce=true、membershipPurchases=true、renewalReminders=false。CloudDB 的 renewal-timer-checkpoint 最后写入时间为 2026-10-17 15:03:02（北京时间）；这不是调试签名判断。当前云端完整代码和环境未导出，不把本地旧环境备份当成当前配置证明。

源码中客户端 buy、ValueAddedService.purchase、云端 domain.intent 三处把云端提醒就绪作为包月购买前提；买断 type=1 不经过该检查，所以会出现买断可选、包月被拦。两个商品的 ID/type 没有交叉：VIP/type1，vip_monthly_auto/type2。新增买断的提交本身没有新增这项检查；后续购买环境统一移除了历史沙盒例外。rc-1.5.8-final 标签的相关云端源码与当前 HEAD 一致，不能仅凭客户端版本号断定当时实际部署的云函数。

## 当前修复

- 按本轮两种方式并存的产品要求，将自建续费提醒作为独立能力；两种购买仍检查官方商品、价格、账号、协议授权、服务开通状态与已购权益。云端验签、绑定、退款撤销、测试/正式订单隔离保持受测。
- 提醒状态和独立开启入口继续显示；提醒故障不会被标成 ready。已有有效会员继续阻止重复购买，恢复及取消流程保留。
- 提示和协议改为如实说明提醒状态，同时列明连续包月与终身买断。确认版本更新为 2026-11-15，原有确认不自动继承。
- 修复打包/加载契约：main.mjs 默认读取包内 config/renewal-verification.json；显式路径错误继续报错。修复包未附造假的提醒验证材料。
- 图形默认维持 DXVK1.10，D3D9=WineD3D/VirGL、D3D11=DXVK；图形 Native、Gallium、Wine payload 与 r16 完全相同。

## 验证

Node22 后端完整 104/104 PASS；真实客户端 service 与 checkout 方法回归 PASS，覆盖两种购买、提醒不可用、账号切换、价格变化、支付取消、待确认订单、重复购买与恢复。ArkTS assembleHap PASS。云函数 ZIP 全部25个源码成员、锁文件与 handler 与受测源码逐项一致；复用现有锁定依赖、真实旧验单材料和校验通过的 PKIX runtime，无下载或新凭据。

Mini r17 的全量54个 UI 成员与本次编译一致，原生库和 Wine 数据逐项保留；签名、profile、payload inventory 已验证。未做线上购买或新包真机支付验收。

## 提交测试

1. 将同目录 vp-account-sync.zip 上传到现有 vp-account-sync 云函数。运行时 Node22，入口 handler.myHandler。保留当前保护环境变量、账号/订单绑定、数据库及触发器，不导入旧环境备份。
2. Mini 覆盖安装配套 VPP-2.0.2-mini-purchase-r17-signed.hap；旧客户端仍有本地提醒前置，需配套升级。
3. 登录无有效 VIP 的测试账号，分别选择连续包月和终身买断，核对官方价格及各自协议；收银台取消可验证链路。已有有效会员仍按原规则避免重复购买。

这是候选包交付，未声称云端已修好或实际付费验收完成。提醒定时器与官方回执服务仍需独立恢复。

## 文件校验

云函数 ZIP：6698474 bytes，SHA256 `ba3e17bcf6237bbcf4b3445f51ed11439d7f30057f5aff5875ab3c2700eb3744`。

Mini r17：629687688 bytes，SHA256 `ac7174a52788585bbc5654cc12a33cd1eaa40da8f26c1b61b9cc6a33b45c8398`。

HAP 唯一位置：`/home/liufeng/src/vpp-account-cloud/workspace_temp/mini-virgl-performance/VPP-2.0.2-mini-purchase-r17-signed.hap`；无 F 盘重复大包。

源码 HEAD 56cfd994（dirty overlay）；本轮未 commit/push/CI，既有根仓及10个 dirty 子模块保持。r16 为回退包，r17 unsigned 为唯一后续增量基底；不要重新运行依赖已退役基底的一次性旧打包脚本。
