# PAL4 x87 首轮优化证据

入口报告：../../pal4-x87-optimization-20261007.md。

本目录只保留紧凑的审查证据。生产默认关闭 PC24 mul 候选；五组 PAL4
对照为原版 33.44 FPS、候选 33.36 FPS，没有稳定游戏收益。

- `pal4-mul24-ab-summary.json`：十轮统计、五个配对及口径限制。
- `ab/*-result.json`：十轮全部有效新到帧样本、顺序、环境与加载上下界。
- `*-package-identity.json`：A/B/D0 包、剥离前后 DLL 与代码节身份。
- `final-artifact-audit.json`：四类编译产物、编译器、真实编译/链接命令、
  开关及候选未编入 D0 的审计。其 dirty diff hash 为审计时点的全工作区，
  不是本轮补丁的单独 hash。
- `final-patch-identity.json`：两个最终 patch 的精确重放及改变文件 hash。
- `review-source-identity.json`：交付时构建入口、回归、probes 及 patch hash。
- `integration-existing-inputs.json`：本轮之前已 dirty 的四个文件 hash；
  保护这些先前改动，不能将全部 diff 归为本轮。
- `scene-d-stable-analysis.json`、`input-domain-summary.json`：有限线程
  raw 时间和初始输入筛选。尚无空边界校准或 guest RIP/模块关联。
- `*disassembly.txt`：实际设备 D0 dispatcher 与候选 FMUL 调用形态。
- `probes/`：目标差分、原生 x87、真实 PE32 ABI、双线程、时钟及微基准。
- `restored-device-dll.txt`：收尾时原版 DLL 核验。

D0 raw 为含探针的墙钟时间；无诊断 guest 简化循环远快于 D0 raw，
不可将 raw 边界数字当成纯保存成本或 FPS 可消除份额。PC24 的输入筛选
比例不是经过结果指数 guard 的完整命中率。加载终点是首次识别场景截图，
没有确认首个可操作时刻。无独立缓存快照或可读温度/频率数据。

`SHA256SUMS.txt` 覆盖本目录除 manifest 自身外的所有文件。原始 records、
截图、日志、hiperf、HAP/DLL/EXE、游戏资产及签名材料留在本地工作目录。
