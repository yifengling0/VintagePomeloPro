# 使用华为托管后端测试登录和同步

不需要自建服务器。可以用华为云函数执行可信的登录换码、权益核验、方案和存档操作，CloudDB 保存记录，OBS 保存存档文件。但“已开通云函数”不等于业务代码已经部署。

## 本次开发包

已合并 `main_proton` 的 1.4.7，保留最新 Steam、FEX、Wine、PAL4 和鼠标修复。包名是 `com.vintage.pomelopro`，开发 profile 的 appIdentifier 是 `6917605913989898754`；签名工具自动把该 profile 写入 HAP。

这个 appIdentifier 不是 OAuth Client ID，也不能据此推定 IAP App ID。本地存档备份不依赖账号。官方华为登录已解耦业务配置并通过开发签名真机测试；业务云同步仍需接入后端。

## 先取得应用公开配置

1. 在 AppGallery Connect 的“我的项目”中打开 **旧柚 Pro / com.vintage.pomelopro 对应应用**，不要使用其他应用或元服务的配置。
2. 在应用的常规/应用信息中记录 App ID、OAuth 2.0 Client ID，以及项目和区域。若页面名称发生变化，核对字段名称，不按数字相似程度替代。
3. 确认 Account Kit 已开通，并登记本次开发证书的 SHA-256 指纹。上线证书和开发证书分别核对。
4. Client Secret、Server SDK 凭据、OBS AK/SK、IAP 私钥只进入云端受保护配置，不能放入客户端 JSON、Git 或聊天。

基础登录直接调用 Account Kit，由系统校验签名应用身份，不要求先部署业务后端。业务云会话需要真实 Client ID、客户端签名登记，以及已部署并接通 CloudDB 的换码接口。输入方案和云存档目前按会员权益开放，还需要真实 IAP 核验；云存档另外需要 OBS。可先完成登录、本地存档，再按顺序接通后续能力。

## 云函数接入差异

当前 `server/value-added/src/main.mjs` 已把领域初始化拆为 `loadService()`，但生产入口仍是 Node HTTP 服务。它尚无 AGC 云函数事件入口，不能直接上传后声称可用。

使用已有云函数前，需从当前控制台确认：

- 函数支持的 Node 版本和 OpenSSL 版本：当前代码要求 Node >=22、OpenSSL 3。
- HTTP 触发器/网关如何提供路径、方法、Authorization、二进制 body；是否提供可供客户端 HTTP 调用的 HTTPS 地址。
- 请求体、内存和执行超时：当前云存档每块最多 4 MiB，若网关把 body 转为 base64，事件体积还会增加。
- 函数的区域、CloudDB zone、OBS 桶是否对应本应用；现有账号 session 和存档记录放在 CloudDB，不能改成只存于某个函数实例内存。

确认上述实际契约后，再把 HTTP 路由与有界 body 检查适配到函数事件，并生成对应部署包。客户端也可改用 `CloudFoundationKit.cloudFunction.call`，但该 SDK 不等价于现有 HTTP 协议，仍要同时适配客户端、函数入口和分块限制。不能只把函数名称填成 apiBaseUrl。

真实部署及存储探针参见 [DEPLOYMENT_CHECKLIST.md](DEPLOYMENT_CHECKLIST.md)。这些要求是当前实现的接入条件，不要求购买一台服务器。

## 同步写入客户端公开配置

取得真实 HTTPS 接口地址和 OAuth Client ID 后，在本机创建仅含公开值的 JSON。字段为 `apiBaseUrl` 和 `accountClientId`；购买测试时再加入已配置 SKU。不要填示例 ID 或未部署的地址。

```sh
python3 scripts/configure_value_added.py /path/to/public-account-config.json
python3 scripts/configure_value_added.py /path/to/public-account-config.json --apply
```

第一条只检查，第二条同时更新 `value_added_config.json` 和 `module.metadata.client_id`，保持设备声明。之后重新构建、开发签名，先验证授权页、换码、退出和过期，再验证同步。

开发签名包、构建成功、主机文件系统闭环和 SDK 对 mock 的测试，都不能替代真实华为登录、云函数部署或跨设备云同步结果。

头像昵称、三档咖啡和单份备份的当前实现及配置见 [ACCOUNT_AND_COFFEE.md](ACCOUNT_AND_COFFEE.md)。咖啡直接调用官方 IAP，不以云会员后端为前置条件。
