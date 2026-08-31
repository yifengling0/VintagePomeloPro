# Smoke 回归架构的产品适配

## 结论

WineHua 的 smoke runner 可以合并。此前把“保留产品 UI”扩大成“不采用 runner/service”，导致数据驱动套件、Want 协议、Host runner 和部分打包资产被错误判定为产品自动化已覆盖。本分支改为保留协议和服务架构，只不导入上游侧边栏页面。

## 三层边界

1. `automation/run_regression.py` 只负责通过 HDC 发起 `winehua.mode=smoke` Want、等待结果并收集文件。它不决定 Guest 图形环境。
2. `SmokeRunner.ets` 读取随包生成的 `C:\\smoke\\suites.json`，按定义调用 Native `runWineProgram`，轮询每项 JSON，并写 `suite-summary.json`。runner 复用 `WineEngineService` 的引擎状态，不另建会话状态机。
3. Native `runWineProgram`、`BuildSessionEnv` 和 graphics policy 决定 Wine、DXVK、VKD3D 与呈现路径。ArkTS 只传套件声明和兼容字段，产品 Native policy 仍是最终来源。

## 产品适配

- 产品的 `pages/Index.ets`、组件、资源和导航没有导入上游 smoke 入口。自动化只在 `EntryAbility` 增加 Want 分发。
- `reuse` 使用产品前缀；`clean` 由 `WineEngineService` 停止当前 broker、清理并启动隔离的 `.wine-smoke`，套件完成后恢复产品 `.wine` 会话，不删除产品用户前缀。
- managed payload 同步验证 `suites.json`、GPU diagnostics、DXVK 2.6 requirements、D3D12 smoke 及产品既有音频、媒体和网络探针。
- `runWineProgram` 接受 WineHua 的 `dxvkBackend`、`presentBackend` 公共字段，同时保留产品的 `prefixMode`、`presentToSurface` 和 `automationMode` 扩展。`setHostShadowProfile` 兼容入口也保留，但旧实验名会在会话启动时收敛到所选 D3D 后端的产品 graphics policy。
- WineHua 最后的 `opBusy` 修复针对其 `WineEnvService.resetSmokePrefix`。产品 runner 不持有该锁，先由 `WineEngineService.ensureReady` 完成状态迁移再启动，所以这里没有同一自锁路径；该提交按产品生命周期覆盖记录。

## 结果协议

每项结果至少含 `schemaVersion`、`runId`、`testId`、`status` 和 `stage`。终态为 `PASS`、`FAIL`、`SKIP` 或 `UNSUPPORTED`。标准模式由 runner 生成带盘符的 `C:\\smoke\\results\\<run-id>\\<test-id>.json` 参数；raw 模式使用 `suites.json` 中的 argv 并替换占位符。

suite 和 runId 只接受 1–96 位字母、数字、点、下划线和连字符。套件文件来自签名载荷，PE 路径和测试 ID 仍由受管 `suites.json` 提供。

## 验收边界

Host 测试、Native 编译、双 ABI HAP、包内资产与 JSON schema 是合入门槛。用户已暂停设备回归，因此本轮不把 runner、实体手柄或新 smoke 套件记录成真机通过。
