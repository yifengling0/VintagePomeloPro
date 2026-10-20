# Linux ARM64 Steam 登录转圈：IPC 会话隔离

## 故障现场

- USB 平板，Linux ARM64 Steam；已保留账号、库和存档，没有使用 Windows Steam。
- 2026-10-20 07:41:27 UTC 已收到 `RecvMsgClientLogOnResponse(): OK`，认证通过后仍停留在登录转圈。
- CEF 随后出现 `Timeout waiting for RegisterForStartupFinished`、`SteamClient globals not available` 和 `RegisterOverlayChatBrowserInfoChanged` 未定义。
- 主线程仍运行，采样调用链反复进入 `libsteamohos_posix.so` 的 `semctl` / `GroupOpen`。
- 持久 `client-ipc/registry-v2` 的 256 个记录全部占满（均为信号量，next=534）。样本包含多轮启动的旧 PID 和旧时间；对象没有随进程结束自动移除。
- 现有 `Get()` 在无可用槽时返回 ENOSPC；SEM_UNDO 只恢复数值，并不释放注册表记录。旧 key/值也会被后续会话复用。

## 修改

- 宿主向每次 Steam 启动传入 `STEAMOHOS_SYSVIPC_ROOT=<本轮私有 x11 目录>/sysvipc`。
- SysV 适配器优先使用该临时目录，所有 Steam/CEF/reaper 后代继承同一目录，包括过滤环境的 GNU exec。独立诊断程序仍可使用原有 IPC_ROOT 回退。
- 会话结束仅回收该临时目录下受控名称、当前 UID 所有的普通 IPC 文件；不跟随软链接，不递归删除目录。
- 持久 `client-ipc` 中的 Cloud 映射、NSS 数据库等保持原位；没有清空账号、游戏、下载或存档。
- 表耗尽时增加每进程一次的容量诊断，避免再次表现为无提示等待。
- 异常结束遗留目录不会被下一会话复用；当前实现只在正常析构时回收临时目录，强制结束留下的目录后续可由专门的临时资源整理处理。

## 回归

- `host_tests/linux_steam_sysvipc_namespace_test.py`：真实创建 256 个遗留信号量、确认 ENOSPC，模拟异常结束；新会话仍能创建相同 key 并完成 fork 信号量/共享内存通信，旧会话与持久文件逐字节保持原状。
- `host_tests/linux_steam_filesystem_test.py`：SysV 原子性、SEM_UNDO、超时、共享内存、POSIX shm、robust mutex owner death 和 NSS 持久化通过。
- `host_tests/linux_steam_exec_test.py`：26 组真实 GNU exec 测试通过，新增 session root 在过滤环境的 handoff 中保留。
- `host_tests/linux_steam_desktop_test.py DesktopConfiguration PrivateSnapshots`：新增临时 IPC 清理边界、软链接和未知文件保护；现有配置/有界快照测试通过。
- ArkTS/C++ 编译和 HAP 签名通过。EntryAbility 继续使用既定基线；Mesa gitlink 保持 557fa3e781ea6e78fcff3ee759c25f60c3055064。

## 平板实测

- 候选 v96 与已校验外置运行库共同安装。
- 保留旧表 256/256、next=534，首次启动已显示并可见 Steam 主界面/游戏库。
- 正常退出：所有 GNU Steam 子进程结束；IPC preflight 通过（142ms），客户端正常返回 exitCode=0。
- v97 补充窗口修正：每个 HWND 启动前清理旧 `.control`；初始放置和窗口/全屏切换均等待两次稳定位置确认，ArkTS 等待 geometryReady 后才给 Xwayland 初始尺寸，并忽略过渡几何。
- 新增 ArkTS 窗口测试覆盖旧串号控制文件和异步尺寸稳定；宿主 Win32 程序以 `-Wall -Wextra -Werror` 编译通过。
- v97 首次启动和正常退出重开均已实际显示完整游戏库，未复现偏移。两轮 namespace 分别为 x11-FJGoaL / x11-fiepmo，初始有效槽位均 5、next=8。
- 正常退出后，旧会话的 registry-v2 已不存在，证明析构回收执行。
- 强制结束重开也已通过：新 namespace 为 x11-aeBgaI，初始有效槽位 5、next=8；窗口稳定为 1200×800、geometryReady=1、restoreRetries=0，实际显示完整游戏库，无偏移。

截图、原始客户端日志、进程信息和采样均存放在任务 artifacts 的 private 文件中，含账号信息，不作为公开文档附件。单次成功不能代表长期偶发问题全部消失。
