# 新仙剑命令队列停滞：定位与事件同步修正

## 现场与判断

基线为 `main_proton_steamarm64` 的 `b7264e77fa5bca838b0c10f86833ceb597c5032e`，
平板使用 v151 Release 调试签名包、独立 Linux ARM64 Steam 和现有 OHOS Wine/FEX。
本次未修改游戏、下载库、账号、存档或 GPU 配置。

最初主菜单停止响应、FPS 为 0；主线程消耗一个 CPU 核心，而 WineD3D 命令线程在等待。
有界 HiPerf 采样中约 86.2% 落在 `NtDelayExecution` 的让出执行路径。
进一步的进程内采样确认：主线程等待 MAP 队列完成，队列仍有待处理命令，
CS worker 却停在 `NtWaitForAlertByThreadId`，`waiting_for_event` 已为 0。

连续间隔采样的队列 head/tail 不再变化；仅向目标 worker 补发一次
`NtAlertThreadByThreadId` 后队列立即推进，之后再次停住。
这将本次故障定位到 WineD3D 命令队列的线程唤醒路径，不能据此断言具体是
FEX、OHOS futex 或 Wine 底层实现中的哪一个环节失效。
没有证据需要修改游戏，或把本次问题归因于 Steam 网页网络访问。

## 修正

`patches/wine/0068-wined3d-ohos-command-event-wake.patch` 接入 `scripts/build_wine.sh`：

- 提交侧使用 `SetEvent`，等待侧使用 `NtWaitForSingleObject`。
- 每个 CS 创建独立的自动复位事件，使用 Wine 原有的事件回退机制。
- 保留 `waiting_for_event` 的竞态检查、队列、CSMT、纹理处理及图形呈现路径。
- 不关闭多线程渲染，不修改 VSync、分辨率、GPU 加速或游戏文件。

补丁属于本工程 OHOS 构建叠加层。32 位及 ARM64/ARM64EC 混合 WineD3D 均已编译；
`scripts/stage_wined3d_queue_runtime.py` 更新既有运行时中的两套 DLL、内容版本与清单，
核对原始 payload SHA-256 后才替换，重复执行为幂等操作。

首次仅替换 prefix 中的 Wine builtin DLL 并未生效，该轮不能作为修正成功证据。
随后更新实际运行时目录，重新启动游戏，并从进程内读取 PE 标识：

| 标识 | 原 DLL | 已加载候选 |
| --- | --- | --- |
| TimeDateStamp | 1791608549 | 1792644619 |
| SizeOfImage | 2793472 | 2793472 |
| SizeOfCode | 1839104 | 1839104 |

已加载标识与最终候选对应。加载器可能调整内存中的 PE 头，因此不把整个内存头
与磁盘文件的直接哈希比较作为验收标准；磁盘 DLL 另做 SHA-256 读回核验。

## 验证结果

- 独立诊断程序模拟线程 ID alert 与事件的队列交接，完全不加载游戏和 GPU。
  一轮 alert 在第 2 次交接停滞，额外唤醒后恢复；另一轮未停滞，说明其有概率性。
  同两轮事件测试均完成 20,000 次，未停滞。
- 持久回归程序 `host_tests/wined3d_queue_wake_probe.c` 默认仅测事件；
  `--compare-alert` 可追加旧路径对照。每种模式最多 15 秒、进程总 watchdog 45 秒，
  停滞或交接不完整返回非零；补发唤醒不会把失败改判为通过。
- 最终回归程序实机 32 位和 ARM64 各完成 20,000 次，`stalls=0`，线程均正常结束。
- 实际游戏第一次有效事件候选启动：4 分钟、25 次 `WM_NULL` 检查全部响应，
  可点击“离开游戏”、弹出确认框并确认退出；进程列表确认已结束。
- 使用最终剥离符号的 DLL 再启动，菜单连续检查通过，再次从菜单正常退出。
- 第三次启动用于进程内 DLL 身份确认，菜单仍响应，随后从菜单确认正常退出。
- `wine_patch_detection_test.py` 3 项、`wine_builtin_identity_test.py` 2 项及
  `wine_build_identity_test.py` 均通过。后者的测试 fixture 补入已成为构建输入的
  `wine_runtime_diagnostics.py/.h`，保留原有不改动源码和缓存的断言。

全部游戏验证只到主菜单与退出确认，未读档或保存；尚未进行长时间游戏、手机或
多款 WineD3D 游戏的性能回归。事件路径增加多少同步开销，本轮未作量化承诺。

## 最终状态与校验

平板已保留修正版 WineD3D；临时采样版 lsteamclient 已恢复标准版，额外 loader trace
配置已清空，诊断 EXE/DLL 已移除。源码补丁及 runtime payload 已同步；本轮未生成新 HAP，
未提交或推送 Git。

| 成果 | SHA-256 |
| --- | --- |
| i386 WineD3D，2,760,704 字节 | `073f3ea61a4356a1d9ded256d6362a05b403d7d96f53c2c0a8bdf8f449edc2bd` |
| ARM64 WineD3D，6,291,456 字节 | `ca79a10c33304e655e539c9b92d23a08b17dd59b17d031e182e32ea436fa3eb6` |
| 标准 i386 lsteamclient | `1d6bcabd765298aacc71b045dab62fde92e60e271c6e9da4a5877033e02d6016` |
| 新 wine-data.zip | `d488d4145eb5ad565228a3a3139648d838c8a300e8380181089d3911c21714d9` |
| 修正补丁 | `86912fe809cf2d1b8edb4ba25cca7c8aeae157d19514a156989f82b88503d5e5` |

有界原始证据保留在工作区固定目录
`artifacts/steam-linux-arm64-client-20261019/`：`tablet-current-pal-samples.private.json`、
`tablet-current-pal-alert-probe*.private.json`、`tablet-current-pal-event-health.private.json`、
`tablet-current-pal-event-health-final.private.json`、`tablet-current-pal-module.private.json`、
`tablet-current-pal-queue-{i686,arm64}.private.json` 和 `tablet-newpal-final-runtime-hashes.json`。
这些技术日志不含认证票据内容，不应附带整个私有设备日志目录公开上传。

任务收尾清理本轮 20 个可重建诊断程序和 DLL 重复副本，共回收 30,922,752 字节
（约 29.5 MiB）；保留源码、增量构建缓存、小型证据及原有 v151/v149 两个安装包。
已执行 `maintain-storage.ps1 -Apply -CompactLogs`，本轮维护无额外删除项、无错误。

## 1.5.0 发布打包补充

上述“本轮未生成新 HAP、未提交或推送”描述定位实验结束时的状态。后续按用户要求已将修正纳入 1.5.0 / 1005000 的 Release 未签名 HAP 与正式 app_gallery 签名 APP。最终打包的 Wine payload SHA-256 为 `dc49459e93f982a4a5ef373b4f8a0905a316b9383d57ec9bf9cfb83f21d6baa3`；两套 WineD3D DLL 与补丁的校验值保持上表一致。Release 编译、签名及包级核验通过；本次打包后未重新进行设备端完整稳定性验收。
