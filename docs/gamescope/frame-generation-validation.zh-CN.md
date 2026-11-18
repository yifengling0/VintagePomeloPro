# 免费 2× 插帧实施检查点（2026-11-18）

本报告记录可安装的第一份通用插帧调试包。**实现、主机验证和包校验已完成，设备画质与性能尚未验收**。基线 `main@d8128446`，工作分支 `feature/gamescope-framegen`；版本保持 2.0.5，不合 main、不推送、不上架。

## 源码与交付

| 提交 | 内容 |
| --- | --- |
| `cb04e2b6` | 固定 Win-FG 计算核心、GPU 包装、可重放补丁、源码锁、许可和 GPU 行为测试 |
| `639332f7` | GPU 历史与 NativeVSync 调度、源序号去重、旁路、同步和原生接口 |
| `2046febc` | 免费全局/单游戏/运行侧栏开关、状态与性能浮层、模型回归和包许可校验 |

包对应源码 `2046febc1e32b25bb104b5e48fd4fa643aeb3747`。随后仅记录验证的文档提交不要求重新编译。

- 调试签名 HAP：`workspace_temp/gamescope-port/VPP-2.0.5-gamescope-debug-signed.hap`。
- 包大小：634,079,548 字节；SHA-256：`e167068badbea7d6d8246f74941927842fc13214409b5c9dd13245b7eb110bcd`。
- 同版本回退：`workspace_temp/gamescope-port/VPP-2.0.5-gamescope-debug-previous-signed.hap`，633,928,457 字节；SHA-256：`5cf02b422709474489bc7468873554f52dfaa647386e91ce0530f5976f582291`，源码 `59d87b0c`。覆盖回退保留应用数据，不卸载。
- 本轮未替换 2.0.5 正式未签名 HAP或 2.0.4 正式交付包。

当前设置默认关闭；通过 Game 设置或运行侧栏选择“2× 插帧”。手机不依赖 NCP，三端入口不接入会员、账号或购买判断。

## 验证证据

1. OHOS ARM64 Gamescope 核心、app-native 和 CompileArkTS 成功。签名/Profile 校验通过；版本 2.0.5 / 2000050，bundle 为 `com.vintage.pomelopro`，仍声明 phone/tablet/2in1。
2. 11 个 shader 源码重新编译并通过 SPIR-V 校验；主机 lavapipe 实际执行所选 model 3 计算图。常量色阶保持；运动物块插值到中间位置；130×96 非块对齐尺寸、场景突变回退、五轮引擎生命周期、descriptor 失败注入和独立实例通过。
3. 调度、显示节拍、嵌入式 ABI、配方 7 项以及 Game/Steam/剪贴板/Direct 行为回归通过；新增 VSync 请求限频断言，防止等待插帧时 1 ms 循环持续 IPC。
4. 最终包逐成员与回退包比较：没有删除成员；只增加两份许可。变化为 `.pages.info`、`ets/modules.abc`、`libentry.so`、`libgamescope_core.so` 和 `application-build.json`。Wine、FEX、DXVK、Mesa及其他既有资源 SHA-256 均保持一致。
5. 包内 core/entry/child 的 ELF 已分配节与当前构建产物一致；core 原始库 SHA-256 为 `a6d490198fc8351c2b6571f96dbc165d07fb38f717d89bac92005b0653963a65`。应用源码收据与包内记录一致，保留 WSL 原有 dxvk 本地修改的有效源码记录；不宣称干净 CI 来源。

任务证据位于固定目录 `workspace_temp/gamescope-port/`：`framegen-package-audit.json`、`framegen-host-gpu-result.json`、`framegen-host-tests.log`、`framegen-app-source.json` 和 `package.json`。可重放审计脚本为 `audit-framegen-package.py`。

## 设备与剩余工作

- Mini 的鸿蒙 AI 查询已有应用内实测：当前系统返回不支持，详见 [实现说明](frame-generation.zh-CN.md)。本包采用通用后备；AI 系统送显尚未接入。
- 本轮设备最近前台为 HSL，已询问共享占用但尚未确认；没有覆盖安装或中断它，也没有新的真机插帧性能结果。
- Mini 待测 PAL4、Heaven、DX11 与已有后端；M80 待测同进程插帧、旋转和后台恢复；PC 待测窗口与焦点场景。尚不能宣称三端全部可用或达到 70% 呈现增益。
- 首轮限制客户区面积不超过 1920×1080；预算/能力不足则显示原因并保留真实帧。场景切换、游戏 HUD 重影、GPU 预算阈值与同队列延迟需按真机结果调整。
- 下一阶段执行同后端、同渲染和输出尺寸的关闭/开启各 3×60 秒对照，真实 FPS、有效生成 FPS和呈现 FPS分别记录。主机 GPU 测试不替代这项评价。

最终清理结果与 F 盘空间记录在任务 `framegen-storage.json` 及工作区 `logs/storage-maintenance/latest.json`，只保留当前与上一份调试 HAP及必要小型证据。
