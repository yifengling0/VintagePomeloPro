# FEX x87 默认性能策略与真机验证（2026-10-07）

用户要求将已验证的 `FEX_X87REDUCEDPRECISION=1` 设为默认，以测试其他游戏。本次在 ARM64 原生 Wine/Proton 的共享环境基线设置它；主进程和独立 Wine 子进程使用同一份默认值。启动参数 `FEX_X87REDUCEDPRECISION=0` 可覆盖为严格 F80；Windows BAT 也可为其游戏子进程设置此变量。切换时完整结束 Wine/FEX 会话再启动。

## 本次代码和包范围

- `entry/src/main/cpp/wine/wine_env_baseline.h`：ARM64 原生 Wine 默认 ReducedPrecision=1。
- 当前产品的 `EntryAbility.ets::parseAutomationEnvironment` 已接受该变量，无需新增 ArkTS 白名单。旧 `GameHook` 不在当前产品编译入口中，保持其原样。
- `entry/src/main/cpp/proc/wine_child.cpp`：覆盖应用后记录最终值，便于确认配置。
- 重建 `libentry.so`、`libwine_child.so`；正常 `assembleHap` 也通过，产出的 ArkTS 与基线包哈希相同。基于已验证的 clean HAP 仅替换这两份 native 库；ArkTS、Wine、FEX、Mesa 和运行时资源保持基线包字节身份。包哈希、签名与设备验证记录见随附 `package-identity.json`、`device-validation.json`。

未启用 preserve_all，也未修改 TSO、SMC、exact-store 或图形画质策略。x64 Wine/Box 的环境基线不添加此默认值。

## 已有 PAL4 开关对照

同一 FEX DLL（SHA256 `002a4a3a399db0743b0563d53ab7880b1d9bfa32ae19b7f513256a1938cbadfa`），每轮完整重启进程；读取同一个第二格存档，固定青鸾峰木屋外场景，计时诊断关闭。

| 模式 | 宿主新画面 FPS 均值 | 确认读取到识别场景的截图时间区间 |
| --- | ---: | ---: |
| 严格 F80 | 33.56 | 28.45–31.72 秒 |
| ReducedPrecision 第一次 | 66.88 | 6.34–9.50 秒 |
| ReducedPrecision 第二次 | 67.18 | 6.30–9.53 秒 |

第四轮末尾截图已是另一款塔防游戏，故其 9.05 FPS 不能归入 PAL4 对照；原始数据保留并标记排除。前两轮 `policy-eval-*` 为流程校准，也未计入正式结果。

FPS 来自宿主约每秒的新到画面统计，不能代替逐帧 Present 延迟；加载终点为截图识别，并非可操作时间。缓存与温控未独立控制，结果不能直接推广为所有游戏翻倍，也不能据此拆分 F80 运算与 helper 调用边界各自的成本。

## 配置生效与兼容性

已先用 PE32 探针验证 PC64 下 `(1+2^-60)-1`：严格模式保留 `2^-60`，Reduced 模式得到零。这证明运行时行为发生改变，不只是环境字符串不同。默认包已安装到 MatePad Mini，并完成直接启动探针的两轮验证：无环境覆盖时 `requested=1, observed=reduced-f64`；显式 Want 0 时 `requested=0, observed=strict-f80`。两轮之间完整结束 Wine/FEX，均未使用设置策略的启动器。结果见设备验证文件。

ReducedPrecision 用原生 F64 路径替代严格 binary80 x87 运算，精度和指数范围会变化；其他游戏的画面、逻辑和长期稳定性仍需逐款测试。若出现兼容问题，显式设 `0` 回到严格模式并重启会话。

Valve 公开 ARM Proton 的同类配置：
https://github.com/ValveSoftware/Proton/blob/5b89db940e0ebe3a137a6009a3589232fe084c09/FEX_Config.json

原始采集：`F:/VintagePomelo-Workspace/workspace_temp/fex-x87-policy-validation-20261007/`。本次构建与安装产物：`workspace_temp/fex-x87-default-20261007/`。正式摘要位于 `docs/architecture/evidence/fex-x87-default-20261007/`。

交付包 SHA256：`1416cdb026f24b909120f83ef859edf832fc9cb659bc057911da67a7779de4ab`。`env_baseline_test` 88 项、`env_spec_test` 21 项全部通过；ARM 基线/严格覆盖与旧 x64 基线隔离检查通过；官方签名验证通过。真机验证结束后已回到普通应用入口，未发送游戏或 Steam 启动命令。

## 64 位补充验证

PE64 直接探针也已在真机通过：无覆盖时 requested=1/observed=reduced-f64；完整重启后显式0为 strict-f80。ARM64EC 与 WOW64 共用上述环境基线，默认均为1；未增加其他性能开关。精简证据与桌面输入修复见 desktop-start-menu-input-fix-20261007.md。本轮没有测64位游戏帧率，不应将32位PAL4结果推广到64位游戏。
