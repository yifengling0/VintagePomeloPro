# Mini Gamescope 与剪贴板实测

2026-11-17，在 Mini / MLR-AL10 / Maleoon 910 上覆盖安装并测试。用户反馈：
“这个性能是可以的，和之前差不多。”本轮按此反馈结束追加跑分，记录为体验可接受；
不把低电量短测推算成优化百分比，也不据此认定较普通模式存在性能回退。

## 候选与安装

- 分支 `feature/gamescope-port`，安装包源码提交 `6f22d37592d678d8ee286dc7b9e73d9df8ea8244`。
- 固定任务目录 `workspace_temp/gamescope-port/`，当前包
  `VPP-2.0.4-gamescope-debug-signed.hap`，633,882,577 字节。
- SHA-256：`ea7313bc5ed55052cebe24a0f50e308817d80595887716ecc4e67d2eee671290`。
- 新 Profile 已实际覆盖安装成功，UID、首次安装时间和已有程序保留；没有卸载或清数据。
  签名不再是阻塞。正式 2.0.4 包未替换，不操作同时在线的 Edge。
- 回退包仍引用 `042600eb` 的客户区候选；当前包与回退包各一份。本轮未重编二进制。

## 已通过的设备行为

| 项目 | 实际判据 |
| --- | --- |
| 旧 Wine 环境 | 记事本与 PAL4 正常启动，已有 prefix 可用 |
| 剪贴板权限 | Entry 前台出现系统授权对话框，用途说明正确，点击“始终允许” |
| 安全粘贴 | 持续授权前，触摸 PasteButton 已能将系统文本送进 Wine |
| 双向中文文本 | 系统中文文本送入 Wine；在 Wine 内用真实按键追加 `wine`，触摸复制后在原生 ArkUI 搜索框粘贴得到 `旧柚双向剪贴板-test-1117wine` |
| Ctrl+V 路径 | 授权后以 HDC keyEvent 发送 Ctrl+V，可将中文送入 Wine；不算实体键盘验收 |
| 客户区与输入 | Game 下无标题栏、800×600 的 4:3 内容等比放大，两侧黑边；菜单、存档槽和读取确认坐标正确 |
| 画面 | PAL4 菜单、存档页中文正常；VirGL 存档 2“青鸾峰”地面正常，本轮截图未见此前明显色偏 |
| 原生 GPU 路线 | 日志 `native GPU compositor active ncp=0 cpuGameCopies=0`；FSR 时 `native=1 filter=3 fsrPasses=1`，线性时 `native=1 filter=0 fsrPasses=0` |
| 默认节拍 | Game 菜单 FSR 约 104 FPS、线性截图 117 FPS，已不受旧 60 FPS 软件上限约束 |
| 会话行为 | 旧会话设置冲突时正确提示先退出；保留会话回库、从运行中恢复、快捷面板正常退出后回库成功；线性候选第二次启动及正常退出成功 |

剪贴板回传避免了自动化假阳性：`uitest uiInput text` 本身会改系统剪贴板，
所以以 Wine 中真实按键追加后的内容作为反向复制判据。记事本已关闭且未保存测试文件。
授权前的 PasteButton 测试不是“明确拒绝持续权限”的设备用例，不能混为一项。

颜色仅为当前场景目视结果，没有普通 Wayland 同场景定量颜色对照，不能扩大成全部
驱动/后端的色彩验收。原生输出的 `storageOutput=1 gpuOutputCopies=0` 写向 stderr，
本轮没有取到该标记，故不声称交换链直写已由设备日志独立确认。

## 短测数据与解释

同一 PAL4、VirGL、FEX、800×600、存档 2 屋前位置；Mini 输出截图 2560×1600，
4:3 游戏区域约 2133×1600。FSR 锐化 10，帧率“跟随屏幕”。

| 模式 | 观察到的存档场景 FPS | GPU 时间戳诊断 |
| --- | --- | --- |
| Game / FSR 1.0 | 约 40～54 | EASU/pre-pass 约 6.0～6.5 ms，RCAS 约 4.8～5.2 ms，二者约 10.8～11.6 ms；另有约 0.3～0.4 ms 合成 |
| Game / 线性 | 约 40～50，截图 47 | 线性合成约 4.0～4.3 ms |

时间戳是既有 GPU 阶段诊断，不是完整帧时间或 GPU 利用率；各窗口不是同步交错测试。
没有 CPU/GPU 占用、功耗或频率数据。全程电量约 0～1%、处于充电状态，末次仍为 0%，
不满足正式三次预热后 60 秒性能比较条件。因此不报告中位数收益、1% low 或回退百分比。

FSR 在当前高输出像素负载下有明显处理成本，但线性短测的场景 FPS 也接近，不能仅凭
这组数据把瓶颈全部归因 FSR、CPU 或 GPU。普通 Wayland 仅打开到窗口化菜单，随后用户
表示当前性能可接受，未继续加载同场景；800×600 窗口也不等于 Game 的全屏输出负载，
故不纳入性能对照。用户的体验反馈与上述受限采样分别保留。

## 收尾与未验收范围

PAL4 恢复原设置：VirGL、Game 跟随当前模式、独立设置开启、FSR 1.0、锐化 10、
跟随屏幕。原主程序和存档未修改。普通模式对照已正常关闭，空 Wine 会话通过产品
“停止全部 Windows 会话”结束，运行中页面无窗口；已发送 `winehua.frameDiagnostics=0`
关闭临时诊断。设备保留在 Game 页面，不继续跑游戏负载。

以下仍独立待验收：手机、X11 Game 最终裁切与输入、Steam 派发与退出返回、Heaven、
DX11、Direct/Venus/Zink 组合、显式 30/60 档、五轮 GPU 游戏生命周期、三轮 resize、
后台/异常恢复与资源增长、第三方应用/空文本/拒绝授权剪贴板。历史 SHM 五轮结果
仍只证明 SHM 测试源。PAL4 已有 Zink 白地面不在本轮修复或通过范围。

## 证据索引

本地固定任务目录的 `device/` 保存 `install-verification.json`、
`clipboard-permission.jpeg`、`clipboard-chinese-roundtrip.jpeg`、
`pal4-current-fsr-{menu,scene}.jpeg`、`pal4-current-linear-{menu,scene}.jpeg`、
`pal4-fsr-sample.log`、`pal4-linear-sample.log`、`pal4-restored-settings.jpeg` 和
有界 `commands.jsonl`。机器可读结果在任务目录 `device-evaluation.json`。
不将含系统通知的临时普通模式截图加入报告。

收尾执行工作区维护，零问题。旧 FSR 原始 PPM 与既有无损 PNG 逐像素比较一致后，
保留 PNG 与散列收据，删除重复 PPM 和相同 readback 临时日志，回收 23,202,191 字节
（约 22.1 MiB）。只保留当前和上一份调试 HAP，没有新建包或复制运行时。
