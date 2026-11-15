# Mini Zink GPU 呈现与能力核查（2026-11-15）

r14 已安装到 Mini，新 GPU 呈现通道已接通。窗口、尺寸切换及 Heaven 短观察通过；此前 varying 和 3D PBO 上传正确性修复仍通过回归。新通道默认关闭，按游戏环境变量 WINEHUA_ZINK_PRESENT=1 开启，0 保留原 CPU 读回路径。

## 实现与正确性

0025 接通 DRI pbuffer → Zink 颜色 VkImage → 已有 Venus 私有呈现 → Host Vulkan presenter。仍走 Venus，未改成系统 Vulkan 直通；Host 仍需 GPU 图像呈现/拷贝，不是完全零拷贝。

DRI 专用 bool 回调报告真实成功/失败，并绕开旧 VirGL SWRAST 类型门槛。Vulkan loader 过滤私有名字时，仅针对 MESA_VENUS 查找 Venus 接口，持有库引用至 screen 销毁。共享窗口页验证大小、magic/version 和 surface 身份。仅处理 level/layer 0、2D、单采样、非深度、非零尺寸图像。

顺序为 transfer-src barrier → 提交渲染 → 呈现 → 新有序 batch/fence 引用，保护 Host 异步拷贝期间的对象寿命。没有新增 GPU idle 等待。真机证明 Zink DRI 原始图像已经是 Host 所需方向；r13 额外翻转导致倒置，已移除。r14 直接呈现原图，无额外 GPU 翻转或 scratch 图像。r12/r13 已淘汰，不作有效性能结果。

窗口 240 帧、三轮 320×240→512×288→320×240，三个像素检查均正确，最终四象限截图方向颜色正确。Heaven 抽查文字、场景方向和材质颜色正常。Heaven 日志 serial=240/result=0，presents=240/readbacks=1；窗口尺寸变化后 presents=120/readbacks=1，证明逐帧 CPU 读回被绕过。

## 性能观察与边界

同一 r14 包、同一 EXE/hook，D3D9 / Low / 1280×720 / AA0 / VSync0 / tess disabled。每组自由相机观察90秒，未运行完整 benchmark。

| 路径 | 60秒游戏内FPS | 90秒游戏内FPS | 外层FPS（60/90） |
|---|---:|---:|---|
| CPU读回，开关0 | 6 | 9 | 6 / 8 |
| GPU呈现，开关1 | 11 | 9 | 10 / 9 |

游戏 FPS 为人工查看截图右上角读数，外层 OCR 只作旁证。机位不同；关闭组起始 Battery/system_h=33/36°C，开启组33/38.055°C。不能把瞬时值计算成平均收益、不能宣称稳定30%、不能直接与旧 VirGL/DXVK9 完整平均相减。

60–90秒进程累计 CPU 差值（100%=一核；CLK_TCK=100 沿用已交叉验证值）：

| 进程 | 关闭 user/kernel/total | 开启 user/kernel/total |
|---|---|---|
| 游戏 | 110.43 / 106.82 / 217.26% | 147.93 / 108.68 / 256.61% |
| 图形服务子进程 | 79.27 / 36.61 / 115.88% | 62.37 / 45.21 / 107.58% |
| 主应用 | 58.19 / 2.44 / 60.63% | 10.36 / 4.23 / 14.59% |

主应用 CPU 降低与绕过 SHM 合成相符；游戏 CPU 上升不等于回退，不同机位也不足以确定效率变化幅度。kernel 时间仍高，没有 GPU busy/timestamp，不能判断 GPU 满载与否。下一步分离 Venus ring drain、Host mapped-memory/整队列等待与实际 GPU 执行，不盲目加线程。

## OpenGL兼容性边界

当前真机 Zink legacy context 实际返回 OpenGL 2.1 Mesa25.0.1；不要混为 VirGL 历史3.1或宿主GLES3.2。32位 PE probe 只创建 instance 查询当前 Venus 物理设备能力，不创建 device/启用 features：

| 能力 | 当前Venus路线 |
|---|---|
| VK_EXT_transform_feedback / transformFeedback | 扩展未暴露 / false |
| geometryShader / tessellationShader | true / true |
| conditionalRendering | true |
| multiViewport / maxViewports | false / 1 |
| shaderFloat64 / shaderInt64 / dualSrcBlend | false / false / false |
| maxImageDimension2D / Cube | 8192 / 8192 |

Mesa GL3.0 判定要求 EXT_transform_feedback，当前缺失是明确阻塞之一。GL4.1还需要更大纹理上限、多视口及前序版本双精度等能力。现有 Zink GS HACK 仍保留，未未经验证删除。Venus 源码本身有 TF 透传定义，进一步归因须对照 Host 原生枚举；不能当作物理 GPU 永久不支持的证明。不用版本号或扩展 override 伪造支持，也不把解禁GS等同于完整版本升级。

## 验证与未完成范围

- 生产呈现函数 mock 覆盖直接原图、提交→呈现→引用退役、resize/backing更换、错误、无效共享页、仅Venus接口回退。普通及ASAN/UBSAN通过。
- 原批量上传生产函数回归/ASAN/UBSAN、Mesa增量构建、完整overlay reverse/replay幂等、两个patch SHA lock、根仓diff check通过。
- r14按游戏开启呈现：varying15/15；3D纹理三组各32层 mismatchBytes=0。
- 未验证仙剑4、所有窗口组合、设备丢失后端到端恢复、长稳或全部OpenGL扩展。
- 新通道暂不默认启用：Wine ready路径export失败后当前返回失败，同帧CPU fallback和Host ready撤销/恢复仍需端到端验证；mock的false返回不代表已完成。显式开关0可回到原路径。

候选只替换Gallium，其余payload和UI逐文件一致；实验overlay不代表全量release parity。签名/profile/证书/payload核验通过。本轮未改UI/图标、系统后端偏好、原游戏/配置文件。没有新提交、push或CI，既有根仓/UI及dirty子模块保留、未提交gitlink。

- signed SHA256 fa0b06cb8be3bd0ec0c71f9acda893ccc54afe88140c1dc7cb316020d92d5886，629688052 bytes。
- Gallium SHA256 639c7cb8e6e193071d49d55bb45a78445173f01627a8a25a8182574965d2c084。
- 0025 SHA256 fc6b3d2f1add26b3dbc38381f07d1c9a49fc3673d12453cceaa93b305ee91d4f。
- HEAD56cfd9946b814fa28b9d1875cd1c659b59d7159e。

原始证据：workspace_temp/mini-virgl-performance/backend-heaven-comparison/zink-r14-*；能力查询zink-r12-vulkan-capabilities；汇总zink-gpu-present-results.json；主机验证zink-present-host-verification.json。所有设备动作仅Mini、持mini.lock、有超时和有界日志。结束应用停止、临时文件清空，原EXE和两份cfg SHA不变。

保留Linux task的r14当前+r11回退两个signed HAP，r14 unsigned为唯一增量基底。已按SHA淘汰r9/r12/r13和旧基底/独立库，累计删除3818576566bytes（Windows 629683606，其余Linux内部回收，不等于F盘净释放）。保留源码/增量缓存/签名材料/用户数据，无额外F盘HAP副本。收尾maintain-storage Apply/CompactLogs完成，actions0/issues0；F可用22.11GiB。详见zink-present-storage.json。
