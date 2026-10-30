# Linux Steam 文件权限预检与存档诊断

2026-10-30，基线 `7f66d9fe`，修改在当前工作树。没有取得用户反馈存档失败的游戏，不能视为该问题已经复现或修复。

## 截图能确认什么

《灾厄逆刃》的弹窗指向 `<client>/.steam/steamarm64/msidb`，不是游戏存档目录。换行发生在 `steam` 和 `arm64` 之间，中间没有斜杠。

检查了当前部署清单，以及本机已缓存并校验的官方 ARM64 `bins_linuxarm64_linuxarm64`、`bins_misc_linuxarm64` 包：均没有 `msidb`。这些事实不能证明测试者的实际文件状态，也不能确定是哪个启动步骤生成了该路径。因此，不创建空文件、不把缺文件伪装成有权限、不改成另一款 Wine 工具以掩盖错误。后续需要真实启动输出中的调用及 errno，判断辅助程序缺失与路径错误。

## 已修复的通用问题

文档盘保存的 GNU ELF 通常没有 Unix 执行位，但本工程通过用户态 GNU loader 执行受控运行时中的文件。原 `access(X_OK)`、`faccessat(X_OK)` 直接询问内核，与实际执行能力不一致。

预检现在与 exec handoff 共用同一个路径解析器，并核验运行时代码树中的真实文件：原生 ELF64、正确机器类型、有效入口、可读的 GNU loader，以及调用者要求的其他真实权限。缺文件、错误架构、共享库的零入口、脚本、不可读文件、其他路径和未知 flags 不获得回退。相对路径及 dirfd 参与同样的检查。

**执行仍使用已经交付的 runtime 代码文件。** ARM64 真机负对照确认：客户端目录中的可读 ELF 副本不一定允许可执行映射。尝试让 rtld 直接使用该副本会出现 `failed to map segment from shared object`，该方案已撤回。仅在客户端出现、runtime 中未交付的程序不能通过预检；不扩大 glibc 的运行时映射白名单。

`STEAMOHOS_EXEC_ACCESS` 限量记录失败或用户态加载器回退，包含路径及 errno。`STEAMOHOS_FS_FAILURE` 在关闭 verbose 的情况下，限量记录 open/fopen、目录操作及 rename 等的权限、只读盘、空间不足错误。每进程分别限制为 16 / 32 条，不记录文件内容；输出进入已有 Wine/Steam 日志收集链路。

## 验证范围

- 新增 `host_tests/linux_steam_exec_access_test.py`：真实 fork → GNU loader → 程序入口返回 73；同时检查 code/client 别名、dirfd、真实拒绝、缺 loader、错误架构和仅有客户端副本的负对照。真实只读写入失败、失败 rename 的 errno 和错误日志保持一致。
- 现有 exec、dlopen、文件系统、共享内存、GNU/OHOS 云存档路径、原子下载/rename、单游戏兼容工具选择及保护原存档的清理回归通过。
- native FEX 与 depot 链接安装/更新/移除回归通过。
- 最终 ARM64 POSIX adapter 构建及 `--verify-only` 通过，清单与二进制一致。恢复原有 runtime 执行路径的真机候选成功进入 Steam 和 Banana；本轮客户端日志没有 `failed to map segment from shared object`。本轮未触发 `STEAMOHOS_EXEC_ACCESS` 回退，因此它是启动链无回退的检查，不能代替该回退的 ARM64 正向专项验收。

存档目录没有批量 chmod、迁移或重置；没有清理用户账号、游戏、数据库或存档。截图所涉游戏及用户保存失败的具体场景仍待复现。

现有云存档目录别名、写入及 rename 回归通过，不等于已复现用户的保存失败。新增限量失败记录覆盖上述文件系统入口，未宣称覆盖所有 write syscall 或所有保存机制。
