# HSL 同步与运行路线实施记录

基线：VPP `03bbf50f`（`main_proton_steamarm64`），HSL 本地
`feature/mini-dpi-native-stardew@b001b25`。保留既有历史修复和 `rc-1.5.7` 标签。

## 1. Steam 选择策略

- 不再因 GNU 组件就绪设置 GNU 默认或逐游戏固定工具。
- Linux 版本选择交给 Steam；Windows 全局推荐 `hmosdeck_proton`，优先级 100。
- `linux-native-ohos-v2` 一次性迁移全局及所有数字账户的 CompatToolMapping，
  清除四个 VPP 内置工具的逐游戏映射，保留第三方、Steam runtime、FEX 和启动参数。
- Steam/游戏运行期间拒绝待执行的迁移。先解析全部配置、保存原配置，
  通过 fsync 和 rename 发布配置，最后标记完成。中断可重试。
- 迁移完成后保留用户后续选择；保留两边前缀，不搬运存档。

回归：`scripts/run_linux_steam_cloud_tool_tests.cjs`，验证新安装、旧设置、账户配置、
第三方保留、后续选择、备份失败、部分发布失败重试、运行中保护。主机测试通过。
尚未构建或安装此批代码，不能据此声明 Mini 验收通过。

## 待完成批次

1. 统一游戏会话、原生入口、可靠窗口归属、启动取消和结束按钮。
2. HSL 公共运行库修复、前缀及组件维护租约、整组来源校验。
3. 独立原生窗口承载，OHOS Wine 按需初始化。
4. 全新检出构建、最终载荷和离线包来源、Mini 功能及三轮性能对比。

既有星露谷 GNU 菜单证据仅为短测；退出码 255 仍需单独查明。
