# Tomb Raider PE32：Direct 流畅现场与 Venus 白屏状态

日期：2026-10-08。权威仓库 `/home/liufeng/src/vpp-proton`，
分支 `feature/main_proton`，HEAD `4cb9e7c8be2acf7545976f0699ef064e796cb2fd`，
包含尚未提交的前序 Direct 和 Wine 适配修改。

本报告保留早先的实验现场。后续 Venus 泛白已在平板修复，普通产品启动的
Direct 自动检测也已通过；最新状态见
[Venus 合成修复与自动 Direct 验证](venus-opacity-and-direct-auto-validation-20261008.md)。

## 当前确认的运行状态

用户在 21:45 左右反馈当前帧率很好、接近历史 Box 版本。
随即保留原游戏和应用进程，采集截图、帧率、温度、进程列表和 DXVK 日志，
没有切换后端、重启游戏、重新安装或修改配置。

| 项目 | 本轮证据 |
| --- | --- |
| 设备 | MatePad Mini，Maleoon 910 |
| 应用 | com.vintage.pomelopro，1.4.5.29 / 1004038 |
| 游戏 | Z:\games\Tomb Raider\TombRaider.exe，PE32 |
| CPU 转译 | FEX，stderr 中有 starting FEX based libwow64fex.dll |
| D3D 后端 | dxvk_legacy，配套 legacy DXVK 1.10.3 |
| 启动实验 | isolate-vulkan-direct-precise-map |
| 实际 Vulkan 设备 | Maleoon 910，Driver version 292.366.2691 |
| Swapchain 尺寸 | 800×600 |
| 最后记录的呈现模式 | VK_PRESENT_MODE_MAILBOX_KHR，4 张图像 |
| 测量窗口 | 21:45:23.596–21:45:48.597，25 秒 |
| 宿主新图像统计 | 24 个采样点，平均 37.974 FPS，最低 31.84，最高 44.00 |

该段截图是实际游戏动态画面，末尾 HUD 显示 39 FPS。
宿主日志 direct=1，acquire/release 持续增长，releaseErrors=0。
帧率是宿主发布新游戏图像的每秒统计，不能代替 GPU 硬件利用率或 GPU 时间。
gameCpuReadBytes=0/gameCpuUploadBytes=0 只覆盖展示路径，不能证明所有资源零复制。

已安装包：rottr-server-o2-v2-signed.hap。
SHA256：7f43e6e30e75fe4d7fea495a5b370eb61be447e5d32fd02ea95d6032525b1c64。

## 这套配置做了什么

生产配置解析器对该实验配对设置：

```text
WINEHUA_VULKAN_BACKEND=direct
WINEHUA_VK_PRECISE_MAP=1
DXVK_WINEHUA_PRECISE_SHADOW=1
DXVK_WINEHUA_FLUSH_DYNAMIC_MAPPED=1
```

Wine WOW64 桥仍在使用；32 位 x86 指令继续由 FEX 执行，没有改成 WowBox64。
ARM64 Vulkan 驱动映射若不能共享到低地址，仍保留正确的 shadow 回退。
DXVK 显式 flush/invalidate 范围负责上传与回读；实验取消 QueueSubmit/Unmap
额外的整块 shadow 上传。这不是普遍适用于所有 Vulkan coherent 内存的零复制方案。

普通产品入口仍走 Venus，且显式恢复 WINEHUA_VK_PRECISE_MAP=0。
因此本次流畅配置还不是普通游戏入口的持久默认配置。
PAL4/RichMan8 的 WineD3D/VirGL 路径、FEX/x87 设置没有因此改变。

## 性能结论的范围

本轮证明该 PE32 游戏在 FEX + Direct precise-map 下可以达到约 38 FPS，
与用户报告的历史 Box 帧率量级接近。这不支持笼统认为 FEX 必然只有 Box 一半性能。
此前 whole-map 的 7.8 FPS 窗口采在开场动画，本轮截图是后续游戏画面；
场景、缓存和交互不相同，不能把两者相除宣称优化倍率。

同源合成资源探针曾验证 whole-map/precise-map 均无数据差异，并观察到 precise
提交耗时减少；详见 direct32-vulkan-map-fix-20261008.md。
真实游戏还需在同一场景完整重启，对照 whole/precise，记录资源复制字节与时间，
再判断额外整块复制在游戏中的具体贡献。
64 位 ROTTR 的约 15 FPS 结果是另一款游戏和另一套资源负载，不能混为本次对照。

## 手机依赖的 Venus 问题尚未修复（本报告采集时的历史状态）

后续已经定位并修复 Venus 的 OPAQUE 合同被当成透明图层合成的问题，
且平板的明确 Venus 会话已正常显示 TR32 片头和主菜单。
普通产品 Direct 自动检测也已通过并进入实际游戏。
详见 [后续修复与真机验证](venus-opacity-and-direct-auto-validation-20261008.md)。
手机本身仍未做本次真机验证。下文保留此前现场记录，不代表最新状态。

本轮普通 Venus 启动和显式 Venus 对照均可显示启动器，开始游戏后白屏。
对照 DXVK 设备是 Virtio-GPU Venus (Maleoon 910)，swapchain 为 800×600。
宿主在 ctx=5、queue_id=161、image_id=265、serial=1 连续返回 -EAGAIN。
切回 Direct 可出画面，但只绕过 Venus 故障，不解决手机路径。

现有 -EAGAIN 日志不能区分对象未找到、类型不匹配、context/object/queue 锁忙，
也不能只凭 guest 的 object publication pending 文案就断言对象丢失。
vkr_renderer.c 已有 stage 诊断；后续应单独启用 trace-present-image 复现，
按 context/queue/image 身份核对宿主对象创建和首帧 lookup，再修确切故障分支。
该诊断 profile 会改变部分宿主上传选择，不能用其 FPS 当产品性能基线。
本轮为了保留用户正在测试的流畅现场，没有再启动 Venus 对照。

## 复测与证据

外部证据目录：F:/VintagePomelo-Workspace/workspace_temp/gumu10-dx11-20261008/。

- tr32-direct-precise-conditions.json、tr32-direct-precise-route.txt：启动实验及历史 EntryAbility 记录。
- tr32-precise-user-good-summary.json、tr32-precise-user-good-fps-window.txt：本轮时间窗及帧统计。
- tr32-precise-user-good-before.png、tr32-precise-user-good-after.png：实际游戏画面。
- tr32-precise-user-good-d3d11.log：本轮取回的设备、分辨率与 swapchain 配置。
- tr32-precise-user-good-live-proof.txt：安装版本和 FEX 标记；其中初次猜测的 prefix 路径不存在，随后改用既有 .wine 路径取回 DXVK 日志。
- tr32-precise-user-good-thermal-before.txt、tr32-precise-user-good-thermal-after.txt：温度现场。
- tr32-venus-host.log、tr32-venus-control-d3d11.log：Venus 对照。
- device-commands.json：有明确设备、超时和脱敏记录的命令清单。

已有 launch_tr32.py 可复现当前启动参数（会 force-stop 当前会话，不应在用户继续游玩时运行）：

```powershell
& 'C:\Users\liufeng\.cache\codex-runtimes\codex-primary-runtime\dependencies\python\python.exe' `
  'F:\VintagePomelo-Workspace\workspace_temp\gumu10-dx11-20261008\launch_tr32.py' `
  tr32-precise-repeat precise
```

本轮仅记录当前状态，没有新增运行时修复、编译 HAP、改变默认路线或 commit/push。
