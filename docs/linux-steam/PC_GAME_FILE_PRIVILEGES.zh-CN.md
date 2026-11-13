# 鸿蒙 PC 欧卡2：运行时权限与 Steam API 初始化

## 已验证的启动状态

输入 logs-20261024-114008.zip，SHA-256 cbd79c0cf34366f566423ec7ffa8e4f6d9100dd74d68fe779ad65e5b78ca5ed9。用户确认上一版修正后 PC 已可进入 Steam；session=running、supervisor ready=true。GNU release 8d93f154ec726872518731409615e42ffca9fc643c5e729c038316b8c5306368。不能继续按 CEF 黑窗排查。

## 两个错误分别记录

1. 第一张远端截图：《Euro Truck Simulator 2》（AppID 227300），Missing file privileges，路径结尾 runtime/amd64/etc。完整路径未全部显示，不能补写中间目录。下载列表可见 Steam Linux Runtime scout/sniper。该次导出只记录到 d3ddriverquery64.exe 探测，没有实际游戏 EXE 请求。
2. 用户确认已强制选择 HmosDeck (OHOS Wine)。随后新截图中，游戏自己的 Windows 对话框报告 Euro Truck Simulator 2 can not access Steam client，背景已出现游戏启动图。当前故障已到游戏自身的 Steam API 初始化阶段，不能再把“未选 HmosDeck”作为既定原因，也不能据新截图声称第一次 Runtime 安装已全部正常。

第一张图 SHA-256：645297156db000d1d2b21c6c62d72e6bcd2d6094affbbb20284f27af5d77942e。

## 源码核对

- Valve steam-runtime/build-runtime.py 的 873–935 行合并架构目录，并用符号链接替代 amd64/etc 等公共目录。当前 GNU filesystem_compat.c 只对限定的 CEF/Cloud IPC 链接提供适配；普通 symlink 保留真实系统结果。OHOS 沙箱限制链接创建，和第一次路径相符，但尚缺该次系统调用返回值，不能定案。
- 全局默认兼容工具不能证明每款原生 Linux 游戏都已选择 Windows 路径；本轮测试者已明确选择 HmosDeck，该项转为已核实前提。
- Steam API bridge 当前生成 SDK 1.50 及少量历史 ABI；Factory 支持 SteamClient014/017/020。若游戏请求其他版本，当前桥会返回未支持，而不会伪造初始化成功。新截图是通用错误，尚不能指认具体接口缺失。
- GNU 服务连接官方客户端后，还会检验真实客户端映像与实际 TCP 连接来源。PC 若在这一步失败，状态为 SourceUnverified，也不能直接当作缺少接口或跳过校验。
- 需要区分：PE 官方兼容模块加载失败、接口版本/方法未支持、Linux 客户端实际未连接、来源校验失败。此截图不足以区分四者。

上游参考：https://github.com/ValveSoftware/steam-runtime/blob/master/build-runtime.py#L873-L935

## 本轮交付与复测

诊断 HAP 保留上轮 PC 启动修正，版本 1.5.1 / 1005010，Release / arm64，无签名。本轮没有修改游戏文件、Wine、DXVK、GNU 链接语义或兼容工具选择；不标记为权限/欧卡认证已修复。

导出增加：

- 官方 content/gameprocess/compat/installscript 固定日志，每文件尾部最多 64 KiB。
- 原有 Steam API Windows 客户端和 GNU 服务端状态文件，各最多 4 KiB，包含接口名/阶段/状态及来源校验结果。
- 最新游戏/辅助程序独立记录、时间戳及现有游戏库摘要。
- 只读解析最多 1 MiB 的 config.vdf，仅导出最多 128 条数字 AppID、工具名和优先级；不导出完整配置、账号、Cookie 或票据。
- 桥凭据、nonce、命令行密码额外脱敏。读取/导出失败不控制游戏生命周期。

请测试者按原签名流程覆盖安装诊断 HAP，保留已选择的 HmosDeck 与现有库，启动欧卡2一次，出现错误后立即在应用中导出新日志。无需在开发者本机安装此游戏；不清空组件、目录、游戏或存档。

## 验证范围

现有导出与游戏启动行为回归通过，覆盖权限错误独立导出、短读/截断/上限、工具配置摘要、敏感值、链接拒绝、真实接口状态、游戏和辅助程序隔离及诊断失败不干预游戏。Release 编译、包完整性、Wine 闭包和无签名检查见 package-verification.json。

尚未取得含本次游戏 Steam API 状态的新日志，开发者本机没有该游戏，也未操作平板/手机。需要远端 PC 复测后再确定兼容补丁。

第二张图 SHA-256：07480baa4f0857e0ec3d65596a58b5d4328a7d4d863baad28023a01fb5b17726。
