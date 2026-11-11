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
