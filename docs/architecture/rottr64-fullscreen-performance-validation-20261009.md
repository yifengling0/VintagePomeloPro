# ROTTR PE64 全屏与性能核验（2026-10-09）

当前平板已按要求改为 **1280×800 渲染和桌面输出、全屏、30 FPS 上限、垂直同步**。从同一“山顶”存档加载到雪山挂壁场景，画面覆盖屏幕，未见先前的桌面蓝底、右侧明显裁切或拉伸比例异常。35 秒窗口采到 34 条宿主新图像 FPS 日志，平均 **28.18 FPS**，范围 **27.13–29.00**；未检出采样脚本匹配的合成失败。30 是上限，本轮没有稳定达到 30，也不能代表移动、战斗或长时间运行。

## 环境与当前设置

- 同一 MatePad Mini、Maleoon 910；安装版本沿用 1.4.5.29 / 1004038。
- 仓库 feature/main_proton，基础 HEAD 4cb9e7c8，包含前序未提交改动。本轮未改生产源码、未重新构建或安装 HAP、未提交或推送。
- 安装包沿用 venus-lowmap-signed-20261009.hap，SHA256：15efc9dc2738930816335a24dc14378a7e232c01752404bd0c4ad7867c523b17；runtime content bee36a048201da89。
- Z:\games\GUMU10\ROTTR.exe -dx11，PE64 / FEX / DXVK 1.10.3，当前会话为 Direct precise-map。计时与 map 诊断关闭。
- 继续加载 2026/10/8 21:20、山顶、0% 的同一存档；加载后不发送移动输入，未新建或主动覆盖存档。
- 游戏注册表 Fullscreen=1、ExclusiveFullscreen=1，Fullscreen/Window/UserWindow 尺寸1280×800；VSync=1、RefreshRate=60、TripleBuffering=0、EnableDX12=0。
- 保留低画质：关闭 AO、阴影、景深、运动模糊、SSR、TressFX、曲面细分及抗锯齿等；LOD、纹理质量和过滤为0。HighPrecisionRT=1保留。
- 游戏目录持久化 dxvk.conf 自动读入：d3d11.disableMsaa=True、dxgi.maxFrameRate=30、dxgi.syncInterval=2。当前启动未传 DXVK_CONFIG_FILE。配置中原“720p”注释不控制分辨率。

## 全屏核验

此前应用桌面设置为“切边（安全区）”，2560×1600屏幕左右各80物理像素被扣除，默认2倍缩放后 Wine 桌面只有1200×800。1280宽的游戏会受到这个桌面边界影响，720p测试时还记录到游戏 fullscreen=0，截图可见下方蓝底。

本次将应用设置切到“全屏（沉浸）”，完整停止 Wine/FEX 后重新启动。当前窗口日志同时确认：

```text
desktopRoot=1 geometry=1280x800+0,0 fullscreen=1
desktopRoot=0 geometry=1280x800+0,0 visible=1 fullscreen=1
Found config file: dxvk.conf
dxgi.syncInterval = 2
dxgi.maxFrameRate = 30
Present mode: VK_PRESENT_MODE_FIFO_KHR
Buffer size: 1280x800
```

1280×800与平板2560×1600同为16:10，按2倍放大铺屏。末尾日志中早期MAILBOX行属于交换链初始化历史，随后实际游戏交换链为FIFO，不能把旧行当作当前呈现模式。

## 本轮测量及解释边界

| 设置与阶段 | 采样时间 | FPS结果 |
| --- | --- | --- |
| Venus lowmap OFF，原画质800×600 | 10:17:03–10:17:38 | 显示8.023 / 源提交7.991 |
| Venus lowmap ON，原画质800×600 | 10:27:16–10:27:51 | 显示9.038 / 源提交9.056 |
| Direct whole-map，低画质1280×720，安全区桌面 | 10:54:57–10:55:32 | 平均21.029，18.97–22.07，33样本 |
| Direct precise-map，低画质1280×720，安全区桌面 | 11:15:12–11:15:47 | 平均20.544，20.00–20.99，33样本 |
| Direct precise-map，低画质1280×800，全屏桌面 | 11:40:32–11:41:07 | 平均28.179，27.13–29.00，34样本 |

各窗口为35秒墙钟采集。Venus两轮各仅2条周期日志，计数差覆盖15.0/13.3秒左右，不以35秒整段估算帧率；Direct统计1秒root新图像FPS的算术平均，没有逐帧P95或GPU timestamp。Direct窗口 gameCpuReadBytes/gameCpuUploadBytes 均为0，只表示这些呈现统计未记录游戏CPU回读/上传，不能称整个游戏零复制。

测试是顺序实验，没有锁定CPU/GPU频率；800×600原画质与后续低画质Direct还同时改变了多个因素，不能计算为单项优化收益。720p两轮使用相似热态，whole/precise结果没有确认PE64额外收益。1280×800这一轮同时改了桌面输出模式、游戏尺寸并重启，**不能将20.5→28.2归因于分辨率提升或某一项代码修复**。值得后续固定1280×800，仅对桌面合成/全屏状态做单变量实验。

最终窗口shell_front43.221→43.716°C、system_h48.000→48.533°C、电池38°C。保留传感器原名；ambient读数不能解释为实际室温。

## PE64与此前32位复制优化的关系

当前构建Wine源码 dlls/win32u/vulkan.c:1764 的shadow映射分支在 !WowTebOffset 时直接返回；1999处同样要求WOW64。0040共享低地址allocator也首先拒绝 !is_wow64()。此前PE64 Venus探针pointer_bits=64、五个64MiB BO均高地址/wine_owned=0，缓冲和纹理回读均bad=0、PASS，说明该游戏不走PE32低地址shadow复制路径。

因此此前PE32 Tomb Raider OFF→ON→OFF→ON约18.52→37.59→17.75→37.53的收益不能直接推广到64位ROTTR。本轮PE64 whole/precise和Venus lowmap结果也未证明相同收益。画质降低可能影响当前表现，但没有完成逐项画质A/B。

曾一起更改ExclusiveFullscreen=0、HighPrecisionRT=0时菜单出现黑底/红蓝碎块，恢复两项后正常；未单独隔离，不能断言是哪一项触发。当前保留两项为1。

## 证据与结束状态

关键摘要、时间窗口日志、启动条件、仅游戏图形设置的差异、当前DXVK/窗口状态及最终无通知截图保存在 [evidence/rottr64-fullscreen-20261009/](evidence/rottr64-fullscreen-20261009/)，有SHA256SUMS。完整注册表、bundle dump、HAP和带私人通知的早期截图只留本地，不放该目录。

原注册表备份rottr64-user-reg-before-720p-20261009.reg及游戏目录dxvk.conf.before-vp-720p-20261009保留。当前游戏会话继续运行供用户测试，应用全屏桌面偏好及游戏1280×800/低画质/30上限配置持久化；Direct precise-map仍属于当前启动会话条件，不把它描述成这轮新改的产品默认。
