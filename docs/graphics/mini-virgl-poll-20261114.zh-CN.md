# Mini VirGL 提交诊断与共享上下文轮询候选

同画质完整 Heaven 对照：基线中位数 16.10 FPS，
候选中位数 16.70 FPS，变化 +3.73%。
30% 目标对应初始基线 16.1 FPS → 20.93 FPS；本轮是否达到：否。
独立重复情况：baseline=2、candidate=2。
样本量有限，结论只覆盖本轮 Mini / Heaven Low。

## 范围与身份

- 权威源码 `/home/liufeng/src/vpp-account-cloud`，分支 `feature/fex-proton-upstream-sync`；
  本轮修改的父版本 `34a96ec3fb3b27481e83948b4cd4da3ea1447f6f`。
- Mini MLR-AL10，target `5KPBB25818203996`；只替换安装，无卸载、清数据或其他设备操作。
- Heaven 4.0 PE32，D3D9 → WineD3D → guest virpipe → vtest → host VirGL/GLES。
- Low、1280×720 全屏、AA0、tessellation disabled、VSync0、writev=0、GLThread 未指定。
- 基线 HAP：`VPP-2.0.2-mini-virgl-egl-signed.hap`，627114490 bytes，
  SHA-256 `d172b43c5f1bb024e03ae8dd7dff7424f3041353e9e829c80c14eef8348277e2`。
- 候选 HAP：`VPP-2.0.2-mini-virgl-poll-signed.hap`，627122682 bytes，
  SHA-256 `18be6f2fb5241951d0215c0f02d21ee57dc16ca0aef343276c48aacf0cb0718b`。
- 两包均为 2.0.2/2000020、release/internaltesting、debug=false、profileable=false，
  签名与 profile/证书匹配已验证。均为实验 overlay，未完成全量发布一致性验收。

## 阶段诊断

补丁 0017 给 submit-read、submit-execute、submit-fence 与 transfer-get/put 添加
默认关闭的 `WINEHUA_VTEST_COMMAND_DIAGNOSTIC` 计时；每五秒聚合，不做逐请求 trace。
保持原协议、同步和错误行为。单独诊断包完成约 120 秒 CPU 采样，未完成完整成绩测试，
因此下表只用于定位，不计算提帧。

使用同一进程 80 条 HiLog epoch/monotonic 与 bracketed device date 对齐，
两端额外保留各 2 秒边界，保守保留 20 个完整窗口，共 100.270843735 秒。

| 阶段 | 调用数 | 总耗时 | 窗口墙钟占比 | 平均单次 |
| --- | ---: | ---: | ---: | ---: |
| submit-execute | 23398 | 32.889366 s | 32.8005% | 1405.65 μs |
| submit-fence | 23398 | 1.560861 s | 1.5566% | 66.71 μs |
| submit-read | 23398 | 1.218271 s | 1.2150% | 52.07 μs |

submit payload 约 5.36 GB / 100 秒。没有 transfer-get/put summary 不代表纹理上传为零；
上传也可能编码在提交命令内。此计时不包含 server 外层全局 poll、guest CPU、锁或请求往返。
submit-execute 内仍可能包含驱动/GPU 等待，不能由此排除 GPU。
诊断 CPU：Heaven 260.54%、VirGL child 61.46%、wineserver 10.88%，100%=一个逻辑核。
此前 busy-wait handler 仅约 0.40%、context_finish 约 0.01%；本轮没有删除同步或伪造 busy=false。

## 实际候选

补丁 0018 修改非线程 fence poll：当存在已知有效的 current renderer context 时，
不再无条件 `vrend_renderer_force_ctx_0()`。原路径先清空上下文缓存再切 ctx0，
后续 submit 又切回游戏上下文。共享上下文可访问 GLsync，非阻塞 client wait 和删除 sync
不要求 ctx0，因此候选尝试减少每条 vtest 命令附近的 EGL 上下文切换。

`WINEHUA_VIRGL_POLL_KEEP_CONTEXT` 默认 0，只有本轮候选编译为 1。
无有效 current context 时仍回退原路径；query poll 仍选择自身所属上下文。
fence 创建后的原有 glFlush 保留，timeout、FIFO、退休与回调顺序、threaded 分支不变。
Mini 路径为 egl-main / GLsync；没有启用原来禁用的 EGL native fence。

复用冻结 host 对象，重链接未改动基线后，renderer 与普通包完全同 SHA：
`e70dd77e556d84d09659f6be3f65a202d3deedd52fb4025eebb4ac9ad3e8a49c`。
仅重编 `vrend_renderer.c`，候选 renderer 为
`92fcccdd69b29d06716223e64b3fef561cae34ec68a272855f5ed81905459cbf`，2938240 bytes。
HAP 只改 `libvirglrenderer.so` 和 `.so.1` 两个别名，其余 326 个 payload 逐 SHA-256 一致。
候选不包含 command/busy 专用诊断，guest Mesa 与 Venus 不变。

## 真机完整对照

统一启动门控 Battery≤35°C、system_h≤40°C、shell_front≤38°C，最长冷却 600 秒。
加载通过画面判定后统一预热 20 秒，完整 26 场景，中央约 240 秒 CPU 采样。
所有列入分析的结果页均人工查看；launch、环境、温度、原始样本和场景截图保存在证据目录。

| 轮次 | FPS | Score | min / max FPS | Heaven CPU | VirGL child CPU |
| --- | ---: | ---: | ---: | ---: | ---: |
| virgl-poll-a1 | 16.1 | 407 | 5.9 / 42.4 | 255.24% | 60.17% |
| virgl-poll-a2 | 16.1 | 406 | 5.8 / 41.6 | 257.09% | 60.55% |
| virgl-poll-b1 | 16.6 | 418 | 6.3 / 40.2 | 259.34% | 59.09% |
| virgl-poll-b2 | 16.8 | 423 | 5.8 / 47.1 | 257.45% | 57.32% |

| 轮次 | 启动温度 Battery / system_h / shell_front °C | 采样末温度，同序 °C |
| --- | --- | --- |
| virgl-poll-a1 | 35.00 / 37.33 / 36.72 | 37.00 / 45.88 / 42.52 |
| virgl-poll-a2 | 35.00 / 37.61 / 37.07 | 37.00 / 46.27 / 42.87 |
| virgl-poll-b1 | 35.00 / 37.72 / 37.20 | 38.00 / 46.93 / 43.26 |
| virgl-poll-b2 | 35.00 / 37.72 / 37.28 | 38.00 / 46.33 / 42.91 |

CPU 占用只是累计成本，不能将占用升高当作提帧，也不能用进程总占用判断关键线程余量。
帧率收益以同设置完整结果为准。该样本不能推广到 High、其他游戏或长时间稳定性。
加载 OCR 在不同场景可能多等待几轮，因此额外加载/等待与温度轨迹并非严格相同；
此小幅差异不足以给出统计显著性或稳定百分比承诺。

## 推广决定与 30% 目标

本轮保持普通构建默认关闭轮询候选，Mini 收尾恢复普通基线包。
除收益量级以外，当前 renderer context cache 与所有 context 创建/销毁、sub-context 切换、
外部 EGL 恢复失败的组合仍需单独覆盖。生产函数单元测试的 cross-context fence 回调，
不等于这些真实 EGL 生命周期已经验收。不能由这次单负载画面正常直接推广所有游戏。

下一项优先拆分 submit-execute 内部的 opcode/字节量和耗时，区分解码、状态设置、
draw、资源上传/复制及驱动等待；同时补 guest CS finish/map/query 与 virpipe 请求排队、
socket mutex/往返的有界聚合。由实际热点选择批量化、去冗余状态或有界资源复用，
再做独立候选对照。提高 CPU 总占用、增加线程数量本身不构成收益。
GPU busy/时间戳与同画质分辨率对照仍未完成，不能宣称 GPU 已排除。

## 验证与复现

- `make test-virgl-command-timing`：抽取生产 submit handler，验证两种宏模式、阶段耗时、
  关闭时无 clock、fence/error parity、payload 计数与五秒限频；ASAN/UBSAN、Werror 通过。
- `make test-virgl-poll-context`：抽取生产 check_fences/merge 函数与生产 list.h，
  验证 current/null context、FIFO timeout、回调、mergeable/cross-context 与 threaded parity；
  两种宏模式、ASAN/UBSAN、Werror 通过。
- 固定源码 30 文件应用完整 0001..0018，effective source SHA 与 lock 一致；
  二次幂等、完整 reverse/replay，以及 busy/command/poll 两模式生产测试均通过。
- 保留原有十个 dirty 子模块与既有 Venus 修改，不重置、不提交 gitlink。
- 既有 production present producer-context/replies 测试 204 项通过；不代替真实 GPU 生命周期验收。

Windows 固定证据：`F:/VintagePomelo-Workspace/artifacts/mini-virgl-command/` 与
`F:/VintagePomelo-Workspace/artifacts/mini-virgl-poll/`。
原始分析可由固定任务的 `analyze-command.py`、`poll-analyze.py` 重算；
本轮安装与完整测试使用 `poll-install.py`、`poll-run.py`，所有设备动作持有 `mini.lock`。
旧 performance/busy-wait 冻结证据不变。包仅在任务目录保存一份，证据用小型 SHA 收据引用。
