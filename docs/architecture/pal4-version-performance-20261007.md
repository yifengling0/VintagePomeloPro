# PAL4：1.4.4 与 1.4.5 FEX 性能定位，2026-10-07

## 本轮结论

用户确认问题是 1.4.4 旧 x64 Wine/Box 架构与 1.4.5 Proton/ARM Wine/FEX 整套运行时的差异。1.4.5 必须使用此前适配过的 FEX；本轮停止 wowbox64 兼容性方向。FEX 的 PE32 运行仍依赖 Wine WOW64 桥，不能把 WOW64 基础设施和 wowbox64 翻译器一起删除。

PAL4 实际复现了新版加载慢、场景供帧不足。旧版木屋外批次多在 58–62 FPS；新版普通 FEX 的一个场景区间为 27.63–33.01 FPS，随后不同镜头约 35–40 FPS。镜头、交互、缓存、采样与热状态未严格匹配，不能声称已测得固定两倍回退，也不能据此给某个补丁定责。

本轮新增的关键证据是**实际 PAL4 的 x87 软件浮点和调用边界热点**。这已从猜测推进到真实地址、同进程映射、匹配构建二进制和 JIT 名称的交叉核对。它是下一步优先优化的路径；尚未通过干预对照证明是全部帧率差距或全部加载慢的原因。本轮没有生产性能修改，没有性能提升结论。

## 包与运行身份

设备：HUAWEI MatePad Mini，MLR-AL10，HarmonyOS 7 / API 26。只操作 `com.vintage.pomelopro`，未启动 Steam。

程序：`Z:\games\games\PAL4\PAL4\pal4.exe`，读取列表第二格、唯一存档，场景为青鸾峰木屋外。实际 GL 图像为 800×600，未更改游戏分辨率。

| 对象 | 身份 |
| --- | --- |
| 比较用 1.4.4 HAP | `65b3eee5499bebd86cc80c28243a29790b5afd3ceeb9a0a8a1ab3693e955ed2a` |
| 1.4.5 干净默认 FEX HAP | `d6f2340843b1f6c76ea18833b6782697c727a68c2a80f8bf3ded8705d6193b27` |
| 包内和设备实际 WOW64 FEX DLL | `002a4a3a399db0743b0563d53ab7880b1d9bfa32ae19b7f513256a1938cbadfa` |
| 符号化使用的原编译 DLL | `3c1bb5f807a4429e2e44690c10b2d3e367c6dfdaee2e999a5fd472d59d75aaf4` |
| FEX 固定版本 | `86ff33bbe2` |
| 1.4.5 宿主版本 | `1.4.5-proton.26-alpha / 1004035` |

1.4.4 比较包只调整安装版本号以绕过系统拒绝降级；native/runtime 字节保留归档版本。旧版运行使用隔离前缀，原用户 `.wine`/`wine` 已恢复。1.4.5 使用现有干净 FEX 实验包，仅 WOW64 DLL 相对 `284026ce` 包基线变化；不能把它叫作整个 `62ca61f` dirty 工作区的完整新构建。

## 实际 CPU 证据

普通 FEX 会话：PAL4 PID 28272，30 秒采样窗口 09:16:39–09:17:10。采样开始时已进入场景，文件名带 `load` 不代表完整存档加载采样。100 Hz，dwarf4096，应用主进程及全部应用子进程；26,000 条全进程样本，lost=0。游戏样本 10,528 条，10,524 条 leaf PC 被系统隐藏。

符号会话：PAL4 PID 31810，20 秒采样窗口 09:28:33–09:28:55。开启 `FEX_GLOBALJITNAMING=1`、`FEX_LIBRARYJITNAMING=1`、`FEX_BLOCKJITNAMING=1`，输出到 `C:\perf-448.map`。17,515 条全进程样本，lost=0；游戏样本 6,997 条，6,994 条 leaf PC 隐藏。**符号会话不用于 FPS 对照**。

| 周期权重位置 | 普通会话 | 符号会话 |
| --- | ---: | ---: |
| 游戏进程占所采应用进程 | 80.66% | 81.84% |
| VirGL 子进程占所采应用进程 | 10.60% | 10.15% |
| 宿主占所采应用进程 | 7.09% | 6.49% |
| 主线程占游戏进程 | 53.36% | 52.75% |
| MSS Timer 占游戏进程 | 23.90% | 23.99% |
| WineD3D CS 占游戏进程 | 16.34% | 16.99% |
| dsound mix 占游戏进程 | 5.68% | 5.61% |

上述是 on-CPU 周期权重，不是每帧墙钟耗时，不是系统全部进程占比；没有量到 off-CPU 锁、I/O 或 GPU 等待。

同 PID 保存的 fault-maps 解决了原先大量 `<user-unassigned>` 地址。Wine 将 ARM64 PE 的 `.text` 匿名映射，Hiperf 不能直接将其和 DLL 名称关联。普通会话的 `0x6ffc430000–0x6ffc5f1000` 匿名代码区，后接 FEX DLL 文件映射；原编译 DLL `.text` RVA=0x1000、长度=0x1bff00、`.rdata` RVA=0x1c1000，与映射边界一致。按实际 DLL 符号解析，可见 caller 命中：

- `softfloat_roundPackToExtF80`：约 4.84% 游戏进程周期权重；
- `extF80_mul`：约 1.86%；
- `softfloat_roundPackToF32`：约 1.49%；
- `softfloat_subMagsExtF80`：约 1.47%；
- `softfloat_addMagsExtF80`：约 1.41%；
- x87 OpHandlers、`extF80_to_f32` 等也有命中。

符号会话进一步解析原先匿名的 dispatcher：

| 可见 caller 路径 | 主线程 | MSS Timer | 合计，分母为游戏进程 |
| --- | ---: | ---: | ---: |
| `FEX_ABI_F80_I16_F80_F80_PTR` | 10.55% | 11.05% | 21.60% |
| `FEX_ABI_F32_I16_F80_PTR` | 3.40% | 2.42% | 5.82% |
| `FEX_ABI_F80_I16_F32_PTR` | 3.14% | 1.93% | 5.07% |

双操作数入口用于 x87 F80 运算。记录还可解析到 JIT 中 `i386-windows/wined3d.dll` 与 `dsound.dll`，所以不能把 PE32 WineD3D 说成已全由 ARM 原生代码执行。

**不能把 caller 权重叫作这些函数的 self-time**。隐藏的 leaf 可能在被调用函数内。寄存器保存/恢复和软件浮点运算本体的独立份额仍未分离；只能确认执行路径与优先调查范围。JIT map 取回时游戏仍运行，5 条空名称或不完整条目被解析器明确跳过；不将跨会话 JIT 块地址相互套用。

## 图形路径与加载边界

旧版 PAL4 PID 22950 有 `winehua_gl_zero_copy`，新版 PID 28272 也有；新版稳态 presents 持续增长，而 readbacks 固定为 21，宿主 `upload_bytes=0`、`failed_swaps=0`。新版上述场景段宿主合成平均 `total_us` 约 2.5–2.8 ms，批次最大值多为数毫秒。

这说明当前稳态不是每帧整幅图像复制回 SHM，也没有直接显示出宿主合成本身持续吃掉 30 ms。**尚未排除上游映射、资源上传、队列同步、GPU 等待或 i386 WineD3D 翻译成本**。native Mesa caller 与 host CPU 份额不能等同 GPU 时间。

加载阶段曾出现 120 帧批次 `fps=4.00`，包含约 32.85 MB 宿主 SHM 上传和最大约 218 ms 的合成帧；不能拿它当纯游戏 FPS 或存档加载时长。两次原始存档点击未准确打标，未测出严格的旧/新加载秒数，也没有完整加载窗口的 CPU 与 off-CPU 对照。启动期大量 signal/maps 日志属于现有诊断代码，不能用它们推导稳态异常频率。

## 为什么成熟的 FEX 仍可能在此路径较慢

当前 PE/MinGW 构建的 `CMakeLists.txt:225` 在 MinGW 环境明确输出 `Ignoring broken clang::preserve_all support`，随后定义 `FEX_HAS_PRESERVE_ALL_ATTR=0`。实际编译命令也包含该定义。`Source/Common/HostFeatures.cpp:660` 据此关闭 `SupportsPreserveAllABI`。

`Dispatcher::GenerateABICall()` 的双操作数 F80 分支在 `Dispatcher.cpp:2141` 左右执行 `SpillForABICall`，设置 Windows ABI 参数，调用 helper，再 `FillF80Result`/`FillForABICall`。这是实际命中路径。FEX Linux 常用的优化调用约定和此处 Windows PE 后端不能视为同一性能环境。

这是可核查的构建/ABI 差异，不是已经证明的 OHOS 移植错误，也不是允许直接开启一个已标记 broken 的编译属性。1.4.4 Box 路线的 x87 精度、指令选择、WineD3D 版本与桥接成本还未做匹配验证。

## 下一步有针对性的修改

1. **先分离真实主线程运算和 helper 边界。** 在当前已经命中的 add/sub/mul/store 家族增加有界、按线程的诊断；或改进本进程 PC/栈可见性。以完整存档加载和静止相同镜头各一次采样，区分主线程与 Miles 音频，不能用全进程累计命中率代替主线程耗时。
2. **优化 Windows FEX 调用边界。** 在保持完整扩展精度和异常行为的前提下，研究有明确 clobber 契约、经过寄存器/栈/异常回归验证的汇编包装器或专用调用约定，减少反复保存/恢复。不能直接将 `SupportsPreserveAllABI` 改成 true。先证明边界 self-time，再决定投入。
3. **针对高频 F80 运算做严格快路。** 此次热点不止 store，add/sub/mul 与舍入都存在。只增加 exact-store 无法覆盖这些路径；优先 normal 值整数实现或精确可判定特例，保留 FCW 舍入、PC 精度、sticky flags、NaN/denormal/overflow 的原 fallback。不全局改成 double，不改 TSO/SMC，不降低画质。
4. **完整加载单独定位。** 同一存档从确认输入到首个可操作场景打标，覆盖文件读取/解压、JIT 编译、纹理创建与上传、shader 编译与等待。当前证据不能承诺浮点边界优化会同时消灭加载慢。
5. **最后才做性能验收。** 干净无诊断版；相同分辨率、镜头、配置、热条件；重启完整 Wine/FEX，反序重复。记录加载秒数、稳态帧率与帧时间，同时核查人物、贴图和输入。没有收益就撤回实验。

按用户此前“发现关键点后暂停交给其他模型”的要求，本轮在取得具体热点后停止生产优化，保留报告与解析脚本，恢复普通默认 FEX 启动。wowbox64 不再作为本轮候选；未提交或推送代码。

## 证据入口

原始文件均在 `F:\VintagePomelo-Workspace\workspace_temp\fex-perf-followup-20261006\`：

- `pal4-144-full-stderr.log`、`pal4-144-scene-events.txt`、`pal4-144-after-intro.png`；
- `pal4-145-fex-load-events.txt`、`pal4-fex-threads.txt`、`pal4-fex-steady-now.png`；
- `pal4-145-fex-load.data`、对应 `.summary.json` 和 `.callers.json`；
- `pal4-fex-symbol-scene.data`、对应 `.callers.json`；
- `pal4-fex-fault-maps.txt`、`pal4-fex-symbol-maps.txt`；
- `pal4-fex-final-jitmap.map`、`pal4-fex-symbol-attribution.json`；
- `capture_pal4_profile.py`、`classify_pal4_callers.py`、`resolve_pal4_fex.py`；
- `pal4-fex-final-runtime.txt`、`pal4-fex-final-restoration.json`。

版本隔离/恢复记录：`workspace_temp\version-performance-20261007\isolated-comparison-lifecycle.json`。签名材料、HAP、原始日志/截图/大采样仅本地保留，不放入 Git。
