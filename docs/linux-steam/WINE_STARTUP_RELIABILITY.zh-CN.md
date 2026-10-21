# Wine 首次初始化挂起、恢复与黑屏验证

2026-10-21，分支 `main_proton_steamarm64`，基线 `e5c43c4a`。本记录与首初始化修复、已有 Deck 和 Linux Steam 稳定性改动一并保存到源码提交。

## 结果

最终候选 v148 已在 USB 平板 MLR-AL10 覆盖安装。两个独立空 prefix 初始化成功，注册分别用时 55.420、52.835 秒；中断初始化后不重置该 prefix，重试注册 52.128 秒成功。原有产品 prefix 最后一次冷启动从 PREPARING 到 READY 为 9.170 秒；屏幕显示蓝色 Wine 桌面、开始按钮，并已验证开始菜单可以打开。

这些是独立隔离环境的注册实测，不以暖 prefix 启动代替首次初始化。未卸载应用，未清理生产 `.wine`、游戏库、账号或存档；未测试手机。

## 根因与实现

1. 首个 Wine PE 进程在 ntdll 中自动执行 `wineboot --init`。旧协调器随后运行显式 `--init --update`，使 wine.inf 又被强制注册一次。v146 在 120 秒截止时仍可见 Wow64 注册推进；不能把整段时间定性为死锁。新增 Wine 补丁 `0067-wineboot-single-managed-initialization.patch`，为协调器提供 `--winehua-managed-init`：自动初始化完成后，显式命令不再强制重复注册，并在注册表持久化后发布完成记录。保留旧 `--init --update` 的兼容握手。
2. 未完成环境可能已写入 `.update-timestamp`。重试仅删除这个受管缓存，保留注册表、用户文件和未完成标记，令自动初始化重新注册一次。只有精确的 Wine 成功记录且 prefix 完整，才移除 `.winehua-init-in-progress` 并继续桌面启动；普通进程退出、注册表文件已出现都不能代替成功。
3. NCP 退出回调可能先于 spawn 返回。使用 spawn ticket 和退出序号，避免登记时擦掉本次退出状态；以保留状态和 250ms 查询补足丢失通知。失败、取消和重试以代次隔离，旧协调器完全退出前不得抢跑新会话。进程创建/执行明确失败会提前结束；仍有注册 worker 存活时不能仅因外层 launcher 退出而误判。
4. 原串行等待和 ArkTS 长时间等待改为有界等待。注册阶段按用户要求保留 120 秒兜底，不再使用 90 秒；重复请求或重复阶段事件不会续期。SDK 不返回时取消业务推进并释放等待者，清理未完成则禁止启动替代协调器。
5. 本轮隔离入口发现两个测试路径错误并修正：clean prefix、Wayland socket、FEX/ICU 模块及运行时版本标记统一使用 `containers/clean/prefix`；嵌套 prefix 的 PROCESSBROKER 仍必须指向应用 files 根的 host broker。此类隔离工具错误不作为用户生产 prefix 的根因。
6. 用户反馈的全黑桌面来自隔离测试先启动默认环境、立即销毁它的竞态：旧 DesktopAbility 在根销毁后才 onCreate，XComponent 创建时 rootId=0；随后新窗口调用还被系统拒绝（201）。clean 前置检查现以 foreground guard 禁止排队打开即将销毁的默认桌面。v148 新环境实际显示、正常产品会话显示和开始菜单均已确认，不仅检查 READY 日志。

## 等待边界

| 阶段 | 上限 |
|---|---|
| 组件准备 | 120 秒 |
| 首次/未完成 Wineboot 注册与持久完成 | 原生 120 秒，ArkTS 阶段 125 秒 |
| 已完成 prefix 的启动事件播种 | 45 秒 |
| 图形准备等普通阶段 | 45 秒 |
| 原生桌面根 | 20 秒 |
| 原生整个协调器 | 150 秒，单调时钟 |
| ArkTS 整个准备过程 | 210 秒，含组件准备 |
| 旧会话清理 | 35 秒，未完成时阻止新协调器 |

120 秒是注册阶段的异常保护，不是承诺整个组件安装、引擎准备都在 120 秒内结束。已确认的错误优先于期限处理。

## 实机证据

| 场景 | 注册到持久完成 | 本次 PREPARING 到 READY | 结果 |
|---|---|---|---|
| fresh-v147-1 | 74.373 秒 | 84.577 秒 | PASS |
| fresh-v148-1 | 55.42 秒 | 65.281 秒 | PASS |
| fresh-v148-2 | 52.835 秒 | 62.591 秒 | PASS |
| recovery-v148-1 | 52.128 秒 | 60.832 秒 | PASS |

v147 用于详细诊断；v148 已关闭本轮额外 setupapi/ole trace 和 stall dump。前两轮旧诊断候选 v144/v145 在 90 秒截止；v146 在 120 秒仍未完成。v147 的注册成功与 v148 的两轮成功均发生在消除重复注册之后。

中断恢复 suite `wine-startup-interrupted-recovery` 只清理独立 clean 容器一次：Wine 写出时间戳、未完成标记仍在时调用生产 stopSession，再通过同一生产 startSession 重试，第二次不 resetWinePrefix。结果 `interrupted=true, firstReady=false, unfinished=true, recovered=true, preserved=true, initialized=true`；预置 sentinel 保留，完成后未完成标记移除。

结构化记录：`wine-startup-final-evidence.json`、`wine-fresh-v148-startup.json`、`wine-interrupted-recovery-v148.json`；带时间戳原始事件保留在 `wine-fresh-device-results.jsonl`。图像及有界设备日志为本机 private 证据。

## 自动检查与包身份

- 完整生产 WineEngineService 的 14 项启动测试通过，包括丢失终态、并发首启、不可返回前置操作、取消/迟到事件、清理超时、失败重试、隔离路由、前置检查不能打开失效桌面。
- 原生状态机、生产等待与精确完成判定测试通过；明确 dispatch 失败、已知非零 worker 退出提前结束，worker 仍活时不可误判。
- 生产进程生命周期 173 项检查通过；阶段/阻塞文本、failed-session recovery、AppSession 启动和桌面根等待检查通过。
- 0011+0067 补丁顺序重放及反向证明通过；`bash -n scripts/build_wine.sh`、`git diff --check` 通过。
- Release ArkTS/NDK 编译、内置载荷审计、Linux Steam HAP 审计通过。沿用现有增量构建；开发签名只用于覆盖测试。
- 最终 HAP：`native-steam-external-v148-debug-signed.hap`，SHA-256 `05e6d160b2a85e92d05a1c1a003054712f9438532f210567c70149ec00a5350d`。
- Wine ZIP：`51f4b051f2a0426ffa0127791e62210a7d45ef512234ebf4bd0233ee4dc3c1ec`；只替换 `bin/wineboot.exe`，其 SHA-256 `77aecc2842c942130532025431705caa2731844ac8cff4c8a5ff87e2c41aa826`。`stage_wineboot_runtime.py` 验证架构、已审核基包及模块特征，同步 ZIP、manifest、提取版本；build_wine.sh 重放新 Wine 补丁。
- GNU 部署身份 `e4c89484ef204ec08e653cd5f7ed7b9fde87856490f0ef88236c9fa9d9cfce12` 不变，Mesa gitlink 仍为 `557fa3e781ea6e78fcff3ee759c25f60c3055064`。不依赖 Windows Steam，不调整 DXVK 或帧率。

已移除本任务废弃 HAP/unsigned 副本，保留最新 v148 及 v147 回退包。31.7 MB 旧诊断日志无损压缩到约 3.6 MB 并校验后删除原始副本；固定有界日志复用。工作区维护检查无问题。

旧的 wine_language_launch_test.py 和 broker_startup_contract_test.py 仍存在前置 fixture 不匹配，未计入通过；本次没有为它们改动无关功能。Linux Steam 登录、仙剑偶发冻结及手机兼容性不在本次首初始化验证结论内。
