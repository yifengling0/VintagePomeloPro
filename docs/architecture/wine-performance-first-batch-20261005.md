# WineD3D 首批正确性修复与按秒诊断

基线：`feature/main_proton` / `3342bf124c1d47bf936da6be0c29906f470192d6`；Wine pin 仍为 `cd547f7a0ee9d3d59b3ec47317a7852dedf0443b`。有效修改仅通过正常注册的 0036/0037 overlay 重放。本批没有 HAP、安装、真机 FPS 或性能收益结论。

## 0036：按 WGL ID 查找紧凑格式表

`adapter_gl.c` 紧凑保存查询成功的格式，`context_gl.c` 返回 WGL ID，旧 `swapchain_gl_present()` 却把 ID 当数组下标。新代码在 `pixel_format_count` 范围内比较 `iPixelFormat`，没有改变 ID 或减一。

备用 DC 仍走 GDI；partial COPY/COPY_VSYNC 在格式缺失或非 COPY 时走 GDI；未知格式的完整 Present 仍走 GL。无效 context 的返回、release、fence、buffer rotation、location bookkeeping 保持原样。完整生产 Present 宿主测试覆盖 15 个分支，旧代码的单个 ID=1、末尾连续 ID、稀疏 ID、单个较大 ID 均能触发 ASan 越界。此缺陷尚未证实是游戏掉帧原因。

## 0037：默认关闭的 `winehua_perf`

本批仅交付 Unix `opengl_unix` scope，每线程有独立、固定大小的计数器。禁用时在读时钟或 TLS 计数前返回；时钟失败/倒退丢弃该次测量，原 API 调用照常执行。计数器本身没有新增热路径锁、分配、后台线程、GL 查询、同步或设置变化。一次操作完成后才累计；距上次汇总至少一秒才输出一个汇总批次，无活动线程不输出心跳，不主动刷新线程退出时不足一秒的尾部。每批最多 14 行，一行一个活跃指标。每行即使计数饱和也小于 512 字节，适配 Wine 的 1024 字节调试格式缓冲；计数按 64 位饱和处理。启用后的 Wine 日志输出是同步的，可能在现有 `wgl_lock` 内阻塞或由日志设施加锁/分配，须真机测量开启诊断的扰动。

每行含 `scope`、Windows PID/TID、`window_us` 和 `指标=次数/总微秒/最大微秒/请求字节/实际复制字节/ok/waiting/not_started/wrong_thread/error`。多个活跃线程各自计时，不能把嵌套时间相加。实际 GL 结果保持原样；对没有返回状态的 API，`ok` 只表示调用返回，未新增 GL 错误查询。本批保留五槽结果格式，仅使用 ok/error；没有新增 fence 状态采样。

实际覆盖：

- `opengl_unix`：现有 WoW64 target/named/core/ARB/EXT map 驱动调用；shadow/direct/pinned/Vulkan/失败分类；shadow 初始化、0035 显式 flush 与隐式 unmap 的实际 memcpy；现有 GL flush/unmap 驱动调用；手写 ReadPixels、Finish 和 WGL swap wrapper
- `map_shadow` 的请求字节不等于复制字节；actual copy 只记在 `shadow_in`、`shadow_flush`、`shadow_unmap`。负的 `GLsizeiptr` 请求仅在诊断请求字节中按零计，原始驱动参数和失败行为保留；driver map 没有为取长度额外查询 GL
- 所有时间是 CPU 观察到的 API/wrapper 墙钟时间。ReadPixels/Finish wrapper 含原有 `flush_context`；WGL swap 含原有失败 fallback/FBO 恢复。本批没有 shader compile/link 计时。WGL swap 不是 D3D Present 到消费端延迟，也不是 GPU 时间
- 不覆盖 native-64 map、全部 direct-OpenGL 调用、生成 thunk、WineD3D PE shader/CS/fence/download、VirGL 内部 readback/finish，也不建立跨进程 frame/clock 对应关系。后续可结合已有 GL-PERF、FRAME-LOOP、NativeImage 记录分析

显式复制仍发生在驱动 flush 之前。0035 的发布代码块保持文本完整，以便原有逐补丁反向检测仍可识别已应用补丁；0037 仅在两个函数附近用局部 memcpy 包装计时，函数后立即取消宏。

## 宿主验证入口与边界

```sh
make test-wined3d-pixel-format test-wine-performance-summary \
  test-opengl-wow64-buffer-flush
make test-wine-shm-state-cache test-wine-patch-detection \
  test-gpu-followup test-egl-multi-consumer test-zc-binding-lifecycle \
  test-frame-loop-diagnostics test-shared-present-dispatch
```

新增测试从正常 overlay 链提取生产 helper、完整 Present 和完整 WoW64 map/flush/unmap/wrapper。以 ASan/UBSan 验证 off/on、复制字节、原始参数/返回结果/调用次序、失败/direct/pinned/Vulkan、线程隔离、时钟失败/倒退、一秒边界、长操作与全部指标饱和日志。两遍重放须逐字节相同。0035 的旧代码陈旧数据复现与修复测试保留，新增开诊断复测。

default-off 宿主微基准输出 baseline 与同 TU Unix hook 的 ns/op。宿主数字会受调度、优化和测试边界影响，只证明此宿主禁用路径成本与零时钟/日志调用，不能替代 ARM64/FEX 真机开关 A/B。WineD3D PE 计时不随本批交付，不报告其微基准。

当前环境未提供 OHOS/llvm-mingw 交叉工具链，不能完成 OHOS Unix 链接或 HAP 构建验证。独立审查在实际 Valve pin 未找到支持 PE 编译器 TLS 的 CRT 定义，而其 PE 链接排除了普通 MinGW startup/runtime。仅有 loader 支持不能证明链接可用，因此删除拟议的全部 WineD3D PE 计时/helper/header，明确延后 compile/link、CS、WineD3D map/download/fence 归因。本批只有 Unix ELF 中固定零初始化、无析构的编译器 TLS；没有 PE TLS 或新运行时依赖。后续不要用 emulated TLS、新 TLS 生命周期基础设施或链接默认变更绕过该问题。构建时新建独立 `BUILD_DIR`，固定 `NATIVE_ARCH=arm64-v8a WINE_ARCH=aarch64 GUEST_ARCH=aarch64`，只通过顶层 Makefile 构建。

## 2026-10-05 独立验证记录

- 通过：`test-wined3d-pixel-format`（15 个完整生产 Present 场景、4 个旧代码 ASan 越界复现、幂等重放）；`test-wine-performance-summary` / `test-winehua-perf`（3 项 helper + 2 项实际 Unix 边界测试，off/on 与时钟失败）；`test-opengl-wow64-buffer-flush`（7 项，含原 0035 陈旧数据复现及修复的 off/on）
- 通过：`test-wine-patch-detection`、`test-gpu-followup`、`test-egl-multi-consumer`、`test-zc-binding-lifecycle`、`test-frame-loop-diagnostics`、`test-shared-present-dispatch`。GPU follow-up 同时通过了构建身份/旧 stamp 拒绝与文件保留检查
- `test-wine-shm-state-cache` 整体未通过：SHM 13、minimize/restore 12、client remap 12 先通过，随后既有 fshack 的 5 个场景遇到本环境 `LeakSanitizer does not work under ptrace` 致命错误；单独原样运行 reserved-texture 也遇到同一限制。仅在补充运行关闭 leak scanning 后，fshack 12、reserved-texture 5 通过，ASan/UBSan 内存访问检查仍开；activation-return 15 正常通过。这些补充结果不等于聚合 Make 目标通过，也没有完成 LSan 验证
- 完整 34 个正常注册 overlay 从固定 Wine pin 重放两遍，结果逐字节相同。Wine/Valve gitlink、checkout SHA 与干净子模块状态保持；`git diff --check` 通过。记录在 `.temp/codex-runs/vpp-perf-validation/`
- 最终诊断负长度修复只把错误请求字节计为零，新增 target/named/EXT 原始负长度参数到驱动的 off/on/时钟失败回归。独立 focused 重跑 `test-wine-performance-summary`（3 + 2 项）和 `test-opengl-wow64-buffer-flush`（7 项）均通过，34-overlay 两遍重放及 hygiene 再次通过；最终源码审查无阻塞项
- 无完整 `make test` 通过声明；无 PE/OHOS 交叉构建、HAP、安装、真机或 FPS 数据。上述结果仅用于源码正确性和诊断行为验收

## 用户手动真机验收

1. 先验收已安装 0035 候选：人物完整性、未访问角色第一次切换、重复切换、地图稳态分别记录。旧采样均早于 0035，不能复用为新候选性能结果
2. 构建新候选后自行启动应用和游戏，保持相同游戏入口、分辨率、窗口模式、温度与内容。Kingdom Rush 保持特殊本地 EXE 入口。不要自动启动 Steam/游戏
3. App 自定义环境设置 `WINEHUA_WINEDEBUG=-all,+err,+winehua_perf`。这是设备端允许的显式覆盖；不要只填被 App 参数过滤的 `WINEDEBUG`。若从独立受控 shell 启动同一个 Wine 子进程，则其环境为 `WINEDEBUG=-all,+err,+winehua_perf`。0028 保留 Windows 子进程转发，每个进程须在启动时继承设置；开关不改已运行进程
4. 开诊断后确认目标 PID/TID 有 `scope=opengl_unix` 行（本批不会生成 WineD3D PE scope）；其他进程的行不能当游戏数据。关闭对照设置为 `WINEHUA_WINEDEBUG=-all,+err` 并重新启动同一测试进程，其余条件一致。对照中只变诊断 channel
5. 冷启动与同次运行内的重复角色选择分开记录，再记录地图稳定 30–60 秒。重复操作改善只能称该路径变暖，不能称已验证 disk shader cache 命中。当前仍保持 cache disabled、`VTEST_SYNC_GL_FINISH=1`、egl-main 和 FEX 默认；不要用 `VTEST_SYNC_GL_FINISH=0` 当禁用（代码判断变量存在）
6. 比较诊断 off/on 的图像正确性、输入/焦点/多窗口、同入口角色/地图耗时和 FPS，保存候选哈希及原始日志。Host tests 通过后仍需这道真机门槛；再据实际主耗时决定 upload、shader cache 或等待策略，不提前改变同步/画质/Box64 默认
