# Linux ARM64 Steam：现有宿主接入第一阶段

日期：2026-10-18。实施树：WSL `/home/liufeng/src/vpp-account-cloud`，
分支 `feature/account-cloud-membership-mvp`，提交基线
`b84e7db15ee9d002ef3a00f4c316fc401a191fdf`。本报告中的实现和候选包还包含
工作树改动，不能全部归属于该提交。

## 结论和范围

现有工程可以承载标准 Wayland SHM 客户端。已把实验工程中的 registry/ABI
探测接入当前产品树，在 USB 平板 MLR-AL10 实际建窗、提交共享内存帧并完成
键盘回环；另有截图证明测试图案能出现在现有桌面上。

这一步执行的是 OHOS 客户端。GNU 客体的启动、线程、系统调用、进程与 FD
桥还没有通过；Xwayland、CEF、Steam 也没有启动。不能把宿主显示验证等同于
Linux Steam 已能运行。

参考工程是用户提供的 `F:/Gits/steamos/steam-ohos-lab`，尤其是
`reports/winehua-host/2026-10-18-integration/REPORT.zh-CN.md`。其中关于
Xwayland/wlroots 的设计、GNU G18 的限制继续有效，本轮没有重复构造器解释器实验。

## 工程接入

- `entry/src/main/cpp/linux_steam/host_v1/` 保存四份实验源码的原样快照，
  `provenance.json` 记录来源及哈希。启用构建无需外部 lab 工作树。
- `surface_probe.cpp` 使用既有 OHOS `libwayland-client.so.0` 及 xdg 协议代码，
  连接当前应用自己的 socket。先空提交并等待 configure/ack，随后轮换两张
  480×270 XRGB8888 SHM 缓冲，严格等待 release/frame callback，最多每秒十帧。
  这个速率只是诊断限速，不是 Steam 或游戏性能结果。
- `host_napi.cpp` 采用异步工作，串行执行探测；registry 最多 1 秒，建窗与输入
  最多 6 秒。JS 定时器只在自己的真实 toplevel 获得键盘焦点时调用既有输入队列，
  注入 W、D、上、下的按下和松开。不会用按键去尝试激活未知窗口。
- 产品的 `service/SmokeRunner.ets` 新增 `linux-steam-host` 内置套件，绕过
  Windows 测试载荷查找，保存详细 JSON 和套件汇总。默认构建返回 UNSUPPORTED。
- 新代码不修改 Wine/FEX 启动、Windows Steam 的 DNS/TLS、游戏配置或购买流程。

构建开关 `WINEHUA_ENABLE_STEAMOHOS_HOST` 默认 **OFF**，只启用候选包；开启时
将模块链接到 `smoke_napi`。无外部 checkout 依赖，无新增正式产品购买或启动入口。

## 实机发现与修正

首次实机已通过 registry、configure 和 55 帧完整 SHM 生命周期，但 keymap 检查
失败，测试器没有发送按键。`input/keymap_xkb.h` 中嵌入的 34205 字节数据以换行
结束；`Seat::seat_get_keyboard` 原先按这个长度发送，没有协议要求的结尾 NUL。

已在发送端分配并映射 34206 字节，明确补 NUL，使用包含 NUL 的长度发送。
键位正文和输入路由不变。实际正文经 Linux libxkbcommon 解析成功；新增不带
NUL 的负例确保诊断器不会把旧格式判为通过。

最终候选的首次运行和关闭后重开分别见 `tablet-result-06.json`、
`tablet-result-08.json`，两次均通过：

| 项目 | 结果 |
| --- | --- |
| registry/sync | 成功，12 个 globals |
| configure | 成功 |
| SHM 提交 / release / callback | 54 / 54 / 54 |
| keymap / keyboard enter | 成功 |
| 键盘事件 | 8，序列完全匹配 |
| display error | 0 |
| GNU 执行 / Steam | 未验证 / 未启动 |

重开还验证出一个测试前提：桌面模式的 `SendKeyEvent` 按当前键盘焦点投递，
传入 toplevel ID 不代表能给未激活窗口发键。早期候选的第二次运行因此失败。
诊断入口已增加焦点核对；必须先点击测试图案激活自己的窗口，不能通过修改
全局输入策略绕开这一点。最终候选未激活的对照 `tablet-result-07.json`
显示 0 个按键、`inputInjected=false`；激活后 `tablet-summary-08.json` 为 PASS。
窗口销毁和重开按独立 runId 留证据。

详细协议结果始终保留 `framePresented=false`。已保存的
`tablet-probe-08.png` 显示棋盘格与原有 Wine 桌面同时可见，
`tablet-after-close.png` 显示图案已移除。这是单独的屏幕证据；不表示已测
显示同步、GPU 零拷贝、鼠标协议回传或客体缓冲桥。

最终完整 ArkTS/原生 HAP 构建通过（`hap-build-focus.log`）；默认 OFF 的
entry/smoke_napi 编译、链接通过，且产物没有探测导出
（`native-default-off-final.log`）。Linux 回归和最终 ASAN/UBSAN 均通过，
FD 数量为 6→6，sanitizer 无诊断。原有 14 项暂存改动的 raw diff 与任务前
逐字节一致。本轮没有提交或推送。

## 编译和复测

本轮在独立源码快照构建完整 HAP，避免修改原树 root 所有的 `.cxx` 缓存。
候选快照仅在 `entry/build-profile.json5` 的 `externalNativeOptions.arguments`
增加：

```text
-DWINEHUA_ENABLE_STEAMOHOS_HOST=ON
```

使用 Harmony Command Line Tools 6.1.1.290 和对应 SDK，构建命令：

```sh
hvigorw --mode module -p product=default -p module=entry@default \
  -p buildMode=debug assembleHap --no-daemon
```

私有签名材料只用于本机候选签名，未加入源码。安装采用替换方式，没有卸载或
清除应用数据。测试套件沿用已有 Want 协议：

```sh
hdc -t <tablet> shell aa start -b com.vintage.pomelopro -a EntryAbility \
  --ps winehua.mode smoke --ps winehua.suite linux-steam-host \
  --ps winehua.run_id <unique-run-id>
```

首次可能等待现有 Wine 引擎准备。为了看到图案并激活窗口，在测试期间通过应用
内的“Wine 桌面”卡片打开桌面，点击左上角测试棋盘格。DesktopAbility 保持原有
非导出配置，不能通过外部 `aa start` 直接启动；未修改这个安全边界。

结果位于应用沙箱：

```text
/data/storage/el2/base/files/.wine/drive_c/smoke/results/<unique-run-id>/
  linux-steam-host.json
  suite-summary.json
```

调试包可用 `hdc -t <tablet> file recv -b com.vintage.pomelopro <remote> <local>`
取回。`shell -b` 的工作目录不是应用的存储命名空间，不应以 `cat` 的权限错误
判断套件未运行。

Linux 回归：`python3 host_tests/linux_steam_host_test.py`。包含 10 个 wire 场景、
3 个 ABI 控制和 6 个真实 libwayland-server 夹具场景：正常、停止释放、缺少 seat、
不给 configure、错误按键、不带 NUL 的 keymap。检查实际 SHM 像素交替和 FD
总数；ASAN/UBSAN 另行检查内存与资源错误。Linux 夹具不替代实机证据。

本机证据目录：
`F:/VintagePomelo-Workspace/artifacts/steam-linux-arm64-host-20261018`。
正式候选以 `package-source-identity.json` 记录源码和包哈希。早期失败日志保留以
解释修正原因；`*.private.*`、设备标识和签名配置不作为公开附件。

## 后续逐步实现的门槛

| 次序 | 最小交付与验收 | 目前状态 |
| --- | --- | --- |
| 1 | OHOS Wayland 客户端建窗、SHM、输入、关闭重开 | 本轮实施；实机详见证据 |
| 2 | 真实 GNU 主程序进入 main/正常退出，TLS、多线程、文件/网络、IPC、受控子进程 | 尚未通过；先确定可扩展的执行后端 |
| 3 | GNU 小程序通过客体 socket/SCM_RIGHTS/mmap 与本宿主往返 | 尚未实现 |
| 4 | rootful Xwayland 和 X11 小程序：显示、输入、尺寸及退出 | 尚未移植；不以迁移整个合成器为前提 |
| 5 | CEF 独立页面的多进程/脚本/交互，再到原版 ARM64 Steam 登录和库 | 尚未开始 |
| 6 | GNU GL/EGL/Vulkan 桥，Steamworks 与现有 Wine/FEX 游戏互操作 | 独立验收，不能从 ARM64 客户端推断成功 |

下一步优先处理第 2 项。实验 G18 的有限解释器仍然是首个构造器条件性进入、
零个返回、414 个未绑定导入；现有 FEX Windows DLL 不是 GNU AArch64 执行器。
应先比较可承载 TLS、系统调用与多进程的完整后端，在小程序上证明成立，再接
Steam。不能通过逐个填假地址、把 OHOS libc 库直接当 GNU 依赖或空壳 GL 库
来制造通过结论。
