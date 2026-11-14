# 2.0.1 手机 Wine 初始化失败

## 用户日志定位

两份用户日志记录于 2026-11-13 21:57 至 22:58。原生日志中 14 次 Wine 服务创建均返回 `createStatus=16010004`，子进程 PID 为 -1。NDK `native_child_process.h` 将此值定义为 `NCP_ERR_MULTI_PROCESS_DISABLED`（多进程模式已关闭），动态库加载失败则是另一个错误 `16010007`。

应用已经识别 `deviceType=phone`，NAPI 设置 `setPhoneMode=true`，VirGL 也实际进入 phone in-process 路线。资源解压、前缀骨架和 745 个 WOW64 文件准备完成后，创建 wineserver 的步骤才被系统拒绝。故障早于 Wine/FEX 和游戏执行；图形后端切换及重复清理前缀不能修复此调用错误。

## 根因与修复

发布于 `e58cb6e5` 的 ARM64 `libentry.so` 导出了与 SDK 相同的 `OH_Ability_StartNativeChildProcess` / `OH_Ability_CreateNativeChildProcess`。内部调用仍存在 `R_AARCH64_JUMP_SLOT` / `GLOB_DAT` 动态重定位。系统库较早进入符号查找范围时，会直接使用系统实现，绕过应用中根据手机状态选择 fork 的包装器。

改为应用自有且具有 hidden visibility 的 `WineHua_*NativeChildProcess` 入口。Wine broker、Xwayland/输入法子进程、图形服务和诊断启动器显式调用该入口。手机选择现有 fork 实现，平板继续显式解析系统库接口；诊断中有意测试系统 NCP 的分支仍保留。手机模式使用原子变量跨线程发布。Broker 日志区分 `phone-fork` 和 `system-start`。

## 回归与成品检查

- `host_tests/ncp_dispatch_linkage_test.py` 使用真实共享库和宿主动态链接器，在系统库先加载时复现旧设计的 16010004；新入口在系统先加载、应用先加载和系统库缺失三种情况均符合预期。OS 和 fork 实现为测试替身，此项不代替手机执行验证。
- Broker 启动合同与真实 socket/FD 传输测试、显示 FD 所有权测试通过。
- 打包保护拒绝导出 SDK NCP 覆盖符号的 `libentry.so`；9 个保护测试通过。原 2.0.1 上架宿主被拒绝，修复宿主通过。
- 在已有干净构建检出的增量目录构建，最终 ELF 中应用入口为 `LOCAL HIDDEN`，没有 SDK 同名覆盖导出或相应动态重定位。
- 相对原 2.0.1，272 个 native/rawfile 成员只改变 `libentry.so`；271 个成员的 SHA-256 一致。37 个上游组件锁继续通过，未回退 Wine/FEX。
- 第一轮 M80 测试使用了 release/internaltesting 签名。进程创建已变为 `createStatus=0 mode=phone-fork`，但 wineboot 以 1 退出，不能视为初始化验收通过。仅将模块 `debug` 字段改为 true 也没有将其变为 debug profile。

## M80 真正调试签名验证

按用户要求改用真正的 debug profile。签前核对 profile 类型、有效期、M80 设备覆盖、`ohos.permission.kernel.ALLOW_WRITABLE_CODE_MEMORY`、development certificate 与本地证书匹配；签后验证签名、嵌入 profile 及完整应用载荷。已有 internaltesting profile 也含此 JIT ACL，因此不能把此前失败简单归因为 profile 缺少 ACL，更不能把带 ACL 的 release 签名当作手机调试签名。

测试包仍为 2.0.1 / 2000010，包含同一修复宿主和本轮经典界面修复。M80 (VYG-AL00) 真机结果：

- 23:47:40，wineserver 和 wineboot 经 phone-fork 创建成功；23:48:41，wineboot 以 0 退出，记录 `wineboot durable completion verified`，随后进入有“开始”任务栏的 Wine 桌面。
- Wine stderr 中有 `[WX-MPROTECT] ... jit_rc=0 first=OK`，证明本轮实际 JIT 内存申请成功。
- 经典界面实际显示“当前阶段：初始化 Wine 环境（已 21 秒）”，后续读秒继续增长；HmosDeck 渲染分支未改。
- 强制退出应用后 `pidof com.vintage.pomelopro` 为空。重新打开并从 Wine 桌面卡片启动，23:52:16.801 启动 wineboot，23:52:19.614 完成已有前缀检查，桌面再次出现。
- 随后用户自行在手机上打开 ComputeMark；此时停止自动输入并保留现场。该画面不算本任务自动完成的图形压力测试。

手机启动和重启验收通过的是 debug 签名包，不代表修复后的 AppGallery 签名包已完成手机验收。

## 经典界面修复

经典启动反馈原先覆盖 Wine 引擎的真实状态，并固定显示“游戏窗口即将出现”。现在准备和重建期间优先显示引擎标题、消息、当前阶段及累计秒数；引擎就绪后再显示等待游戏窗口。错误态保留明确错误和重试操作。扩展现有进度测试，执行实际 ArkTS 方法，覆盖带 launch feedback 的准备、重建、就绪和失败状态。

## 源输入

- 原应用日志 SHA-256：`2d9fff4cce3ffa8f1b6da95f1c8777f29358898aac5a0a8c2092cca34de2e641`
- 原原生日志 SHA-256：`3057a3b211f80aea0af63ede0e7a89f169979dcd3c7f4090e7fa3f31e4a2986c`
- 原上架宿主 SHA-256：`1dada56788c034b054df32dee16d39ba6bf19311beea50ae2e388b6035b3ab53`
- 修复宿主 SHA-256：`b33f3ae7ced9960985975653f7f9a40b9f0a69819fb47b048d05085c57e32b9a`
- 首轮 internaltesting HAP SHA-256：`b280fb0ecb3f4ff6efc95df7cd6f5eafc861babe16f91d0cdc2539b36107c873`
- 已通过 M80 冷初始化及退出重启的 debug HAP SHA-256：`4cfcaf7468e6a36f204e2bdffdc8806dbc059885c8536a7afd5bd587ecd0d153`

未触发线上 CI，未替换历史上架 APP 或移动标签。完整的构建输入、包核对及签名记录保存在固定任务目录 `workspace_temp/v2-development/phone-init/` 与已登记的测试包目录。
