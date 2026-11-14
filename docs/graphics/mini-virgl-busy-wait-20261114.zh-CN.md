# Mini VirGL 完整 busy-wait 诊断

本轮补齐计时并完成 Mini Low 真机诊断，没有修改同步语义，也没有得到新的 FPS 提升结论。
在保守截取的 105.246 秒完整窗口中，23,984 次 busy-wait 处理共 0.422893 秒，
占宿主处理线程墙钟窗口的 0.4018%；其中 229 次 context_finish 合计 10.511 毫秒，
poll_loops=0、fd_waits=0、errors=0。该负载不支持把 busy-wait 内的 glFinish 作为优先优化目标。
这不代表 GPU 没有瓶颈，也没有计入 guest 排队、锁竞争、socket 往返及其他命令的执行。

## 基线与包身份

- 源码父版本：`2c7cef080ab5b613e157c678f1e11c21b8f6002d`，分支 `feature/fex-proton-upstream-sync`。
- VirGL 固定源码：`bb33e52aa03199926f662e50ff22bd00d19d7e0a`。
- Mini MLR-AL10，Heaven 4.0 PE32，D3D9 → WineD3D → guest virpipe → vtest → host VirGL/GLES。
- Low、1280×720 全屏、AA=0、tessellation disabled、VSync=0；writev=0，GLThread 未指定。
- 诊断包：`workspace_temp/mini-virgl-performance/VPP-2.0.2-mini-virgl-busy-signed.hap`。
- 诊断包 SHA-256：`2d091f61bcd0f3ed6093562542c03c70494784c9cf215f9eb07f09e47f186106`，627130345 bytes。
- 常规 EGL 包 SHA-256：`d172b43c5f1bb024e03ae8dd7dff7424f3041353e9e829c80c14eef8348277e2`。
- 诊断 vtest ELF SHA-256：`c2bf9a6453dbafd4e284f00a5b8f9c8d4d9ead690a8f1f694e1650957405593f`，147208 bytes。
- `vtest_renderer.c` SHA-256：`7542276ecf279f7ee667d42894d803d524fd682a002358877fb6fddc1731cf0c`。
- 补丁 0016 SHA-256：`56278bccb7b8b0a66803b4f5b3cfc1af60ce9376c00b8f2b0d274409b352e479`。

只复用冻结 host 对象、重编 vtest_renderer.c 并隔离链接。未改动基线重新链接后与
已验证 EGL HAP 的 vtest 库完全同 SHA。诊断包只替换
`libs/arm64-v8a/libwinehua_vtest_server.so`；328 个 payload 成员逐项 SHA-256 验证，
其余 327 个成员与常规包一致，Venus 优化保留。签名、profile 与证书匹配验证通过，
版本 2.0.2/2000020，release/internaltesting、debug=false、profileable=false。
这是受控 overlay 诊断包，未通过顶层与内嵌 runtime 的全量发布一致性验收。

## 计时与正确性

旧计时从 context_finish 之后开始，可能漏记主要同步成本。补丁 0016 从请求处理入口
覆盖 read、context_finish、poll、reply、total/max，分别聚合 WAIT 与非 WAIT 请求。
每五秒最多两条 summary，不启用逐请求 trace。

`WINEHUA_VTEST_BUSY_DIAGNOSTIC` 默认 0。普通构建仅在原有显式 summary 开关开启时计时；
本次专用库使用 `-DWINEHUA_VTEST_BUSY_DIAGNOSTIC=1`，没有改变产品 resolver/default。
VTEST_SYNC_GL_FINISH 仍按 getenv 是否存在判断，字符串 `0` 也会启用。
请求/回复顺序、context_finish 条件、fence wrap 比较和所有错误路径维持原样。

`host_tests/virgl_busy_timing_test.py` 直接抽取生产 handler，以注入阶段耗时和故障验证
两种诊断宏模式：关闭时无 clock 调用、强制诊断时可计时、完整 finish/read/poll/reply、
wire/sync parity、非等待查询、协议 probe、失败、wrap 和五秒日志限频。
ASAN/UBSAN 与 Wall/Wextra/Werror 通过。入口为 `make test-virgl-busy-timing`。

固定版本的 29 个相关文件隔离应用 0001..0016 后，所有 effective source SHA 与 lock
一致；二次应用幂等、完整反向/replay 与生产 handler 两模式测试通过。
现有工作树三个 Venus 文件另有先前修改，因此没有以 dirty 整树通过锁匹配为验收条件，
没有重写这些文件，也没有提交任何子模块 gitlink。

## 真机采集与结果

持有 Mini 独占锁后替换安装；没有卸载、清除用户数据或联系手机。
启动前沿用 Battery≤35°C、system_h≤40°C、shell_front≤38°C 的有界冷却门控。
画面确认进入内置 benchmark 后采样约 120 秒；起止截图分别为 scene 1/26 与 13/26，
已人工查看。提前停止是诊断设计，没有完整 26 场景 FPS/Score 成绩，不能与旧 16.1/16.2
FPS 结果计算提帧比例。

系统拒绝 shell 读取 /proc/uptime 和执行临时 native 时钟程序，相关失败已记录，没有提升权限。
使用同一进程 80 条 HiLog 记录的 epoch/monotonic 双格式与有界 date 查询建立时间映射。
HiLog 的单调时间秒字段与 wall-clock 小数部分存在精度限制，映射差值跨 1 秒，
因此在 CPU 采样起止边界额外各留 2 秒，只统计完整窗口。
最终保留 21 个窗口，共 105.245857 秒、42 条聚合；加载、边界和导出阶段均排除。

| 类别 | 调用数 | 处理总耗时 | 占窗口墙钟 | 最大单次 |
| --- | ---: | ---: | ---: | ---: |
| 全部 busy-wait | 23984 | 422.893 ms | 0.4018% | 0.2411 ms |
| WAIT | 229 | 14.998 ms | 0.0143% | 0.1568 ms |
| 非 WAIT 查询 | 23755 | 407.895 ms | 0.3876% | 0.2411 ms |

| 阶段 | 累计耗时 | 占窗口墙钟 |
| --- | ---: | ---: |
| 请求 read | 96.036 ms | 0.0912% |
| context_finish | 10.511 ms | 0.0100% |
| poll | 3.256 ms | 0.0031% |
| reply | 311.078 ms | 0.2956% |

229 次 WAIT 均执行 context_finish，平均 finish 约 45.9 微秒；没有进入 poll 循环或 fd 等待。
reply 是该 handler 内部的大项，但其绝对时间只占窗口约 0.30%，没有证据支持优先优化它。
上述墙钟占比不能直接换算成端到端提帧上限，因 guest 往返、唤醒和排队不在此计时中。

120 秒 CPU 采样使用 100%=一个逻辑 CPU：Heaven.exe 260.03%、VirGL child 61.29%、
wineserver 10.73%。它们描述进程累计 CPU 成本，不代表关键线程余量或 GPU busy。
保留的场景画面正常；呈现日志仍有 upload_bytes=0、failed_swaps=0，
但呈现上传为零不等于所有纹理上传为零。

## 后续优化顺序

1. 量化 guest WineD3D 的 CS 提交/finish、map/query 及 virpipe socket mutex、请求往返，
   区分游戏/FEX 执行成本、排队和宿主处理。已有 CS worker 不能证明其无串行瓶颈；
   不把增加 CPU 总占用当作优化目标。
2. 给宿主 SUBMIT_CMD 解码执行、TRANSFER_GET/PUT 与 present 补有界阶段统计。
   本轮仅覆盖 busy-wait，不能把宿主其他 GL 调用中的驱动/GPU 等待排除。
3. 同画质做分辨率对照并结合 GPU busy/时间戳，区分提交、shader 与带宽成本。
   High 仍需独立采样，不能将本轮 Low 的结论推广给所有画质。
4. 按已证实的热点实现小范围候选，再做统一温度、完整 26 场景的关闭/开启交叉对照。
   writev 目前没有明确收益，继续默认关闭；不改动 glFinish 语义，不伪造 busy=false。

## 证据和设备状态

Windows 固定证据目录：`F:/VintagePomelo-Workspace/artifacts/mini-virgl-busy-wait/`。
`busy-analysis.json` 可由固定任务的 `analyze-busy.py` 重算；保留包/构建/签名收据、主机测试与
干净补丁栈日志、起止截图、CPU 样本、原始 host 尾日志和时钟映射记录。
旧 `artifacts/mini-virgl-performance` 的冻结 manifest 保持原样。

诊断后恢复常规 EGL 包，替换安装结果与最终进程状态以 `restore.json`、`final-state.json` 为准。
任务目录保留常规与诊断两个签名 HAP；删除 unsigned HAP 和临时 clock ELF，
复用现有构建缓存，不复制游戏或完整构建树。存储维护结果见工作区固定 latest.json。
