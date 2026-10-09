# 旧 Box、Proton Venus 与 Direct：呈现路径差异和优化优先级

日期：2026-10-08。权威仓库 `/home/liufeng/src/vpp-proton`，
`feature/main_proton`，HEAD `4cb9e7c8`，含此前尚未提交的修改。
本轮核查固定的 1.4.4 发布源码与现有设备证据，没有切换运行后端、编译或安装新包。

> 2026-10-09 实测补充：已恢复并命中 Venus 宿主 NativeBuffer 实验，尚未观察到明显帧率收益；新映射诊断确认 TR32 的 Venus 仍进入 WOW64 CPU fallback。后续优先核对共享 fd 查找和实际复制成本，见 [后续评价](venus-direct-followup-evaluation-20261009.md)。下文为原始原因候选和历史证据，不能当作已证实的呈现主因。

## 结论

旧 Box 接近当前 Direct 的用户反馈，不能用“Venus 固定有很大协议成本”解释。
**1.4.4 同样使用 Venus，但其宿主 presenter 已包含自动选择的 NativeBuffer
呈现路径；当前 Proton Venus presenter 没有这条分支，默认走 WSI 并等待
本帧复制完成。** 这是本轮找到的具体实现差异，优先于继续猜测 FEX 的整体效率。

旧代码存在且进入生产构建清单，不等于用户那次旧游戏已成功走到该分支。
仍缺旧游戏 `transport=direct-native-buffer` 的实际运行日志，也未取得
保持 FEX/DXVK 不变、只切换两种 Venus 宿主 presenter 的设备对照。
因此这是优先验证的原因候选，不是已经证明解释了全部帧率差距。

## 固定的旧版本与执行链

核查对象为 main 的发布提交 `38e7522d`（1.4.4 / 1004004）；本地
main / origin/main 为其后一提交 `5383b784`，只补 LZMA 构建头文件。
旧 runtime 固定项：Wine `1cb1ac93e4`、Box `0411b38569`、Mesa
`2939cbb816`、virglrenderer `fde243e144`、DXVK legacy `f3436e1796`。
这些与已有 `version-performance-20261007/payload-comparison.json`
中的归档 runtime 标记一致。归档 HAP 本身当前未在记录路径找到，
本轮没有独立检查旧包二进制或部署旧包。

`38e7522d:scripts/build_ohos_guest_vulkan.sh` 明确构建 x86_64 loader /
Mesa Venus；`wine_env.cpp` 设置 x86_64 Venus ICD，`Box64EmulatedLibs()`
把 libvulkan 列入 emulated。旧版并非游戏直接调用宿主 Maleoon Vulkan。

| 路径 | Vulkan 命令与资源 | 最终呈现 |
| --- | --- | --- |
| 1.4.4 Box | x86/x64 游戏与 DLL → x64 Wine/Mesa 经 Box → Venus/vtest → 宿主 Vulkan | 自动尝试宿主 NativeBuffer + release fence；失败才回 WSI |
| 当前 Proton Venus | FEX 执行需要转译的游戏代码 → Wine/原生 ARM Mesa Venus → vtest → 宿主 Vulkan | WSI copy/present；默认本帧 CPU fence wait |
| 当前 Proton Direct | FEX 执行需要转译的游戏代码 → Wine Vulkan → guest 进程中的原生 Vulkan 驱动 | 共享 NativeBuffer/fence → 宿主合成 |

第一行的“宿主 NativeBuffer”依然保留 Venus 转发，和第三行的 guest
Vulkan Direct 是不同层面的优化。旧版无需用户启用 guest Direct，
也可能已经享有更好的 CPU/GPU 呈现重叠。

## 最关键差异：宿主呈现和源图像复用

固定旧源码：

- `venus_surface_presenter.cpp:1116`：Unprobed 状态自动执行
  `vkDirect_.Configure()`；Ready 选择 DirectNativeBuffer，失败锁定 WSI 回退。
- `native_window_vk_target.cpp:125`：检查
  `vkGetNativeBufferPropertiesOHOS`、`vkAcquireImageOHOS`、
  `vkQueueSignalReleaseImageOHOS`，并配置目标 NativeWindow。
- `venus_surface_presenter.cpp:411`：提交游戏 source → NativeBuffer 的 GPU 复制。
- `venus_surface_presenter.cpp:428`：SignalRelease 返回 native fence；
  `:445` 把该 fence 交给 EndFrame，正常成功分支不在本帧末尾等待复制完成。
- `:322`：循环 frame slot 的 command buffer/fence 在下次使用前等待。
  它减少等待的位置，并没有取消资源完成约束。
- `38e7522d:entry/src/main/cpp/CMakeLists.txt:148`：
  `native_window_vk_target.cpp` 纳入 libvirgl_child 的生产构建。

当前 `graphics/venus_surface_presenter.cpp:327` 循环 frame slot，
`:342` WSI acquire，`:527` copy submit，`:539` queue present，
`:551` 默认执行 release fence wait。`:547` 已释放 queue mutex，
所以不能说它持有 queue mutex 等 GPU；但本次 Present 回复仍须等 fence，
与 guest 后续帧的流水线重叠可能受限。`fifo-async` 是显式诊断选项，
当前产品缺省仍是 `fifo`，不是已验证的产品异步路径。

**不能只删 vkWaitForFences 或全局打开 fifo-async。** 必须明确源图像
何时可以重新 acquire/渲染、目标 NativeBuffer 的消费与回收、所有 pending
copy 的完成点、frame slot 重用、队列次序，以及窗口/设备销毁和重建。
呈现提交成功与 source 安全复用不是同一个事件。

建议先在 Venus 宿主恢复可回退的 NativeBuffer/fence 实验路径，保留当前
多窗口身份、代次、输入/前台状态和 OPAQUE 合成修复，单独验证收益。
即使最终不采用旧实现，也应以异步完成通知和安全的 in-flight 管理达到
同等流水线效果，而不是只改显示 HUD 或少画一层。

## 资源复制差异：存在，但未确认当前游戏命中

旧 `1cb1ac93:win32u/vulkan.c:1302` 对不适合 32 位的高地址映射直接拒绝；
没有当前的 `winehua_wow64_copy_maps` 和 QueueSubmit 全表复制。

当前 Wine 增加低地址共享别名与 CPU shadow 回退。
`winehua_wow64_flush_copies()` 在非 precise 的 copy map 上，
每次 submit 复制整个有效映射跨度。Direct 已配对 precise-map 和 DXVK
显式 flush/invalidate，因此能省掉额外整块发布。
Venus 产品配置的 `WINEHUA_VK_PRECISE_MAP` 为 0；**但只有实际进入
CPU fallback 的 map 才有这项额外复制，alias map 不走这个列表。**

当前 Mesa `vn_renderer_vtest.c:817` 特意在 OHOS ARM64 保留 backing fd，
供 Wine 建立第二个低地址 MAP_SHARED 视图。这项修复已针对 Venus 的复制
回退，不能无视它，把“存在 fallback”当成当前每帧都在复制的证据。

下一步应记录 alias / copy 数量、存活 map 字节、submit 的实际复制字节
和耗时，以及 explicit flush 范围。当前日志未取得对应游戏的完整映射
命中统计，不能宣称这项就是 Venus 的全部损失。
如果 alias 全部生效，立即降低此项优先级；如仍有显著 copy，再为
已验证的 legacy DXVK 评估 Venus precise-map 实验和合法低地址共享映射。
不能把需要显式发布合同的优化推广给任意 coherent Vulkan 应用。

## 不应当作迁移新增原因的项目

1. 旧、新 Venus 产品均使用 precise shadow、remote-memory sync、
   no_multi_ring 和相应 feedback 禁用策略。
2. 对比 Mesa `2939cbb8` 与当前源码，`vn_device_memory.c`、`vn_ring.c`、
   `vn_queue.c` 没有此轮迁移引入的修改。同步 flush/invalidate 与 ring
   等待仍值得优化，但目前并非一个已发现的新回退点。
3. 旧宿主同样有 8 个 shadow-upload slot，复用时也必须等待 in-flight
   fence/timeline。不能把这项等待说成 Proton 独有。
4. 当前默认 inline upload 会开启 dirty generation 互斥保护，旧默认未
   开启；这是防止 flush/submit 数据竞争的修复。现有 host 尾段
   contended_total=0，累计 wait_total_us 仅约 1.5ms，暂不支持锁争用造成
   数十 ms/帧的说法。这是保留日志的局部证据，不是新的游戏 A/B。
5. strong ring publish barrier 为 CPU memory fence，不是 QueueWaitIdle。
6. Vulkan image 的展示为 GPU 复制/消费。EGL 统计 upload_bytes=0 不代表
   Vulkan buffer 上传零复制，也不能据此认定最终合成无 GPU 成本。

## 现有帧率证据的范围

同一新包、FEX、TR32、legacy DXVK、800×600、同一存档：
Direct 的 23:16:00–23:16:29 参考窗口均值 35.207 FPS；Venus 在
23:23 的四个约 10 秒窗口约 17.555 FPS。同一 Venus NativeImage 的
600 帧 / 34.25 秒约为 17.518 FPS，与宿主新图像计数相符。

镜头/角色位置、运行时间、热条件没有严格匹配，不据此计算净协议损耗。
菜单中的 Venus 也出现约 32–34 FPS，随后实际游戏约 17–18 FPS，
说明不是所有场景都被固定锁在同一低帧率。

Venus 稳态 EGL 活动循环均值约 5.4–5.8ms，failed_swaps=0，
展示 CPU upload 为 0，但新帧间隔约 57ms。
主要差距需要往上游 production、present wait、资源上传和 GPU 执行定位；
活动循环计时并未覆盖所有阻塞，也不是 GPU 时间。

## 优化顺序与验证

1. **先分离呈现成本。** 保持 FEX、DXVK、资源策略、场景和温度条件不变，
   比较当前 Venus WSI 与 Venus 宿主 NativeBuffer/fence 路径；Direct 作
   上限参考。关联每帧的 guest Present、host acquire、copy submit、
   completion/reply 和 NativeImage 消费，而非相加跨线程累计时间。
2. **再处理实际命中的资源搬运。** 同时统计 Wine WOW64 和 Venus host
   的两段复制，分清“低地址 shadow → guest map”与“guest共享 shadow →
   GPU allocation”；按范围发布、合并 flush 和 batching，保留回读正确性。
3. **上传与等待继续细分。** 使用已有 upload prepare 的 slot wait、reset、
   dirty scan、buffer record、uncovered scan、end 时间，以及 guest ring
   的 notify/roundtrip 统计。测命令转发本体，避免把 GPU 等待算为编码成本。
4. **Direct 上限也有优化空间。** 对齐 DXVK draw/submit、游戏 CPU、Wine
   event/semaphore RPC 和 GPU timestamp。此前 ROTTR PE64 有高频 RPC 与
   SNORM texture 失败，但它是另一款游戏，不能把它的占比套给 TR32。
   GPU 计时结果异步读取，不能为了计时新增每帧 WaitIdle。

性能验收要在相同游戏镜头交替重复，诊断关闭后再比较帧时间及 P95/P99。
正确性覆盖加载、移动、菜单、窗口切换、最小化恢复、32/64 位、设备销毁
和长期运行。手机保留 Venus 回退；恢复宿主路径是否可用于手机，必须做
手机能力探测和真机验证，不能从平板 guest Direct 成功推导。

## 复核入口

旧版本源码快照已保存到外部证据目录：
`F:/VintagePomelo-Workspace/workspace_temp/gumu10-dx11-20261008/`

- `main144-venus_surface_presenter.cpp`
- `main144-native_window_vk_target.cpp`
- `main144-graphics_profile.cpp`
- `main144-win32u-vulkan.c`
- `box-venus-compare-live.txt`：只读取得的保留 host 尾段；现场进程已变化，
  不用它替代上述稳定 FPS 窗口。

此报告只新增分析文档和外部源码快照，没有修改产品默认配置或提交/push。