# 2.0.6 未签名 HAP 交付

版本 **2.0.6 / 2000060**，分支 `feature/gamescope-framegen`。按用户要求递增版本并编译未签名 HAP；相对 `f1691efe` 仅修改 `AppScope/app.json5` 的两项版本字段。包内应用源码提交为 `056746f276ab0c41097200005fadb7562bf6babd`，之后的交付文档提交不要求重编。

## 文件与构建

- 文件：`F:/VintagePomelo-Workspace/artifacts/VintagePomeloPro-2.0.6/VPP-2.0.6-arm64-unsigned.hap`。
- 大小：631,025,277 bytes（601.79 MiB）。
- SHA-256：`2ecb482b9406af4c9c7b4380f17786ca6b02e471c5d0335b7ea28034023809a3`。
- 包身份：`com.vintage.pomelopro`，ARM64，最低/目标 API 23，`debug=false`；声明 phone、tablet、2in1 和 `READ_PASTEBOARD` 权限。
- 既有 Docker `winehua-dev` / `vpp-pal4-release-build`，通过 `make -f /workspace/workspace_temp/gamescope-port/review205.mk gamescope-unsigned-hap` 增量构建；复用锁定的 Gamescope core 与运行时，没有生成签名 APP 或签名 HAP。
- SDK `hap-sign-tool verify-app` 明确报告 `No Hap Signing Block before ZIP Central Directory`，与未签名要求一致。复制到交付目录后重新核对大小和 SHA-256。

## 校验结果

编译、包 CRC、应用版本/有效源码收据、选定 Wine/FEX 运行时锁、原生运行库检查、图形包身份检查及 Windows Steam 运行时检查通过。Win-FG 许可由打包器检查通过；包内不含 Gamescope 测试像素探针。

与上一份 Mini 测试包 `99030044` / SHA-256 `1c81c0eb28d8bdc42286daa836445478e9bd64e27631b53af5d31f91a0a80cb8` 逐成员 SHA-256 对比：

- 无新增成员；移除 `.pages.info` 和 `libs/arm64-v8a/libgamescope_pixel_probe.so`。
- 仅 `module.json`、`pack.info`、`pkgContextInfo.json`、`resources/rawfile/application-build.json` 改变。
- 所有共用原生库、Wine/FEX、DXVK、Mesa、Gamescope core、`wine-data.zip` 和 UI ABC 字节相同，没有以旧运行时替换当前插帧实现。
- 保留 WSL 中原有的 DXVK 本地修改及有效源码收据，不宣称干净 CI 构建。

## 验收边界与保留成果

本次为版本与打包变更，没有安装或操作设备，也没有重新测量游戏。此前 Mini PAL4/VirGL 的性能数据属于 2.0.5 调试包，见 [插帧评价](../gamescope/frame-generation-validation.zh-CN.md)；之后用户查看时仍捕获到明显运动重影，未完成关闭态配对定位，不能宣称已经修复或完整画质通过。插帧默认关闭，全局、单游戏及侧栏入口保持现有行为。

不合 main、不推送、不打 tag、不上架，不替换 2.0.4/2.0.5 正式交付文件。保留当前与上一份 2.0.5 调试签名 HAP，以及本次唯一的 2.0.6 未签名交付；核验 Windows 留存后删除 Linux 任务目录中的同 SHA 重复副本，继续复用构建缓存。

详细交付记录：同目录 `delivery.json`（指交付 HAP 所在目录）；固定任务目录 `workspace_temp/gamescope-port/release206-delivery.json`、`release206-storage.json`，以及工作区 `logs/storage-maintenance/latest.json`。
