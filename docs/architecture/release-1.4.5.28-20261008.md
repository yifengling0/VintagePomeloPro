# 旧柚 Pro 1.4.5.28 交付

> 2026-10-08 首装复核：28 的受管理 FEX 升级同步漏掉了全新 prefix，已由 1.4.5.29 修正。首装发布请使用 29；详见 [手机首启报告](phone-first-install-fex-20261008.md)。

包名 `com.vintage.pomelopro`，版本名称 `1.4.5.28`，版本码 `1004037`。版本名称只含数字和点，符合已反馈的上架规范。

完成 `proRelease / release` 的正式 Hvigor 构建，交付未签名 HAP 和已有正式证书签名的上架 APP；`debug=false`，ARM64，保留 FEX 默认、x87 性能设定和既有混合图形策略。没有内置或自动启动 Steam。

本版包含 32/64 位 FEX 原生异常边界修复、Wine ARM64 非前进回溯保护，以及受管理 CPU DLL 的升级同步。机制、回归与真机验证边界见 [兼容性说明](fex-native-callret-compatibility-20261008.md)。

| 文件 | 字节 | SHA256 |
| --- | ---: | --- |
| VintagePomeloPro-1.4.5.28-1004037-unsigned.hap | 350997236 | d58397afee7f673584d00b02b41eb53547696b778f89de54107703c9d3b92987 |
| VintagePomeloPro-1.4.5.28-1004037-appgallery-release.app | 272401000 | d76986cb03eda00f0a2926a34311ab91caefc820d08e612e82503281fc770e22 |

官方工具验证 APP 容器、内嵌 HAP 签名和 profile 通过；profile 为 `release / app_gallery`，包名、证书和有效期匹配。独立 HAP 的无签名状态已验证；APP 内嵌 HAP 与其载荷一致，仅 pack.info 格式排版不同。运行时 ZIP 哈希与 manifest、版本 marker 一致，包内没有临时 PC0 诊断或签名私钥。

最终 release 载荷使用本地设备签名后覆盖安装到平板。实际 prefix 的两颗 FEX 和 ntdll 均匹配新包，64 位基础执行校验通过。未向 AppGallery 上传；正式上架签名 APP 本身未安装。主机回归不等同所有游戏兼容性或性能结论。

交付目录 `F:\VintagePomelo-Workspace\artifacts\VintagePomeloPro-1.4.5.28-20261008`，附 manifest、SHA256SUMS 与报告。构建输出、游戏文件和大日志不纳入源码提交；本轮未提交或推送代码。
