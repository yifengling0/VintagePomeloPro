# RichMan8：DISCARD 映射优化与上传计时候选

基于 `feature/main_proton / 284026ce64c631961394f2aee6fadb3a4380e579`。
本轮延续 [设备性能研究](richman8-device-performance-research-20261005.md)，保留原生 ARM64 Wine + FEX、VirGL、0035 显式刷新修复及现有画质设置。

## 1. 已证实的问题与本轮边界

前一轮 **主菜单** 的 60.08 秒样本中，242567 次映射均进入 WoW64 shadow，每次初始化复制 256 KiB，累计 63,587,483,648 字节；平均实际 flush 约 4.4 KiB。初始化复制累计约 3.22 秒，折合约 0.90 ms/次 swap。

这些数据证明有可削减的复制开销，但不能解释数秒停顿，也不是切角色样本。不能据此推断全部映射来自内部 streaming buffer：当前内部 streaming buffer 的最小容量是 512 KiB，尚未获得资源身份与映射 flags 的对齐证据。

因此本轮没有缩小普通 buffer 的映射范围，也没有删掉 WRITE/NOOVERWRITE 的初始复制。后者必须保留未写入的字节；一次显式 flush 允许覆盖比实际写入更大的范围。

人物矩形缺块、片头长停顿的根因尚未定位；本候选不声明这两个问题已修复。

## 2. 生产变更

### 0038：将 D3D DISCARD 语义传给 OpenGL

`dlls/wined3d/resource.c / wined3d_resource_gl_map_flags()` 原来丢失了 DISCARD 提示。即使旧内容已经作废，WoW64 仍初始化整个低地址 shadow。

现在只有 **WRITE + DISCARD 且不包含 READ** 时增加 `GL_MAP_INVALIDATE_BUFFER_BIT`。该转换器位于非 buffer-storage 映射路径；persistent、allocator chunk 和 buffer-storage 路径仍使用原有逻辑。

实际效果：

- DISCARD 的 shadow 不再从驱动映射复制作废数据。
- 已有 fence 等待、BO 重命名、映射大小、显式 flush 及 unmap 流程不变。
- 普通写入、NOOVERWRITE、读取映射仍保留原数据。
- 没有增加 `glFinish`、改变 FEX 或降低画质。

收益仅适用于确实使用 DISCARD 的调用。已有主菜单统计没有记录 D3D flags，因此不能把全部 63.6 GB 初始化复制都算作本补丁可省的量。

### 扩展 0037：填补上传和着色器调用计时盲区

继续使用默认关闭的 `winehua_perf` 通道，每个活动指标按线程每秒汇总一次；关闭时不读时钟。

| 指标 | 覆盖调用 | 字节字段 |
|---|---|---|
| `buffer_subdata` | glBufferSubData、glBufferSubDataARB | 合法正 size 的请求字节数 |
| `tex_image` | glTexImage2D | 0：未查询 unpack 状态，字节量未知 |
| `tex_subimage` | glTexSubImage2D | 0：字节量未知 |
| `compressed_tex_subimage` | glCompressedTexSubImage2D | 合法正 imageSize 的请求字节数 |
| `compile_shader` | glCompileShader | 0 |
| `link_program` | glLinkProgram | 0 |
| `shader_status` | glGetShaderiv，所有 pname | 0 |
| `program_status` | glGetProgramiv，所有 pname | 0 |

计时记录 API 的墙钟耗时，可能包含驱动 CPU 工作和等待，不是 GPU 执行时间。Compile/Link 可能延期到查询或首次 draw 才完成；本批仍不覆盖驱动内部编译和队列的全部工作。`ok` 只表示调用返回，不表示 shader 编译成功或 GL 无错误；不额外读取、消费 GL error 队列或改变查询结果。

原生和 WoW64 入口均经过相同 wrapper，保留可用函数检查、指针转换和状态更新。`make_opengl`、`unix_thunks.c` 和 `unix_thunks.h` 同步更新，并用生成器固定的 Khronos registry 提交完整重生成核对。

0037 的补丁文件内容已更新；0038 单独注册。旧目录的 Wine 身份会变化，必须使用新的独立 BUILD_DIR。若 Wine 源目录已经应用旧版 0037，应在独立、干净的 Wine pin 上重放整套补丁；不要强行反向打补丁或覆盖含用户修改的源目录。

## 3. 代码验证

执行命令（WSL 权威仓库）：

```sh
make WINE_SRC=/home/liufeng/src/vpp-proton/thirdparty/wine-valve \
  test-wined3d-discard-map test-opengl-wow64-buffer-flush \
  test-wine-performance-summary test-wined3d-pixel-format
```

22 个 unittest 方法通过，包括多组 C 子场景。回归提取、编译生产 C 函数，并将 D3D flag 转换与真实 WoW64 map/flush/unmap 函数联测：

- 旧代码在 DISCARD 初始化复制断言处失败，确认基线冗余复制。
- 新代码省去该次复制，写入仍经显式 flush 发布。
- NOOVERWRITE 的宽范围 flush 保留未修改字节。
- 后续映射能读取已发布内容，unmap 不发布未 flush 的修改。
- READ 组合、coherent 标志、负 size 计数，以及诊断开关覆盖。
- 上传/编译/查询调用参数和返回内存保持一致，长调用进入对应指标。
- 0035、像素格式查找与既有计时相邻回归通过。
- 35 个注册 overlay 从 Wine pin 连续重放两次，全部文件一致。

C 回归使用 ASan/UBSan；该环境仍将 LSan 关闭，不声明完整聚合测试或泄漏检查通过。补丁文件中的空白上下文行是统一 diff 格式，不是生产源码行尾空格。

## 4. 独立构建与设备回测

BUILD_DIR：`build-richman8-discard-20261005`。重新生成 Wine configure、对象及身份记录；已有非 Wine 依赖通过原有 Make 依赖检查复用。

证据目录：

- WSL：`/home/liufeng/src/vpp-proton/workspace_temp/richman8-discard-20261005/`
- Windows：`F:\VintagePomelo-Workspace\workspace_temp\richman8-discard-20261005\`

构建、签名、运行时组件检查及包内容核验均通过，已于 `2026-10-05T20:51:53.523181+08:00` 覆盖安装到 MatePad Mini 的 `com.vintage.pomelopro`。应用数据保留，安装后未发出应用、Steam 或游戏启动指令。

- 版本：`1.4.5-proton.26-alpha / 1004035`；同版本候选以包哈希区分。
- 签名包 SHA256：`6b19dc91ff5a611ae485a8124ad45e8ce847692e293542034da45b6b890a78de`。
- 未签名包 SHA256：`c6b1a45eecb308100c198741161c1f6f5cd6abfd87b951175b37730fcd5ef692`。
- HAP 内 32 位 WineD3D 的可加载 PE sections、原生 Wine ELF sections 与独立构建匹配；FEX 和 ArkTS 与继承基线一致；实际 zlib 与依赖输出匹配。
- 用户真机反馈仍有画面和操作卡顿，没有明确体感提升；人物矩形缺块仍可见。后续补采的主菜单复制成本约 0.88 ms/swap，0038 只省去约 1.5% 初始化复制。后续可开关候选见 `richman8-dynamic-buffer-followup-20261005.md`。

升级前补采 `on-menu-before/`：宿主 3 个 FPS 样本为 59.61、59.89、59.77；125354 次 shadow 初始化共复制 32,860,798,976 字节，计时累计约 1.676 秒。聚合窗口共 31.026 秒，首窗口可能早于约 30 秒采集边界；该数据只作主菜单参考。截图仍能看到人物矩形缺块。

用户手动启动 RichMan8，先走原来的正常入口、关闭计时，检查片头、主菜单、角色和地图完整性。若表现可复现，再手动运行 `C:\vp-richman8-perf-on-20261005.bat`，分别采首次切角色、重复切角色、同地图稳态；运行之前应关闭旧游戏，避免同时存在两份实例。

采集必须动态识别 host PID 和 Wine PID/TID，读取应用 stderr 的有界字节区间；不能用空 hilog 判定 Unix 计时为零。用截图和用户交互确认真实场景，不能再次把主菜单样本标为切角色。

覆盖安装仅结束 `com.vintage.pomelopro`，保留数据。应用、Steam、游戏和 BAT 均由用户手动启动。

## 5. 后续依据

若 `buffer_subdata`/纹理上传峰值与切换停顿对齐，继续分解 VirGL busy 查询、传输和 CS 队列；若编译或状态查询峰值明显，转向 shader 生成与缓存。若这些 API 的总耗时仍很小，补游戏线程文件读取和 off-CPU 等待证据。

人物缺块需单独对齐纹理内容、上传范围和 draw 使用的资源；未定位前不扩大本优化范围。main 与 Proton 也仍缺同条件真机 A/B，不能把剩余差异直接归因为 Box 与 FEX。
