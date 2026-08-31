# WineHua 公共回归入口

使用 PowerShell 7 (`pwsh`) 在仓库根目录运行（系统 `bm` JSON 含大小写不同的键，
需保留键名的 Hashtable 解析）。只读预检不会连接设备、安装或启动应用：

```powershell
.\automation\Invoke-WineHuaAutomation.ps1 -SkipBuild -SkipInstall -PreflightOnly `
  -HapPath '<已验证的 ARM64 debug HAP 绝对路径>' `
  -ExpectedHapSha256 '<该文件已记录的 SHA-256>'
```

移除 `-PreflightOnly`，添加 `-Suite core` 即测试当前已安装包。它会退出当前
游戏，通过正常 `game` 入口逐个运行 x64/x86 音频与 OpenGL probe，结束后停止测试应用。
`-Suite opengl` 只测两种 Guest 位数；`-Prefix reuse` 为默认值，保留用户 Wine 前缀。

`-SkipInstall` 不验证安装二进制与参考 HAP 相同，只核对产品版本。报告明确写为
`reused-installed-package-unverified-reference`；设备上手工覆盖的 Wine/Mesa 库也
不会因 HAP 哈希相同而变成已验证。使用安装日志及运行库哈希记录补足溯源。
不要把这种功能复测标成新构建或候选包验收。

需要覆盖安装指定已验证包时，保留 `-SkipBuild`，移除 `-SkipInstall`。
`-SkipBuild` 现在必须同时指定 `-HapPath` 和 `-ExpectedHapSha256`，不再隐式读取
可能过期的 signed 输出。参考包检查包括整包哈希、产品身份/API、全部关键 Host
ELF 和内嵌 runtime 哈希；签名与 Guest 嵌套载荷完整性须已在该哈希的原始验收中通过。

确需增量构建时显式选择现有 ext4 源码和容器，例如当前机器：

```powershell
.\automation\Invoke-WineHuaAutomation.ps1 -Suite core `
  -RepoWsl /home/maple/vp-src -Container vp-build -WslDistro Ubuntu
```

该模式只执行现有容器内的 `make hap NATIVE_ARCH=arm64-v8a`，从不创建镜像、容器、
源码副本或清理缓存；源码绑定必须与参数一致。SDK 可在既有镜像内，也可只读绑定。
先自行同步本次相关源码到该构建树，不能把其 Git HEAD 当作 Windows 工作区的提交。
`-PreflightOnly` 在构建模式只检查现有容器绑定，不构建或安装。

包名读取当前 `AppScope/app.json5`；默认记录位置为 `.hvigor/outputs/automation`。
多设备必须显式指定 `-DeviceId`，不再猜选第一台；记录不包含设备唯一标识，hilog
过滤其他应用和序列化启动环境。`batchMappedFlush` 使用产品开启策略，拒绝 off。
主机汇总中的 `batchMappedFlush: null` 表示没有覆盖，而非关闭。

产品 PowerShell 入口继续通过正常游戏会话执行 `core/audio/opengl` 回归。与此同时，
WineHua master 的数据驱动协议已作为后台服务接入：`automation/run_regression.py`
发送 `winehua.mode=smoke` Want，设备端 `SmokeRunner.ets` 读取随包的
`C:\smoke\suites.json` 并写 `suite-summary.json`。它复用产品 Native 图形策略和
WineEngine 生命周期，不替换产品页面或侧边栏。`reuse` 使用正常前缀；`clean`
使用隔离的 `.wine-smoke`，避免清空用户前缀。两套 host 入口可以并存，结果协议同源。
核心 probe 通过仍不等于视频、五次生命周期或十分钟稳定性全部验收。

主机测试（无需设备、Docker 或 HAP）：

```powershell
.\automation\Test-AutomationPreflight.ps1
.\automation\Test-GlTiming.ps1
.\automation\Test-GraphicsTestPolicy.ps1
.\automation\Test-NormalSmoke.ps1
.\automation\Test-SmokeVisual.ps1
.\automation\Test-FrameOrderGate.ps1
```

DXVK 性能测量的 `-ConditionSet` 仅支持 `product`（两代交替）、`legacy`、
`modern`。游戏启动与帧序工具仅支持 `-BatchMappedFlushMode product/on`；
旧 off 参数、off 条件组及 `DXVK_WINEHUA_BATCH_MAPPED_FLUSH=0` 临时注入均在
访问设备之前拒绝。历史开关对照只留文档，不再提供可误用的执行入口。

## 本轮验证状态（2026-08-31）

预检单测、HDC 零退出码失败模拟、GL 计时解析测试通过；真实 1.3.2 ARM64
基线的哈希/身份/ABI 只读预检及现有 `vp-build` 绑定检查通过。测试工具迁移
本身没有触发 HAP 重建或安装；后续因发现 Native 后台错误忙循环，单独增量
构建并安装了[限速候选](../docs/graphics/gl-background-backoff.md)。

重连后确认：18:31/18:52 的旧请求虽被系统接受，但应用忽略 `mode=smoke`，
probe 并未启动；此前将其仅归因断连的判断不完整。改为正常 `game` 入口后，
18:57 四个 probe 完成，旧判图把蓝色桌面也计入 GL 蓝色块，导致视觉误报。
失败原件保留；修正饱和蓝阈值并补背景/旋转/镜像/缺色单测后，18:59 完整
重跑 `phase2-20260831-185944` 的四项功能及两张视觉图均通过。

但日志复核发现该次 x64 GL 在 19:00:48 出现一次 `0x505` blit drop，随后恢复；
原报告 PASS 仅代表旧门禁的功能检查，**不能作为无丢帧稳定性验收**。
现按每个启动 PID 隔离日志，新增 blit drop、非零 failed_swaps、fatal signal
门禁，避免既混入旧会话错误、又漏过本次错误。未知的 GPU/CPU-copy 指标仍为
未知，不补零，不把模拟输入/音频 drain 当成真人听感或全套生命周期验证。

同名 Wine/Appspawn 子进程不能靠 `pidof` 的数量判定重启。公共入口按
`PID,PPID,NAME` 进程树选唯一主 PID，主机测试覆盖同名子进程与多根歧义；
PowerShell 函数向 `ps -o` 转发字段时使用单个带引号字符串。
