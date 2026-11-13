# 鸿蒙 PC Linux Steam 1.5.1 黑窗调查与候选修正

## 新日志结论

输入：logs-20261024-102253.zip，SHA-256：306154ad17ce8fdb1104e9a464f8d6a35693fd41d8384594e58bc1fe2192b453。
设备：鸿蒙 PC / 2in1，Maleoon 935，NCP systems 后端。诊断版 1.5.1 / 1005010。

Steam 主进程、Wine 承载窗口、Xwayland 与 IPC 自检均已启动。supervisor 持续更新，但 ready=false；停在 steam_client_starting。CEF browser 约每 10 秒重开，未产生可用界面。

按北京时间 10:22:44–10:22:45（CEF 内部 UTC 为 02:22:44–02:22:45）：

1. GPU 子进程 63646 和 63668 尝试加载官方 libGLESv2.so，dlopen 返回 cannot open shared object file: Permission denied。
2. 随后报告 Exiting GPU process due to errors during initialization。
3. 后备子进程 63710、63721、63724 在 proc_util.cc:97 的 fstatat 检查失败，errno=ENOENT，SIGTRAP / exit_code=133。
4. browser 报 GPU process isn't usable. Goodbye，然后重新启动，持续黑窗。

现有 --disable-gpu 与 --disable-gpu-compositing 已传入并生效；Chromium 仍需要图形服务子进程，不能把重复禁用 GPU 当作修正。网络连通性探测成功，现有证据不指向账号认证或网络。

libSDL2、libpci、DBus、udev、NETLINK 与 Vulkan Xlib surface 等探测失败单独记录，不据此认定黑窗主因，也不改动 Wine/DXVK。

## 本轮实现

- 核对实际 ARM64 libcef.so 的动态符号，确认调用 GLIBC_2.17 的 __fxstatat64，原适配层只实现新版 fstatat/fstatat64。新增 __fxstatat 和 __fxstatat64，沿用相同的私有目录、Cloud 与 Singleton 路径语义，保留旧 ABI 版本检查。
- 在显式启用 GNU 原生内存模式时，对于本进程 /proc/self/fd/N 或 /proc/<当前PID>/fd/N 的跟随查询，仅在原调用返回 ENOENT/EACCES 且真实 descriptor 仍有效时，用 fstat/fstat64 读取真实 inode 元数据。支持相对于 proc fd 目录的调用；不处理其他进程、不制造 stat 结果，no-follow、非法 flags、无效 ABI 与已关闭 FD 保持失败。
- 仅在已经验证的外部运行时、原版 steamwebhelper 的预加载初始化阶段提前加载官方 libGLESv2.so / libEGL.so（RTLD_LOCAL）。子进程继承已有映像，减少在后续阶段重新打开文档目录库的需求。不创建 GL context，不开启硬件加速，也不修改 Valve 文件。
- 图形提前加载结果和实际 FD 恢复均有有界日志，可继续区分“提前加载也被拒绝”和“后期才被拒绝”。EACCES 具体来自文档提供器授权还是 CEF 子进程阶段限制，仍需该 PC 实机复核。
- GNU 适配组件 release 已更新。已有安装启动时沿现有配套升级流程复用校验一致的官方文件；不下载新客户端，不重置账号、配置、游戏库或存档。若组件不完整，保留数据并提示组件页修复。

## 验证

- 12 项宿主回归通过：CEF 旧 ABI、/tmp 路径、注入 procfs 查询失败、真实 FD 元数据、no-follow/非法 flags/无效 ABI/关闭 FD、子进程在源文件无法访问后复用已加载模块，以及原文件系统、Singleton、Cloud、单一游戏库、发行包分离与校验测试。
- GNU AArch64 适配层使用现有 GNU 工具链构建，-O2 -Wall -Wextra -Werror 通过。
- 原增量工程 Release assembleHap 通过；版本 1.5.1 / 1005010，arm64-v8a，debug=false。
- ZIP 完整性、官方无签名检查、GNU 修正标记与 manifest 哈希、Wine 运行库闭包与 Modern DXVK 锁校验通过。
- Wine payload SHA-256 保持 0e03e79514f252e0751bd76b9d357a001a6d12a344da216ac4e94f706b698431。
- 仅宿主与构建验证，未连接鸿蒙 PC，未操作用户平板或手机。后续用户已确认 PC 可启动；logs-20261024-114008.zip 的 running/ready=true 也验证了黑窗修正生效。游戏权限错误另见 PC_GAME_FILE_PRIVILEGES.zh-CN.md，不等同于所有游戏已通过。

## 复测

按原有签名流程签名并覆盖安装本目录 steam-pc-fix-unsigned.hap，然后正常启动 Linux Steam。保留所有原数据，无需卸载或清空组件。观察是否进入登录页/游戏库；如仍黑窗，导出新日志，重点核对 STEAMOHOS_CEF_EARLY_GRAPHICS、STEAMOHOS_PROC_FSTAT_RECOVERED / FSTATAT_FAILURE 和新的 CEF 首错。

原 1.5.1 正式未签名交付包保留。旧诊断包被当前候选替代，历史校验保留在 previous-diagnostic-verification.json；未执行 Git 提交或 push。
