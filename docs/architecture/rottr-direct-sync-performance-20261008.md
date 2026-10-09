# ROTTR Vulkan Direct：同步开销与 wineserver 构建核查

日期：2026-10-08。仓库 `feature/main_proton`，基于 `4cb9e7c8`，包含前序
未提交修改。本轮不切换 FEX，不改变 x87、PAL4 / RichMan8 的图形设置。

## 已取得的证据

平板 MatePad Mini / Maleoon 910，bundle `com.vintage.pomelopro`，
`1.4.5.29 / 1004038`。游戏为 PE64 `Z:\games\GUMU10\ROTTR.exe -dx11`，
使用 legacy ARM64X DXVK 和 LAB Direct；800×600 窗口、低画质、AA / VSync
关闭，沿用 `rottr-no-msaa.conf`。每次结束整个 Wine/FEX 会话再启动。

首次计时采样的主线程约 4,345 请求/秒，RPC 墙钟占比 42.39%；Flush 线程
约 7,941 请求/秒，占比 45.31%。主要请求分别是 release_semaphore 和
event_op/select。第二次资源稳定后的 30 秒采样重复了这一现象：

| 线程 | 请求/秒 | RPC 墙钟占比 | 主要请求 |
| --- | ---: | ---: | --- |
| ROTTR 主线程 | 4,357.59 | 42.07% | release_semaphore |
| Flush | 7,955.83 | 44.79% | event_op、select |
| PostCollect | 2,832.92 | 27.88% | select、release_semaphore |

这不是 CPU 使用率。计时仅围住 `server_call_unlocked()` 的发送与回复，
不包含随后的 wait-pipe 阻塞，也不包含外层 pthread_sigmask。不能把不同
线程的墙钟累计相加当成单帧耗时，或据此断言全部瓶颈都在 CPU。

## 默认关闭的请求计时

`0048-ntdll-opt-in-server-request-timing.patch` 新增
`WINEHUA_SERVER_PERF=1`，默认关闭，按线程约每秒汇总最多八类请求。
在 request union 被 reply 覆盖前保存请求身份，保留 errno 和返回状态。
没有修改对象、句柄、等待或唤醒语义。

专项测试覆盖关闭状态、请求身份覆盖、错误返回、errno、边界、TLS 和汇总
窗口。独立来源 `workspace_temp/wine-rottr-server-perf-source-20261008`，
构建 `build-rottr-server-perf-20261008`，仅重建 Unix ntdll。
符号库 SHA256 `751b9c01993a764175500d2cbf9a743ebd48eb7f34f12442892a0d3683296079`。
包内 `.text` 与符号库一致，446,824 字节，SHA256
`9a61718d496ad0b2f89a2e8c583829e07da22b7f106221e99d0cdd31b8a52651`。

## DXVK 与其他实验

启用既有 `DXVK_WINEHUA_PERF_DIAGNOSIS=1`。资源稳定后约每帧 1,170 次
draw、24 次 dispatch、约 10 次队列提交；compiler_busy=0，图形管线约
852 条。Present CPU 墙钟均值约 10–13 ms；多数帧间隔约 66–68 ms。
日志中的 cs_syncs/gpu_syncs 为零，不代表驱动没有阻塞或 GPU 不忙。
DXVK 1.10 gpuload 是完成队列空闲时间的估算，不能替代 GPU timestamp。

`WINE_CPU_TOPOLOGY=4` 实验在同存档雪山约 7.44 FPS，没有收益，已恢复默认。
该设置也改变 CPU affinity，且 HUD 条件不同；不能据此断言减少工作线程
必然无效，不能未经核实猜测大核 CPU 编号。

初到雪山时一次 30 秒窗口约 10.77 FPS，后续同会话稳态约 14.87 FPS。
这说明热态会明显影响结果；菜单、加载期、移动期和稳态不能混为一个基线。

## wineserver 的构建问题与修复候选

`scripts/build_wine.sh::build_wineserver()` 原先单独拼接编译参数，漏掉
Unix Wine 的 `-g -O2`。旧二进制反汇编与未优化代码吻合。补入共享 Wine
发布参数，并显式保留 Wine EXTRACFLAGS 中的 `-fno-strict-aliasing`，
不定义 NDEBUG，不移除 Wine 断言，不改变 Windows 对象同步机制。

新增编译参数/编译器版本指纹。没有身份记录或参数变化的 server 缓存必须
重编所有对象；指纹只在链接成功后写入。新增
`--unix-modules wineserver`，仍经过源码身份和 configure 检查，空 DLL
目标列表不会触发全量 make。`make test-wine-server-build` 执行实际构建
函数的录制编译器测试，覆盖旧缓存、编译器/参数变化、重复复用、发布参数、
别名规则和断言保留。
缓存命中时也重新发布选定构建的 server 库，防止目标目录已存在另一构建的
旧库时，仅凭文件存在跳过复制。
发布采用临时文件后原子替换，避免写穿 Hvigor 与原始输入之间的 hardlink。

第一版仅补 O2 的候选在启动中黑屏、无新提交，已经结束，不能发布，也不能
拿来测性能。之后发现 standalone 编译还漏了 Wine 要求的 no-strict-aliasing
规则。黑屏原因尚不能只凭静态检查定论；已保存现场，并回装原包复核。
第二版使用独立 `build-rottr-server-o2-v2-20261008`，未复制 Wine 对象。

第二版候选 `rottr-server-o2-v2-signed.hap` SHA256：
`7f43e6e30e75fe4d7fea495a5b370eb61be447e5d32fd02ea95d6032525b1c64`。
包内 server `.text` 与符号库一致，443,652 字节，SHA256
`d8354fcedd3bb870a77d518968e92421135a4b06cc6ae7d67c8559d5055130e3`。
包内 ntdll 与原计时包 `.text` 完全相同。

## 第二版真机对照

第一版失败后回装原计时包，正常加载雪山；随后安装第二版，又正常加载同一
存档。双方相同 Direct/画质/诊断/HUD 条件，均等待资源稳定后站在初始道路，
没有进行角色移动。两个 30 秒窗口如下：

| 指标 | 原 server | 第二版 O2 + no-strict-aliasing |
| --- | ---: | ---: |
| 采集时间 | 20:34:43–20:35:13 | 20:41:18–20:41:48 |
| root 新画面 FPS 均值 | 14.9163 | 14.8707 |
| 主线程 RPC 墙钟占比 | 42.76% | 41.29% |
| 主线程平均 RPC µs | 97.64 | 94.20 |
| Flush RPC 墙钟占比 | 44.88% | 42.83% |
| Flush 平均 RPC µs | 56.38 | 53.94 |
| wineserver CPU ticks | 1,368 | 1,307 |
| 游戏 CPU ticks | 8,715 | 8,858 |
| shell_front 温度 °C | 42.947→43.256 | 43.956→44.133 |

结论：本轮没有确认 FPS 提升。请求成本只是小幅下降，优化 server 自身执行
并没有消除传输、排队和线程调度开销，剩余同步往返仍值得继续调查。
这是顺序实验，温度与资源热态仍有差异，不能把小幅 ticks/耗时差异当成
严格的因果提升，更不能预报快速同步一定能达到 30 FPS。

首版缺少 no-strict-aliasing 时停在启动黑屏，回装原包及补齐参数的第二版
均重新出帧。第二版保留；首版不得交付。还需独立同步压力/兼容性回归，
不能仅凭一个游戏的两个启动宣称所有游戏均安全。

证据 `rottr-opt-o0-repeat-warm-*`、`rottr-opt-o2-v2-warm-*` 及
`rottr-server-run-comparison.json`。统计按主线程 tid=pid 识别，不能仅凭
comm=ROTTR.exe 选择线程，因为多个工作线程也沿用了该名字。

## 关闭诊断后的复核与最终状态

第二版再次完整退出 Wine/FEX 后启动，关闭 server / DXVK 计时与 DXVK HUD，
保留原有宿主 FPS 显示。正常加载同一雪山存档，20:53:44–20:54:14 的
30 秒窗口持续有新画面，均值 14.8693 FPS，范围 13.93–15.98 FPS；
shell_front 温度为 44.090→44.355°C。截图
`rottr-opt-o2-v2-normal-warm-before.png` / `after.png` 保存了现场。

20:57:41–20:57:51 再核对 10 秒，游戏和 server 进程仍存活，每秒都有新帧，
均值 10.696 FPS，范围 9.94–10.92 FPS，前后截图的角色姿态和风雪在变化。
这段不是前述对照窗口，不能以它计算 O2 收益或回归；它只确认当前游戏仍
持续渲染，未复现首版启动时的无提交黑屏。shell_front 为
42.771→42.737°C，尚不足以确定这次帧率下降的原因。

截至 20:57:51，宿主 PID 45207、游戏 PID 46852、server PID 46533；
游戏保留运行，Direct 和 FEX 保持原测试设置。没有启动 Steam。
本轮代码和报告尚未提交或 push，首版失败候选不得交付。

## 验证范围

已通过 server 构建专项（执行真实构建函数，使用录制编译器，包含原子发布
对 hardlink 的保护）、server 请求计时专项、Wine 构建身份和 Makefile 身份
专项、`bash -n scripts/build_wine.sh` 及相关 `git diff --check`。
第二版 HAP runtime closure 为 `PASS: wineArch=aarch64`；已核对包内
server / ntdll 的 `.text` 身份，并完成回装原包、第二版诊断开启和关闭的
真机启动复核。未运行聚合全测试，未完成所有游戏的同步兼容性验证。

最终构建脚本在第二版编译后又加入原子发布保护；它不改变第二版实际编译
参数或二进制，专项已验证该发布逻辑。后续构建应使用新的独立 BUILD_DIR，
避免脚本身份变化后误用旧目录。

## 后续快速同步适配的边界

wineserver 仍需管理进程、对象、句柄权限和生命周期。游戏内部频繁调用
API 本身没有错误；优化目标是这些调用的成本，不能删事件或跳过等待。

当前 Proton 的 fsync 被 OHOS 显式禁用：futex_waitv 在应用沙箱会 SIGSYS；
没有确认可用的 ntsync 设备。不能直接开启 WINEFSYNC。SRW / WaitOnAddress
已有普通 futex 路径，DXVK 内部 mutex 也不能笼统归入 wineserver RPC。

后续若适配快速同步，须同时处理允许的跨进程共享内存、普通 futex 与多对象
等待、APC、WaitAll、句柄关闭/复用、权限、超时和线程退出，保留上游语义。
共享全局唤醒代次可能提供 waitv 的替代实验，但会有惊群成本，尚未实现，
不能当成可用方案或已有性能收益。

外部证据：`F:\VintagePomelo-Workspace\workspace_temp\gumu10-dx11-20261008`。
`rottr-opt-o0-warm-diag-*` 是稳定 RPC/FPS 基线；`rottr-opt-o2-black-*` 是
第一版失败现场；`rottr-server-o2-package-identity.json` 和验证脚本保存
第二版包/符号库身份。HAP 和原始采样不进入 Git。
