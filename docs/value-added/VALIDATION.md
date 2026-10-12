# 首版验证记录（2026-10-12）

## 当前代码已过

- 官方 SDK TypeScript 模型编译 + 既有 catalog/input/AppSession + 新增账号服务、v6、存档本地文件系统回归。
- 真实 CloudSaveService/LocalSaveSnapshotService → 真实云档领域逻辑 → MemoryStore/对象传输 mock → 主机真实文件系统闭环：手动跨设备映射、多块/零字节文件、下载不改游戏、单独恢复、换号/取消、哈希失败、清理失败后不永久 BUSY。
- 后端串行 `node --test --test-concurrency=1 test/*.test.mjs`：25/25 PASS。
- 实际锁定 AGC SDK 对本地 HTTPS mock：请求、认证/requestId、响应/409错误、超时、查询/哨兵事务序列化 PASS。
- 实际 OBS 3.26.2 signer-only + mock transport：PUT/GET/DELETE 签名头与已验证同字节数据 PASS；测试强制 XMLParser.parse/XMLBuilder.build 抛错，调用未触达两条路径。
- 全 entry ArkTS 第8轮：PASS、0错误、11秒；原 native 声明64错已按真实C++契约窄修。产物 modules.abc 3,461,512 bytes，SHA256 `d0a2b1499ea3e32adce7c2699a348a2ff4e038a7f435f0e8f316ee14a13ac070`。编译后全部源码hash与冻结清单相同。
- 独立只读安全复核已关闭退款终态、换号异步、profile列表体积、NSP_STATUS header、PKIX、存档恢复空/缺失目录、短写、云上传延迟lease/GC、清理锁死、OBS真实命名空间/BPA门禁等发现；独立云档10组及主机闭环再次PASS。
- `git diff --check`、三平台声明/打包配置/gitlinks不变、密钥/私库材料扫描通过。

## 依赖审计，不能称 audit 0

最终新增后端：0 critical / 0 high / **2 moderate**，两个条目为同一传递依赖及 SDK 汇总。

- 路径：`esdk-obs-nodejs@3.26.2` → 声明 `fast-xml-parser@^4.0.11` → 锁定 `4.5.7`。
- 公告：[GHSA-gh4j-gqv2-49f6 / CVE-2026-41650](https://github.com/advisories/GHSA-gh4j-gqv2-49f6)，XMLBuilder 在注释/CDATA内容中未转义分隔符，影响版本 `<5.7.0`，修复版 `5.7.0`。
- 2026-10-12 npm registry 核查4.x最新仍为4.5.7，没有同主版本修复可直接升级。没有为清零audit未经兼容证明强换5.x。
- 当前可达路径仅官方 SDK 的本地预签字符串/HMAC计算。业务输入不会进入 SDK XMLBuilder；SDK网络/XML解析方法也完全禁用，实际传输用 Node fetch。`test:obs-signer` 用真实包并把 XMLParser/Builder 执行方法替换成必抛异常，覆盖实际 adapter PUT/GET/DELETE 后通过。
- 这项测试证明当前受测路径隔离，**不证明整个依赖无漏洞**。正式上线需保留本风险评估；升级SDK或新增任何SDK网络/XML调用必须重新审计、回归，锁变动使既有云探针报告失效。
- 首片仅AGC时audit为0；该旧结果不可用于包含OBS的最终版。

## 仍然没有证明

没有真实 Account/AGC/OBS/IAP、沙盒付款/退款、真机、完整 native/HAP、签名或正式发布结果。没有 CRL/OCSP 联网撤销检查。没有验证现有AGC云函数运行时/网关是否满足Node22/OpenSSL3/4MiB二进制请求及超时，当前入口是独立Node HTTP服务。

所有真实云能力默认闭锁；用户决定方便时再登录后台、确认隔离测试部署及安全配置。需要的步骤见 `DEPLOYMENT_CHECKLIST.md`，不是只补签名就能收费。
