# Mini Heaven DXVK9 EVENT 适配（2026-11-15）

真实 D3D9 Heaven 已越过 EVENT 查询导致的启动停顿，进入动态场景。画面仍有黑色区域和明显光照异常，尚未通过正确性，不能用即时 FPS 排名或宣称30%性能收益。VirGL/GLES 保持稳定默认，所有新增实验默认关闭；Vulkan 实际仍经 Venus。

## 停顿证据与修复

目标进程 CRT 和 Engine::init 均收到 direct3d9，模块明确为 Z:\d3d9.dll 与 builtin vulkan-1.dll，d3d11/dxgi 未加载。Heaven 创建 type=8 EVENT 查询，以 Issue(END)、GetData(nullptr,0,FLUSH) 等待。原路径在约50秒轮询八千多万次仍 S_FALSE；内部诊断显示 CS End 已执行、resetCtr=0、pending submissions=0、CS chunk empty，但 GPU event 状态持续 Pending。不能归因为参数丢失或漏传 FLUSH；尚未细分 VkEvent/Venus/宿主驱动的最终根因。

0017-d3d9-opt-in-event-completion-fence.patch 提供 WINEHUA_DXVK9_EVENT_FENCE=1，利用已有 sync::Fence 和 ctx->signal 的真实提交完成通知代替 VkEvent 查询。CPU Issue 递增 issued generation，CS End 按 FIFO 递增 submitted generation，旧提交不能满足重新 Issue。GetData 保持非阻塞，不伪造完成、不添加全局 GPU wait。设备丢失时返回 D3DERR_DEVICELOST，避免提交清理中的 CPU signal 被误认为成功。默认 VkEvent 和其他查询行为保留。

## 对照范围

原始证据固定在 F:/VintagePomelo-Workspace/workspace_temp/mini-virgl-performance/backend-heaven-routing/，各 label 保存实际 launch、命令台账、日志和清理收据。

| label | 结果 |
| --- | --- |
| d3d9-query-flags | EVENT type8，Issue flags1，GetData flags1/bytes0 |
| d3d9-query-internal-event-rpc | CS已执行，reset0/pending0，event持续Pending |
| d3d9-native-event | 进程内Unix VN_PERF加入no_event_feedback仍停顿；不能证明关闭了已初始化的feedback |
| d3d9-event-fence-control-off | 同DLL关闭开关，约1890万调用、Present/Draw=0 |
| d3d9-event-fence-confirm-on | 同DLL开启，Engine返回，1161次成功Present、1237262次Draw |
| d3d9-final-no-com | 最终device-loss guard版，撤掉所有COM hook/Unix环境修改，仍进入动态飞船场景 |

off/on使用同一4296704 bytes DLL，SHA256 7586fcf0a167641dabd71b9d1f9f969af78f849678a4f4f8c238813c395fc929。最终4300800 bytes DLL SHA256 b45bf0bb5a0b93b05b5096d15daabf7fd07ea2d25ce5c615c0e5ff403316fbb9；final-candidate-build.json记录该版本和minimal hook。不能将两轮设备SHA混用。

最早d3d9-boundaries诊断错误截断DXVK私有vtable尾部，造成探针引入的Release停顿，该轮排除定位结论。后续改为仅原地替换公开槽位，保留私有和析构槽位；最终无COM hook复核排除了该探针影响。

## 尚未通过的验收

场景有纹理且会动态变化，但存在黑色区域和白亮边缘/光照异常。多处 invalid call 已归属 IDirect3DDevice9.UpdateTexture -> D3DERR_INVALIDCALL。尚未采集失败源/目标纹理参数，不能断言它解释全部图像异常。15–27 FPS仅不同场景即时值，图像错误且没有统一benchmark，全部排除性能成绩。没有GPU busy或连续CPU/GPU关联测量，不能判断GPU是否跑满。

当前成功仍依赖临时目标basename、同游戏bin目录以及SetDllDirectory("Z:\\")；产品原Heaven.exe的后端路由尚未完成。Wine search_winehua_dxvk_overlay 尚未纳入d3d9，不能将诊断路由当作产品修复。

Zink本轮未修改，既有guest GL2.1/Heaven异常仍待复核实际API并定位。PC桌面OpenGL继续暂缓。

## 可复现性与验证

基于39d3edfa，本轮源码由0017、runtime lock和scripts/test_d3d9_event_completion.py承载。17补丁/34文件reverse+replay字节一致，两次幂等verify和当前源码verify-only通过。bool/SPIR-V、push constant、buffer、vertex input/FF publication及EVENT契约均通过，含ASAN/UBSAN。EVENT测试提取生产Issue/GetData/GetQueryData/End，使用真实Rc/Fence，可控CS/GPU完成顺序覆盖延迟、重发、128轮、device loss、缓存/默认/CPU路径；stale-generation mutation在assertion被拒绝。它不是全驱动并发压测。

增量构建成功，仅既有nontrivial-memcall警告。patchedSourceSha256=68c2b2cf8a8efdbf2cc9f0c2be77f90f16806c3c369e4537313c4bd9fd01b2e1。未更改旧cached分支的bool*写入问题。

## 设备与后续

本轮无新HAP，无UI/图标或用户配置变更。所有设备操作持mini.lock，仅Mini；最终应用停止、临时EXE/hook/public DLL/截图/日志/cache清除，leftovers=[]，原Heaven.exe SHA256 20b0a083a72444ac3d5b55216b21ac83ea4e3837860b87d988859dc4a3d224aa保持。十个既有dirty子模块及gitlink保留，不push/CI，不运行prepare-host.py。

下一步只针对failed UpdateTexture记录pool/format/usage/mips/尺寸，先形成可重复的小型资源案例再改源码。图像正确、真实API与产品路由确认后，进行同D3D9配置A-B-B-A benchmark。性能目标仍为历史Low720p中位16.3 FPS提高至至少21.19 FPS；尚未达到或证明。
