# Mini Heaven 后端实测对比（2026-11-15）

设备 Mini MLR-AL10，OpenHarmony 7.0.0.111，应用元数据 2.0.2 / 2000020。
这是候选路线的完整 Heaven 4.0 比较；生产原入口的 D3D9 overlay 尚未接通。未更改产品默认配置。

## 完整成绩

统一 Direct3D9、1280×720 fullscreen、Low、AA=0、VSync=0、Tessellation Disabled。使用同一临时诊断 EXE / hook，F9 内置 26 场景测试，结果截图逐张人工复核。

| 路线 / 轮次 | FPS | Score | Min / Max FPS | 电池温度 前→后 °C |
| --- | ---: | ---: | --- | --- |
| virgl / virgl-a1-run | 16.5 | 417 | 6.1 / 43.6 | 32.0 → 36.0 |
| dxvk9 / dxvk9-b1 | 18.5 | 467 | 6.4 / 38.3 | 36.0 → 39.0 |
| dxvk9 / dxvk9-b2-confirmed | 17.0 | 427 | 5.4 / 28.8 | 38.0 → 39.0 |
| virgl / virgl-a2-retry | 16.1 | 407 | 5.3 / 40.6 | 37.0 → 39.0 |

按本组各路线两次成绩的中位值，DXVK9 相对 VirGL 为 +8.90%。样本量小、起始温度未完全匹配、菜单识别导致预热时长不同，因此只作为探索性结果，不是严格控温后的稳定收益结论。
连续测试存在轮次波动，不取最高值代表整体。按用户最新方针，以适度收益和兼容性为目标，不设统一30%门槛。

## CPU 开销

100% 表示一个逻辑核。来自 /proc/<pid>/stat、AT_CLKTCK=100，取跑分约第26–230秒的同一窗口。游戏 GetProcessTimes 以5秒间隔独立核对；本次两者趋势一致。

| 轮次 | 游戏 CPU % | 图形服务端 CPU % | wineserver CPU % | App CPU % |
| --- | ---: | ---: | ---: | ---: |
| dxvk9-b1 | 210.9 | 141.9 | 8.9 | 19.7 |
| dxvk9-b2-confirmed | 227.8 | 146.2 | 10.8 | 18.7 |
| virgl-a1-run | 254.1 | 60.1 | 10.6 | 19.6 |
| virgl-a2-retry | 256.3 | 61.1 | 12.4 | 17.9 |

DXVK9 的游戏进程开销下降，但服务端开销上升；不能把“提高 CPU 利用率”本身视为优化目标。本次没有 GPU busy、GPU timestamp 或线程等待耗时，不能断言 GPU 没跑满，也不能把服务端 CPU 上升直接归因为某一种等待。

## 兼容性改善的已验证范围

此前DXVK9存在EVENT查询长期Pending、CPU写入影子内存未发布导致黑色绘制、固定功能顶点输入超限、顶层尺寸相同但mip数不同的UpdateTexture被误拒绝，以及backing列表并发读取风险。已保留显式适配开关与定向正反对照；本轮同一修复DLL完成两次Heaven全部26场景，使结果从无法完成/黑纹理推进到可计分。不同修复的独立证据见此前EVENT/纹理/固定功能适配报告。这证明Heaven和已测probe的范围，不能推广为全部DX9老游戏兼容。

## 真实路线与局限

- VirGL：builtin d3d9 / wined3d / opengl32，renderer=virgl (Maleoon 910)。
- DXVK9：实际 Z:\d3d9.dll，d3d11 / dxgi / wined3d 未加载；GPU 名称 Virtio-GPU Venus (Maleoon 910)，仍经过 Venus。请求的 isolate-vulkan-direct 不是直通证据；目标进程实际记录 WINEHUA_GRAPHICS_PROFILE=product-vulkan。
- D3D9 使用上一轮 0017 EVENT、0018 mip 匹配、0019 backing 快照修复及其余 shadow 开关；本轮没有新增生产性能补丁。
- 采样画面通过检查、完整测试完成；不是每个场景逐帧正确性或长稳验收，也没有高画质/其他游戏的性能结论。
- 进程内 BCrypt SHA-256 探针核对了实际安装的 Gallium / EGL / wine_child 库；安装HAP本体仅核对大小，未取得完整安装HAP的SHA。VirGL / DXVK9 成绩来自UI基线运行库，Zink使用更新Mesa/EGL/wine_child的候选包，不能称三路线所有运行库完全一致。

## 未计分项

- dxvk9-b2: AssertionError('F9 benchmark not confirmed: Sonnrj 0 下 下 Tim 5 ． 2s 下 rame 头 81 Max FPS: 22 1 Scene: 7 / 26 亠 一 ． Press ESC to Cancel FPS: 15 ． 6 Time_ ， 5 ． 2s 81 Min FPS: 150 Max FPS: 22 一 1 Scene. 一 1 / 26 Pre ss ESC to Cancel 出 Camera TesselIation ； Settinas')；不计入 FPS 比较。
- virgl-a1: AssertionError('/data/local/tmp/vpp-heaven-evidence.jpeg')；不计入 FPS 比较。
- virgl-a2: not-admitted；不计入 FPS 比较。
- zink-admission: not-admitted；不计入 FPS 比较。
- zink-candidate: not-admitted；不计入 FPS 比较。

首次Zink准入使用UI基线运行库，停在像素格式/Direct3DCreate9初始化；不能归为readback候选失败，也不计入FPS。

## Zink兼容性定位与修复

首次 zink-admission 使用的是UI基线库：wined3d_caps_gl_ctx_create找不到合适像素格式，不能作为新Zink候选的失败。安装并用实际进程SHA验证readback候选后，基础GL clear/draw/readback通过；但zink-candidate的Heaven仍黑屏和上沿条纹，未计分。

新增同一PE32分段像素探针：清屏、立即模式、VBO/IBO、非零偏移subdata/map、显式flush邻接切片保留、纹理首次上传/局部更新/PBO、GLSL纹理与常量更新、矩阵、默认帧缓冲及FBO→窗口复制，共15项。VirGL 15/15通过；旧Zink候选14/15通过，2×2 RGBA局部更新后的绘制仍采样绿色而非新蓝色。

0017-zink-shadow-image-staging-byte-range.patch修正0016上传范围：图像暂存对象虽然是buffer，box.width的单位仍是texel，不能当字节数。图像暂存改发布width0表示的完整紧密排列暂存切片，包含行/层和压缩块字节；普通buffer继续按实际byte offset/size发布，保持默认关闭及既有GPU同步。

主机测试提取真实生产flush函数，覆盖2×2 RGBA、行/层、压缩字节数、图像非零原点、普通buffer偏移、暂存buffer偏移及开关关闭行为；ASAN/UBSAN通过，旧算法mutation被拒绝。主机测试不替代真实设备验收。

修复候选真机分段结论：未全部通过，详见probe.txt。
0017的字节范围回归通过，但同一15项真机矩阵仍14/15，不能称它解决了上述采样错误或Heaven黑屏。

进一步glGetTexImage读回确认纹理2×2区域本身已为正确蓝色，绘制仍采样绿色；ZINK_DEBUG=noreorder,sync诊断也未改善。当前阻点位于坐标/采样/状态链路，没有证明缺少同步就是根因；诊断选项不作为性能优化发布。

同一新增坐标诊断PE32，VirGL 18/18通过，Zink15/18；三个失败项均来自固定功能纹理采样。纹理glGetTexImage的2×2区域正确蓝色，CPU当前纹理坐标为(0.5,0.5,0,1)，GLSL片元硬编码vec2(0.5)采样蓝色通过，重绑纹理仍失败。支持继续定位固定功能坐标/矩阵/属性与生成shader；尚未证明具体根因，不能称Zink或Heaven已修复。

不能将一项局部上传修复等同于Heaven或全部老游戏已兼容，也不能用黑屏时的应用overlay帧率参与比较。

VirGL 首次 A2 在 Engine::init 后未过加载门槛，窗口130秒仍无帧，采样线程也无有效输出；仅能记为启动未完成，不能判定 GPU 或 VirGL 根因。停止后重试通过。DXVK9 首次 B2 因 OCR 漏识别 Frames 中断，截图实际已进入1/26跑分；修正判据后重新完整测量。

## 接下来如何优化

1. 复用已经存在的系统默认+单游戏覆盖机制（GraphicsBackendSettings、AppSettingsStore、AppSessionService、WineEngineService），不要重做配置系统。当前游戏default继承系统，已有dxvk_1_10/dxvk_2_6/virgl三项；Zink通过游戏准入后再扩展。
2. 原入口接入仍需同时补Wine search_winehua_dxvk_overlay的d3d9.dll、D3D策略中当前d3d9=b的路由、runtime manifest/架构DLL及游戏级shadow开关；本轮诊断basename/SetDllDirectory不能代替产品接入。DX9适配不能改变D3D11既有策略，游戏内用户DLL覆盖和VirGL回退需单独验收。
3. 优先给 Vulkan 服务端补每帧聚合计时：命令解码/提交、shadow上传/flush、驱动API、fence/query等待、present。再与游戏 CS/主线程和 GPU 时间对齐；只对关键路径做一项改动的对照。
4. 根据计时结果选择减少重复上传、缩小dirty range、批量提交或减少同步往返。不能靠删除必要flush/fence换分。
5. Zink 先解决真实 API/renderer/caps/纹理正确性，再加入排名；当前不靠 GL 版本号覆盖强行跑分。

## 证据与恢复

原始证据：`F:/VintagePomelo-Workspace/workspace_temp/mini-virgl-performance/backend-heaven-comparison`。每轮 launch、模块/Engine参数、result.jpeg、verified-result、CPU raw/summary、final-device-state 单独保存。
各轮均持 mini.lock，只操作 Mini。应用已停止，临时 EXE/DLL/日志/诊断 cache/截图已清理，原 Heaven.exe、data/heaven_4.0.cfg、bin/unigine.cfg 校验值保持。未修改 UI/图标、其他设备或用户存档。
任务存储清理结果另见 storage-final.json；新Zink图像修复候选只替换既有readback包的Gallium库，其余有效payload逐项校验一致。收尾保留稳定包与最新候选各一份，旧实验包以小型SHA收据代替。
