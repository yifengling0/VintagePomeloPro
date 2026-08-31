# WineHua 重构架构对齐审计

冻结对比范围为产品 `2c0436360ad3f821afe3fd6ea29d76e89f3781e7` 与 WineHua `61cb4c6400a1c9a759fe2c511e6ef782d8586edb`。本审计回答“功能已存在”是否被误用来跳过上游架构。

## Native C/C++

WineHua 当前 `entry/src/main/cpp` 的每个目标路径都存在于产品分支；T7 的 181 个路径搬移没有留下旧目录作为主实现。产品另外保留图形诊断、会话、手柄和性能相关文件。

目录存在并不等于接口已经对齐。本轮额外核对 ArkTS/NAPI 边界后补回以下公共控制面：

- `ProgramOptions` / `WineProgramOptions` 的 `dxvkBackend`、`presentBackend`；产品扩展字段改为可选，WineHua caller 可直接调用。
- `LaunchParams`、`SessionEnvPolicy` 的 `dxvkBackend`、`wineLang`。
- `launchClient` 同时接受 WineHua 的 `home,d3d,dxvk,wineLang,compat` 和产品的 `home,automation,prefix,d3d,compat`。第六个参数是字符串或布尔值，可确定布局，不靠值猜测。
- `BuildWineEnv` 恢复可选语言参数，并同时生成 `LANG` 与 `LC_ALL`。非法语言回落到 `zh_CN`。
- `getProcessList` 返回 WineHua 的 `desktopShell` 和产品的 `sessionId`，形成兼容超集。`Index.d.ts` 的上游 NAPI 导出名已全部存在；旧 `setHostShadowProfile` 在启动时通过所选 D3D 后端收敛到产品 graphics policy。

具体 runtime、presenter 和 Guest 图形参数仍由产品 Native graphics resolver 决定；兼容字段不会在 ArkTS 再复制一套策略表。

## Smoke 架构

此前确有未采用项：`SmokeTypes.ets`、`SmokeRunner.ets`、`automation/run_regression.py`、`validate_frame.py`、`suites.json`、GPU diagnostics、DXVK 2.6 requirements 和 D3D12 smoke 打包链。原因是审计把“不要替换产品 UI”错误扩展成“不要采用上游 runner”。这些项目现已适配，产品页面没有改动。`clean` 运行会切换整个 broker/spawner 的 prefix 会话并在完成后恢复产品会话，避免只改子进程环境却被 broker 覆盖。详情见 `docs/SMOKE_REBUILD_20260831.md`。

## 未复制文件与产品映射

- `WineEnvService.ets` → `WineEngineService.ets` + `AppSettingsStore.ets` + `ManagedSmokePayloadService.ets`。产品已有更完整的启动、恢复、前缀迁移和图形策略状态机；再复制类会产生两个 Wine 会话所有者。
- `ProcessService.ets` → `AppSessionService.ets` + Native process registry。产品以 sessionId 关联应用卡片和窗口，并在 NAPI 结果中补回 `desktopShell` 兼容字段。
- `InputSettingsService.ets` → `InputProfileService.ets`、`InputDispatcher.ets`、`InputRouter.ets`、`InputDeviceMapper.ets`。产品保留物理手柄、触摸、键盘和每应用 profile 的单一输入状态源。
- `Box64Dynarec.ets` → `AppModels.ets` 与 `AppSettingsStore.ets` 的 Box64 preset。参数表和持久化已经由产品设置模型使用，不再引入第二份表。
- `VirtualDesktopAbility.ets` / `VirtualDesktop.ets` → `DesktopAbility.ets` / `DesktopWindow.ets` / `WineWindowManager.ets`。产品支持手机、平板和 PC 展示模式；复制上游 Ability 会产生第二个 desktop root/window owner，因此保持产品实现并对齐 Native compositor/window 接口。

这些映射是“由产品等价或更完整实现承接”，不是以 UI 不同为由跳过能力。后续上游增加状态或接口时，先扩展现有产品 owner 或增加薄兼容适配器，不新建并行状态机。

## 自动审计

`docs/sync/20260831/audit.py` 检查上游 Native 路径集合是产品 HEAD 的子集，并检查 smoke service、Want 入口和公共 C 控制面字段存在。UI 页面、组件、资源、品牌、手柄产品接口和 gitlink 仍受保护。
