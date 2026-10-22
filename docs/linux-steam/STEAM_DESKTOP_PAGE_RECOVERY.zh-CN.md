# 退出大屏后游戏库黑屏：CEF 页面恢复

## 现场与原因

2026-10-20 USB 平板现场：退出 Big Picture 后，Steam 普通窗口的顶栏、底栏仍显示，网页区域变黑，地址为 `data:text/html,...`。主进程没有退出；同一会话的网页助手重建后，点击游戏库触发：

```text
ERROR:bad_message.cc(29)] Terminating renderer for bad IPC message, reason 213
WARNING:sad_tab.cc(255)] Tab Killed:
```

对照 Chromium 126.0.6478.183 的 `content/browser/bad_message.h`，213 为 `INVALID_INITIATOR_ORIGIN`；`renderer_host/ipc_utils.cc` 在渲染进程发起导航、来源与该进程的来源授权不符时拒绝请求。现有日志能确定页面被来源检查终止，尚不能确定 Valve CEF 内部来源状态为何不一致。不能把这个错误归因于 Wayland、GPU 或账号认证。

本轮 SysV 表为 6/256 个有效记录，排除了此前表耗尽的现场。早期诊断脚本通过 Z 盘取到了历史日志，后改用 Wine 的明确 Unix 路径重新采集；结论仅依据本次会话的时间和 PID。

实机验证：在同一 Steam 主进程中发送 `steam://open/library` 可以恢复游戏库。退出大屏会重建网页助手，在重建期间过早发送该命令可能被丢弃，因此单纯在退出大屏后立即补发命令不足以解决问题。

## 本轮处理

- 每秒检查本轮新增 CEF 日志，跳过启动前历史；每轮最多读取 64 KiB，记录按行处理，兼容分段写入、文件替换和截断。
- 只匹配实际观察到的 CEF 213 终止记录。检测到后，通过现有串行命令队列请求 `steam://open/library`，由浏览器端重新导航。
- 仅在 Steam 普通窗口位于前台时处理。后台游戏、Deck 主屏、Big Picture 不被恢复任务抢焦点；更新的用户操作优先，退出请求会取消排队中的恢复。
- 同一网页助手最多两次，两次之间至少 30 秒；重复记录不反复处理。再次切换模式创建的新助手使用独立额度，避免正常多次切换后失效。命令失败给出重新打开游戏库的提示。
- 保留 Chromium 来源检查、现有进程、账号、配置、下载和存档；不关闭网页安全，不重启 Steam，不清缓存。
- `session/page-recovery.json` 记录原因、次数及 `navigation_dispatched`，它仅表示导航命令被接收，不作为页面已显示的证明。

这是针对已识别 CEF 故障的有限恢复措施，未修改 Valve 的 Steam/CEF 二进制；其他原因的黑屏不由此规则处理。

## 验证

`scripts/run_linux_steam_deck_tests.cjs` 覆盖历史日志、精确匹配、分段/重复记录、轮换/截断、冷却及次数限制、链接与读取上限，以及前台条件、并发用户操作优先和恢复不重启客户端。

ArkTS 编译、窗口桥与 native 声明契约测试通过。候选使用固定 EntryAbility 基线，Mesa gitlink 保持 `557fa3e781ea6e78fcff3ee759c25f60c3055064`。v110 已实测在同一主进程内自动恢复库页面，v111 将重试额度限定为每个网页助手。

v111 已安装到 USB 平板，连续三次执行“大屏 → 普通窗口”。三轮均实际查看截图确认完整游戏库恢复；Steam PID、会话 generation 和启动次数均未变化。随后实际点击“商店 → 库”并打开仙剑详情，游戏库和详情正常显示，菜单响应。实机记录为任务目录中的 `mode-recovery-v111-verification.local.json`，含账号画面的截图仅本机私有保存。

模式切换仍需等待网页助手重建；本方案解决的是已识别故障后的自动恢复，不能宣称消除了切换加载黑屏或根治了 Valve CEF 的来源状态错误。验证记录的耗时还包含状态轮询、等待画面稳定及截图时间，不作为纯导航延迟。

Chromium 对照源码：

- [bad_message.h（126.0.6478.183）](https://chromium.googlesource.com/chromium/src/+/126.0.6478.183/content/browser/bad_message.h)
- [renderer_host/ipc_utils.cc（126.0.6478.183）](https://chromium.googlesource.com/chromium/src/+/126.0.6478.183/content/browser/renderer_host/ipc_utils.cc)


## 2026-10-22 手机的内容焦点漏判

无线手机在同一个 Steam 会话中记录 CEF 213 后，窗口仍在前台，但恢复一直没有发出。
22:46:18 点击空白标题栏后，输入日志明确为 `KbdFocus oldTl=5 newTl=4`；绑定记录为
`source=5, host=4`。22:46:19 随即发出本会话第一次自动恢复导航，截图确认游戏库恢复。
该操作没有点击“库”，也没有重启 Steam。这证明至少这一次延迟恢复由前台判定造成，
不是日志匹配失败。

旧判断只接受 Wine 宿主的键盘焦点，遗漏原生激活时落到已绑定 Xwayland 内容窗口的焦点。
现在接受同一有效绑定的两种焦点身份，并检查双方窗口 generation、最小化、停止、
Deck 前台及游戏前台状态；无关窗口不触发恢复。主机回归覆盖这些边界。
这是页面恢复触发条件修正，不是 CEF `INVALID_INITIATOR_ORIGIN` 或网络服务崩溃的根因修复。
本次源码修正已纳入按用户要求重新生成的 1.5.0 未签名 HAP 和上架 APP；
窗口与会话回归、Release 编译、签名及包级核验通过，尚待新包实机复验。
