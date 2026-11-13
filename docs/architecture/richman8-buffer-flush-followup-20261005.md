# RichMan8 首次资源卡顿及人物残缺：2026-10-05 跟进

本次保留 ARM64 原生 Wine + FEX，以及已经安装的窗口恢复、GPU 合成、字体、zlib 和多语言修复。应用和游戏均由用户手动启动；本次不启动 Steam。代码基线为 `0a877ea3724cf02e248d90c413aac92afbb38fd0`，Wine pin 为 `cd547f7a0ee9d3d59b3ec47317a7852dedf0443b`。

## 已取得的设备证据

- RichMan8 实际是 32 位 `strat game.exe`。现场从告示、SOFTSTAR 片头进入选人，再进入地图。本轮最初的无画面属于启动慢，没有复现永久窗口丢失。
- 15:06:29–15:07:00 连续切换角色时，宿主帧率出现 21.29、14.17、13.13 FPS 低点，随后回升到约 33–55 FPS。用户反馈第一次点击更慢，重复选择改善。
- 这些低点中，宿主合成单帧约 2.5–6 ms，`failed_swaps=0`。`upload_bytes=0` 是宿主 SHM 上传统计，不能解释成游戏没有上传纹理或顶点。这里主要缺少及时的新帧，不能通过缩短宿主合成耗时直接解释全部卡顿。
- 用户另报人物看起来残缺。15:20 地图截图及日志为约 20–24 FPS；截图本身不足以确定是纹理内容、顶点/索引数据还是着色器问题。
- 角色切换采样 57,886 samples，lost=0。`wined3d_cs` 的已符号化 native 栈出现 `wow64_glMapBufferRange` / `wow64_glUnmapBuffer`，支持继续检查缓冲映射，但采样比例不是首加载等待的墙钟占比。
- 15:20:20–15:20:40 额外进行了指定游戏 PID 的 `--offcpu` 采样，13,636 samples，lost=0。这是地图阶段，不能代替首次点击/完整冷启动的采样。
- 启动采样开始晚于游戏 Main 约 31 秒，未覆盖启动最初阶段。文件 I/O 的权限限制和未知特殊 IP `ffffff0000000fff` 尚未消除；不能宣称磁盘读取是唯一根因，也不能把这些特殊样本当普通 FEX JIT 指令。

诊断材料入口在 Windows `workspace_temp/richman8-20261005/README.md`。当前设备进程身份已记入现场材料，之后必须重新核对 PID。

## 实际图形路径及 main 对照的纠正

渲染器为 `virgl (Maleoon 910)`，实际路径是 D3D9 → WineD3D → OpenGL → VirGL。后续核查纠正了早期日志解读：`load_builtin_unixlib ... d3d9.so / wined3d.so` 只是加载尝试，当前 staging 没有这两颗 Unix 库，maps 显示实际加载的是 `i386-windows/wined3d.dll`。游戏、i386 D3D9 / WineD3D 仍经 FEX；ARM64 OpenGL Unix 层与 Mesa 原生运行。完整边界见 [main / Proton 对照](richman8-main-proton-virgl-comparison-20261005.md)。

本轮会话日志同时显示 `render adapt mode=dxvk_1_10` 和 `desktop D3D=dxvk_legacy`。这是会话档位，不能证明 D3D9 使用 DXVK。本分支的 DXVK overlay 没有打包/接管 `d3d9.dll`。核查 `origin/main` 的 `wine_env.cpp` 后确认，它也明确保留 D3D9 → WineD3D → VirGL。因此此前把 9 月 25 日的 DXVK 档位日志解读为 RichMan8 实际使用 DXVK，需要纠正。

旧 main 使用 x86_64 Wine + Box64，当前使用原生 ARM64 Wine + FEX / WoW64。两者的 OpenGL 指针地址、映射路径、Wine 代码和执行成本有差异。本文没有证明性能回归发生于某个具体提交，也不通过改回 Box64 掩盖问题。

## 已复现的生产代码缺陷

`opengl32/unix_wgl.c` 的 WoW64 映射在驱动返回超过 32 位地址范围的指针时，给游戏提供低地址 shadow。写入先发生在 shadow，原实现只在 `Unmap` 时整段复制回驱动映射。

WineD3D 在可写的动态缓冲映射中使用 `GL_MAP_FLUSH_EXPLICIT_BIT`，并在 `flush_bo_ranges()` 中调用 `glFlushMappedBufferRange()`。原 `flush_buffer()` 只处理 Vulkan backing 的 flush，没有复制普通驱动映射的 shadow。于是顺序成为：

1. 游戏/WineD3D 写入低地址副本；
2. 驱动 flush 仍含旧数据的高地址映射；
3. Unmap 才复制新数据，但显式刷新映射的 Unmap 不承诺自动发布这些写入。

这违反显式刷新路径的数据可见性要求，能够导致顶点、索引或纹理上传缓冲中的旧数据被使用。它是已经用生产函数复现的缺陷；是否解释本次人物异常的全部表现，仍需设备复测。旧 main 的相关 shadow flush 代码也有同类缺口，不能仅凭此认定当前分支新引入了该缺陷。

## 修复候选

新增 `0035-opengl-wow64-explicit-buffer-flush.patch`，注册在正常 Wine overlay 链中：

- 记录本次映射的访问标志。
- 在 core、named 和 EXT named 显式刷新调用驱动之前，先复制请求范围的 shadow 字节。
- 使用相对映射起点的 offset；无效、负值转换及越界范围不触碰 shadow，继续交给驱动处理错误。
- 显式刷新映射在 Unmap 时不再整段复制；隐式刷新映射仍保留原有整段 copyback。
- 读取、直接低地址、pinned 和 Vulkan backing 路径保留原有行为。

这同时避免显式映射在 Unmap 时额外复制整个缓冲，但没有量化其 FPS 收益。没有关闭 CSMT、GPU 或内存同步，没有调整游戏分辨率或覆盖游戏目录里的 DLL。

## 验证及验收边界

新增 `host_tests/opengl_wow64_buffer_flush_test.py` 从正常 overlay 重放后的生产源提取完整 map/flush/unmap 函数，只有驱动与平台边界为 stub。模拟驱动在 flush 调用当时抓取字节，因此能区分刷新前写回和 Unmap 才写回：

- 旧生产函数明确复现 stale bytes 并触发断言；
- 修复后覆盖三种 flush 入口、非零映射/刷新 offset、部分刷新、非法范围、映射重用、隐式 copyback、只读映射、直接映射、pinned、Vulkan flush；
- 6 项回归在 ASan/UBSan 下通过，overlay 重放幂等；
- 原有 reserved texture 5 项和 fshack context capability 12 项通过。

新候选使用独立 `build-richman8-buffer-flush-0a877ea3-20261005`。旧 Wine 对象、配置和身份记录不复用，只链接未修改的外部依赖。HAP、签名、加载 ELF 对应关系及设备安装结果写入诊断入口；这里不将构建成功当作人物显示或帧率验收。

设备验收：用户手动重新启动相同 EXE，先切换未选择过的角色，再重复切换相同角色，并进入同一地图。对比人物完整性、首次资源停顿和帧率。必要时再准备仅改变 CSMT 的独立入口，避免把多个变量混为一个对照。

## 候选构建与安装结果

- Wine 独立构建及身份检查通过，HAP runtime closure 通过。
- HAP 运行库加载区段与新编译 ELF 一致；继承的宿主库、ArkTS、FEX 和真实 zlib 已核验。
- signed HAP SHA256：`c04c99c764418ce3e10599e3d831db3d3e8f4f625f1a89eee0f3700689a8d8c7`。
- unsigned HAP SHA256：`fac8eef16a54b8235e940cb6a15f9e5d9aec967f42862816583a382f3cb759dc`。
- 15:47 覆盖安装成功，版本 `1.4.5-proton.26-alpha / 1004035`。停止范围仅为旧柚 Pro。安装后未发送应用、Steam 或游戏启动命令。
- 当前仍需用户手动测试；不把生产回归通过、构建成功或安装成功写成贴图/FPS 问题已解决。
