# FEX PE32 测试启动失败：配置读取与早期异常保护

日期：2026-10-06；基线 feature/main_proton / 284026ce；FEX pin 86ff33bbe2。

## 结论边界

用户手动运行原 CPU 测试及配置隔离入口都失败。没有有效 TSV、没有 FEX/Box CPU 对照，也没有确认 RichMan8 帧率提升。本候选修复 Windows 文件读取和初始化阶段的异常处理；是否解决实际启动失败仍需用户手动运行后确认。不能把这次启动失败当作游戏冷加载卡顿的已证实根因。

## 已核实的异常链

- 原进程 OS PID34075、隔离进程 PID37195 均在测试 main 之前退出。控制台 Exit code: 0 不等于成功：FEX 部分 UNIMPLEMENTED 退出路径使用零状态码。
- 隔离文件 C:\vp-route-probe-config\fex-emu\Config.json 实读 15 字节、内容为有效 JSON 空 Config 对象，仍出现同类断言。因此不能归因于用户配置语法错误。
- 旧候选 DLL 与实际 fault maps 对齐：初始 SIGILL PC 0x6ffc44aa40 对应 ForcedAssert；LR 0x6ffc453980 对应 LoadJSonConfig 的 invalid JSON 分支。
- 后续 reset 在 FEX 尚未完成初始化时把 WOW64 TLS 初值 2 当作线程指针，访问 0x42，约 453 次递归后触及 native stack guard。这个次生异常链已定位，不能证明原配置解析失败的具体文件路径和底层原因。

原证据目录：F:\VintagePomelo-Workspace\workspace_temp\fex-jit-cost-20261006。关键文件 config-isolation-failed-log.log、config-isolation-failed-paths.txt、bootstrap-fault-analysis.json、bootstrap-fault-symbols.txt。

## 生产改动

scripts/patches/fex-windows-file-loading.patch 由 build_fex.sh 独立应用，Makefile 记录依赖和时间检查。

1. Windows LoadFile 改为 checked CreateFileA / OPEN_EXISTING、SetFilePointerEx、ReadFile，并自动关闭句柄。按长度精确读满，处理短读、错误和提前 EOF，失败后清理部分数据；FixedSize 不再被忽略。LoadFileToBuffer 从文件开头读取，空/EOF 返回 0、错误返回 -1，并限制 DWORD 请求长度。Linux 读取路径不变。
2. 配置无效时通过原生 Wine 输出路径和读取字节数；正常日志 handler 还未安装时也有可见诊断。保留原 JSON 校验，未吞掉错误或绕过配置。
3. WOW64 process init 开头清 ThreadState、末尾发布 ready；thread init/term 清理 ThreadState。reset 在 process 未 ready 或线程为空时返回 native Wine 异常路径，避免已观察到的初始化阶段错误被 FEX 二次处理。这不是任意损坏 TLS 或全部生命周期安全性的证明。

没有改变默认 FEX、TSO、SMC、x87 精度、画质、缓存策略或呈现政策。JIT 诊断仍默认关闭。

## 验证与部署

专项提取实际生产 reader 和 reset 前置保护，覆盖完整 JSON、FixedSize、partial reads、缺失文件、seek/read failure、partial-then-failure、空文件、提前 EOF、DWORD 上限和关闭句柄。补丁正反重放两轮逐字节一致。旧 reader fixture 复现忽略 FixedSize 和文件末尾 LoadFileToBuffer 返回 0；这两项静态缺陷并不单独证明本次 valid JSON 失败原因。

CRT open、JIT symbols、synthetic return、JIT cost 相邻回归通过，bash -n 和本次 tracked 修改 diff-check 通过。全 dirty diff-check 仍报告既有 0037 patch 的四处空白，本轮未动这些内容。没有运行完整聚合回归。

独立 BUILD_DIR 为 build-fex-fileload-20261006，source 为 workspace_temp/fex-fileload-20261006/source。ARM64EC 与 AArch64 PE 均 RelWithDebInfo / O2 / NDEBUG 构建成功。首次编译发现 Common 没有 Windows 私有 wine/debug.h include 路径，改为与现有 header 一致的输出函数声明后，在同一独立目录重新编译成功；新 patch 已重新回归并核对 staged source。

候选以已安装 e0c1caa7 包为基底，只更新 WOW64 DLL 和 payload manifest/runtime marker。非 discardable PE 加载节与新编译输出一致，PE imports 不变；其他包内容逐字节继承，ARM64EC 仅构建校验，包内版本保持原值。官方 verify-app 成功，签名保持既有 archive 内容。

| 项目 | SHA256 |
|---|---|
| 生产 file-loading patch | a48ab5b7eb8f692bebacc703047638749b8a93c06e5416eb89d444ddb20f9c2b |
| 编译 WOW64 DLL | 140248f753c4fd91f63163928c756c50577be761b797fa977dd06927d86c9263 |
| 包内 WOW64 DLL | 1cbece85e595deaabcc85759ce924821a92291fefce2114ab2aa574cf7cc0903 |
| 未签名 HAP | 95fd670bdb259f2ef8c2bc0f6b7f916100319156ae8ccd3c93fa288d7b068c65 |
| 签名 HAP | 97573dc87b1467ab846c57bc5a72dfc85ac027f24462ab1b96e86c7427f0b7c5 |

版本：1.4.5-proton.26-alpha / 1004035。新 marker：content=6b83b243e383edb7;wine-valve=cd547f7a0e;fex=86ff33bbe2;arch=arm64-v8a:aarch64。

安装记录：2026-10-06T15:20:20.033619+08:00。状态：Manual CPU probe PID44038 still failed before main at JSON assertion; no TSV; init guard prevented the previously repeated 0x42 faults; process spun in native exception handling and target session was stopped。结束目标旧会话并覆盖安装，保留数据；未卸载、清数据，未工具启动应用/Steam/游戏/BAT或发送输入。15:22:36 设备解包后的 DLL SHA256 和 marker 与候选一致；CPU 测试进程实际加载和成功运行仍待验证。

## 后续验收

用户手动打开旧柚 Pro，运行 C:\vp32-route-probe-fex-20261006.bat，暂时保持游戏关闭。必须核对新运行时 marker/DLL 以及结果 TSV 的 complete=1、精度检查、25 行、7 项各 3 轮和 checksum；不能仅相信退出码或 HODLL 环境变量。

若仍失败，先看新 FEX-CONFIG-ERROR 的真实路径/bytes 和初始异常，而非仅次生崩溃。CPU 测试成功后继续 RichMan8 首次/重复资源场景 JIT 计时与同 payload Box 的手动对照。人物缺块保持独立调查，尚无画质修复结论。

本轮没有 fetch、commit、push，没有更新记忆；已有 Wine、0037、手柄与其他 dirty 修改均保留。

## 最新手动运行：候选仍未解决启动

用户反馈卡在批处理窗口。15:23 后 OS PID44038 的第一 SIGILL 为 PC 0x6ffc44aa60、LR 0x6ffc453a48；使用本次实际编译 DLL 符号化仍分别为 ForcedAssert 和 LoadJSonConfig 的 invalid JSON 分支。设备 DLL 哈希与候选一致，不能再解释为旧运行时。

没有生成 TSV，也未到 main。这次未出现此前连续访问 0x42 的递归链；进程仍在 native 异常处理路径消耗 CPU。10 秒 DWARF4096 有界采样保存 1083 samples、lost0，主要可见 caller 地址在 native ntdll 代码区；这不是游戏/JIT 性能测试结果。

直接 kill 单个测试进程被设备权限拒绝，HDC 返回码 0 不能当作成功；后续使用已授权的 aa force-stop 结束目标旧柚 Pro 会话，确认目标进程全部退出。未清数据，未启动程序或发送输入。

已发送并回读 C:\vp32-route-probe-fex-filetrace-20261006.bat，SHA256 971f35730a17c01e51fe722e6f3bb18fa40c90b624ea5646a85997dd09fc4f3a。只给这次 CPU 测试设置 -all,+err,+file，用于实际配置文件访问定位；用户手动运行，当前结果待采集。文件读取修复并未消除本次解析断言，仍须查真实路径和字节，不能宣布启动修复完成。

## 文件访问诊断的新事实

第二次手动入口 OS PID47496、Wine TID02d4 的文件访问日志已保存（filetrace-manual-log.txt）。失败前，实际打开的是 Z:\.config\fex-emu\AppConfig\，映射到 /storage/Users/currentUser/Download/com.vintage.pomelopro/.config/fex-emu/AppConfig/ 目录。NtCreateFile options=0x20 允许目录，随后只有大小/位置查询，没有 NtReadFile；LoadFile 返回空数据后在 JSON 分支断言。

该调用本应使用包含 EXE 名和 .json 后缀的应用配置路径，而非 AppConfig 目录。当前证据确认了目录被当作 JSON 读取；路径后缀在原生运行时丢失的底层机制尚未证明，不据此宣布整个 fmt 库或 FEX 性能存在同一根因。

第二版生产 patch 保留完整 EXE/.json 路径，拒绝目录和空路径，仍对真正空 JSON 文件保留原解析错误，并避免错误路径诊断依赖格式化输出。新增实际生产路径函数的 normal/global/fallback/string_view 长度测试，以及 reader 的目录和元数据查询失败回归，均通过。第二版独立目录 fex-fileload-v2-20261006，设备仍需候选安装和手动验收。

本目录 fileload-candidate-v1.patch 固定记录第一版生产补丁（a48ab5b7...），当前 scripts/patches/fex-windows-file-loading.patch 已包含第二版改动，不能拿现有 canonical patch 哈希追认第一版包。


## 最新自动化验收：V2 启动修复已通过

用户现已明确授权直接启动和操作游戏。本轮不再要求用户运行 BAT，已经自动启动 CPU 测试及 RichMan8，并执行触摸／按键、截图和有界采样。以上 V1 手动失败记录保留为历史，不能作为 V2 当前状态。

V2 使用新的 build-fex-fileload-v2-20261006，完整保留 AppConfig 的 EXE/.json 后缀、拒绝目录和空路径。签名包 SHA256 为 `e1a3acb23830fc9d8fc6dd7f9cdd5977b9855e6a298cd4ec4432153e43e33846`，包内 WOW64 DLL 为 `6c0df6e2c3056f1d4cbf2eeb6fee365087a605898df108ec810f9791bdfd6354`。实际 FEX 测试 cpu-fex-v2.tsv（WinePID424）25 行齐全、complete=1、精度和各项重复校验通过。启动失败已解除；路径后缀丢失的底层格式化机制仍没有证明，也没有游戏帧率改善结论。

明确指定 WINEHUA_WOW64_ENGINE=box 的第二次对照才实际加载 Box；只设置 HODLL 的第一次请求仍是 FEX，不纳入 Box 结果。真正 Box 对照扩展精度检查失败、x87 checksum 不同，不能当等价正确性性能目标。

V2 RichMan8 同 OS PID57637／Wine848 的四个人物有效冷暖对照，各40秒、lost0：首轮点击后宿主窗口约31.69–33.00 FPS，重复55.15–61.00 FPS；人物缺块两轮均存在。编译和上传计时不足以单独解释完整停顿，重复轮入向复制更多仍更流畅。

随后独立 FEXCore/SoftFloat ThinLTO 候选完整通过同一 CPU 测试；一次 x87 中位耗时约降低22%，游戏仍复现卡顿和缺块。游戏样本限制、安装身份及自动化记录见 fex-softfloat-lto-automatic-followup-20261006.md 与 workspace_temp/fex-softfloat-lto-20261006/README.md。

自动化测试结束已停止目标 bundle，保留应用数据，未启动 Steam、未修改持久配置。本轮没有 fetch、commit、push，所有已有 dirty 保留。
