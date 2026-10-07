# Proton 主程序修复与源码发布（2026-10-07）

本次补交 `feature/main_proton` 的主程序、运行时补丁和必要构建脚本。提交前基线为 `57f0c8565547c824c39cc8c3b8dbf45541557c95`；该前序提交只发布历史 Steam 客户端的独立 Zstd 兼容方案。Steam 客户端补丁不内置到主程序。

## 最终行为

- ARM Proton 保持 FEX 默认。32 位 WOW64 和 64 位 ARM64EC 共用 `FEX_X87REDUCEDPRECISION=1` 默认；游戏入口可以显式覆盖为 `0`，完整退出 Wine/FEX 后生效。旧 x64 Wine/Box 基线不增加该默认。
- 手柄输入采用与 main 对齐的成对摇杆更新、径向死区、来源接管和释放逻辑。宿主与 Winebus 同步为 WHGP v2，修复重复 Y 反转、斜向输入以及 socket polling 无法推进的问题。
- 手机子进程存活依据真实 waitpid、NCP 回调或 fork server 退出报告；沙箱内 `/proc` 不可读不再被当成退出。回收只消费登记的直接 fork 子进程，保留退出码和 PID 代次，避免把初始化中的 wineserver 误判为死亡。
- 初始化界面同时接受 `state:starting:<stage>` 和 `phase:<stage>`，显示总准备时间；重复阶段不重置计时。桌面输出保留已接受的横竖屏几何，忽略旋转期间的临时反向尺寸。
- Wayland 输入线程跟随目标窗口桌面，修复首次桌面开始菜单无法命中。所有构建宿主显式启用 Wayland EGL，并检查实际配置、链接参数及驱动 ELF 导入，拒绝缺少 OpenGL 实现的驱动。
- DXVK 默认保留 DirectDraw / D3D8 / D3D9 builtin，D3D11 / DXGI native；legacy 的 D3D10 链保持 native。游戏专属 override 仍在默认策略后合并。
- 发布 Wine 映射 DISCARD 修正、上传计时，以及默认关闭的动态缓冲暂存、VirGL 低地址共享映射、WGL 锁等待诊断。发布 FEX synthetic-return、Windows CRT 文件读取、配置读取和初始化保护修正，以及默认关闭的 JIT 诊断和 PC24 mul 实验。没有把尚无稳定收益的实验设为生产默认。
- 上架版本名称为纯数字 `1.4.5.27`，版本码 `1004036`。

## 本轮验证

19 个相关 Make 目标通过，覆盖 DLL 路由、GPU 显示/输入契约、构建身份、broker 启动、手机进程生命周期、手柄合并/协议/传输、OpenGL 计时、映射和 FEX 修正。其中手机生命周期 145 项、手柄合并 59 项均通过。环境基线 88 项和环境序列化 21 项另外通过。

生产 ArkTS 方法通过实际 SDK TypeScript 编译器执行的初始化进度与桌面方向回归；使用受控时钟和平台替身，这不等同于新增真机验收。6 个相关 shell 构建脚本通过 `bash -n`。

在临时目录从锁定 Wine pin 重放 `build_wine.sh` 的全部 41 个生产补丁注册：7 个已经包含在 pin，34 个成功应用。OpenGL 计时、上传、DISCARD、动态缓冲、低地址共享映射、Winebus polling、WGL 锁和手柄协议的 9 个专项入口再次在重放来源上通过。

当前 Wine 子模块脏树包含较早候选：缺少 0040 的 ntdll 实现和 0042 的头文件引用，0039 仍为初始数据提交后才设置策略的旧稿。提交采用父仓库中的完整生产补丁；0039 修正在初始数据提交前设置策略。子模块指针和既有脏状态保留，不把本地子模块树声称为生产补丁的完整镜像。

本轮为源码发布验证，未重新编译 HAP/APP，未增加真机游戏对照，也未宣称聚合 `make test` 或所有游戏性能通过。既有构建与设备证据的时间、来源和局限分别保留在各报告中。

## 构建与评审入口

更新主仓库及锁定子模块后，用新的独立 `BUILD_DIR` 构建，正常构建入口重放随仓库提交的 Wine、Mesa、FEX 补丁；不要用缺少身份记录的旧对象目录。FEX 计时和 exact-store 命中诊断保持关闭，诊断包不用于测 FPS。

- [默认 x87 策略和 32/64 位验证](fex-x87-default-20261007.md)
- [Wayland EGL 回归和图形路由修正](wayland-egl-startup-regression-20261007.md)
- [桌面输入修复](desktop-start-menu-input-fix-20261007.md)
- [手柄与 main 对齐](controller-main-alignment-20261006.md)
- [FEX Windows 初始化修正](fex-windows-file-loading-followup-20261006.md)
- [1.4.5.27 构建交付记录](release-1.4.5.27-20261007.md)

研究报告、脱敏文本摘要和分析源码一并发布。HAP/APP、DLL/EXE、压缩包、游戏资产、原始大日志和采样留在本地；桌面截图也留在本地。文本统一 LF，证据目录的文件清单校验更新为当前发布字节；构建/运行身份文件中原有哈希仍表示当时产物，不追溯改写。
