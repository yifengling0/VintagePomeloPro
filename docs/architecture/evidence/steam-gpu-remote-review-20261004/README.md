# Steam GPU 远端修复评审核心证据

完整报告：[steam-gpu-remote-review-20261004.md](../../steam-gpu-remote-review-20261004.md)。

复核版本：`2add84d3298924279eed5c38ccde3a61663855c2`，旧缺陷基线：`960bf939835e53494118a6be592bf68bd5f638de`。这是报告/证据提交，不包含业务修复。

## 接续判断的重点

1. **游戏被 Steam 遮住**：第二次游戏 `(37100,58)` 持续消费至第 1200 帧，failures=0；父窗口 #10 可见且全屏，但 11 次中心探针都命中 Steam #4。用户确认被 Steam 遮住，首轮退出是手动关闭。先检查全屏候选、FsPriority、z-order 和实际绘制层；这些日志没有给出完整候选优先级，唯一根因尚未确定。
2. **弱 owner 解析仍可错绑**：完整生产 ResolvePresentBinding 接受 1×1 producer 绑定 101×398 的同进程唯一 owner。两处弱后备入口均应收紧；不能仅用尺寸阈值代替合法角色和身份关系。
3. **Overlay 异常退出**：GameOverlayUI 有 13 条 exit=11，多个发生在启动后 1–2 秒的自动重复启动阶段。游戏手动关闭也出现 exit=11，故不要把所有退出当作用户看到的自行闪退；还没有直接异常栈。
4. **性能口径**：低速段宿主交换均值 18.393 FPS，宿主工作占 6.801%，节拍等待占 92.843%，不是严格 A/B。宿主失败 0 不包含 Venus 上游 EAGAIN；最后一条回调累计 fail=300。先解决可见性，再对无遮挡场景测 producer/consumer/实际绘制间隔。

## 证据位置

- `device-session-analysis.json`：完整源日志推导的按表面、时段和退出原因统计；其 source 字段指本地完整日志，仓库提供下面的摘录。
- `device-session-key-events.txt`：关键启动/消费/退出/清理时序及用户反馈对应的节点。
- `device-render-excerpts.txt`：从宿主 30320 / 图形进程 30865 的当前会话提取。包含全部匹配的 GPU 消费样本、Venus target/callback、游戏窗口快照、相关 fullscreen 请求、fs-pick、进程完成事件；不是完整 hilog。
- `venus-host-present-excerpts.txt`：完整 virgl-host 日志最前/最后各 3 条私有 Vulkan host present，证明实际出现该链路；不是 acquire/fence 压力测试证据。
- `resumed-observe/frame-loop-rows.json` / `gl-perf-rows.json` / `session-summary.json` / `cef-lifecycle.txt`：宿主帧统计、consumer 与 CEF 生命周期。
- `host-tests.log` / `contracts-baseline.log` / `weak-owner-repro.log`：新版回归、指定旧版本断言复现和剩余弱绑定复现。
- `wine-build.log` / `wine-build-current-source.log`：初次缓存漏编及重新配置后真实构建。`assemble.log` / `assemble-current-source.log`：初次 guest 架构失败及正确架构组装。`hap-build.log` / `sign.log` / `package-identity.json`：HAP 构建与身份。

## 复核命令

在仓库根目录运行（需要 Python 3 和 g++；此前已在 WSL Ubuntu-22.04 通过）：

```bash
python3 docs/architecture/evidence/steam-gpu-remote-review-20261004/weak_owner_repro.py
python3 host_tests/steam_gpu_contract_test.py --baseline
```

弱 owner 脚本可用 `--repo /path/to/checkout` 指定另一份源码；默认沿自身目录向上定位 .git，适用于普通 checkout 和 worktree。它逐字提取整个 ResolvePresentBinding，只替代平台资源。生产修复后，该旧行为复现断言失败应视为需要调整预期，不代表修复有误。

低速段可从帧统计独立重算：选择 renderer=1 且 time 在 `[14:01:02.000,14:01:41.000)` 的 20 行，FPS=`sum(presents)*1000000/sum(window_us)`；每行表示结束时刻前约 2 秒。宿主等待只统计这个线程，不能推出 Wine/FEX/CEF 或 GPU 总体利用率。

## 完整本地材料

`F:/VintagePomelo-Workspace/workspace_temp/gpu-remote-review-20261004/` 留有全部原始日志、截图、源码快照、358 MB HAP 和 `review-evidence.zip`；这些文件没有重复进入本提交。ZIP SHA256：`71d25767bf40fbc39a0f76c0d3c425eec3b92a4c74e33aa22181395f3a2e6bfb`。HAP 身份见 package-identity.json。

本目录 SHA256SUMS.txt 仅覆盖提交的证据文件，额外包含 `../../steam-gpu-remote-review-20261004.md` 报告的哈希。日志已过滤凭证行、SteamID 和设备序列号；本提交不包含截图、签名材料或安装二进制。

导入仓库的文本统一为 LF 并移除行末空白；完整本地材料及其原始 SHA256 保持不变。
