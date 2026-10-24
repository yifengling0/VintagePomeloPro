# 用户反馈日志导出

入口不变：系统设置 → 日志管理 → 导出日志。复现后尽快导出，分享 ZIP 即可，无需另行复制 Wine 或 Steam 目录。读取、脱敏、整理和 ZIP 压缩均在 TaskPool 后台线程执行；主线程只等待结果并打开分享面板。任务仅传入路径和版本字符串，不传 UI/Ability 对象，也不将大块日志回传主线程。

ZIP 包含：

- `app-*.log` / `native-*.log`：应用日志的脱敏尾部快照；最新优先，最多 16 份、单份最多 2 MiB、合计最多 16 MiB。
- `steam-startup-diagnostics.log`：Wine / wineserver stderr、DXVK D3D9 / D3D11 / DXGI 日志、Linux ARM64 Steam 与 Windows Steam 的启动、游戏进程、下载、连接、云同步、安装脚本与 CEF 日志，以及原有接口桥和启动状态摘要。最多约 8 MiB 诊断正文，另含来源及缺失说明。
- `export-manifest.log`：应用版本、采集时间、收集范围、大小限制及遗漏说明。每份源日志记录原大小、尾部偏移、读取量、是否截断和修改时间。

Wine 错误记录默认开启 `-all,err+all`，不全局开启 trace、fixme 或 DLL 加载跟踪；现有专项诊断覆盖规则不变。普通游戏、Wine 子进程与 wineserver 共用捕获路径，手机 fork 与平板 NCP 均调用同一入口。stderr 改存应用私有 `files/wine-logs/wine_stderr_YYYYMMDD.log`，重启或缓存清理后仍可导出；日文件最多 16 MiB，多进程锁保护，满后原 inode 截断并标注。新进程启动时清理超过七个本地日期的常规日文件。日志线程创建失败不重定向 stderr，避免产生无人读取的管道阻塞游戏。

兼容旧版本的 `base/temp` Wine 日志。导出分别取私有持久目录和旧临时目录最新四份，每份末尾最多 256 KiB；不限定当天，允许反馈前一晚的崩溃。DXVK 扫描默认及最多八个游戏容器的固定 Windows temp 目录，仅接受指定后缀，合计最多 16 份。Windows Steam 支持受管理的 `Z:\Steam`、旧 `C:\Program Files*\Steam` 和浅层 `Z:\games` 导入客户端，最多四个；不会递归打包游戏库。

仅白名单日志可导出，拒绝常规文件位置上的符号链接；不导出游戏、存档、用户账号数据库、浏览器配置或完整 config.vdf。保留原有兼容工具映射摘要；密码、令牌、Cookie、授权票据、账号标识和 HTTP URL 查询参数过滤后再写入快照。脱敏不能替代审阅：用户自行写入日志的任意私人文本不属于可穷举的凭据字段。

多个导出请求共享一次操作；压缩成功或失败均清理临时快照，失败时删除不完整 ZIP。ZIP 沿用缓存中的最新三份保留规则，源日志不变。Steam/Wine 日志缺失或被轮转不会阻止应用日志导出，报告会标注 unavailable；不据此认定游戏未启动。新版只能补齐此后产生的错误，无法还原旧版本未记录的输出。

验证入口：`scripts/run_steam_startup_diagnostics_tests.cjs` 覆盖生产采集与压缩生命周期；`host_tests/stderr_forwarding_test.py` 覆盖真实生产管道读取和限频；`host_tests/stderr_file_test.cpp` 覆盖多进程限额、截断后的最新异常、七日清理和链接拒绝。另需 Release ArkTS / NDK 编译和设备导出复核。

本轮 Release ArkTS / NDK 编译、HAP 运行库闭包和上述测试通过。VYG-AL00 手机实测确认工作线程 TID 与 UI 主线程 PID 不同：8113 ms 完成导出，期间能读取界面树且显示整理状态，随后分享面板正常打开；取消分享后设置页仍可操作，进程继续存活。17 个 ZIP 条目、未压缩约 16.8 MiB、压缩约 1 MiB；确认持久 Wine 日志内含 wine 与 wineserver 标记，旧临时日志、DXVK、双 Steam 日志和导出清单同时存在，临时快照已清理。未操作平板。
