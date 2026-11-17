# Gamescope 颜色与性能复核

工作分支 `feature/gamescope-port`，日期 2026-11-17。Mini 断连后用户要求先推进代码，
充电后再测试。本记录区分源码/主机结果与尚未完成的设备验证，不承诺 FPS 增幅。

## 颜色修复与证据

SDR 输入是 sRGB 编码值，最终输出为 RGBA8 UNORM / SRGB_NONLINEAR。
storage image 写入不会替应用自动编码 sRGB。嵌入路径此前没有设置
`applyOutputColorMgmt`，shader 解码输入后直接把线性值写出，造成偏暗。
现在主合成显式开启 SDR 输出编码；源分辨率的裁切/行方向归一化使用 passthru，
保留原始编码值，供 EASU 或最终合成采样。多批次合成重新采样已编码的 SDR 图像。

`test-gamescope-color-gpu` 使用 OHOS core 构建所生成的实际 SPIR-V，在容器 lavapipe
执行。关联配方、构建收据、测试源码与五份 shader 的 SHA-256；拒绝旧配方输出。

| 用例 | 主机结果 |
| --- | --- |
| SDR 中间色 64/128/192、灰色 128/128/128 | 输出保留输入值，容差 1 |
| passthru 归一化颜色 | 保留编码值；本例不代替裁切几何验收 |
| 关闭输出编码的失败对照 | 同一输入变为 13/55/134，被拒绝 |
| 重复编码的失败对照 | 偏亮输出被拒绝 |
| 实际 FP32/FP16 EASU shader | 17×17 奇数输出范围像素正确 |
| RCAS 普通/1px shader | 最小图边缘取样及不透明输出正确 |

这些是实际 shader 的主机正确性测试，不证明 Maleoon 驱动、屏幕色彩或 GPU 同步已验收。
此前 Mini 上 BGRA8 compute 中间纹理的异常已通过 RGBA8 候选隔离；继续保留 RGBA8
修复、边缘钳制、alpha=1 及跨提交内存屏障，不因恢复 FP16 而撤销这些修复。

## 已实现的性能调整

| 项目 | 改动和预期作用 | 边界 |
| --- | --- | --- |
| 按需管线编译 | 嵌入模式显式设置 `shader_precompile=0`，避免因不经过 main 的配置解析而预编译完整组合矩阵 | 主要降低启动 CPU 开销；首次用到某个管线仍可能编译，不算稳定 FPS 收益 |
| EASU 精度 | 恢复上游基于 `shaderFloat16` 和 `shaderInt16` 的 FP16 选择；缺能力继续 FP32 | 两种 shader 主机正确；Mini FP16 画面和耗时待测 |
| 直接写交换链 | 真实 WSI storage usage 可用时，core 最终合成直接写 RGBA8 交换链 | 省掉一次全输出 GPU 拷贝和每槽完整中间纹理；不是 CPU 零拷贝声明的替代 |
| 创建失败回退 | storage 不可用或交换链/视图等创建失败时回到原 GPU copy 传输 | 不切换用户图形后端；运行中设备丢失仍按原错误退出处理 |

Mini 既存有界日志中 `supportedUsageFlags=0x9f` 包含 STORAGE，具备尝试直接输出的依据。
新路径仍使用 UNORM storage view 与显式 SDR 编码，不创建不需要的 sRGB sampled view，
不要求交换链 mutable format。借用 VkImage，只拥有视图，不释放交换链的图像或内存。
acquire 信号量在 compute 写入前等待，结束屏障转 PRESENT；消费完成 fence、生产者归还、
每个交换链图像的呈现信号量和 resize 排空沿用原生命周期。嵌入路径把 acquire 等待并入
已有输入提交，不额外增加正常帧提交次数。

以历史输出 2446×1231、RGBA8、三个交换链图像估算：省掉的拷贝每帧约 24.09 MB 读写，
60 FPS 时约 1.45 GB/s；本地中间纹理约 34.46 MiB。这是未计压缩、分配对齐的理论数据量，
不是已测带宽、显存或 FPS 收益。

`test-gamescope-output` 编译实际生产 `recreateOutput`、storage 视图初始化和清理方法，
注入不支持 storage、创建/信号量/视图失败、回退失败、无有效能力和 resize。
校验被退休交换链不被重复作为 predecessor、部分资源正确清理，并执行实际屏障/拷贝
记录函数验证直接路径零拷贝与回退路径一次拷贝。这不是实际 WSI 驱动验证。

## 与 HSL 的关系和后续测量

HSL 的上传复用、描述符和 shader 修复已迁入；本次复用了其真实颜色 shader 测试思路，
并在 `migration-sources.json` 记录原文件、提交和散列。HSL 的运行环境和最终输出不同，
不能仅凭其 FPS 推断 VPP 一定是 CPU 或 GPU 瓶颈。VPP 原先额外的最终拷贝已从代码消除；
GLES 行方向/客户区归一化 pass，以及 FSR 的 EASU/RCAS 仍有真实 GPU 成本。

充电后先验证 PAL4 菜单/存档页中间色、边缘、标题栏与触摸坐标，再核对
`storageOutput=1 gpuOutputCopies=0`、FSR pass 和真实图形后端。随后五轮正常退出重入、
三轮 resize/后台恢复，检查归还 fence、资源增长和第二次启动。手机使用同进程路线，
但必须独立验证；X11、Steam、Heaven、DX11 和旧 Zink 白地面仍分别列项。

同后端、相同游戏与输出尺寸，对比普通模式/上一候选/新候选；每组预热后 3×60 秒，
记录游戏 FPS 和帧时间。利用已有默认关闭的 `[GAMESCOPE-GPU]` 时间戳报告区分
EASU/最终合成 GPU 时间，结合宿主调度与 acquire 等待；没有可用 CPU/GPU 占用采样时
明确标记缺项。降低游戏分辨率后的 FSR 单独比较，不能计入同条件架构收益。

当前仅本地提交与调试包，保留 2.0.4 版本及正式交付包；不合 main、不推送、不上架。
