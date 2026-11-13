# Steam GPU 修复 1936fa6 独立复核与平板验证

评审日期：2026-10-04。对象：`feature/main_proton`，`1936fa68864dbb0ecda8ed6d571c167e14991842`。上轮评审报告基线：`87b0c5c1ec44a9700faed44217b078ea67de8533`。

当前结论：新增修复的静态复核、专项回归、完整主机测试、全新 Wine 构建、OHOS 原生层和 HAP 构建均通过。签名包已升级安装到 MatePad Mini；系统锁屏拒绝启动，交互验收等待解锁。此前“游戏有 SHM、GPU 也出帧但被 Steam 遮挡”的问题仍未在此构建真机验收，不能据此宣布解决或宣称帧率提高。

## 1. 修复评价

| 范围 | 独立复核结果 | 验证与边界 |
| --- | --- | --- |
| 绑定拒绝绕过 | 通过 | `GetLayerInfo` 的无协议角色路径只调用 `ResolvePresentBinding`；拒绝或 owner 缺失为终止结果，旧的第二次几何搜索已移除。新回归覆盖活跃占用、歧义候选和 1×1 无角色 producer；明确协议子面仍可保持 1×1。 |
| Producer 接管与代次 | 通过主机回归 | 查询不会退役旧 producer；第一帧经有效代次消费后接管，旧代次消费和退役 producer 重绑被拒绝。几何匹配仍是弱 bootstrap 路径，跨 PID 唯一尺寸匹配不是显式身份凭证。 |
| GPU 显示与输入资格 | 通过主机回归 | `IsContentVisibleLocked` 同时供场景层和输入使用，包含 SHM 或经真实消费的 GPU 内容。`ActiveOwner` 在树锁下校验活跃 key、消费记录、owner 资源、协议父关系、绑定代次、最小化/后台状态。 |
| 菜单与模态窗口 | 通过主机回归，待真机 | 覆盖 GPU-only parent 上的 SHM 菜单、GPU 模态窗口及 owner 暴露区拦截。不能用平台桩测试替代真实 Steam/游戏的显示和点击体验。 |
| 前台诊断 | 静态复核及回归通过，待运行日志 | 场景候选、当前中心 hit、绘制调用及 swap 使用同一 sample。被动 probe 不修改真实输入历史，`lastInputTarget` 是上一次指针事件，手柄按键不能直接视为指针命中。 |
| SHM 上传统计 | 通过 | bytes/calls 按实际 `glTexImage2D`/`glTexSubImage2D` 请求累计；静态纹理与 NativeImage 为零上传，失败 swap 仍保留已发起工作。计时是 CPU 提交耗时，不代表 DMA 完成；未引入 glFinish/readback。 |
| Wine 构建缓存检查 | 通过，包括真实旧目录拒绝 | 全新目录记录实际 source HEAD、gitlink、补丁序列及有效源码摘要；构建后复核通过。旧目录包含 `wine-ohos-aarch64.protonmerge` 残留旧 srcdir，明确拒绝，所检查 configure/Makefile/config.status 的 SHA256 保持不变。 |

目前没有从独立静态复核中确认新的阻断缺陷。全屏优先级策略没有改变，原 SHM 游戏遮挡问题继续作为优先验收项；GPU 消费成功本身不足以证明图像最终可见。

## 2. 测试记录

以下均在本地 WSL Ubuntu-22.04 实际执行，日志见 [评审核心证据](evidence/steam-gpu-followup-review-1936fa6-20261004/README.md)：

- `make test-gpu-followup test-egl-multi-consumer test-zc-binding-lifecycle test-steam-gpu-contracts test-frame-loop-diagnostics test-cef-render-switches`：通过。
- `make -k test`：完整通过，退出码 0（`host-test-result.json`）。包含 Wine 稳定性、实际 Unix socket broker/gamepad 等测试；作者环境中的 LSan/EPERM 阻塞没有在本次执行器复现，没有禁用 LSan 或删减这些测试。
- `gpu_owner_contract_test.py --baseline`：指定旧版本出现 3 个预期断言，分别为错误小尺寸绑定、活跃 owner 拒绝绕过和已占用候选掩盖歧义；其余兼容用例通过。
- `gpu_scene_input_test.py --baseline`：指定旧版本出现预期“GPU-only parent 显示与实际输入不一致”断言。
- `steam_gpu_contract_test.py --baseline`：Venus、binding、scene 三个历史断言均复现。
- 实际旧 Wine cache 拒绝与文件保全检查：通过，见 `old-cache-rejection.log`、`old-cache-preservation.json`。

主机测试是生产代码加平台 I/O 桩或确定性提取测试，验证逻辑合同；并非 OHOS 驱动、Steam CEF、游戏交互或性能基准。

## 3. 构建、签名与包身份

全新 Wine `BUILD_DIR`：`/data/src/winehua/build-gpu-followup-1936fa6-20261004`。没有复制旧 Wine configure、对象或身份记录，没有跳过检查。仅通过明确符号链接复用未改动的依赖和 FEX/DXVK/guest runtime 产物，详细清单见 `build-fresh.sh`；Wine native tools、OHOS Wine 和 wineserver 均从当前有效源码编译。

- Wine source HEAD 与活动 gitlink：`cd547f7a0ee9d3d59b3ec47317a7852dedf0443b`，本次 manifest 中两者均实际核验。
- 新目录 source/configure 身份记录及构建后 `--check-cached-identity`：通过。
- OHOS 原生层与 ArkTS/HAP：`BUILD SUCCESSFUL`，见 `hap-build.log`。
- 包含新 `[SCENE-DECISION]`、`[SCENE-CANDIDATE]`、`[SCENE-DRAW-ISSUED]`、`[SCENE-SWAP]`、`upload_calls_issued=` 标记。
- 7 个 HAP 库与当前剥离产物字节一致；Wine 关键库与全新目录到 `entry/libs` 的输入一致；5 个改动 native 编译单元对象 SHA256 已记录。
- HAP runtime closure：`wineArch=aarch64` 通过。
- 签名和 `verify-app`：通过。第一次使用旧通用材料解密失败，随后使用本地配套 `debug-sign` 材料成功；临时签名材料已移除，未将密码或私钥放入证据包。

包：`VintagePomeloPro-proton26-followup-1936fa6-debug-signed.hap`，358,167,485 bytes。

SHA256：`9af9a07b0f5c84d2bac68ea70613c9258ec56b69033993dcdd635488bafadd48`。

版本：`1.4.5-proton.26-alpha` / `1004035`。与上一轮版本号相同，应以 commit、HAP 哈希和新运行日志标记区分。本包为 Debug，主应用 native 为 `-O0`，Wine 为 `-O2`；用于正确性诊断，不作为 Release 性能结论。

## 4. 未签名 HAP 交付

本次另按用户请求重新执行 `package.sh hap-unsigned`，Wine 身份检查和 HAP runtime closure 均通过。SDK `verify-app` 明确报告 `signature not found` / `No Hap Signing Block before ZIP Central Directory`，与未签名状态相符；ZIP CRC 校验通过。

- 文件：`VintagePomeloPro-proton26-1936fa6-arm64-debug-unsigned.hap`。
- 版本：`1.4.5-proton.26-alpha` / `1004035`，ARM64 Debug。
- 大小：356,363,292 bytes。
- SHA256：`5caeb4f947ea83061fe4d36337d706cd3f7f692f7db3a934167f0c40960f46f7`。
- 7 个关键库与已测试签名包及当前剥离产物字节一致；WSL 构建产物与 Windows 交付副本 SHA256 一致。
- 本地交付：`F:/VintagePomelo-Workspace/workspace_temp/proton26-1936fa6-unsigned-20261004/`。

身份、构建和验证日志见核心证据的 `unsigned-hap/`。HAP 二进制不进入源码仓库。

## 5. 平板状态与验收协议

设备：HUAWEI MatePad Mini / MLR-AL10。包：`com.vintage.pomelopro`。已保存升级前现场，停止旧会话后采用 `install -r` 升级，没有卸载或清除数据。HDC 输出为 `install bundle successfully`，`bm dump` 核对版本为 1004035 / proton.26。

启动被系统拒绝：`10106102` / `The device screen is locked during the application launch`。这不是 Wine/CEF 崩溃。已请求用户解锁，未将启动命令退出码误报为成功。

解锁后将使用与上轮相同的 GPU 启动参数、DXVK legacy、1200×800 和 frameDiagnostics，不改动原 `1.bat`。

待验收顺序：

1. Steam 大屏稳定导航，打开常用菜单/弹窗，用户确认完整显示且操作生效。
2. 从 Steam 启动此前被遮挡的《千恋万花》，记录游戏首次可见状态；持续对齐游戏/Steam 的 candidate、fullscreen priority、currentHit、draw-issued、swap 与截图。
3. 游戏 → Steam → 游戏往返，用户按手柄或使用已有切换入口；若需验证指针路由，再做明确点击并检查实际事件。采集期间由用户操作，自动化不混入测试输入。
4. 再打开菜单/模态弹窗，验证显示和输入归属一致。

关键判据：游戏 producer 有消费记录、游戏所在层为 included、实际发起绘制、swap 成功以及截图/用户确认可见要分开记录。如果游戏仍被遮挡，先定位它是被 fullscreen-cascade 过滤、选错 priority、后绘制层覆盖，还是原生窗口/输入焦点问题，再决定修复位置。

## 6. 暂存结论与下一步

可确认：本次修复通过独立逻辑回归，并完成此前缺失的全新 Wine、OHOS/HAP 构建、签名和升级安装。

尚不能确认：此前有 SHM 的游戏遮挡已解决、真机菜单与输入全部正常、GPU 崩溃完全消失、Steam 大屏帧率提高。上轮 Overlay/wallpaper 的 SIGSEGV 不因主机回归通过而自动关闭；需新会话的退出原因和首个致命栈证明。

推进建议保持“先多窗口显示/输入正确，再追帧率”：这次新增观测点足以缩小前台遮挡原因。性能比较应另用 Release 构建和同交互、同缓存状态的稳定场景进行，分开报告宿主 swap、GPU 消费与实际内容更新频率。

## 7. 证据交付与后续验收

本提交仅补充独立评审、构建/身份/测试/升级证据。修复代码及其新增回归已经在被评审提交 `1936fa6`；本轮没有额外业务修复。导入仓库的日志统一为 LF、去除 ANSI 控制符和行末空白，仓库 SHA256 覆盖导入后的字节，原始材料哈希另存 `source-artifacts.json`。

完整本地材料：`F:/VintagePomelo-Workspace/workspace_temp/gpu-followup-review-1936fa6-20261004/`。原始 ZIP SHA256：`f4959f0b2ab29d25195949c8cc1fa7c9c08f2332c0c3a941dfc08217ccf40b0f`。ZIP 包括评审阶段 60 个文件的目录清单，HAP 另提供；后续未签名构建证据独立位于上一节的交付目录。

本轮平板交互验收仍未执行，保留锁屏阻塞和待测项目，不用旧会话日志补充为新版结果。
