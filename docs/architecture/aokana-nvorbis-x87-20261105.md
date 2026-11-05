# 苍之彼方：NVorbis 与 FEX x87 精度诊断

在普通平板、1.5.6（1005060）、原 OHOS Wine/FEX、DXVK 1.10.3 下测试用户提供的 PE32 Unity 2018.2.19f1 游戏。没有修改游戏二进制。

## 已确认的数值兼容问题

同一游戏文件、运行库及设备，默认 `FEX_X87REDUCEDPRECISION=1` 停在 sprite 标志，首个异常是 `NVorbis.VorbisCodebook.Init` 的 `InvalidDataException`，调用链经过 `AssetIO.FileReadOgg` / `ScriptOps.Op_voice`，随后反复出现 `VorbisStream.Finalize` 的空引用异常。

仅在新游戏进程覆盖 `FEX_X87REDUCEDPRECISION=0` 后，未出现该异常，观察到 OP 的不同画面，并自动返回中文主菜单。恢复默认值反测，再次出现相同异常（本轮日志中 InvalidDataException 1277 次、NullReferenceException 1271 次）。这是缩减精度与游戏启动异常的正反证据；尚未定位 FEX 中具体的运算指令，不能宣称修好了 FEX 的缩减精度实现。

本轮严格精度日志确认原生 GStreamer 插件 76/76、必需元素 6/6，使用原生解码器。并非凭 OP 不播放就认定媒体库丢失。1.4.4 是旧 x64 Wine/Box 路线，本轮没有对 1.4.4 做设备 A/B。

## 用户可选的兼容策略

游戏设置 → 兼容性 → **FEX 严格浮点精度**：

- 按应用保存，旧配置默认关闭；关闭不覆盖共享的性能默认值。
- 仅原 OHOS Wine 的 FEX 路线注入 `FEX_X87REDUCEDPRECISION=0`。Box64 忽略此选项，GNU Proton 的设置不受此处控制。
- 通过首页游戏启动流程传递到实际游戏进程；完全退出旧游戏，再从首页重启生效。启动器的子进程沿用该进程的 Unix 环境。
- 使用现有的设置克隆、持久化及游戏目录配置导出流程，不按游戏名称、文件名或哈希强制打补丁。
- 严格 F80 会增加软件模拟开销，可能降低帧率、拖慢加载；不能因此将全局默认改成 0。已有 PAL4 性能和 GNU PAL1 音频记录见 `fex-x87-default-20261007.md` 与 GNU Proton 实验文档。

Box64 使用不同的浮点实现，理论上可以规避 FEX 特有问题，但不等于该游戏整体兼容。本轮单独选择 `wowbox64.dll`、DXVK 1.10 的初测停在白屏，没有完成 OP 验收。补测 Box64＋WineD3D/VirGL 显示 Unity 的 `Failed to initialize graphics / InitializeEngineGraphics failed`，这是另一条图形初始化失败，不能用它证明 Box64 的浮点实现有错，也不能承诺 Box64 已可替代。

## 验证边界与未完成项

- 用户暂时不方便听测，因此扬声器声音、音质未验收。
- 用户另反馈画面不更新。已观察到 OP 不同帧及返回主菜单，但交互与持续更新需继续核验，不能据此称游戏全部正常。
- 曾尝试桌面 BAT 设置严格精度。NCP broker 目前从 Unix `environ` 转发环境，BAT 修改的 Windows 环境并不等价于修改 Unix 环境。子进程原生日志仍为默认 1，不能将该快捷方式作为经过验证的最终入口；现有首页进程环境覆盖为可信路径。
- 没有全局回退精度、替换游戏 DLL 或改动用户存档；这不是对其他游戏 OP 故障的自动归因。

## 本轮代码检查及交付状态

设置默认值、旧配置克隆、JSON 往返、FEX/Box64 隔离和实际 `AppSessionService` 游戏启动传参测试通过；完整 ArkTS/HAP 编译及候选签名验证通过。广义模型测试脚本运行到既有 `run_native_contract_unit_tests.cjs` 时，因测试桩未提供 `./LinuxProtonDxvk` 依赖失败；该测试文件未在本轮修改，不能宣称整个测试集合通过。

候选包安装被设备以 `9568288 / insufficient disk memory` 拒绝，保留了现有已安装版本、全部游戏和存档。因此新设置页的实机操作、保存重启及从首页手动启动验收尚未完成，不能将主机测试替代实机验收。调试包与校验记录保留在 `workspace_temp/aokana-video-packages/`。

原始有界诊断保留在工作区固定任务 `workspace_temp/aokana-video/`，原始日志和设置文件不提交。归档 SHA-256：`b726c2dc83a2561c566c3ffab394a668a2f46535829b2a65c9c9cf3400166274`；游戏 EXE SHA-256：`9c6a938189d4e18dfdbd1891204e35d7b3ed1f8325f24386d354b5838721eb91`。


## 上游已有方案调查（2026-11-05）

用户决定暂缓 Box，集中验证 FEX。以下为公开源码、issue、维护者评论和提交的交叉核对，未向上游发送报告。

- [X87ReducedPrecision 官方说明](https://github.com/FEX-Emu/FEX/blob/main/FEXCore/Source/Interface/Config/Config.json.in#L666)：用 FP64 模拟 x87 会降低准确性、可能导致渲染错误；通用默认 false。旧柚性能默认 1 是性能策略，不能代表全部 x87 兼容。
- [#5827](https://github.com/FEX-Emu/FEX/issues/5827) 有独立的 FILD/FISTP 整数往返复现，缩减精度会损坏数据，使安装器报路径错误。[维护者回复](https://github.com/FEX-Emu/FEX/issues/5827#issuecomment-5312453428) 明确称它为有意的 hack。[#5411](https://github.com/FEX-Emu/FEX/issues/5411) 记录错误纹理；[#5988](https://github.com/FEX-Emu/FEX/issues/5988) 记录 ARM64EC Proton 启动崩溃，单独关闭缩减精度可启动。这些不是本游戏同一指令根因的证明，但确认该兼容开关有上游依据。
- 已有的底层精度修复：[ffca27cb](https://github.com/FEX-Emu/FEX/commit/ffca27cbde1cf499860a76d440ccd9c8f694759a) 接入 Cephes；[0fbb6aa0](https://github.com/FEX-Emu/FEX/commit/0fbb6aa02f6951423b00207caec67da8d1550efb) 改为显式 SoftFloat float128_t，避免 Windows long double 等同 double 的问题。固定 FEX 源 86ff33bbe 包含二者（merge-base --is-ancestor 均为 0），也有 cephes_128bit API，无需重复移植。不能把本次故障误报为该库丢失。
- 新的性能候选：[c76fccf2，2026-11-03](https://github.com/FEX-Emu/FEX/commit/c76fccf256a185a2a20008de2578fa5c8dab217f)，对 Clang >=22.1 允许 MinGW preserve_all，减少完整 x87 fallback 的调用保存开销，依据是 [LLVM PR #166436](https://github.com/llvm/llvm-project/pull/166436) 的 Windows ARM64 ABI 修正。旧柚固定源仍对所有 MinGW 禁用它；本机工具链为 Clang 23.1.0。适合作为后续独立候选，先验证 WOW64/ARM64EC 寄存器、异常展开及游戏。本轮没有直接开启或声称提速。
- [PR #5626](https://github.com/FEX-Emu/FEX/pull/5626) 的 FYL2X/FYL2XP1 优化明确允许更多精度损失，不能当成本例的精度修复。本次定向检索未找到与此 Mono/NVorbis 完全匹配、可直接移植的 FEX 修复，不代表绝无相关报告。

现成兼容方案是使用 FEX 已有完整精度实现：按游戏保存严格浮点精度，向实际新进程注入 FEX_X87REDUCEDPRECISION=0。此前相同 Mono 的 65536 组测试严格模式全通过，缩减模式有 1398 组差异。本轮又观察到 OP 结束后返回中文标题菜单。声音、完整剧情及输入尚未全部验收；新设置页因设备空间不足未安装，不能宣称全部完成。Box 保持原版，不继续实验。
