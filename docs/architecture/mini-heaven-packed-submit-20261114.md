# Mini Heaven：小提交单次写入实验

2026-11-14，接续 `2cf02b10`。本轮降低 Venus/vtest 小提交的 socket 写入次数，
不增加游戏线程，也不降低画质。以前三个阶段的冻结资料保持原样。

## 实现与开关

`patches/mesa/0012-venus-pack-small-vtest-submits.patch` 已注册到 Mesa OHOS
补丁栈。`VN_WINEHUA_PACKED_SUBMIT=1` 开启，其他值和未设置均关闭，初始化时缓存。

仅单 batch、无 sync、命令流非零且不超过 128 字节、4 字节对齐时，将协议头、
batch 数、batch 头和命令流放进最多 164 字节的栈缓冲，一次发送。
原 `sock_mutex` 仍覆盖完整提交；多 batch、带 sync 和大提交继续走原路径。
不跨提交合并，不改变通知次数、同步顺序、strong barrier 或 always-notify 策略。

写入循环处理短写和 EINTR；零字节写入按 EPIPE 断连处理，其他错误仍进入原终止路径。
首次符合条件的提交输出 `WineHuaVtestSubmit: packed=0/1`，并可写入
`VN_WINEHUA_PERF_LOG`。每个连接只记录一次，正式 FPS 测量无需开启 ring 累计计时。

## 构建身份和对照边界

被测旧 HAP 的 Venus SHA-256 为
`e3e852c7afc81574adb0dbe436368f95a778fcd89ace140f6a583292b65ada49`。
本地已有库和缓存均不匹配，因此先在固定任务下新建约 29 MiB 的增量目录，
未复制源码、运行时或游戏资源。未修改源码的基线构建成功，但 strip 后仍与旧 HAP 不同。
本轮只以新包内的开关对照判断收益，不用历史 FPS 作为本补丁的收益基线。

候选基于上一轮普通签名包，只替换 `libs/arm64-v8a/libvulkan_virtio.so`；
其余 payload 清单一致。新库为 AArch64，SONAME 不变，依赖 libc、libdrm 和
libwayland-client。debug=false、profileable=false，内测签名、profile/证书匹配及验签通过。
替换安装保留原有应用数据。

候选 HAP SHA-256：
`4e41a49b438342208b4129dba793d00db2034e38420d2d8a2931f59476366be8`。
新库 SHA-256：
`5648eea4d30fa5659cc62d057ed2947be1a0d773f8f3590dfcd930d2e7507158`。

Mesa 固定提交为 `557fa3e781ea6e78fcff3ee759c25f60c3055064`；原有未提交源码差异
在修改前独立归档，并记录 SHA-256。主仓库仅提交本轮 managed patch、注册、测试和文档，
不提交或重置其他子模块的原有修改。

该 HAP 是控制实验包：内嵌旧运行时及其锁文件保持原样，不能当作全量发布组件一致性验收。
后续推广需同步重建顶层/内嵌 Venus 并更新对应身份记录。

## 验证

增量交叉编译通过。`make test-venus-vtest-submit` 抽取实际生产序列化函数，
ASAN/UBSAN 与 -Wall/-Wextra/-Werror 下验证开关前后字节一致、普通路径写入 4→1、
短写、EINTR、零写入、错误及大命令/多 batch/sync 回退。
另从固定 Mesa 提交只提取补丁涉及文件，完整补丁栈连续应用两次，确认幂等；
在该干净补丁结果上再次通过序列化测试。

## 真机条件与结果

Mini MLR-AL10，Heaven PE32，legacy DXVK / D3D11，Low，1280×720 全屏，
AA、tessellation、VSync 关闭。Heaven 实际走 Venus，桌面合成走 Direct Vulkan。
精确发布保持开启，FIFO 请求为 0，正式 FPS 轮关闭 ring 计时，未启用 profiler。
每轮完成 26 场景，结果页人工核对；CPU 取中间 240 秒，100% 表示一个逻辑核。

| 顺序 | 打包 | FPS / score | Heaven CPU | renderer CPU | 电池温度（采样前→后） |
| --- | --- | --- | --- | --- | --- |
| A1 | 关 | 14.4 / 363 | 195.01% | 164.37% | 33→36°C |
| B1 | 开 | 19.3 / 486 | 206.79% | 165.48% | 37→39°C |
| A2 | 关 | 11.8 / 298 | 222.95% | 175.90% | 39→39°C |
| B2 | 开 | 15.0 / 377 | 248.57% | 187.19% | 39→39°C |

两对相邻开启轮分别提高 34.03% 和 27.12%。关闭中位数 13.1 FPS，开启中位数
17.15 FPS，算术比较提高 30.92%。两对方向一致，但样本少、按顺序连续运行，
温度及系统调度未完全隔离，不能当作统计显著结论或所有画质/游戏的预期收益。
加载和预热阶段不计入 FPS/CPU 窗口；每轮采用相同的菜单就绪门控和 20 秒预热。
四轮最终路线均确认 `preciseMaps=1 mappedFlush=1 ringPerf=0`，extent=1280×720；
一次性收据分别确认 `packed=0/1/0/1`。检查了中间截图和四个最终结果页，未观察到图形异常。

Heaven CPU 两轮均值约 208.98%→227.68%，renderer 约 170.14%→176.33%，
对应约 8.9% 和 3.6% 增长；帧率增长更大，支持减少提交开销的方向。
CPU 取 benchmark 中间区间，FPS 为完整 26 场景，不能由这两个值精确推算每帧 CPU 成本。

## 独立 ring 诊断

另跑关闭/开启两轮，每轮仅测内置 benchmark 前部的 120 秒，打开累计 ring 计时，
不收集最终 FPS，也不并入上述四轮。实际收据均确认了开关和 `ringPerf=1`。
取两轮主 ring 的相同 600,000→3,600,000 提交区间，各 3,000,000 次提交：

| 差值指标 | 关闭 | 开启 |
| --- | --- | --- |
| submit 平均耗时 | 27.77 µs | 16.41 µs |
| notify 平均耗时 / 提交 | 26.52 µs | 10.40 µs |
| seqno 平均等待 | 728.22 µs | 1573.74 µs |
| mutex 累计等待 | 34.43 s | 22.91 s |

notify 平均耗时约减少 60.80%，
支持减少 socket 写入开销的实现方向。seqno 单次等待反而增加，是下一步需要定位的回复等待线索。
这些累计线程计时可以重叠或嵌套，不能相加或折算成整帧占比；
计数区间含启动/预热的一部分，也未与 CPU 采样窗口或同一组帧精确对齐，
不能把 seqno 增长直接归因于本补丁，或从 notify 降幅预测 FPS。


## 后续范围

该实验检验的是串行 transport 开销。更多 CPU 总占用本身不代表效率改善；
应同时看每秒完成的帧数和进程工作量。缺少 GPU busy 及完整关键路径轨迹，
仍不能据此断言所有场景均不受 GPU 限制。

保留默认关闭的开关；High/Ultra、更多游戏、设备、长时间稳定性及全量发布组件一致性
需要后续覆盖。进一步优化先根据独立 ring 诊断确定通知、seqno reply 和锁等待的剩余成本，
再选择下一处实现，不直接撤销同步屏障或无条件增加线程。

完整证据存于 `F:/VintagePomelo-Workspace/artifacts/mini-heaven-notify/`。
安装包留在已登记的固定 `workspace_temp/heaven-performance/`，保留本轮包与上一轮回退包。


## 存储收尾

本任务删除已验签 unsigned HAP、淘汰的旧旧签名包和中间 native 副本，回收约 1.17 GiB；
另核对历史两个 unsigned 参考包与各自保留的签名包，逐 payload 成员 SHA-256 相同且
签名工具验签通过后删除冗余 unsigned。保留小型 hash/版本记录，保留约 31 MiB 的可复用
Linux 增量缓存、补丁测试片段和测试缓存，以及两个可安装签名 HAP。

全工作区扫描时 workspace_temp 约 46.6 GiB、artifacts 约 50.0 GiB，仍高于 20/30 GiB
整理目标。用户 prefix 私有备份、正式 AppGallery .app、持久依赖/客户端缓存和签名参考包
需要保留；其他历史任务的唯一性尚未全部判定。本轮只清理证据充分的冗余副本，
收尾维护结果见工作区固定 logs/storage-maintenance/latest.json，详细清单随本轮证据保存。
