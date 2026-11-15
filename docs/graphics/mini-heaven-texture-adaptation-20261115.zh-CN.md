# Mini Heaven 纹理与并发资源适配（2026-11-15）

本轮修复两个具体资源问题。最终无COM探针的真实DXVK9 Heaven运行150秒，75秒飞船、135秒龙像场景均已显示材质与光照，先前大片黑色和异常亮边明显改善；采集日志未再出现invalid call、constant publication failed或CS fatal。该结果属于场景冒烟，尚未完成全部场景、相同机位图像比较、产品入口或标准benchmark，不能称全面正确或30%性能提升。稳定默认仍为VirGL/GLES，Vulkan仍走Venus，PC桌面GL继续暂缓。

## 1. UpdateTexture 顶层mip匹配

定向探针只替换CreateDevice/UpdateTexture等必要公开槽位，不触碰私有vtable尾部；失败时有界记录最多96组资源参数，不再进行无意义的FPS长跑。d3d9-texture-properties记录到上限96组，全部为ATI1/ATI2、SYSTEMMEM源→DEFAULT目标、格式与顶层尺寸相同，但目标少两个末尾小mip。

例如源128x128/8级、目标128x128/6级，旧版本一律用8-6=2作为源mip偏移，误把32x32与128x128比较并返回D3DERR_INVALIDCALL。目标纹理因此未更新，足以解释一部分法线/材质异常，但未逐纹理归属全部画面问题。

0018-d3d9-opt-in-match-top-texture-mips.patch回移上游bfd47ec8767e的顶层匹配规则，增加WINEHUA_DXVK9_MATCH_TOP_MIPS=1（默认关闭）。顶层尺寸相同时从源mip0复制，否则保留底层匹配；pool、格式、级数和尺寸等既有拒绝条件保留。没有忽略错误或伪造成功。

上游原始提交：[Try to match either top or bottom mips in UpdateTexture](https://github.com/doitsujin/dxvk/commit/bfd47ec8767e7589f86ce4b54372669da2963907)，2022-08-26。已保存精确SHA和patch到upstream-top-mips.json。Microsoft的[UpdateTexture文档](https://learn.microsoft.com/en-us/windows/win32/api/d3d9/nf-d3d9-idirect3ddevice9-updatetexture)描述更严格的底层尺寸规则；这里采用上游实际游戏兼容处理，并用Mini实测，保留开关便于回退。

小型真机探针分别为ATI1、ATI2、DXT1、RGBA生成每级不同颜色，真正绘制、GPU回读并核对三个目标mip（每轮12个像素，不只是HRESULT）。同DLL关闭时第一例UpdateTexture=8876086c，开启时12项通过；源16/5级→目标4/3级的底层匹配另12项通过。ATI驱动hack的LockRect pitch仍按现有兼容规则，probe按真正BC block pitch填充。

## 2. mapped buffer backing列表数据竞争

仅加入0018后，d3d9-mip-top-heaven-on不再记录UpdateTexture失败，但CS退出，报D3D9: shader constant publication failed。源码确认：API线程allocSlice在m_freeMutex保护下push_back到m_buffers；CS线程flushMappedSlice和其他map操作无锁遍历并保留vector元素指针，扩容会使读者失效。这是明确的数据竞争；未在失败时捕获完整并发trace，不能认定它是该次失败的唯一原因。

0019-synchronize-mapped-buffer-backing-lookup.patch将三个map路径统一为getMappedRange，使用与allocSlice相同的锁，在锁内只查找并复制VkDeviceMemory/offset/size；释放锁后才调用Vulkan或加入command-list batch，不把vector指针带出作用域，不新增GPU全局等待。m_freeMutex改为mutable供const查询使用。既有flush/invalidate使用该修正，无额外功能开关；DX11等调用方未单独真机回归，不扩大验收范围。

生产allocSlice/getMappedRange/pushSlice与真实Spinlock的24000次并发分配/新旧slice查询通过ASAN/UBSAN，检查未知handle、atom对齐、锁释放。最终TSan在当前WSL多次启动均遇unexpected memory mapping，因此最终记录明确TSan未完成，不能宣称全驱动无race或mutation已验证。

## 设备证据与验收边界

所有label位于F:/VintagePomelo-Workspace/workspace_temp/mini-virgl-performance/backend-heaven-routing/，各自保存实际环境、模块、构建hash、命令与清理收据。

| label | 实际结果 |
| --- | --- |
| d3d9-texture-properties | 96组有界ATI1/ATI2 mip参数失败记录 |
| mip-top-control-off | 同0018 DLL关闭开关，UpdateTexture拒绝 |
| mip-top-candidate-on | 同DLL开启，四格式x三个mip采样/Present通过 |
| mip-bottom-candidate-on | 底层匹配，四格式x三个mip通过 |
| d3d9-mip-top-heaven-on | UpdateTexture失败未再记录，但constant publication导致CS退出，排除为游戏通过 |
| d3d9-mip-backing-final | 最终0019 DLL、无COM/Unix环境hook，150秒连续场景；未观察到上述错误 |
| mip-top-backing-final | 同最终DLL四格式x三个mip采样/回读/Present通过 |

最终DLL仍明确Z:\d3d9.dll，builtin vulkan-1，d3d11/dxgi未加载。诊断仍依赖临时basename、同目录启动与SetDllDirectory；原Heaven.exe产品路由未修复。两个场景截图的22–23FPS为即时值，不能与历史16.3FPS中位基线作收益比较。没有连续GPU busy、CPU线程/提交/等待对齐采样，不能声称GPU跑满或未满。

## 构建与源验证

基于EVENT修复本地提交2657fa9d，新增0018/0019与两套测试/小型probe；不提交既有十个dirty子模块/gitlink，不push/CI。前序0017的EVENT停顿修复已单独提交，报告mini-heaven-event-adaptation-20261115.zh-CN.md记录其同DLL开关对照与最终无COM复核。

- 最终PE32 d3d9.dll：4300800 bytes，SHA256 e7dbcc95fc326ee576039bd007751b0eafd7e6624702b8d42dcfd0ccb79c8029。
- probe：70144 bytes，SHA256 f901643c710e9e58ede77c6c2a551080483661131f7944bba2f560431e317623。src为scripts/probes/mini_backend_d3d9_texture_mips.cpp。
- 0018-only DLL历史SHA35a546f5e554a4247514cc851b0c24ef88b5478c4b8c9c92621edf4f6b9a6841，增量路径已被最终DLL覆盖，无重复副本；不能把该历史SHA写成当前路径hash。
- 19补丁/36文件reverse+replay字节一致，两次幂等verify、当前源码verify-only通过。bool/SPIR-V、push constant、buffer、vertex input/FF、EVENT、mip与backing并发契约均通过；适用项目含ASAN/UBSAN，mip bottom-only mutation在行为assertion失败。
- patchedSourceSha256：f96146a52d7d4026ee37d56d665bc473f53dfc668a73d8a86df6fa6a58a17768。源码测试保留实际范围；TSan最终受运行环境限制。增量构建通过，仅既有编译警告。

## 清理、产品边界与后续

没有新HAP，没有改UI/图标、原Heaven.exe、游戏资源或用户设置。所有设备操作持mini.lock，最后应用停止，临时probe/DLL/cache/日志/截图清除，leftovers=[]，原游戏hash保持。重复截图按SHA去重，COM日志gzip后逐字节SHA验证才删原文；manual-cleanup.json记录21个文件、净回收13849938 bytes（约13.21MiB），原命令中的旧路径可经该收据定位。两份既有signed HAP、增量构建、最终DLL、一份必要基线与证据保留；没有复制大包。

下一步先完成原游戏入口的d3d9 overlay路由和完整场景验收，再用确认实际API的同D3D9参数跑VirGL/DXVK9/Zink。Zink仍需单独修正caps/纹理路径。性能阶段采用同设置A-B-B-A，并同步采集CPU各线程、CS/socket等待和GPU busy/timestamp，目标仍为稳定达到至少+30%，目前未证明。
