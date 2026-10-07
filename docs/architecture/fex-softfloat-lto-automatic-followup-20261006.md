# FEX ThinLTO 与 RichMan8 自动真机对照（2026-10-06）

本轮已直接启动程序并操作游戏；不再依赖用户运行 BAT。

## 当前结果

- V2 文件读取／AppConfig 路径修复后，FEX CPU 测试完整通过。此前“卡在 BAT”已解除，不能等同于游戏卡顿修复。
- ThinLTO 只替换 WOW64 FEX DLL；其他 Wine、Mesa、宿主及 ARM64EC 继承基线。CPU 测试全部七项校验值一致，扩展精度检查通过。x87 单次会话中位耗时 16.9964 → 13.2745 ms（约 -22%）；整数、SSE、REP、调用、接口项没有普遍翻倍改善。
- RichMan8 仍有首次加载掉帧和人物缺块。三个有效冷加载的宿主窗口为 33.45、34.26、33.39 FPS；对应重复加载约 57–62 FPS。首次沙隆巴斯截图 12 FPS，人物缺块在稳定约 60 FPS 时仍出现。
- 这不是严格 V2/LTO 或 main/Proton A/B：不同会话、温度未匹配，前三固定人物也不同。不能宣布 ThinLTO 带来游戏提升。
- 当前候选仅为实验，未修改生产默认 LTO 参数，未降低精度、TSO、SMC 或画质。

## 有效样本与排除

`avatars-cold-load.data` / `avatars-warm-load.data`：同 OS PID 4062、Wine PID 660，各请求 40 秒，100 Hz、DWARF4096、lost=0。配对 `*-symbols.map` 均为该进程采后读取。

首轮开始时实际已有四人，因此 index 0 仅高亮，未加入钱夫人；重复轮 index 0 才是钱夫人首次加载。两轮 index 0 都排除，不能把完整四人序列称为有效冷暖对照。其余三个头像的加入、移除及截图匹配。前三固定人物实际为两种忍者和金贝贝，和旧 V2 基线不同。

`automatic-paired-summary.json` 汇总 Unix GL 计时和宿主 FPS。LTO 这次缺少匹配的 FEX-JIT-PERF 记录，`jit=null`，不是 JIT 成本为零；入口遗漏 FEX_SILENTLOG=0。旧 V2 中的少量 JIT 编译不应被新空记录追认。

可见采样栈包含 FEX x87 ABI 辅助入口、WineD3D CS 及系统调用。Hiperf 隐藏实际 leaf PC，`avatars-cold-jit-profile.json` 与 `avatars-cold-timebins.json` 只做 caller 身份分类，不能把入口权重当叶函数成本。

## 其他同包诊断

`../richman8-render-isolation-20261006/`：CSMT 关闭、动态缓冲系统内存暂存开启及 glsl-vkd3d 请求，均由工具启动并操作。

CSMT 关闭时 GL 计时转移至 Wine 主线程（PID444/TID448），主菜单和选人仍缺块。系统内存暂存以真实 `dynamic_buffer_sysmem ... enabled=1` 确认；入向 shadow 复制在观测窗口中消失，剩余映射约每帧一次，而 BufferSubData 增多。人物仍缺块，不能宣布帧率收益。

暂存开启时选人组成与脚本目标不一致，完整序列不作为冷暖性能对照。glsl-vkd3d 只是请求的配置：主菜单仍缺块，没有捕获确认该角色渲染使用新 shader backend，不能宣布完成后端级排除。

测试结束已 force-stop 目标 bundle，确认目标应用不再运行。测试设置仅在进程环境中使用，未改用户持久配置，未卸载或清数据，未启动 Steam。

## 构建、身份与复现

`package-identity.json`、`build-validation.json`、`lto-install.txt`、`lto-current-runtime-sha.txt`、`cpu-lto.tsv` 保存构建、安装、实际 DLL 与测试验收。

`thinlto-boundaries.patch` 为相对 V2 已补丁 FEX 源码的实验 CMake 差异。`reproduce-build.sh` 使用新的独立目录重建，仅为构建复现脚本；脚本尚未重新执行，不能宣称已独立复现。初次全 LTO 因原生 compiler-rt 引用 FlushInstructionCache、内联汇编引用 BTCpuSimulateImpl 失败；最后保留 CommonWindowsRuntime / wow64fex 模块为原生 COFF，FEXCore / SoftFloat 继续 ThinLTO。

FEX 源码在 MINGW 下主动关闭 preserve_all，即使编译探测缓存为 true，也不能据此认为实际开启。x87 因精度语义调用 SoftFloat 和 ABI glue，这是后续可以量化的优化方向；不应直接开启源码注明 broken 的 ABI 或降低精度来对齐 Box。真正 Box 的 CPU 测试精度检查与 x87 checksum 未通过，也不能把该结果当作等价正确性性能目标。

`SHA256SUMS.txt` 覆盖本目录文件（排除自身与 __pycache__）；原始采样、包及签名材料不提交。源码、文档与实验均保持未提交，已有 dirty 保留。
