## 2.0.0：OHOS Wine / Windows Steam 主线

- main 汇合此前全部历史修复，默认采用原生 ARM64 OHOS Wine 与 FEX；Box64 保留为可选兼容后端。
- HmosDeck 与 Windows Steam 游戏库、启动入口保留；移除 Linux Steam、GNU Proton 产品界面与运行载荷。
- 包含 Steam 启动和可选社区请求的超时处理、原生 XHR 期限、SHM 上传及 GPU staging 访问修复。
- CI 与本地使用同一组构建、来源、运行时闭包和产品边界检查；配套发布 Windows Steam 离线导入包。
- 离线包沿用已在 Mini 导入的固定内容，应用启动前更新匹配的辅助模块。ZIP 无需解压，不含账号、游戏或存档。

本发布先冻结现有运行时；FEX/Proton 的完整上游升级在独立后续分支开展。
Steam 登录、库界面和输入已有 Mini 验证；界面仍有卡顿，游戏性能与长期稳定性不作扩大承诺。
GitHub HAP 为未签名包；安装前须使用匹配设备或发布权限的签名。
CI 制品使用明确的 source-build-candidate 策略，通过源码、ABI、逐模块校验和与依赖闭包检查；
制品记录保留 deviceValidated=false，不将旧 1.4.9 二进制锁或历史 Mini 结果视为此次 CI 字节的完整真机验收。
