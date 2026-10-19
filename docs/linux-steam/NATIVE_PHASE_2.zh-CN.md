# Linux ARM64 Steam：原生 GNU 执行第二阶段

日期：2026-10-18。实施树 `/home/liufeng/src/vpp-account-cloud`，分支
`feature/account-cloud-membership-mvp`。开始基线 `b84e7db1`；收尾时 HEAD 为
`31cf44c3`（另一个媒体改动提交）。本阶段未执行 commit、stage、reset 或 push，
工作树中已有改动保留；候选包实际输入以 identity 清单为准。

## 结论

在普通签名 HAP 的 USB 平板 MLR-AL10 中，真实 GNU AArch64 主程序已经完整执行。
原生运行 main、TLS、pthread、dlopen、dlvsym、析构及退出，两轮结果一致。
没有 QEMU、TCI、指令解释或跨架构转译，没有修改 Steam 文件。
**Steam 本身尚未启动。** 本阶段解决 GNU 程序执行前提，不等于客户端已可用。

第一阶段是 OHOS Wayland 客户端；本阶段暂未运行 GNU Wayland 客户端、Xwayland、
CEF、GNU Vulkan 或 Proton。后续应逐层验证，不能将现有 OHOS musl 库直接用作
GNU 依赖，也不能从原生 ARM64 执行推断官方 Proton 可原样运行。

## 执行结构

独立默认关闭开关 `WINEHUA_ENABLE_STEAMOHOS_GNU_EXEC` 创建诊断 NCP 库和异步
NAPI。正常产品构建不包含这些入口，既有 Wine/FEX/DXVK/Steam 启动路径未接入
该执行后端。套件 `linux-steam-gnu` 不要求 Wine 引擎 ready。

官方 NCP 启动隔离的 OHOS 子进程，再 fork 一次性执行进程。父 NCP 保留原有
运行时与 TP，负责有界收集 stdout/stderr、超时、waitpid 和回收。执行进程关闭
继承的应用 FD，重置信号状态，用独立带 guard 的 RW 栈构造 argv/envp/auxv，
原生分支到 GNU rtld 的入口。GNU rtld 自行初始化 GNU TLS、加载配套 libc 与
主程序；此后不返回 OHOS 栈，也不调用宿主 libc。

GNU rtld 的 RX 段始终映射已安装、签名的原文件；没有匿名可执行页或 W→X。
没有把第二份 glibc dlopen 进 UI/Wine 线程，没有手动替换宿主 TP。父 NCP 每次
比较 TP，确认未变。执行失败、退出 37、SIGKILL 超时都按实际 waitpid 结果记录。

## 平台差异与修正

1. 安装代码目录中的 ELF 为 0644，直接 kernel execve 返回 EACCES。只在本应用
   私有诊断目录制作字节相同的 loader 副本并设为 0500，对照仍返回 EACCES；
   临时副本自动清理，没有修改安装库、签名身份、APL 或系统策略。
2. 同一已签文件的单页 RX 映射通过，但 GNU rtld 将整个虚拟跨度映为 RX 的
   预映射返回 EACCES。libc 文件本身大于该跨度，因此不只是 EOF 问题。
   glibc 补丁 0001 改为匿名 PROT_NONE 预留地址，再按真实 PT_LOAD 范围分别
   映射文件段，保留对齐与空洞。原生 loader 还按 Linux padzero 语义清零 BSS
   最后一页直至页末，避免 GNU early allocator 读到文件尾残余数据。
3. 实机捕获 SIGSYS syscall=293（rseq）。使用 glibc 官方 tunable
   `GLIBC_TUNABLES=glibc.pthread.rseq=0` 关闭这个可选优化。
4. 线程创建走 clone3 时仍被 sandbox 中止。该次未捕获到具体 syscall 编号，
   不能写成直接观察到了 435。OHOS 专用 GNU runtime 补丁 0002 令
   `__clone3_internal` 返回 ENOSYS，由已有 glibc fallback 调用真实 legacy
   clone，保留 TLS、parent_tid、child_tid；替换配套 libc 后双线程测试通过。

两份补丁位于 `patches/glibc/`，仅用于独立 GNU runtime，未修改系统 libc。
上游 glibc 2.38 源码包 SHA256：
`fb82998998b2b29965467bc1b69d152e9c307d2cf301c9eafb4555b770ef3fd2`。
源码来源 `https://ftp.gnu.org/gnu/glibc/glibc-2.38.tar.xz`，构建 manifest 保存
编译器、配置、原包及补丁哈希。COPYING.LIB、LICENSES 随候选包提供，源码包和
补丁在本机证据与源码树保留；对外分发时仍需交付相应源码及构建材料。

## 实机验收

最终候选 `native-legacy-clone-debug-signed.hap`，结果
`tablet-native-gnu-10.json`、`tablet-native-gnu-11.json` 及对应 summary：

| 检查 | 两轮实际结果 |
| --- | --- |
| 后端 | native_aarch64 / file_backed_gnu_rtld |
| GNU 主程序 | gnu_main_verified，exit 0 |
| 主程序 local-exec、初始 DSO initial-exec TLS | 通过 |
| 两工作线程 TLS 与 errno 隔离 | 通过 |
| 创建线程后 dlopen DSO 的 TLSDESC TLS | 主线程及两工作线程隔离通过 |
| dlvsym 版本符号 | STEAMOHOS_NATIVE_1.0 通过 |
| barrier、join、pthread key destructor | 通过，两次 key destructor |
| 非零退出对照 | exit 37，已回收 |
| 超时对照 | timedOut=true、signal=9，已回收 |
| 父 NCP TP | 全部未变 |
| NCP 退出 | callback 已验证，signal=0 |
| suite-summary | PASS / PASS |
| Steam | steamClientStarted=false |

最终源码的本机 x86_64 原生 Linux 参考测试也通过：kernel exec、rtld handoff、
真实 GNU main/TLS/线程/符号、FD 清理、输出上限、非零退出与超时回收。未用 QEMU。
默认 OFF 的最终 smoke_napi 增量编译及链接通过，未导出 GNU 或 host probe。

## 可复现构建与包身份

脚本依次使用：

```sh
python3 scripts/build_linux_steam_glibc.py --archive <glibc-2.38.tar.xz> \
  --cc <aarch64-none-linux-gnu-gcc> --output <new-glibc-dir> --jobs 16
python3 scripts/build_linux_steam_gnu_fixture.py --cc <aarch64-none-linux-gnu-gcc> \
  --glibc-build <new-glibc-dir> --output <new-runtime-dir>
```

只在独立诊断快照的 build-profile 打开 HOST 与 GNU_EXEC 两个开关，对
`gnu-native/**` 禁止 strip。先用官方 Hvigor 构建 unsigned HAP，再执行：

```sh
python3 scripts/package_linux_steam_gnu_fixture.py --hap <unsigned.hap> \
  --runtime <new-runtime-dir> --output <new-prepared-unsigned.hap>
```

该脚本核对 manifest 与库字节，将 Hvigor 忽略的无扩展名可执行文件按真实原名
放在 libs/arm64-v8a/gnu-native 下，随后才执行官方签名。未篡改已签包、未将
程序伪装成 .so。本机核查官方 CodeSigning.isNativeFile 对 libs/ 下条目签名，
安装器实际部署原名程序，RX 映射通过。

启用候选套件：

```sh
hdc -t <tablet> shell aa start -b com.vintage.pomelopro -a EntryAbility \
  --ps winehua.mode smoke --ps winehua.suite linux-steam-gnu \
  --ps winehua.run_id <unique-run-id>
```

用 `file recv -b com.vintage.pomelopro` 取回应用目录
`/data/storage/el2/base/files/.wine/drive_c/smoke/results/<unique-run-id>/` 的
`linux-steam-gnu.json` 与 `suite-summary.json`。不以 shell cat 的权限错误判断失败。

本机证据目录 `F:/VintagePomelo-Workspace/artifacts/steam-linux-arm64-native-20261018`。
`native-legacy-clone-identity.json` 记录候选源码与 runtime 来源；
`final-verification.json` 确认 26 项源码输入与全部 5 个 ELF 哈希一致，原 14 项
暂存 diff 未变。报告与 README 的收尾文档不属于此前已签候选的代码输入。
本轮保存早期失败对照，未卸载应用、清数据、操作手机或更改系统权限。

## 后续的实际门槛

- 原生 rtld handoff 尚非完整 kernel exec 语义：/proc/self/exe、内核 auxv 与
  进程元数据仍来自 NCP，虽用户栈 auxv 足以启动本测试程序，仍需专门兼容验证。
- 成功映射的是签名安装代码目录。可写 data 中下载的 GNU code 不能据此假定可
  RX 映射；Steam 自更新、下载依赖和 CEF 子进程路径仍是独立关键门槛。
- 尚未验收 C++ thread_local 析构、完整文件/网络、GNU FD/SCM_RIGHTS/Wayland。
  下一阶段先用真正 GNU libwayland-client 做建窗、SHM 及 FD 往返。
- 随后是 Xwayland/X11、CEF 多进程、原版 ARM64 Steam 登录与库。
- GNU Vulkan/GL/EGL 与 OHOS 驱动的 ABI 桥、Steamworks 及现有 Wine/FEX 互操作
  独立推进。Windows x86 游戏仍需要已有 FEX/Wine 路径；ARM64 Steam 原生执行
  不消除这些游戏自身的跨架构需求，也没有排除以后接入 DXVK/Proton 的可能性。
