# Mini VirGL opcode 诊断

本轮进一步定位 submit-execute：draw 和帧缓冲切换占主导。
这是有计时开销的诊断，没有完整 FPS 成绩，不能计入性能提升。

## 条件与边界

Mini MLR-AL10，Heaven 4.0 Low / 1280×720 fullscreen / D3D9-WineD3D / AA0 /
tessellation disabled / VSync0 / writev=0 / GLThread 未指定。
安装 2.0.2/2000020 release/internaltesting，debug=false、profileable=false；只操作 Mini、替换安装、保留用户数据。
renderer 单独启用 `WINEHUA_VIRGL_OPCODE_DIAGNOSTIC=1`，0018 poll 候选未启用。

约 120 秒基准采样，起止图已查看：scene 1/26 → 13/26，均处于 Benchmarking。
同进程 80 条 HiLog epoch/monotonic 与 bracketed date 对齐，两端各留 2 秒，
保留 21 个完整窗口，共 105.063401546 秒。

## 分解

batch 25966 次，37.245753 秒，占墙钟 35.4507%，
编码字节 5576928452。下列 handler 互斥计时；gl-error-check 和 context-switch 单列。

| 阶段 | 次数 | 总耗时 | 墙钟占比 | 平均单次 | 编码字节 |
| --- | ---: | ---: | ---: | ---: | ---: |
| DRAW_VBO | 2301502 | 18.951672 s | 18.0383% | 8.234 μs | 119678104 |
| SET_FRAMEBUFFER_STATE | 57210 | 7.622683 s | 7.2553% | 133.240 μs | 921660 |
| RESOURCE_COPY_REGION | 17990 | 2.055677 s | 1.9566% | 114.268 μs | 1007440 |
| SET_SAMPLER_VIEWS | 587606 | 1.229972 s | 1.1707% | 2.093 μs | 15452532 |
| gl-error-check | 9809856 | 0.984194 s | 0.9368% | 0.100 μs | 0 |
| TRANSFER3D | 33363 | 0.861555 s | 0.8200% | 25.824 μs | 1868328 |
| context-switch | 25966 | 0.773071 s | 0.7358% | 29.772 μs | 0 |
| SET_CONSTANT_BUFFER | 1844890 | 0.741585 s | 0.7058% | 0.402 μs | 5211198184 |
| SET_VERTEX_BUFFERS | 1225105 | 0.571408 s | 0.5439% | 0.466 μs | 67212268 |
| SET_INDEX_BUFFER | 2247270 | 0.568242 s | 0.5409% | 0.253 μs | 35956320 |
| BLIT | 1576 | 0.373919 s | 0.3559% | 237.258 μs | 138688 |
| BIND_SHADER | 485666 | 0.255552 s | 0.2432% | 0.526 μs | 5827992 |
| BIND_OBJECT | 301311 | 0.249096 s | 0.2371% | 0.827 μs | 2410488 |
| CLEAR | 6309 | 0.240718 s | 0.2291% | 38.155 μs | 227124 |
| BIND_SAMPLER_STATES | 542230 | 0.167319 s | 0.1593% | 0.309 μs | 14503532 |
| SET_FRAMEBUFFER_STATE_NO_ATTACH | 57210 | 0.045990 s | 0.0438% | 0.804 μs | 686520 |

未分配 batch 开销 1.501581 秒，包括解码循环与计时等；
每条 opcode 增加多次 clock 调用，不能与无诊断的命令阶段采样直接相减。
handler 内含驱动执行和可能的 GPU 等待，不是纯 CPU 自身耗时，也不能排除 GPU 瓶颈。

`SET_CONSTANT_BUFFER` 占编码量大部分，说明后续应测 guest WineD3D→virpipe 的常量重复发送和
DRAW 内真实 uniform 上传成本；编码字节不等于 GPU 上传字节。CPU 采样 Heaven 260.53%、
VirGL child 63.66%、wineserver 10.53%，100%=一个逻辑 CPU，不能据此判断关键线程是否可并行。

## 本轮决策

1. 不删除 GL 错误检查：占墙钟仅约 0.94%，不足以解释 30% 目标。
2. 优先试验帧缓冲完整性结果缓存。现有 `glCheckFramebufferStatus` 仅用于错误诊断，
   但每次 FBO 更新都会查询。只记住已被驱动判定完整的内部、不可变、单 mip 附件组合，
   固定 32 项，不保留资源引用；对象插入/销毁清空，外部导入、多 mip、可变存储和 incomplete 不缓存。
3. 保留附件绑定、viewport、sRGB、shader/blend dirty、decoder GL 错误检查、fence 与协议路径。
   默认关闭 `WINEHUA_VIRGL_FBO_STATUS_CACHE`，候选关闭所有性能诊断，完整 A-B-B-A 独立验证。
4. 若收益有限，下一步拆 DRAW 的 uniform、program selection、sampler/vertex binding、实际 GL draw，
   再测 guest 侧打包/锁/CS 等待；不要为拉高 CPU 总占用盲目加线程。

原始证据、clock bridge、CPU 采样与 `opcode-analysis.json` 同目录。
候选的完整成绩另见 `artifacts/mini-virgl-fbo/report.md`，本诊断不宣称提帧。
