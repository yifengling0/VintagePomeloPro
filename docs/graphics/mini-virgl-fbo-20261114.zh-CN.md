# Mini VirGL 不可变附件完整性缓存试验

完整 A-B-B-A 同画质对照：基线中位数 16.30 FPS，候选
16.10 FPS，观察变化 -1.23%。观察变化较小，尚不能确认稳定提帧；不推广，已恢复普通基线。
30% 目标本轮尚未达到。
原始 16.1 FPS 参考目标为 20.93 FPS；按本轮基线计算为 21.19 FPS。
0018 上轮轮询候选与本轮候选独立测试，收益不能相加。

## 范围和方法

- Mini MLR-AL10；Heaven 4.0，Low / 1280×720 fullscreen / D3D9-WineD3D / AA0 /
  tessellation disabled / VSync0 / writev=0 / GLThread 未指定。
- 顺序 A1-B1-B2-A2，每轮完整 26 场景，中央约 240 秒进程 CPU 采样。
  结果页与每轮 60/120/180 秒场景图已查看，抽样未见损坏，不等于逐帧或全游戏视觉回归。
- 启动前统一门槛：Battery≤35°C、system_h≤40°C、shell_front≤38°C，最长 600 秒。
  门槛相同不等于每轮温度完全相同；只保留启动前与采样末快照，不推断峰值或持续降频。
- 正式 FPS 轮未启用 opcode/command/busy 计时或 profiler，未启用 0018。
  CPU 百分比 100%=一个逻辑核；不能用总占用作为效率或 GPU 瓶颈排除指标。

| 轮次 | FPS | Score | Min / Max FPS | Heaven CPU | renderer CPU |
| --- | ---: | ---: | ---: | ---: | ---: |
| virgl-fbo-a1 | 16.3 | 412 | 6.0 / 38.4 | 255.16% | 60.25% |
| virgl-fbo-b1 | 16.0 | 403 | 5.8 / 39.8 | 256.18% | 60.80% |
| virgl-fbo-b2 | 16.2 | 408 | 6.0 / 41.7 | 256.27% | 60.64% |
| virgl-fbo-a2 | 16.3 | 410 | 6.3 / 41.7 | 255.70% | 61.32% |

| 轮次 | 启动前 Battery / system_h / front °C | 采样结束 Battery / system_h / front °C | 启动至 F9 |
| --- | --- | --- | ---: |
| virgl-fbo-a1 | 33.00 / 36.33 / 35.39 | 37.00 / 45.88 / 42.57 | 149.9 s |
| virgl-fbo-b1 | 35.00 / 37.67 / 37.15 | 38.00 / 45.62 / 42.99 | 107.7 s |
| virgl-fbo-b2 | 35.00 / 37.94 / 37.29 | 38.00 / 46.93 / 42.85 | 86.4 s |
| virgl-fbo-a2 | 35.00 / 37.61 / 37.10 | 38.00 / 46.87 / 43.09 | 128.6 s |

每组两轮，只支持此设备/此画质的小样本观察，不能宣称统计显著性或推广到 High/其他游戏。
采样场景截图会短暂扰动测试，四轮使用同一采样方法。
就绪由有界 OCR 判断，确认后统一暖机 20 秒；加载及识别耗时不同，实际启动至 F9 时间列于上表。
没有在 F9 时另测温度，不能假定四轮开始基准时处于完全相同的热状态。

## 由数据选择的候选

此前 opcode 诊断 105.063401546 秒内，DRAW_VBO 占墙钟 18.0383%，
SET_FRAMEBUFFER_STATE 占 7.2553%，GL 错误检查约 0.9368%。
opcode 时间包含驱动/GPU 等待和计时开销。没有直接分离附件绑定与完整性查询的成本，
不能把整个 7.2553% 都归到 `glCheckFramebufferStatus`，也不能直接换算预期 FPS。

补丁 0020 仅缓存驱动已经判断完整的附件组合，固定 32 项、每 sub-context 独立，
不持有资源引用，不创建额外 FBO，不改变渲染次序。限定内部不可变单 mip 纹理，
外部 EGL/GBM/memory-object/D3D 导入、可变和多 mip 资源一律重新查询。
对象插入/销毁前清空缓存，处理 handle/pointer/GL name 复用；incomplete 不缓存，继续记录错误。
附件绑定、所有 handle 验证、viewport/sRGB、shader/blend dirty、decoder `glGetError`、
fence、协议和正常轮询路径保留。

宏 `WINEHUA_VIRGL_FBO_STATUS_CACHE` 默认 0，不默认推广。
完整性能收益和真实 GL 生命周期仍需更广验收；host stub 覆盖不等于真实驱动异常恢复验收。

## 构建与验证

复用固定冻结增量对象，重链接基线 strip 后 SHA 与既有 HAP 完全匹配，
候选只重编 renderer.c 并启用 cache，保留原始增量树。

- 基线 HAP：627114490 bytes，SHA `d172b43c5f1bb024e03ae8dd7dff7424f3041353e9e829c80c14eef8348277e2`。
- 候选 HAP：627137785 bytes，SHA `56e3a46594d4c291de07d17d537772e64cc9d56f8e88a93f02009666b8af68b7`。
- renderer SHA：基线 `e70dd77e556d84d09659f6be3f65a202d3deedd52fb4025eebb4ac9ad3e8a49c`；
  候选 `172131e6c9a6f31fab7ddd9a02d4d56074b79b86fee07f6a638de40b8694e13c`。
- 两包 2.0.2/2000020 release/internaltesting，debug=false、profileable=false；
  签名/profile/证书验证通过，只换两个 renderer 别名，其余 326 payload 逐 SHA 一致。
  实验 overlay，不等于整份提交的全量发布构建。
- FBO 生产函数两种宏模式 ASAN/UBSAN、Wall/Wextra/Werror 通过，覆盖首次/命中、MRT 顺序、
  深度/空附件、无效 handle、对象插入销毁、外部/多 mip/可变排除、incomplete 重查、容量逐出、子上下文隔离。
- opcode 生产计时验证默认关闭无 clock；busy/command/poll 生产回归均通过。
  固定 32 文件，0001..0020 全栈 SHA lock、幂等、reverse/replay 通过；既有 present 204 项通过。
- performance/busy/command/poll 四份旧冻结 manifest 共 313 文件已逐 SHA 验证未变。

## 下一步

优先把 DRAW 分为 program selection、uniform 上传、sampler/vertex binding 和实际 GL draw，
并把帧缓冲阶段分为附件更新、状态发射与完整性查询。必要时增加 cache hit/miss 聚合，验证真实命中率。
opcode 数据中 SET_CONSTANT_BUFFER 编码约 5.21 GB/105 秒，先测重复率和 guest 侧 WineD3D→virpipe
打包/CS/锁等待；现有 encoder 已用 memcpy，不能仅靠改成 memcpy 推断收益。
保留 GPU busy/时间戳和同画质分辨率对照作为未完成项，不盲目加线程或绑定所有大核。
30% 若按串行帧时间简化模型意味着减少约 23.1% 的帧耗时，仍需命中主要关键路径。

完整原始数据、签名和构建收据同目录；普通基线恢复及停止状态见 `final-state.json`。
只做本地提交，没有 push/CI/其他设备操作。HANDOFF 为后续入口。
