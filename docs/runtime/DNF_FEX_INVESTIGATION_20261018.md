# DNFLauncher / ARM64 Wine + FEX 调查（2026-10-18）

启动死锁的确定根因已修复，启动器可以进入选区和登录界面。服务器连接失败尚未修复，未进入游戏测试帧率。不能把独立 TLS 测试通过当作游戏登录通过。

## 已修复：读取自身上下文时挂起自身

OHOS fork 的 `wow64_NtGetContextThread` 在软件 CPU 后端无条件挂起目标。DNFLauncher 请求自身上下文时把自己挂起，无法执行恢复操作；日志明确出现 `Suspending self`。

产品补丁 `patches/wine/0065-wow64-self-context-no-suspend.patch` 比较线程对象，涵盖复制句柄和 OpenThread。自身请求交给 CPU 后端刷新并读取上下文；其他线程行为和权限检查保留。构建脚本已接入。没有加入强制恢复其他线程的逻辑。

平板 8 项回归全部通过：自身伪句柄、GET_CONTEXT-only 复制自身句柄、OpenThread 自身、另一个挂起线程、无效句柄、缺少 GET_CONTEXT 权限、另一个运行线程、ALL_ACCESS 复制自身句柄。检查挂起计数、错误码及其他线程最终完成状态，结果 `PASS failures=0`。原版旧 5 项对照中，自身伪句柄及 OpenThread 自身均超时。

候选 ARM64 wow64.dll SHA-256：`60365a4979607ae34aa88cfc77a6e7ac649e8fd3228e2294dcad4c5d53218a51`。与原件的 28 个导出、唯一 DLL 依赖 ntdll.dll 一致。

## 新旧 DNS / TLS 对比

旧 main Wine gitlink 为 `1cb1ac93e464fe6db019845f3451f50ef6fc78f4`；新版 wine-valve HEAD 为 `cd547f7a0ee9d3d59b3ec47317a7852dedf0443b`。

- resolver 没有相关行为差异，差异主要是版本资源和 WIN32_NO_STATUS 声明。
- 两边均保留 Schannel 的 `NORMAL:-CHACHA20-POLY1305:%LATEST_RECORD_VERSION` 配置和 TLS 1.0–1.3 支持。
- 旧 main 包含新版缺少的 CNG/NCrypt 客户端证书修复，属于 mTLS；本次没有证据表明启动器使用该路径，未移植。
- WinHTTP 存在较多源码差异，但实际启动器未加载 WinHTTP、WinINet、SChannel，不能靠差异数量归因。

设备 ARM64、x86 测试都能解析 a4club.org 并连接 8443；普通 HTTPS 对照返回 200。独立 WinHTTP 对目标握手失败，不代表游戏服务器不可用。

OpenSSL 发现该目标发送 SNI 时断开，不发送 SNI 时可完成 TLS 1.3 并校验证书和域名。但是启动器实际 ClientHello 没有 SNI，因此不能用这个差异解释游戏失败，也没有添加全局禁 SNI 或协议回退。

## 启动器实际握手与独立对照

进程内 PEB 模块枚举及固定静态字符串确认启动器内置 Mbed TLS。取证期间 BCrypt 只观察到随机数请求。临时诊断仅记录公开 ClientHello / ServerHello、记录头和长度，不记录登录请求、私钥、会话密钥或解密内容。

实际过程：DNS、TCP 成功；发送 200 字节 ClientHello（TLS 1.3、无 SNI、X25519）；服务器选择 `0x1303`（ChaCha20-Poly1305）；客户端收到 ChangeCipherSpec 和第一条长度 23 的加密握手记录后，发出长度 2 的 TLS alert 并断开重试。

后续专用诊断文件确认 alert 为 level=2、description=20，即 `bad_record_mac`。通过启动器内置 Mbed TLS 已有 debug callback，仅保留函数标识和非零数字返回码，取得错误链：`mbedtls_cipher_auth_decrypt_ext = -0x6300`（认证失败），继而 `ssl_decrypt_buf / ssl_get_next_record / mbedtls_ssl_read_record = -0x7180`（INVALID_MAC）。尚未确定是派生密钥、握手摘要、内置库配置、保护代码还是某条未覆盖的模拟路径导致认证失败。这不是证书校验错误，不能用禁用证书、全局禁 SNI、禁 ChaCha 或减少 TLS 版本来替代根因修复。

在同一个启动器进程内，使用独立测试上下文调用其原有 ChaCha20-Poly1305 实现，执行 RFC 8439 §2.8.2 的公开已知答案：有效标签成功且明文完全一致；修改标签后返回 `-0x56` 且输出清零，两项均通过。未修改游戏文件，也未改变协议协商、证书或支付等产品逻辑。为定位调用点只保存原始只读代码节（RVA 0x1000，长度 0x5cd48），不保存数据节、堆栈、密钥或解密后的连接数据。该检查覆盖这组向量，不能保证整个握手或所有指令正确。

本轮新证据：`dnf-alert2-codes.txt`（PID 384）、`dnf-mbed-returns.txt` 和 `dnf-embedded-public-vector.txt`（PID 412）。Wine 日志为追加文件，第一次尝试时实际进程 WINEDEBUG=-all；其中历史 VP_TLS 行不计作本轮新结果。后续改用上述有 PID 的专用文件。

独立 Mbed TLS 3.6.5 及固定 framework 子模块只放在忽略的任务目录，不是产品依赖。

| 执行路径 | 加密已知答案测试 | 目标 TLS 1.3 握手 |
| --- | --- | --- |
| ARM64 PE 原生 Wine | 30 组通过 | ChaCha20、AES-128、AES-256 全通过 |
| Clang / llvm-mingw x86，FEX | 30 组通过 | 三种算法全通过 |
| x86，开启 SSE2/MMX | 30 组通过 | 三种算法全通过 |
| MSVC x86，开启 SSE2/MMX，FEX | 30 组通过 | 三种算法全通过 |
| Windows 主机 x86 | SSE2 / MSVC 自测通过 | Clang 对照三种算法全通过 |

握手程序不发送应用请求；保留证书链和域名校验，成功结果的 verifyFlags 均为 0。仅对独立测试库关闭 SNI 扩展，以匹配目标服务的可用路径。产品证书策略未变。

这些结果证实当前 FEX/Winsock 能与目标完成现代 TLS 握手，但没有覆盖启动器内置版本、编译选项、保护代码和全部指令路径。现已取得具体认证失败错误，但尚无可独立复现的底层缺陷；公共默认值保持原状。

单次关闭 FEX 优化、隐藏 hypervisor bit 的对照没有观察到握手改善，未写入持久配置。WOW64 已经不暴露 AVX，disableavx 对照不能作为有效 A/B 证据。没有全局开启 full SMC、关闭优化或更改 CPUID。

## Steam 兼容与交付边界

没有修改游戏文件，没有重建旧 main 环境，没有安装新 HAP。设备保留已验证的两份 ARM64 wow64.dll 修复。临时 x86 ws2_32.dll 已恢复为原件，两份 SHA-256 均为 `67b9593dbd35afbfdf05970d071ee91678c638b9563201e63ca942306bdc5805`。 后续 alert / Mbed 返回码诊断也已结束，再次 force-stop 后恢复两份原件并按默认配置冷启动，哈希复核见 `alert-restore-hashes.txt`。诊断回调仅存于已结束的进程，没有持久化开关。

Steam 专用配置、DNS/TLS 默认值、证书策略、FEX 模块、既有线程退出及图形修复均未改动。平板标准路径没有 Steam 安装，本轮没有完整 Steam 启动 / 登录回归，不能声称该项通过。

候选 runtime rawfile：`/home/liufeng/src/vpp-account-cloud/workspace_temp/dnf-wow64-runtime-20261018/rawfile`。

以 10 月 17 日星露谷修复基线生成，只替换 `bin/aarch64-windows/wow64.dll`，其余 2359 项逐项不变。更新 payload hash、content 版本和 wow64Context 身份，保留 sharedAudio、threadExit、graphicsMigration、textureCompatibility 等记录。新 payload SHA-256：`3ef0ae8de1d757a33fb6e02114b005135772354793fd2884f39c225356f8a507`。

该目录不是完整 HAP；后续打包应搭配同一 10 月 17 日基线的 native 库，不得使用主树较旧的生成库。生产构建从产品补丁入口重建，隔离源码中的 socket 诊断代码不进入产品。

验证包括：8 项设备回归、补丁对干净 HEAD 的 dry-run、build_wine.sh 语法、Wine 构建身份测试、PE 导出 / 依赖对照、payload 全成员校验。本轮未对外发送诊断日志。

尚未完成：启动器内置 TLS 的兼容修复、实际登录及游戏性能采样、最终完整候选包的 Steam 回归。此前选区旧画面偶发停留也未查明根因，不能声称 compositor 已源码修复。

本地证据：`F:/VintagePomelo-Workspace/artifacts/tablet-dnf-20261018`。截图可能含用户登录信息，不对外附图。命令审计为 commands.jsonl。
