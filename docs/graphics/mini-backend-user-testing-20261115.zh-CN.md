# Mini 后端切换试用候选 r16（2026-11-15）

已安装 Mini，应用版本 2.0.2 / 2000020，release/internaltesting，debug=false、profileable=false。
用户最终要求是各后端平等切换，Zink 自动采用已验证的优化，用户无需理解内部调参。

## 最终行为

- 系统设置 → Wine 引擎 → 默认渲染后端；游戏卡片设置 → 启动参数 → 游戏渲染后端，可覆盖或跟随系统。
- VirGL、DXVK9、Zink 均可选；同时保留 DXVK 1.10 / 2.6 原有选择。切换后下次启动生效，改变 Host 路线时重启 Wine 会关闭其他 Windows 程序。
- 选择 Zink 自动启用批量上传和 GPU 呈现，无额外面向用户的性能开关。Native 默认与 AppSession 注入均启用；离开 Zink 时 AppSession 显式置 0，避免跨游戏泄漏。
- 保留 0022/0023 的 varying / 3D PBO 正确性修复和 0024/0025 优化；仍为 Venus 路线，Host 仍有 GPU copy。本轮未新增版本号 override。
- Zink GPU 呈现仍属实验路径：成功路径与 resize 已验收；导出在 ready 后失败、Host ready 撤销、长期多窗口恢复尚未全面验证。不能声称已自动兜底所有故障；有游戏异常时通过后端选择切换路线。

## 验证

- 实际生产模型/引擎方法回归通过：系统继承、游戏覆盖、旧配置迁移、跨游戏重置、启动串行、自动化不改系统默认；本轮加入 Zink 自动开启与其他后端清除状态。
- ArkTS assembleHap 成功，Native 增量构建成功。界面全量 54 个资源成员与编译产物一致；保留 r14 Gallium / Wine payload。只更新 UI 与 libentry，签名及 payload inventory 验证成功。
- 真机未传 WINEHUA_ZINK_PRESENT 等内部开关，GL probe 240 帧、320×240 → 512×288 → 320×240，三次逐像素检查和 SwapBuffers 全部通过；日志 presents=120 时 readbacks=1，证明默认进入 GPU 呈现。
- 真机同一设置页 Zink → DXVK9 → VirGL 均可切换，六个选项均 enabled。VirGL 跨应用重启保留，最后恢复原有“跟随系统”。未展示内部优化开关；本轮用内建桌面卡片验证共享设置链，未替用户运行仙剑4。
- 会员/提醒服务相关现有回归 18/18 通过。git diff --ignore-submodules --check 通过。
- 无新增完整 Heaven 分数，不能宣称稳定 30% 或所有游戏 Zink 最快。下一阶段由用户以固定分辨率/画质和同一存档机位进行游戏实测。

## OpenGL 与 D3D9 位数

当前真机 legacy context 实测 Zink OpenGL 2.1 Mesa 25.0.1。Zink 理论上能支持更高版本，但本 Mini Venus 路线实测缺 transform feedback、多视口等能力；还存在既有 GS 禁用策略。需补特性/透传并逐项验收，不能只改版本字符串升到 3.x / 4.x。

DXVK 的 D3D9 上游支持 x86 和 x64；当前 Mini 候选仅接入并验证 x86 D3D9。64 位库及原生适配尚未交付，因此保留“32位实验”标注。

## VIP 购买阻塞核查

当前 Mini 包并未被识别为 debug。新日志明确：commerce=true membership=true reminders=false；界面实际是 REMINDERS_UNAVAILABLE，对应连续包月的续费提醒就绪检查，未发起支付。

当前客户端 checkPurchaseEnvironment 不按调试签名或 tester 身份阻止收银台，r14 已装界面也未包含旧 sandboxCheckout 门控。会员页文字已明确为“云端续费提醒服务未就绪”，新增日志仅记录三个布尔值，不记录账号、订单和凭据。

服务端 /v1/capabilities 经 RenewalReminders.timerHealthy 判定：推送 sender.ready、数据库 ready，以及最近 36 小时内的定时任务心跳均需满足。还要从部署侧核查推送凭据/验证记录、实际运行版本和 timer checkpoint，当前设备数据不足以判定具体哪一项失效。本轮未改云端配置，未付款；包月购买问题仍待服务端恢复，不应宣称已修复购买。

## 包与源码记录

当前包：VPP-2.0.2-mini-zink-r16-signed.hap
signed SHA-256: b78ae1e4038056e66fda6a9914c4faddaac6bc205590a6c73385141528e531fc
unsigned SHA-256: 9063455389ff679746f2b4761b1489dbdc49632a0c940265bc154a1e6f2c8955
libentry SHA-256: 2e106e1d8e6b34b8674f0bd28326be928b27298acc40dc2aa368ff351d6ea70d
Gallium SHA-256: 639c7cb8e6e193071d49d55bb45a78445173f01627a8a25a8182574965d2c084
Wine payload SHA-256: ba7a1c6f6b395f2cd04123b7004365844c5698f53a46557d10bdc4fb903ec6a0
HEAD: 56cfd9946b814fa28b9d1875cd1c659b59d7159e，feature/fex-proton-upstream-sync，dirty overlay。本轮未提交、push 或 CI，既有源码与 10 个 dirty 子模块保留。

包位于 Linux /home/liufeng/src/vpp-account-cloud/workspace_temp/mini-virgl-performance/。
仅保留 r16 signed 当前包、r14 signed 回退包、r16 unsigned 增量基底。不在 F 盘复制大包。
证据目录：workspace_temp/mini-virgl-performance/release-readiness/；签名/构建记录与退役收据位于同 task。

## 存储收尾

已按 SHA 核对并退役 r11 signed、r15 signed/unsigned、r14 unsigned，合计 2.342 GiB（含本轮临时候选，WSL 内部回收，并非 F 盘净释放）。已移除本轮 strip 副本与不必要的中间截图/账号页原始布局；当前保留两份签名包和一个增量基底。最终 maintain-storage -Apply -CompactLogs：actions=0、issues=0。
