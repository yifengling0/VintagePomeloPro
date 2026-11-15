# 2.0.3 发布构建

版本为 `2.0.3 / 2000030`，bundleName 保持 `com.vintage.pomelopro`，Native ABI 为 `arm64-v8a`。本次在已整理、审计的 2.0.2 源码基础上提升发布版本，保留既有运行库输入及 52 项固定组件身份。

- 保持默认 DXVK 1.10；默认 D3D9 使用 WineD3D/VirGL，D3D11 使用 DXVK。
- 系统设置与每游戏设置可选择 VirGL、DXVK9、Zink。DXVK9 当前面向 x86；Zink 自动使用现有兼容修复、批量上传和 GPU 呈现策略。
- 包月和买断并存，云端提醒能力不可用不再阻断包月开通；保留商品、价格、验单、账号、协议及重复购买检查。
- 产品包不包含本轮 Smoke、Lab 图形实验入口、Vulkan 测试模块或 Heaven 回放载荷；主机测试保留在产品源码集之外。

前述功能与验证边界见 [发布整理记录](../graphics/mini-release-readiness-20261115.zh-CN.md)。旧正式 2.0.2 与整理包已做逐文件包体审计：仅移除测试专用文件，保留 Wine/FEX、DXVK/VKD3D、Mono/Gecko、音视频和界面资源。

## 构建与交付

沿用增量构建及正常未签名发布入口：

```sh
bash scripts/package.sh app-unsigned
```

该入口同时生成发布 APP 容器和独立未签名 HAP，核验当前运行库锁、产品边界、SDK stub 防护和 Windows Steam 组件。APP 内嵌 HAP 还须与独立 HAP 比较，确认只有正常 pack.info 差异；版本、ABC、Native、资源及嵌套运行库应一致。

产物、哈希、源码提交和构建结果记录于 `artifacts/release-2.0.3/manifest.json`。APP 用于后续正式发布签名，HAP 安装前需签名；本次构建不上传市场，不触发 push、标签或 CI。

购买链路由用户在 r17 验收；最终清理包及 2.0.3 版本包的真机覆盖安装验收需另行记录。包体检查不等同于游戏兼容性或性能验收，不宣称本次版本提升带来额外帧率收益。
