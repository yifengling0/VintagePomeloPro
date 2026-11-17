# 2.0.5 未签名测试交付

版本 2.0.5 / 2000050，分支 `feature/gamescope-port`。
产品与打包源码提交 `926e40e9480c9034882727a122eee4b3d1552a3a`。
本轮不合 main、不推送、不上架，不覆盖 2.0.4 正式交付。

## 包与构建

- 文件：`artifacts/VintagePomeloPro-2.0.5/VPP-2.0.5-arm64-unsigned.hap`。
- 大小：630868936 bytes（601.64 MiB）。
- SHA-256：`4f7148a7506a8954c4aebb9fa4b3f15bf0f9225e7a49cdd7d4c890a5d95836e8`。
- ARM64，最低 API 23，`debug=false`；使用既有 Docker `winehua-dev` / `vpp-pal4-release-build` 增量环境。
- 通过 `make gamescope-unsigned-hap` 编译实际 unsigned HAP，不从已签名包移除签名来制作交付。
- SDK `hap-sign-tool verify-app` 明确报告 `signature not found` / `No Hap Signing Block before ZIP Central Directory`，符合未签名交付要求。
- 新 unsigned 路径拒绝绑定签名的产品配置，不加入 `libgamescope_pixel_probe.so`，检查手机声明、包架构、版本及来源收据。

逐成员 SHA-256 对比 Mini 已验证的 `818201af` 调试包：
仅 `module.json`、`pack.info`、`pkgContextInfo.json` 和 `application-build.json` 改变；
移除 `.pages.info` 和测试像素探针。全部共用原生库、Wine/FEX、DXVK、Mesa、Gamescope core
及 `wine-data.zip` 字节相同，UI ABC 也相同。大小变化来自签名与测试探针等打包差异。
首次交付保留两份 2.0.4 调试签名 HAP。随后为 M80 保留数据升级，复用该 unsigned
载荷生成旧证书连续签名的 2.0.5 手机测试包；当前只保留此包及 Mini 已验的 `818201af`
2.0.4 调试包，删除更早的 `6f22d375` 测试回退。正式 unsigned 交付未变。

## 手机支持核查

1. `EntryAbility.onCreate` 先设置手机进程模式，再准备 Gamescope；没有把手机交给系统 NCP。
2. 手机 Game 会话通过同进程嵌入 core 输出；Game 页面和游戏启动不以独立 Gamescope 子进程可用为条件。
3. 手机独立窗口 IPC 输出被明确拒绝；早期 clean fork 不可用或死亡时不回退系统 NCP/晚期 fork。
4. 自动 Direct 能力探测在手机上返回 `phone_requires_venus`，保持现有 Venus 路线。
5. 设置面板按窗口宽度收窄并可滚动，沿用手机旋转与统一输入链路；包中含 `phone` 设备类型及 `READ_PASTEBOARD` 权限声明。

打包时 M80 未在线，仅完成静态及主机验证；随后按用户要求恢复 M80 无线调试，取得以下
独立手机结果。没有操作其他设备代替手机验收。

## M80 无线补测（2026-11-18）

M80 / VYG-AL00 / API 26，从原 2.0.3 调试版覆盖至 2.0.5，未卸载、未清数据、未重置 prefix。
原 HMOS Deck、11 张本地卡片和 PAL4 旧存档保留。两轮启动退出均未发现阻塞。

| 检查 | 结果及边界 |
| --- | --- |
| Wine 已有数据升级 | 组件升级后 `wineboot --init done (authoritative exit)`，`preparing -> ready`；wineboot 约 68 秒，首次启动约 80 秒，没有初始化 Wine 错误；不是全新安装测试 |
| 手机进程/呈现路径 | `setPhoneMode=true`、`NCP backend=fork deviceType=phone`；同进程 VirGL host，`mesa-virpipe`；Gamescope `ncp=0 native=1 cpuGameCopies=0`，没有使用系统 NCP；Direct 探测保持 `phone_requires_venus` 保护 |
| PAL4 实际画面 | 从竖屏 Game 库进入横屏完整客户区，无标题栏、黑边正常；中文主菜单及第二槽存档文字可读，载入青鸾峰场景，地面/植被/人物可见，未观察到黑屏或白地面 |
| 手机侧栏及输入 | 快捷栏打开/收起侧栏、滚动和触摸选项可用；能加载旧存档并点击游戏内退出 |
| FSR 设置 | 切换至 `filter=3` 成功，但本场景 `fsrPasses=0`；只通过设置切换，不能宣称手机 EASU 上采样实效通过 |
| 30 FPS 设置 | HUD 约 28 FPS、日志呈现节拍约 26 FPS，切回跟随屏幕后恢复；确认宿主限帧起作用，不代表生产者精确 30 FPS 验收 |
| 游戏库/重入/退出 | 保留游戏返回库，从运行中恢复同一窗口；侧栏请求正常退出后回库、运行列表清空；第二次启动进入中文主菜单，游戏内“离开游戏”后再次自动回库 |
| 收尾 | 已恢复线性/跟随屏幕，关闭 PAL4 独立画面设置；全局 Game=false，恢复 HMOS Deck 游戏库；没有覆盖存档或修改游戏入口 |

PAL4 场景瞬时 HUD 约 43–46 FPS，主菜单约 98–118 FPS，仅为功能检查中的呈现读数；
没有完成同尺寸普通/Game 模式三轮对照，不给出优化百分比。Heaven 旧卡片尚未设置可用主程序，
自动进入设置，未将此现象算作 Wine 初始化失败，也没有更改该卡片入口。

手机旧安装证书与新剪贴板 Profile 绑定证书不连续。为保留数据，手机调试包使用旧证书/Profile，
仅调整 `module.json`：`debug=true`，去除旧 Profile 未授权的 `READ_PASTEBOARD` 声明；
ABC、Wine/FEX、全部 native 和业务载荷保持最终 unsigned 原字节。其 SHA-256 为
`98f28144731e36ff9bc150e37d0fcc16d6d23cff6b222ee39c8ed28f3b16901d`，633899023 bytes。
签名校验、手机覆盖授权及 JIT ACL 通过；该包不能作为新剪贴板 ACL 的验收证据。
正式交付 unsigned 的剪贴板声明和 hash 保持不变，本轮未实测手机 PasteButton 授权回退。

本次仅验 PAL4/VirGL/Wayland 的有限流程；手机全新初始化、Steam/DX11、DXVK9/Zink、
X11、FSR 上采样实效、生产者限帧、后台恢复、完整旋转/五轮生命周期及实体外设仍待验收。
本地证据见 `workspace_temp/gamescope-port/phone205/device-result.json`、有界 `startup.log`
和命令账本；截图/layout 保持本地，不入 Git。未修改产品代码，不重编最终包。

## 已执行验证

- `test-gamescope-process-backend`：手机无早期服务、服务死亡、平板/手机分流和 FD 所有权通过。
- `test-ncp-dispatch`：不同加载顺序及系统接口缺失场景通过。
- `test-game-presentation`、`test-game-mode-models`：几何、调度、设置继承、会话冲突、输入、剪贴板授权与手机快捷栏命中通过。
- `test-gamescope-recipe`：7 项配方测试通过；`test-gamescope-output`：输出、resize、回退及终止失败场景通过。
- `test-windows-steam`：参数、启动就绪、目录、派发所有权与恢复场景通过。
- Hvigor release HAP 构建通过；最终包 `runtime_library_guard`、`v2_product_contract`、Windows Steam helper、X11 闭包（40 ELF）、`verify_ci_release artifact` 的当前来源/运行时锁检查通过。
- 最终包无重复成员、签名材料或测试像素探针；版本/ABI/API/剪贴板声明通过。

旧 `check_runtime_components.py` 仍失败：其 `runtime-core.lock.json` 以 **1.4.9** 为基线，
首先拒绝现有 `vkd3d/limited-500k/x64/d3d12.dll`。本轮未放宽旧锁，也未替换运行时；
当前分支的完整 Wine/FEX/图形载荷锁与来源校验通过，且完整载荷与 Mini 已验证包字节相同。
此项保留为旧校验入口与当前所选运行时的基线不一致，不能将本轮称为全仓预检全部通过。

本轮补充了上述手机功能检查，没有正式设备性能对照，也没有完成全部手机/图形矩阵，继续保留 `renderQualified=false`。
详细逐成员证据及旧校验结果见交付目录 `delivery.json`，工作记录见 `workspace_temp/gamescope-port/unsigned-delivery.json`。
