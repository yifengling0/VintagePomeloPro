# Windows Steam 目录查询限时

Mini 上冻结 Windows Steam 的 `GetCMListForConnect` 多次等满 60 秒失败，
随后用本机缓存的 WebSocket CM 在约 2 秒内登录。现在只将这两个目录 API
调用点的 HTTP request deadline 改为 5000 ms，而且只在 WebSocket CM 数量大于零时生效。
无缓存保持原 60000 ms。下载、登录、证书校验与游戏的超时不变。

`vpsteam_runtime32.c` 沿用原验证器与精确 SHA256 保护，在首次单线程客户端加载时
校验 request ctor 和两个 enqueue 调用，再安装寄存器/EFLAGS/栈保持的 thunk。
磁盘 `steamclient.dll` / `steamclient64.dll` 不修改，未知客户端拒绝安装。
`VPP_STEAM_CM_TIMEOUT_DISABLE=1` 可用于原行为对照。

`entry/src/main/resources/rawfile/windows-steam/manifest.json` 绑定源码、编译器和小型 PE32 helper。
用 Ubuntu MinGW GCC `13-win32` 运行：

```
python3 scripts/prepare_windows_steam_runtime.py --rebuild
python3 host_tests/windows_steam_runtime_test.py
```

日常打包验证源码/二进制关系，并检查最终 HAP 实际携带的两个资源。
既有安装只在 Steam/游戏停止、旧 helper 和五个客户端依赖精确匹配时原子升级；
自定义、缺失、损坏的离线包保留原文件。运行中不升级，账号与游戏目录不修改。
ArkTS 回归覆盖运行中、未知文件、资源损坏和校验过程中启动等情况。

本机 PE32 执行测试覆盖两个调用点、缓存 0/-1/1/32、request 写入范围、stdcall 栈清理和 RX 权限，23 项通过。
2026-11-11 Mini 首轮实测：18:04:11 查询，18:04:16 失败，18:04:19 登录成功；
另轮正常目录请求约 2 秒成功。UI 卡顿是独立问题，不因目录限时通过而认定已解决。
旧 helper 的完整下载/游戏证据不自动适用于新 helper，打包 manifest 明确区分两者。
