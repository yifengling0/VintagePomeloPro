# HSL 同步与运行路线实施记录

基线：VPP `03bbf50f`，继续在 `main_proton_steamarm64` 推进；参考 HSL 本地
`feature/mini-dpi-native-stardew@b001b25`。保留既有修复与 `rc-1.5.7` 历史标签。

## Steam 选择与迁移

- 不再因 GNU 组件就绪设置 GNU 默认或覆盖 Linux runtime；Windows 默认推荐
  `hmosdeck_proton`（OHOS Wine），保留内置工具 ID、路径、Cloud 识别。
- `linux-native-ohos-v2` 一次性迁移全局及所有数字账户 CompatToolMapping，清除
  四个 VPP 内置工具的逐游戏固定设置；保留第三方、Steam runtime/FEX、启动参数。
- Steam 完全退出后先解析全部配置、备份原配置，再 fsync/rename 发布，最后写迁移
  版本。中断可重试，备份失败停止；完成后保留用户后续选择，不搬存档、不合前缀。
- OHOS Wine 确认失败后提供手动 GNU 重试引导，打开 Steam 游戏属性。未知退出状态
  不提供重试；不复用旧 ticket，不自动改后端，游戏版本/Cloud 仍交给 Steam。

## 会话与窗口

三条路线共用 HostGameSession，记录 AppID、backend、request/generation、进程身份、
窗口 incarnation、phase 与真实退出。启动尚无窗口时也有 HmosDeck 运行卡片，可取消。
Windows 保留 HMOSDECK-LAUNCH/1，Linux 使用 HMOSDECK-NATIVE/1。控制消息不接受 PID；
wrapper 只控制自己创建的进程组，SIGTERM 后超时再 SIGKILL，真实 waitpid 结果回报。

broker 使用随机 nonce、SO_PEERCRED、祖先及 PID starttime；GNU worker 通过既有 NCP
报告真实 child PID。窗口只按此身份关联，取消“后来出现的未归属窗口都是游戏”的推断。
窗口消失不等于退出；未知退出保持未知。rootful Xwayland 无可靠游戏 PID 时仍保留
可控的无窗口会话，不猜测共享 Steam 表面的游戏归属。

LinuxSteamNativeWindowHost 复用既有 Wayland/NativeBuffer presenter、surface 代次和
FD/fence 生命周期；external toplevel 使用 isolated renderer。Steam 启动不先启动
Wine desktop；首次 OHOS Windows 游戏请求才准备 Wine，保留已存在 Linux 会话及窗口。
ARM64 ELF 在 FEX 检查之前直接分流，x86/x64 继续使用匹配 runtime/FEX/GL/Vulkan。
ARM64 shebang 脚本不从扩展名推断架构；用户仍须选有匹配解释器的实际 Linux 版本。

## HSL 公共修复

| 项目 | 本次处理 |
| --- | --- |
| NSI/锁、ICU/Common Controls、Mono | 保留 03bbf50f 及前置修复，最终 GNU 清单继续校验 |
| metadata worker | 信号屏蔽、wake/join、fork 关闭继承 FD、最后 guest 仅回收本进程自有 worker |
| 合作式查询 | 认证 V5 自有真实 stat 快照供 broker 使用；V3 枚举真实 TCP FD/PID/UID/inode，lsof 保留原筛选与输出；不伪造 /proc 权限 |
| FEX | 从固定 Valve 2607 成组回移信号、time64、i386 GLX 指针/opaque handle、guest copy/进程操作边界；GL guest32/64 与 host32/64 成组校验 |
| 冷前缀 | 文件、wineboot、locale、media、registry 分阶段；失败不进入游戏，绑定组件组身份和前缀注册表缓存 |
| 升级与 DXVK | kernel flock 跨进程维护租约，ArkTS 到 NAPI/NCP 保留同一 OFD，整组就绪 marker 最后发布；失败不允许混用组件启动 |
| VPP 显存释放 | 保留原实现与既有图形回归，不引入实验 SMC、syscall 捕获或 CPU 读回 |

只替换 libGL 的四个配对 thunk；其他 thunk 保留 Valve depot 原配版本。
合作查询只向同 UID 认证进程暴露其自有数据；真实错误保留。
不并入 HSL 桌面软件中心、AI 软件与实验捕获后端。

## 构建与验证状态

统一入口见 `GAME_COMPONENTS_BUILD.zh-CN.md`。GNU 不可变基础 ZIP 不重新标记；
HAP companion 带源码、补丁、工具链和 ABI 身份。普通本地 assemble 和 CI 共用校验。
两次完整 FEX/GL 重编的六项 hash 一致；旧 shim 原始/转换输入均做负向回归。

当前已完成主机回归：选择/旧配置迁移、统一会话及取消、native broker 实际 socket、
PID/窗口代次、native carrier、维护锁/FD 交接、组件组/升级、ARM64 分流、FEX 计划、
受限 /proc 下真实 TCP 查询、lsof 筛选和 worker 回收。ArkTS 与 OHOS native 编译通过。
具体脚本及结果随本轮交付记录更新。

真机新包、普通 Steam 库三条路线、退出/重启/后台恢复、HmosDeck 结束、仙剑媒体、
会员/离线导入/字体、三轮交替性能仍须新包实测；不得将上述主机结果写作真机通过。
历史星露谷 GNU 仅有菜单短测；退出码 255 保持未解决记录。

## 2026-11-11：启动自检与回归补充

- `0cd72ad5` 补齐当前 session 诊断和 IPC/GL 自检错误提示。Mini 新包在 Steam
  客户端启动前失败：共享 robust mutex 的持锁子进程被 SIGKILL 并 wait 后，
  timedlock 返回 ETIMEDOUT，未获得 EOWNERDEAD；不能计为游戏或性能验收。
- `c45b81e6` 修正 AMS startAbility 返回早于 WindowStage 注册时的重复 carrier
  创建；pending 保留到窗口注册，拒绝回调不再错误关闭已在创建的真实窗口。
- `ee08b6c2` 加入仅失败后运行的 OHOS raw robust 对照，区分无 GNU/TLS/ptrace
  的内核观察；本地打包通过，尚未设备执行，根因仍待对照结果。
- CI `42998707999` 的 FD 断言存在采样竞态：认证拒绝可早于 worker accept，
  FD 数量短暂等于基线不能证明 worker 空闲。改为显式唤醒并 join 自有 worker，
  再严格比较启动前完整 FD 数量，并确认应用 FD 与自有 endpoint 生命周期。
  本机完整 metadata 套件 5 项通过、1 项缺固定 lsof 输入跳过；生命周期
  40 轮、20,480 次查询通过。该修复没有降低运行库断言或跳过产品 IPC 自检。

Windows Steam 性能聊天已交还共享构建目录与 USB Mini；后续串行构建和设备验证。

## 2026-11-11：原生 carrier 就绪状态

`8da32e9c` 在 Mini 普通 Linux Steam 入口已进入登录及库页，初始无 Wine
子进程，但未发布 Deck ready；可选 `d3ddriverquery64.exe` 随后又提前准备了 Wine。
修正如下，主机回归已过，等待新包真机复测：

- 独立 native carrier 启用既有 X11 状态管理，不依赖旧 Wine HWND 开关；
  真实已映射窗口驱动 Deck ready 和控制命令，沿用无读回的呈现路径。
- 仍校验官方 helper 身份；Wine 未就绪时，可选驱动查询返回实际不可用结果，
  不生成虚假 GPU 信息，也不准备前缀。首个真实 Windows 游戏仍须完整准备 Wine。
- 原生窗口身份/归属变化写有界日志，继续定位启动 Wine 后“运行中”空列表，
  不放宽窗口归属或按窗口出现顺序猜测。
- Xvfb 验证 native 映射窗口、当前 generation、ready、退出 closed 及无 PPM 读回；
  既有 legacy 桥接和窗口布局回归保留。Node 验证冷查询不会启动 Wine、随后
  Windows 游戏会准备 Wine，以及 native carrier 的归属和控制行为。

robust owner-death 失败仍为间歇性待查；本轮启动成功不能作为该问题已修复的证据。

## 2026-11-11：Mini 原生启动复测与全部后端延迟准备

`9c964c6f` 已完成 260 项 preflight、完整运行库回归、ArkTS/OHOS native 编译及
最终 GNU 2 组件/7 companion 校验，使用 release/internaltesting 签名覆盖安装 Mini。
从普通 Linux Steam 入口约 19.7 秒发布 running，实际显示登录及库页，未出现 OHOS
Wine 子进程；但可选 driver_query 选择 GNU 后仍准备了 GNU 前缀，未满足纯 Linux
无 Wine 准备的完整目标。客户端约 113 秒后真实 signal 11 退出，尚未取得栈证据，
不能依据同期 BadWindow 推断根因，也不能计为游戏或稳定性验收。

- 冷态 driver_query 同时覆盖 OHOS Wine、GNU Proton 和 GNU WineD3D。GNU 只有
  当前 Steam 代次中真实 Windows 进程完成准备和 dispatch 后才允许后续可选查询；
  组件文件存在、失败准备或取消不能开启查询。查询未执行返回实际不可用状态，
  保留请求的真实 backend，不伪造 GPU 信息。实际游戏和安装脚本仍正常准备运行库。
- Native carrier 归属变化改用应用 LogService，导出日志包含首次空快照及后续变化；
  不按标题或窗口出现顺序放宽凭据、PID/starttime 与代次校验。
- 定向 Node 回归覆盖全部冷态后端、准备失败/取消、新代次重置、温态 GNU helper、
  Windows 游戏首次初始化以及空/不变窗口快照。
- 较早完整 CI `42996602647`（`21bfda35`）全部构建成功；当前修复的 CI 和
  新包真机结果仍须分别跟进，历史通过不替代当前提交验收。
