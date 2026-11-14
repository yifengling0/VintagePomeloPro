# Mini VirGL 与 DX11 合并评价

2026-11-14。基于 `89bfeea6717923a963d9cdef1af2348d7d6d4a33`，没有重复 cherry-pick。
DXVK/Venus 和 WineD3D/VirGL 保持各自的开关、同步规则和性能结果。

## 本轮判断

已将 89bfeea6 的 DX11/Venus 改动保留在同一个 Mini 实验包，并修复原包缺少真实 Mesa EGL 导致 WineD3D 无法初始化的问题。VirGL 同命令合并写入的 Low 对照为关闭 16.1 FPS / 405 分、开启 16.2 FPS / 409 分，尚无明确提帧，保持默认关闭。更早的关闭参考为 16.2 FPS / 407 分。GLThread 请求轮为 15.9 FPS / 401 分，但没有确认真实生效，不作为有效的开关性能对照。High 基线为 9.8 FPS / 246 分，且进程 RSS 明显增大；不能据此排除 GPU、纹理/着色器或资源同步成本。

## 修复与候选

首先修复 Mini 上实际阻止 WineD3D 初始化的包问题：原 DX11 notify HAP 缺少
`libEGL.so` 与 `libEGL.so.1`，Wine 报 `egl_init Failed to load ... libEGL.so`。
`scripts/build_native.sh` 的旧清理函数把真实 Mesa EGL 与 SDK 链接桩一起删除。
现在通过 SDK llvm-readelf 检查 `DT_NEEDED libgallium-*`，保留真实 guest Mesa EGL，
继续移除 SDK 桩。两个 EGL 别名均验证；补齐后的包可以完整运行 Mini D3D9 基准。

新增默认关闭的 `VIRGL_WINEHUA_PACKED_WRITE=1`：只把同一条 submit、busy-wait
或 present 请求的头和正文从两次 write 改为 writev。保持原 socket mutex、
命令顺序和请求/回复边界，不合并不同命令。处理短写、EINTR、零写、错误与长度溢出；
busy-wait 写失败先终止，避免等待不存在的回复。一次性日志证明真机命中对应模式，
正式基准没有逐命令 trace 或 profiler。

`VN_WINEHUA_PACKED_SUBMIT=1` 是已有 Venus 优化，仅适用于其自身路径；
本轮保留其 ELF 和补丁，不把其历史收益当作 VirGL 收益。

另外使用现有 Mesa `mesa_glthread` 选项单独探索 GL 调用的线程化；
它与 WineD3D 已有 CS worker 不同，没有把增加 CPU 总占用作为验收目标。

## 包与源码身份

- 设备：Mini MLR-AL10，Maleoon 910，OpenHarmony-7.0.0.111。
- 版本：2.0.2 / 2000020，release/internaltesting，debug=false、profileable=false。
- 候选：`workspace_temp/mini-virgl-performance/VPP-2.0.2-mini-virgl-egl-signed.hap`。
- 候选 SHA-256：`d172b43c5f1bb024e03ae8dd7dff7424f3041353e9e829c80c14eef8348277e2`。
- 原 notify 回退包仍引用 `workspace_temp/heaven-performance/VPP-2.0.2-heaven-notify-signed.hap`，不复制。
- 保留 Venus SHA-256：`5648eea4d30fa5659cc62d057ed2947be1a0d773f8f3590dfcd930d2e7507158`。
- 四个 Gallium/DRI 别名统一为 `9892ddd3e85d0dd6a117f1b5f4813499c8ad5d4fe10dcdf0b022e0080de0e867`。
- 两个 EGL 别名统一为 `e9d02df5e2e18c5950955edb37cdda1a8c6d3379610ec9eec865d4689b2929fc`。

Mesa 固定提交为 `557fa3e781ea6e78fcff3ee759c25f60c3055064`。
只读复用 source-integrity 冻结对象缓存，隔离重编 socket 对象、归档和链接；
未修改基线重新链接、去 RPATH、strip 后与原 HAP Gallium 完全同 hash。
四个 DRI 别名是实体 ELF 副本，均同步替换。
签名/profile/证书匹配验证通过；逐成员 SHA-256 证明其余 payload 保持不变。
替换安装使用 `install -r`，没有卸载或清除用户数据。

这是用于双路径对照的合并实验包。原内嵌 wine-data.zip 与组件锁文件保持原样，
不能当作全量发布包的一致性验收；推广发布前仍需通过正常构建链同步所有组件和身份记录。

## Mini VirGL 完整成绩

Heaven 4.0 PE32：D3D9 → WineD3D → guest Mesa virpipe → vtest → host VirGL/GLES。
实际 1280×720 全屏，AA=0、tessellation disabled、VSync=0，每轮完成 26 场景。
结果页和记录中的场景图已人工检查。

| 记录 | packed write | GLThread 请求 | 画质 | FPS / Score | Min / Max FPS | 电池温度：启动 / 采样开始 / 结束 |
| --- | --- | --- | --- | ---: | ---: | --- |
| virgl-low-a3 | 0 | 未指定 | low | 16.2 / 407 | 6.0 / 41.1 | 33.0 / 34.0 / 37.0°C |
| virgl-low-b1 | 1 | 未指定 | low | 16.2 / 409 | 6.1 / 40.6 | 35.0 / 35.0 / 37.0°C |
| virgl-low-a4 | 0 | 未指定 | low | 16.1 / 405 | 6.0 / 40.4 | 35.0 / 35.0 / 38.0°C |
| virgl-glthread-low-on | 0 | true | low | 15.9 / 401 | 5.8 / 40.1 | 35.0 / 35.0 / 37.0°C |
| virgl-high-a1 | 0 | 未指定 | high | 9.8 / 246 | 5.1 / 19.9 | 35.0 / 36.0 / 38.0°C |

| 记录 | Heaven CPU | VirGL child CPU | wineserver CPU | 全部相关进程 CPU | 采样秒数 |
| --- | ---: | ---: | ---: | ---: | ---: |
| virgl-low-a3 | 250.66% | 57.91% | 10.94% | 338.59% | 240.11 |
| virgl-low-b1 | 255.75% | 59.38% | 11.46% | 345.52% | 240.12 |
| virgl-low-a4 | 256.55% | 59.93% | 11.55% | 347.00% | 240.11 |
| virgl-glthread-low-on | 259.00% | 59.87% | 11.60% | 349.43% | 240.11 |
| virgl-high-a1 | 243.46% | 49.71% | 12.30% | 324.65% | 240.12 |

100% 表示一个逻辑 CPU。CPU 取中间约 240 秒，FPS 为完整内置基准；
这两个窗口不能精确换算成每帧 CPU 成本。内置 min FPS 不是 1% low。
没有 GPU busy、逐帧 GPU 时间或完整频率轨迹，不能仅从 CPU 总占用断言 GPU 没有瓶颈。

已有呈现日志的独立解析见 presentation-analysis.json。a4 采样内保留 31 个完整统计窗口，
每个窗口的呈现回调总耗时 p50 为 2.529–3.411 ms，CPU upload_bytes=0、failed_swaps=0。
这些是各窗口百分位范围，不是全局帧时间百分位；呈现回调不包含上游游戏/命令队列及完整 GPU 执行。
该观察支持优先量化上游提交与等待，而非先新增整帧 CPU 上传优化。

每轮停止应用后等待 Battery≤35°C、system_h≤40°C、shell_front≤38°C，再启动并统一预热。
最早 a3 为 33°C 起步，仅保留为参考。后续记录以实际启动和采样温度为准；
散热较慢时将最长冷却从 300 秒延长到 600 秒，没有提高温度门限。
本轮样本量有限，没有统计显著性或跨游戏推广结论。
High 轮曾因场景背景影响菜单 OCR，多等待了几次就绪检查才开始同样的 20 秒预热；
其基准成绩有效，但额外加载/等待与温度差异使 Low/High 不能作为完全隔离的画质成本实验。

失败的缺 EGL 启动、窗口模式诊断、OCR 门控误判中止及未启动游戏的冷却超时均不算 FPS 结果。
DX11 回归的第一次启动也在 F9 之前因 OCR 空输出解析中止；已保存 setup-attempt1.json，
修复任务自身采集器后重跑，不把采集器故障归类为运行时崩溃。
普通 HDC 不能枚举该 release 应用的私有 task 目录；失败的线程枚举不作为 GLThread 生效证据。

## 同包 DX11 回归

Heaven D3D11 / legacy DXVK，1280x720 fullscreen，Low，26 场景完整成绩为 **19.0 FPS / 480 分**，min/max=6.4/39.2。
CPU 采样 240.08 秒：Heaven 202.72%，renderer 169.16%，100% 为一个逻辑核。

一次性收据确认 Venus packed=1；最终进程确认 preciseMaps=1、mappedFlush=1、ringPerf=0，
实际 extent=1280×720。结果与场景截图另存于 dx11 子目录，验证没有因 EGL/Gallium 变更而失去 DX11 路径。
这是当前合并包的功能/短时性能回归，不能用历史不同温度的成绩计算本轮新增收益。

## 检查与下一步

生产 VirGL 序列化函数的 ASAN/UBSAN 测试覆盖三类请求字节流、2→1 写入数、
请求/回复顺序、短写边界、EINTR、零写、错误、溢出，以及 busy-wait 写失败先终止。
已有 Venus 协议测试保持通过。EGL 清理测试直接抽取生产 shell 函数，使用真实 Mesa
ELF 和 SDK stub，验证两个别名分别保留或移除。补丁栈幂等检查、shell 语法和 diff 检查通过。

优先给 vtest busy-wait 的完整请求、socket/回复、锁等待及 virgl_renderer_context_finish 补有界聚合计时，先确认主导项。现有 host 源码的 busy-wait begin/end 日志从 context_finish 之后才开始，漏掉该成本；而且 VTEST_SYNC_GL_FINISH 按 getenv 是否存在判断，设置字符串 0 也不会关闭。不要直接撤掉 glFinish 或把 busy=false 缓存成通用修复。当前 host 还忽略请求中的资源 handle，若推进资源级 fence，需要覆盖提交顺序、资源/映射代际、跨 context、读写方、销毁与传输完成。

并行保留另一项分析目标：按资源类别统计 guest/host 分配、shadow/staging、副本和实际纹理上传字节。High 的 Heaven RSS 中位数为 2229.56 MiB、VirGL child 为 1318.04 MiB；Low a4 分别为 1046.48 与 419.62 MiB。RSS 可能包含共享页，不能相加当作总物理内存，也不等于 GPU 显存。呈现 upload_bytes=0 不代表纹理上传为 0。优先验证避免重复转换/复制及受 fence 保护的有界资源复用，不先无条件扩线程或扩大池。

WineD3D 的 CS worker 已存在；后续应分别量化 cs_finish、map、query 和命令/状态切换的次数与等待。手机上的等待策略候选没有可推广的收益，本轮没有引入。GLThread 在确认实际启用路径后再做显式关闭/开启对照。进一步通过相同画质的分辨率对照及 GPU busy/计时，区分 CPU 提交与 GPU/带宽成本。默认推广前需更多交叉复跑、游戏、帧时间及生命周期回归，并用正常全量构建更新顶层/内嵌组件与锁文件。

手机已离线，本阶段没有继续操作；其先前实验 DLL 和图形设置仍待回接并重新授权后恢复，
不把此前静态报告中的“已恢复”状态当作当前设备状态。
Mini 的最终应用/设置状态与源代码提交状态见 final-state.json。

## 证据与存储

完整证据位于 `F:/VintagePomelo-Workspace/artifacts/mini-virgl-performance/`。
仅保留当前新签名包；回退引用已有 notify 包。删除淘汰候选、unsigned HAP、
重复基线链接 ELF 和可再生临时副本，保留小型 hash/版本/结果记录及必要增量对象。
任务结束执行工作区 maintain-storage.ps1 -Apply -CompactLogs，详情见 storage-final.json。
