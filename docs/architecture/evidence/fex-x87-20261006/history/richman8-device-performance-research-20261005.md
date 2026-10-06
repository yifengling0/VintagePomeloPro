# RichMan8 性能与图形完整性研究：284026ce 真机证据

日期：2026-10-05。测试对象为 MatePad Mini 上的 `com.vintage.pomelopro`。源码基线 `feature/main_proton / 284026ce64c631961394f2aee6fadb3a4380e579`。本报告延续同目录 README 的独立构建、包身份和默认关闭诊断测试记录。

## 结论及边界

已经确认 WoW64 缓冲映射存在明显的整块复制放大，值得优化；地图 CPU 栈另有 BufferSubData → VirGL 同步查询路径，需补计时。现有证据不能把秒级加载停顿或 main/Proton 差距全部归因于这两个问题，也不能给出 FEX 比 Box 慢多少的结论。

人物缺块仍存在，且在约 60 FPS 的主菜单也可见。因此帧率和画面正确性必须分别验收。0035 显式 flush 修复不能因为 memcpy 成本而撤回。

此次仅修正诊断入口、采集和研究记录；没有修改生产渲染逻辑、构建第二个性能候选或完成新的修复回测。

## 1. 批处理已修正，实际诊断通道已生效

旧入口错误地假定 `Z:` 对应 Unix `/`。实际 `thirdparty/wine-valve/dlls/ntdll/unix/ohos_file.c:46` 用 HOME 作为 Z 盘根；本会话 HOME 是旧柚 Pro 的下载目录。因此入口是：

```text
Z:\games\RichMan 8\RichMan 8\strat game.exe
```

两份 BAT 已覆盖到平板 C: 根目录，支持传入完整 EXE 路径，也保留 `Z:\game` 候选；先验证文件再切换工作目录。只由用户手动执行。

- on SHA256：`53d5de0d3fc40fe15789a171c32c8c9a246d4c625669128356745c72fa7f539e`
- off SHA256：`66c54b86e860460582e6a87b06e52543dc887ff94761525522c246005821fe22`
- 官方 bundle-aware HDC send/recv 逐字节一致，记录为 `diagnostic-scripts-deploy-v2.json`；失败旧版保留于 `diagnostic-bat-v1-invalid-mapping/`。

19:50:21 新游戏 host PID 64408 启动，最终 WINEDEBUG 为 `-all,+err,+winehua_perf`，HODLL 为 `libwow64fex.dll`，cwd 对应原游戏目录。Unix 计时行确认 Windows PID 932、线程 956。这次 BAT 启动的计时输出在应用原始 stderr 文件中；只检索 hilog 会漏掉，不能把缺行当作开关未生效或耗时为零。

## 2. 实际场景，而非计划目录名

| 证据目录 | 实际观察 | 可用于什么 |
|---|---|---|
| off-first | 默认关闭；切角色并混入进入地图 | 用户确认每次切换卡；不能将所有低点归为纯切角色 |
| off-repeat / off-map | 地图约 25–28 FPS；人物头部缺损 | 默认关闭的地图表现和上传/查询采样 |
| on-first / on-first-profile | 片头/SOFTSTAR 停顿；用户确认黑屏或卡片头 | 启动阶段，不能称首次切角色采样 |
| on-repeat / on-repeat-profile | 已进入主菜单动画；相邻截图均是主菜单 | 诊断开启后的主菜单渲染 |
| on-characters | 19:54:55–19:55:55，前后截图是主菜单；尚无切角色操作确认 | 有效的主菜单 Unix API 计时，不是受控角色切换对照 |

`on-first-profile`：35 秒、200 Hz、50732 条 sample records、lost=0。`on-repeat-profile`：35 秒、200 Hz、65060 条、lost=0。均为目标 bundle UID 的进程采样，非系统级，不能解释所有 off-CPU 等待。栈分支百分比不可当游戏总墙钟时间。

截图 `on-progress-screen-recv.png`：主菜单约 60 FPS，数个人物的头部/身体出现背景色矩形缺块。`on-characters-end-screen-recv.png` 中另一批人物也有局部缺损。不能据此断言缺的是纹理、顶点或合成块，需要继续按绘制阶段区分。

## 3. 已量化：小量修改引发 256 KiB 整块复制

来源 `on-characters/stderr.txt`，汇总 `on-characters/summary.json`。同一 Windows PID/TID，60 个计时窗口，共 60.083494 秒：

| 指标 | 调用数 | 累计耗时 | 实际复制量 | 单次最大耗时 |
|---|---:|---:|---:|---:|
| map_driver | 242567 | 140.218 ms | 不计复制 | 0.222 ms |
| map_shadow | 242567 | 3308.129 ms | 此项仅记请求量 | 1.225 ms |
| shadow_in | 242567 | 3222.062 ms | 63587483648 bytes | 1.167 ms |
| shadow_flush | 242567 | 128.167 ms | 1070876032 bytes | 0.292 ms |
| gl_flush | 242567 | 77.009 ms | 不计复制 | 0.101 ms |
| gl_unmap | 242567 | 99.401 ms | 不计复制 | 0.164 ms |
| wgl_swap | 3597 | 8942.749 ms | 不计复制 | 6.011 ms |

所有 map 都落入 shadow；该区间没有 map_direct/map_pinned/map_vk。实际 shadow_in 每次恰好 262144 bytes，flush 平均仅 4414.76 bytes。复制量比例约 59.38 倍，入向复制约 1009 MiB/s。

`map_shadow` 包含 `shadow_in`，不可相加。3.222 秒入向复制约占该计时跨度的 5.36%，折合每次 swap 约 0.896 ms。这是可减少的 CPU/内存流量，**不是几秒卡顿的充分解释，也不是翻倍 FPS 的证据**。该段宿主采样为 58.87–60.35 FPS，仍可能受显示节奏限制。

源码链条：

1. `dlls/wined3d/context_gl.c:2834` 在非 persistent 路径总是从 0 映射到 `bo->size`。上层 `map_bo_address` 虽接收 size，底层仍映射整个 BO。
2. `dlls/wined3d/resource.c:388` 对写映射设置 WRITE、FLUSH_EXPLICIT、UNSYNCHRONIZED；未传 invalidate 提示。
3. `dlls/opengl32/unix_wgl.c:3237` 的 WoW64 映射在原生指针超出 32 位时分配低地址 shadow；没有 invalidate 时从 host_ptr 复制整个映射。
4. 0035 在显式 flush 时只将所需范围复制回 host_ptr，再调用驱动 flush。这里的 publication 顺序是正确性要求。
5. Mesa vtest 的资源共享内存在 `virgl_vtest_winsys.c:351` 用 `os_mmap(NULL, ..., MAP_SHARED, fd, 0)` 分配；原生 ARM64 没有低 4 GiB 保证。

## 4. 已见调用路径，但还未量化：BufferSubData 与同步查询

默认关闭的地图采样、实际构建 ELF 的 addr2line 解析得到：

```text
wow64_ext_glBufferSubData
  -> u_default_buffer_subdata
  -> virgl_resource_transfer_map
  -> virgl_vtest_resource_is_busy
  -> virgl_vtest_busy_wait(flags=0)
```

这条路径通过 socket 写请求、读响应，有同步 IPC 成本，但 `flags=0` 不要求等待 GPU 完成。宿主 `vtest_resource_busy_wait` 只在 WAIT flag 且存在 `VTEST_SYNC_GL_FINISH` 时调用 context_finish。不能把函数名翻译成“每次上传都 glFinish”。

当前忙闲判定实际使用 context 的提交 fence 与全局完成 fence；传来的 resource handle 没用于精细判断。0037 没有覆盖 BufferSubData 或内部查询，所以快的 map_driver 不能排除此路径慢。

同样发现 DrawElementsInstancedBaseVertex → st_prepare_draw → st_upload_constants → virgl_encoder_write_constant_buffer。尚未分离常量重复上传、驱动编译和真正 GPU 工作的耗时。

## 5. main 与 Proton：已核对的差异

main 参照是本地 `origin/main / 5383b784`，并非本次验证的远端最新版本，也没有安装同条件 main 包。

| 层 | main 参照 | Proton 测试包 | 含义 |
|---|---|---|---|
| Wine | 1cb1ac93，VERSION 11.10 | cd547f7a，VERSION 11.0 + overlay | 不能当仅换翻译器的 A/B |
| ARM 入口 | Box64 加载 x86_64 Wine ELF | 原生 ARM64 Wine + FEX | PE、Unix 和图形桥接的执行位置不同 |
| D3D9 | WineD3D → OpenGL → VirGL 路径存在 | 本游戏也走 WineD3D → OpenGL → VirGL | 设置页的 DXVK 档位不能证明 D3D9 正在用 DXVK |
| Mesa | 2939cbb8 | 330124bf | 六文件差异以能力诊断及 Venus 修改为主，VirGL transfer/busy 算法没有这一批改动 |
| VirGL | fde243e1 | bb33e52a | sRGB/呈现和上下文等有差异；两个 pin 的 vtest_renderer.c 无差异 |

WineD3D 的 buffer.c 只有少量 usage/CS 标志差异，cs.c 主要是日志调整；没有找到 main 独有的“大幅改进缓冲队列”的实现可以直接搬回。两边已有相似 WoW64 shadow 机制；**尚未证明 main 在该游戏里一定命中 direct map**。

VirGL sRGB 存储/视图处理存在实际差异，值得做图像对照，但当前矩形缺块并不能直接证明由 sRGB 导致。WineD3D 纹理 resolve 和着色器生成也有版本差异，需按触发 API 验证后选择移植。

## 6. 改善顺序与代码方向

### P0：先守住图形完整性

保留 0035。针对人物的纹理/顶点更新，在游戏进入选人前开启一次有界 API 诊断或捕获，核对更新范围、映射偏移、纹理 stride/格式和 draw 使用的资源代次；把缺块缩成可重放案例。只有匹配实际触发路径后，才选择移植 main 的相关修复。不要靠取消 flush、同步或降低纹理质量掩盖问题。

### P1：减少 WoW64 整块复制

优先做 WineD3D 内部 streaming upload 的窄范围映射原型：把实际写入 offset/size 传到底层，维护映射基址，所有 flush offset 随之转为相对映射范围。仅在已证明写入全部有效范围、没有共享/嵌套映射的非 persistent 路径启用，其他情况回退原实现。

另一方案是在明确的 transient/discard 语义下使用失效提示，让 shadow 不必读回旧内容。不能给全部 WRITE/NOOVERWRITE 映射无条件加 INVALIDATE_BUFFER，也不能简单删掉初始 memcpy；那会破坏保留旧数据和部分写入语义。

架构方案是让选定的 VirGL staging/shared buffers 可映射到 Wine 管理的低 4 GiB 地址区，直接给 32 位调用方使用，失败时保留 shadow。需要与 Wine 地址空间分配和资源生命周期整合，不能在 Mesa 中盲目 MAP_FIXED 覆盖，也不能假定 OHOS ARM64 支持 MAP_32BIT 或 MAP_FIXED_NOREPLACE。范围优化风险较小，低地址共享映射应单独推进。

验收必须包含非零 offset、部分写、保留字节、显式多次 flush、buffer 重建/删除、不同线程/context，以及现有 0035/0036/0037 回归。

### P2：量化并减少同步 IPC

补默认关闭的 BufferSubData/BufferData 总时长、请求字节和 VirGL query/wait 次数、耗时、flags。若同步查询确为热点，可依次评估合并 header/body syscall、按明确提交代次缓存完成状态、协议协商的完成进度页或资源使用 fence。完成状态必须随 context/resource 生命周期失效，不能把“未查询”视为“GPU 空闲”。

### P3：继续追长停顿和编译/资源加载

片头出现约 98.77 秒的计数窗口，但其中只有 9 次 swap，swap 累计 26.1 ms；该窗口不能当单次 GL API 阻塞 98 秒。首轮 profile 实际在片头，没有覆盖稳定切角色。下一轮需对齐真实切换动作，补游戏线程/CS 队列等待、资源读取、shader compile/link 计时。

默认 early-fault 诊断会在若干唯一信号 PC 上读取/保存 maps 并打印大量内容；它是可恢复异常路径上的额外成本和稳健性风险，值得单变量关闭对照，但当前没有证据证明它解释这次全部停顿。出现 SIGSEGV 日志不等于游戏致命崩溃；本次进程随后继续渲染。

FEX 的 JIT、内存屏障或游戏逻辑仅在独立归因后再调。不得仅凭未知 PC 或 NtDelayExecution 包含栈认定 FEX 忙等；CPU cycles 的包含栈、off-CPU 时间和端到端耗时不同。

## 7. 尚未完成

- 新一轮受控的首次/重复角色选择、同地图开关对照；当前主菜单样本不能替代。
- main 与 Proton 同版本内容、温度、分辨率、入口和交互下的设备 A/B。
- 人物缺块的 API/资源级最小复现。
- BufferSubData、着色器和 CS 队列内部计时。
- 新性能修复候选的生产代码、构建与设备回测；本报告不宣称 FPS 改善。

研究和设备证据均在本目录。原始采样、HAP、运行库和签名材料不进入源码提交。可提交本报告的脱敏结论。
