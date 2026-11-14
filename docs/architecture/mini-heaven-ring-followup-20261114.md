# Mini Heaven：精确发布后的 ring 诊断

2026-11-14，接续本地提交 `a33ebfdf`。原 `mini-heaven-performance` 和
`mini-heaven-optimization` 冻结资料未改写。本阶段没有新增经验证的提帧默认值。

## 新增的测量入口

`WINEHUA_FRESH_VENUS_PERF_SUMMARY=1` 仅在 fresh Direct 兼容回退 Venus
时打开现有 `VN_WINEHUA_PERF_SUMMARY`，其他值与未设置均关闭。
保持原图形策略、精确发布条件、strong barrier、always notify 和 remote sync。
最终 broker 日志打印 `ringPerf`。测试剔除两个观察键后比较完整环境向量，
确认计数开关没有附带其他路线变化。计数存在计时与原子操作成本，默认关闭。

## 真机结果

Mini MLR-AL10，Heaven PE32，D3D11 / legacy DXVK，1280×720 全屏，
AA、tessellation、VSync 均关闭。游戏实际走 Venus，桌面合成走 Direct Vulkan。
每轮完成内置 26 场景，结果页人工核对，FPS 期间无 profiler。

| 轮次 | 画质 | FPS / score | Heaven CPU | renderer CPU | 用途 |
| --- | --- | --- | --- | --- | --- |
| next-high-precise | High | 9.7 / 244 | 未采到 | 未采到 | 旧普通优化包的新增画质覆盖 |
| next-ring-a | Low | 12.1 / 304 | 213.45% | 171.16% | ring 诊断，FIFO 请求为 0 |
| next-ring-b | Low | 11.8 / 296 | 222.64% | 174.08% | ring 诊断，FIFO 请求为 1 |

CPU 为内置 benchmark 中间 240 秒累计进程 CPU，100% 为一个逻辑 CPU。
High 轮 OCR 漏掉场景斜线，未启动正式 CPU 窗口；保留完整 FPS，不能补推 CPU。
Low 两轮同一普通签名包（profileable=false/debug=false），精确发布始终开启，
最终路线确认 `preciseMaps=1 mappedFlush=1 ringPerf=1`，extent=1280×720。
它们是诊断轮，不并入上一阶段约 39.6% 的提帧对照。

## Ring 线索

日志在每 600,000 次提交输出累计计数。取首条 600,000 至末条 6,000,000
的差值，各 5,400,000 次提交，覆盖范围含启动/预热的一部分，未严格对齐 CPU 窗口。

| 差值指标 | A | B |
| --- | --- | --- |
| submit 平均耗时 | 32.94 µs | 32.98 µs |
| notify 平均耗时 / 提交 | 30.69 µs | 31.64 µs |
| notify / submit 累计函数耗时 | 93.16% | 95.93% |
| seqno 等待次数 | 157,777 | 157,476 |
| seqno 平均等待 | 857.21 µs | 835.78 µs |
| mutex 累计等待 | 78.40 s | 73.00 s |

这些是可重叠、可嵌套的线程累计墙钟计时，不能相加当作总帧时间；
93%–96% 只指 ring submit 函数内部，不能说占整帧或游戏总耗时。
目前没有 GPU busy 或完整关键路径轨迹，不能据此证明所有场景都不受 GPU 限制。
相比盲目增加线程，先降低串行通知和 reply 等待更有证据支持。

## FIFO 候选的结论边界

直接读取被测 HAP 内嵌 `wine-data.zip` 的目标 DLL，没有解压整个运行时。
legacy ARM64X 和 x86 DLL 均包含 FIFO、batch-flush stats 和统计日志标记；
modern 2.6 DLL 未检测到 FIFO 开关。DLL SHA-256 记录于 `next-dxvk-capabilities.json`。
本地 rawfile ZIP 与被测 HAP 的运行时大小不同，未将本地文件当作被测身份。

两轮请求分别传入 `DXVK_WINEHUA_FIFO_BUFFER_SLICES=0/1`，但未取得 DXVK
启用标记或上传统计文件。后续通过 cmd.exe 导出 Windows 环境的有限探测也未成功。
因此不能确认 FIFO 在 DXVK 的实际 Windows 环境中生效，不能将 12.1→11.8
归因于 FIFO。B 轮还出现系统通知；温度与调度也未精确隔离。
没有证实收益，FIFO 继续默认关闭，尚不能宣称该思路无效。

## 下一项实现的范围与验收条件

现有 Mesa 工作树 `vn_renderer_vtest.c::vtest_vcmd_submit_cmd2` 对普通
单 batch、无 sync 的 ring 通知分开发送协议头、batch 数、batch 头和命令流。
这是源码候选，尚未证明与被测二进制精确一致，且该文件已有其他未提交修改。
本阶段未改动或提交 Mesa / virglrenderer / DXVK 的既有工作。

优先验证将同一条小通知的四段字节合成一次发送，保持通知次数、字节顺序、
同步边界和生命周期，处理 EINTR、短写和错误；复杂多 batch / sync 路径保留回退。
在修改前核对组件构建身份；先验证流协议逐字节一致与短写路径，再做
同包交错开关的无计数 FPS 对照，ring 计数另跑。不能用 93% 预测提帧比例。
当前 shadow idle 状态不可靠，不直接禁用 always-notify 或恢复定时通知合并。

## 构建、交付与存储

`make test-direct-session` 和 arm64 release native 增量构建通过；
内测签名、profile/证书匹配及 payload 清单校验通过，替换安装保留应用数据。
新普通包 SHA-256：
`5ba7633926abf1be391166f4e4738dfb00520d0bd27553453f1b2a46c9158e3e`。
native SHA-256：`5c81adf64629450f296452bb25e9ad9ea46c481f9f9fccb103e46070a50cea95`。
它由 a33ebfdf 加本提交的测量入口构建；其他 payload 沿用已验证普通基线。
本阶段源码补丁及精确文件 hash 另外归档，避免将原有 dirty 子模块当作本提交成果。

原始截图、采样、计数、签名回执、安装身份和分析位于：
`F:/VintagePomelo-Workspace/artifacts/mini-heaven-followup/`。
安装包复用固定 `workspace_temp/heaven-performance/`，保留新普通包和上一普通回退包。
删除已验签 unsigned、中间 native 副本和淘汰的 profileable 诊断包，回收约 1.17 GiB。
