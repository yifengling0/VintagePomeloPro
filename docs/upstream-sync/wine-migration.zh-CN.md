# Proton Wine 完整迁移与 Mini 局部验证

核验日期：2026-11-13。工作分支：`feature/fex-proton-upstream-sync`。
本次完成完整 Wine 候选的迁移、构建、成品校验和 Mini 局部运行验证。
生产 Wine/FEX pin、`main`、历史标签均未更换；未触发线上 CI。

## 固定来源与适配保全

| 内容 | 固定身份 |
| --- | --- |
| Proton Experimental | `0764d2920a128cee74555afcab0819919ddf416d` |
| 完整 Valve Wine | `e8ca5806eec488f5916e83d655ffc5349b6b1702` |
| 上游文件树 | `8ef394afdf44f3d55c8ed79ecfc26dbf32a454df`；11,170 文件、331,571,967 字节 |
| OHOS 等价旧基线 | `a02acbd358e3f2d82eb4d6e0375dfbff0e5e0131` |
| 新 OHOS 候选 | `098ece0eedff40bea426d2d57b215bb81411e239` |
| 新候选文件树 | `4775755262105ab9381128cadd142ad3c92f1b6b` |
| 保持的生产 Wine pin | `cd547f7a0ee9d3d59b3ec47317a7852dedf0443b` |

79 项原 OHOS 主题/补丁均已逐项迁移。另加生成文件更新和独立构建修复，共 81 项。
`wine-migration-ledger.json` 记录原提交、新提交及冲突决策；锁文件登记每份补丁 SHA-256。
`wine-adapted-history.json` 保留原始 commit 对象，以免仅重建相同文件但丢失真实父系。
实际从上游 tree 重放 81 项后，最终 tree、每个父提交和最终 commit 均精确一致。

需要改写的共同修改点：

- `signal_arm64.c` 保留上游对旧 APC/suspend worker 的回滚，不恢复被删除的线程机制。
  上游依据：`c97ffea75a57f298d235e0c10dcf7c520d6446df`、
  `cc17cbcb13e3a8fcdcbd2e272cc77f7b9c601f2a`、`8cd623d83f4832fa9b002e37df4a72de2b8f7052`。
- Unix bridge 保留新上游接口和 lookup，同时保留 OHOS basename dlopen namespace。
  私有查询类使用新名称 `MemoryWineLoadUnixLib`，数值保持 1000/1001，匹配 FEX。
- WOW64 自线程 context flush 保留 duplicated-handle 身份判断与错误传播；其他线程用新上游接口。
- Vulkan driver 保留上游六个 timeline semaphore 回调顺序，再追加 OHOS 回调。
  Unix driver、win32u、winevulkan 和 PE 必须成套更新。
- X11 保留新上游 `state_locks` 和 OHOS per-window frame extents cache。
- Vulkan XML 已与 Vulkan-Headers `v1.4.357` 的官方 blob 核对；重生成 protocol、Vulkan、configure。
  八项 Vulkan 生成输出逐字节一致，WOW64 映射恢复、alias/readback、guard/overflow 回归通过。
- Vulkan smoke 的共享诊断头文件纳入 Wine 自有源码，解除旧父目录布局依赖。

Linux Steam 产品路线和已移除的 Linux 桥接没有重新引入。DXVK/VKD3D/独立 ICU 等依赖保留现有 pin，
不能将本次交付称为整套未修改的 Linux Proton。

## 可还原的本地入口

权威源码使用 WSL `/home/liufeng/src/vpp-account-cloud`。先按
[旧基线重建说明](wine-baseline-reconstruction.zh-CN.md) 准备独立对象库；再执行：

```sh
python3 scripts/prepare_wine_upstream.py \
  --maintenance workspace_temp/v2-development/upstream-transition/wine-adaptation.git \
  --source workspace_temp/v2-development/upstream-transition/wine-candidate \
  --cache workspace_temp/v2-development/upstream-transition

python3 scripts/build_wine_upstream.py \
  --source workspace_temp/v2-development/upstream-transition/wine-candidate \
  --build workspace_temp/v2-development/upstream-transition/wine-build \
  --dependencies workspace_temp/source-integrity/checkout/build \
  --llvm-mingw .temp/llvm-mingw-20260826-ucrt-ubuntu-22.04-x86_64 \
  --container vp-account-cloud-build-20261012 --stage full --jobs 8

python3 host_tests/wine_upstream_candidate_test.py \
  --source workspace_temp/v2-development/upstream-transition/wine-candidate
```

准备入口核对完整 Git tree、每个缺失 blob、原始上游 commit 和 81 个适配提交。
重新执行复用已有对象，本轮最终准备检查无需下载。已有 worktree 不符或有未登记修改时失败，保留现场。
主机回归覆盖真实迁移函数、精确历史重放、篡改拒绝及不修改工作树，共 4 项通过。

构建复用依赖安装和隔离增量目录，不发布到生产 `entry/libs`。
输入包含源码 tree、构建脚本、编译/链接/strip 工具及依赖文件哈希。
输入变化先清理候选缓存，重新配置；失败保留 pending 状态，成功后才提交输入记录。
日志循环保留末段，单文件最多 32 MiB。核心编译通过后已完成全量编译，
并实测输入变化后的完整重建；最终约 311 秒，登记 1,596 个构建输出。

## 成品与设备证据

`package_wine_upstream_candidate.py` 基于已验签的 FEX 候选制作组件测试包：

- 更新 32 个 Wine 原生库、PE DLL/程序和相应数据；登记 1,659 个打包输出。
- 独立 ICU、FEX、双架构 Gecko、DXVK、VKD3D、GStreamer 依赖、字体资源保留。
- 验证 WineD3D 修复、Gecko 源码配对与真实 DLL、ICU、FEX、音频、线程退出及动态依赖闭包。
- 使用新 Wine INF，复用产品字体替换块，按最终内置 DLL 内容更新前缀升级身份。
- 更新运行时、组件来源与内容哈希；逐成员核对未修改的 HAP/ZIP 内容。
- 本次未发现需要移除的旧 Wine 模块；新增输出与来源在验证记录中单独登记。

测试包：`VintagePomeloPro-2.0.0-wine-098ece0e-mini-internaltesting.hap`，629,394,268 字节，
SHA-256 `e2d91b472839e9ab6d309ef0541189a2ed8a3726b2923721f287cf615e9e23b8`。
release/internaltesting profile、证书配对、签名及签名前后载荷核对通过。
设备为 USB MatePad Mini MLR-AL10，覆盖安装，未卸载或清除数据。

Mini 实测：

- 从普通 HmosDeck 按钮启动 Steam，登录保留，游戏库和游戏列表显示。
- 选中 Resident Evil 2 后，约 4 秒的观察截图内出现头图和操作按钮；未点击运行或串流。
- x86/x64 探针均完成；前缀内两种架构 ntdll 与 runtime 匹配。
  设备 ntdll/win32u/winevulkan/wineserver 及 FEX 四组件哈希与成品记录一致。
- `steam.exe -shutdown` 后 Steam/CEF 消失，保留共享 Wine 服务；再次启动能进入 Steam。
  第二轮退出曾出现执行访问异常并启动 `winedbg.exe`，因此不能视为干净退出验收通过。
- 重开出现商店 `-118`，转入库主页可用。此前 FEX 候选也出现过同类超时，原因仍未确定。
- 有限 CPU 样本：重开加载阶段 CEF 约 45–47%；库页稳定后约 7–12%，100% 表示一个核心。
  两阶段不等价，没有同 base 交替 A/B，不能据此宣称上游更新改善了性能。

首次启动包含全套运行时解压/前缀升级，且观察是离散截图，不作为精确启动性能数据。
探针的历史 loader 日志与空 self-maps 不作为本次映射证明；只采用当前查询和文件哈希。
本轮没有启动游戏、串流或读取存档，也没有重复完整会员、媒体或游戏兼容性验收。

### 退出异常对照

首次候选第二轮退出时记录到：

```text
Unhandled page fault on execute access to 0000000053704740
at address 0000000053704740 (thread 0658), starting debugger...
```

时间为 2026-11-13 16:46:42（UTC+8）。调试器的 Windows 父 PID 为 1620，
同一日志中该 PID 使用 ARM64EC FEX；CEF 是待验证的来源，尚无异常线程栈证明。
进程消失不能替代真实退出结果；`cleanExitAccepted` 保持 false。

随后在同一 Mini、同一登录和 Steam 数据上覆盖安装“新 FEX＋旧 Wine”包，
从普通 HmosDeck 入口进行两轮启动、选中 Resident Evil 2（未运行/串流）、`-shutdown`。
两轮均确认 Steam/CEF 消失、共享 Wine 服务保留，没有 `winedbg.exe` 或同类退出异常。
组件探针确认旧 Wine 已生效，避免将签名回执误当作当前设备版本。

再次覆盖安装本次新 Wine 候选后，重复两轮启动/退出也没有复现同一异常。
第一轮尝试对 CEF 使用有界诊断附加，返回 `ERROR_ACCESS_DENIED`（5），未取得线程现场；
随后仍通过原有 `steam.exe -shutdown` 退出。第二轮未附加调试器。
这组有限结果不足以证明是 Wine 升级回归，也不能证明偶发问题已修复。
最终 Mini 保留新 Wine 候选，Steam 已退出，页面返回 HmosDeck，前缀和登录未清除。

`host_tests/probes/fex_component_probe.c` 的 `WINE_COMPONENT_PROBE` 开关核验 Wine 组件；
另加 `WINE_EXIT_PROBE` 时仅查询 Steam/CEF/调试器进程，并读取当天 stderr 的最后 256 KiB。
这两个开关均未加入产品载荷，不改变权限或异常处理；x86/x64 版本均在 Mini 完成执行。
本地保留原异常、两组对照及最后组件探针的小型证据，机器记录保存 SHA-256。

机器可读记录：[wine-mini.json](validation/wine-mini.json)、
[wine-full-build.json](validation/wine-full-build.json)、
[wine-build-inputs.json](validation/wine-build-inputs.json)、
[wine-package.json](validation/wine-package.json)。

## 后续验收

优先定位偶发退出异常；保留异常时间与进程身份，并在可取得现场时关联线程和代码映射。
再用相同前端、相同 Steam/缓存/网络和画面状态进行旧/新运行时交替 A/B，
拆分解包/前缀升级、Wine 启动、CEF 初始化与网络等待；继续定位商店 -118 和重开延迟。
游戏、音视频、输入与长期生命周期需要独立验收，之后才决定是否提升生产 pin。
继续保留“新 FEX＋旧 Wine”测试 HAP 作一份回退，移除本次重复 ZIP、unsigned HAP 和设备探针临时文件。
