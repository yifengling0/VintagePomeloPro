# RichMan8：动态缓冲系统内存暂存候选及回测状态

基线 `feature/main_proton / 284026ce64c631961394f2aee6fadb3a4380e579`，Wine pin `cd547f7a0ee9d3d59b3ec47317a7852dedf0443b`。本轮承接 DISCARD 映射与上传计时报告。

## 当前结论

用户仍反馈画面与操作卡顿。上一候选未取得明确体感收益，人物矩形缺块仍可见。现有主菜单样本证明缓冲复制放大，但不能解释数分钟片头停顿，也不能替代受控角色切换样本。

0038 DISCARD 优化实际只省去约 1.48–1.55% 的 shadow 初始化。其余大量 NOOVERWRITE 仍保留整块复制。两段主菜单样本的入向复制耗时约 0.878、0.880 ms/swap，流量约 934、994 MiB/s。该耗时值得削减，尚不足以解释长停顿。

2026-10-05 21:41:18–21:41:39 的补采，起止截图均为主菜单动画，宿主两次采样 59.49、60.04 FPS；83701 次初始化复制 21941714944 bytes，耗时 1127.657 ms，1259 次 swap。聚合窗口可能跨采集边界，不能当精确交互延迟。截图仍可见人物背景色矩形缺块。

实际显示链路是 WineD3D / OpenGL / VirGL，FEX 保留。`winehua_readback_present` 在本轮符号化所命中的指令为零拷贝分支中的 glFlush；readbacks 计数保持 2，presents 持续增长。不能仅凭函数名称推断持续 CPU 回读。

## 0039：默认关闭的动态缓冲候选

开关 `WINEHUA_DYNAMIC_BUFFER_SYSMEM=1`。严格匹配字符串 1，其余值关闭。仅作用于 32 位 WoW64、GL backend、动态且 GPU accessible / 可写的 vertex/index buffer，排除 persistent map、查询失败和其他 bind flags。

复用 Wine 原有 `pin_sysmem`、location tracking 与 dirty_ranges 上传机制。保留 GL BO，CPU 修改先保留在系统内存中，绘制前上传有效脏区，避免每次小更新都构造整块 WoW64 GL shadow。

政策在 common buffer init 的 resource 初始化成功后、初始数据提交 CS 线程前设置。第一版在 GL init 返回后设置，存在与初始数据提交竞争的风险；第一版未打包或安装，后续只使用修正后的 v2 独立构建。

该候选可能增加系统内存保留、BufferSubData 和 VirGL 同步查询开销。不能保证更快，也不声明修复人物缺块、片头长停顿或所有 main/Proton 差距。0035 显式 flush 正确性仍保留。

## 验证

27 个 unittest 方法通过，提取编译真实生产 helper、common init、GL init、map/unmap/location/dirty-range 函数。覆盖默认关闭、范围排除、初始化失败传播、初始数据提交前政策可见、非零 offset 部分更新、重复 NOOVERWRITE、未写字节保留、嵌套锁与两段脏区上传、GPU location 失效后读回、DISCARD 完整上传。

36 个 overlay 连续重放两次一致。相邻 DISCARD、WoW64 显式 flush、Unix 计时、像素格式回归通过。C 回归使用 ASan/UBSan，LSan 关闭，不声明完整聚合测试通过。

Wine 使用干净的独立 worktree `workspace_temp/wine-sysmem-source-20261005`，新的 `BUILD_DIR=build-richman8-sysmem-v2-20261005`。独立 Wine 构建与顶层身份检查通过；其余打包及设备状态以证据目录 `package-identity.json` 为准。Docker 中外部 worktree Git 元数据不可达时，身份工具使用完整源码 SHA256 并明确 `source pin verified=False`；WSL 已单独验证 worktree HEAD，不绕过 guard。

## 真机协议

证据目录：Windows `F:/VintagePomelo-Workspace/workspace_temp/richman8-sysmem-20261005/`，WSL `workspace_temp/richman8-sysmem-20261005/`。

同一包提供两份 C: 根目录 BAT，仅动态暂存开关 1 / 0 不同，计时通道均开启。入口为 `Z:\games\RichMan 8\RichMan 8\strat game.exe`，支持传入完整 EXE 路径。

1. 用户手动运行 `C:\vp-richman8-sysmem-on-20261005.bat`；以实际 `dynamic_buffer_sysmem ... enabled=1` 日志确认命中，而非仅看 BAT 回显。
2. 以截图确认真正选人界面，再对齐首次和重复角色切换；随后采同地图稳态。每次动态识别 PID，不沿用旧会话。
3. 同包 off 重复相同交互，记录人物完整性、操作响应、映射复制与 BufferSubData 总耗时。时钟是 API 墙钟，不是 GPU 时间，嵌套指标不可累加。
4. 若开关未命中，继续查内部 streaming buffer 资源身份；若复制减少但卡顿仍在，追游戏线程文件加载、CS 队列和 off-CPU 等待。不得把主菜单平均 FPS 当输入改善。

覆盖安装只结束目标 bundle，保留应用数据；应用、Steam、游戏和 BAT 均由用户手动启动，不发送操作。源码尚未提交，本轮不提交包、运行库、签名材料或原始采样。

## 本轮包与安装结果

独立 v2 Wine 构建、顶层身份重入检查、HAP 构建、运行时组件检查及官方签名校验均通过。包内可加载 Wine ELF / i386 WineD3D sections 与独立编译产物匹配，36 overlay 哈希一致。FEX 与继承基线相同，zlib 与实际构建输出匹配。

- 签名包 SHA256：`0d078310c6a353262962704334925cdca6c5fd3e16f9ddc33627552b5a82202f`。
- 未签名包 SHA256：`1fce7bfae302115ebb5ba4e64bd317f077ca1473c53b3968bb2cef82542fa3d5`。
- 版本：`1.4.5-proton.26-alpha / 1004035`，同版本候选按哈希区分。
- 覆盖安装时间：`2026-10-05T21:55:05.039320+08:00`。
- HDC 明确返回安装成功，bundle/version 查询通过，安装后目标应用未运行。
- on/off BAT 均通过 bundle-aware send/recv 内容核对，已放到 C: 根目录；没有执行它们。
- 实际 on/off 真机验收待用户手动运行；尚无新候选性能或画面完整性改善结论。

安装只结束 `com.vintage.pomelopro` 和其子进程，未卸载或清数据。`on-startup-v2` 是最长 240 秒的只读观察，需按实际截图与用户反馈判断阶段；若用户未在该窗口内启动，不把空采集当游戏故障。

只读启动观察 `on-startup-v2`（10-05 21:55:38–10-05 21:59:35）期间没有观察到 RichMan8 进程，未产生游戏截图或游戏计时样本。这不是游戏启动失败或优化效果的证据；新候选真机验收继续等待用户手动启动。
