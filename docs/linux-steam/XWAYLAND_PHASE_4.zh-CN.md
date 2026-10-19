# Linux ARM64 Steam：Xwayland / X11 第四阶段

日期：2026-10-19。实施树 `/home/liufeng/src/vpp-account-cloud`，分支
`feature/account-cloud-membership-mvp`，HEAD `31cf44c3`。

## 结论与路线

普通签名 HAP 的 USB 平板 MLR-AL10 已运行真正 GNU AArch64 Xwayland 24.1.3，
真实 GNU XCB 客户端五轮完成建窗、18 帧交替色绘制与像素读回；第七轮屏幕截图
实际显示了对应橙色窗口。服务端、客户端和 NCP 均正常退出并回收。

链路是 `GNU X11 客户端 → GNU Xwayland → 已有 OHOS Wayland 合成器`。
现有 Wayland 显示栈继续使用，本轮没有改动合成器协议或渲染实现。
无 QEMU、TCI、指令解释或跨架构转译；未修改或启动 Steam。

实验室已有静态审计的 `steamclient.so`、`steamwebhelper`、`libcef.so` 存在
libX11 等真实 DT_NEEDED。原包需要先满足这些动态库依赖；这本身不能证明所有
执行路径都使用 X11，也未完整排除 CEF 的原生 Wayland 后端。Xwayland 是当前
保留原包、复用已有合成器的兼容选择，不是把整个产品切回 X11。

审计来源：`F:/Gits/steamos/steam-ohos-lab/reports/winehua-host/2026-10-18-integration/evidence/winehua-source-inventory.json`
中的 `steam_needed`；本轮未重新获取原版 Steam ELF。

## 实现

- `scripts/build_linux_steam_xwayland.py` 校验四份上游源码归档及 41 个 Debian
  bookworm ARM64/all 包，只解压到私有构建目录，不安装系统包、不使用 Debian
  libc。沿用阶段 2 的配套 glibc 2.38 与阶段 3 的 GNU Wayland 运行时。
- 原生交叉编译 Xwayland、上游 xkbcomp 编译器共享库及真实 GNU XCB 小程序。
  按 DT_NEEDED 收集并校验 AArch64 依赖闭包，最终 32 个 ELF。
  Debian 包校验依据固定 Packages.xz 索引与 SHA256，本轮没有验证 Release 签名。
- 本阶段关闭 glamor、GLX、DRI3、DRM 后端与输入线程，采用 rootful SHM 软件输出。
  DRM 开发包仅补齐上游公共结构所需的类型头文件，不代表设备 DRM 后端已接通。
- `xwayland_probe.cpp` 由隔离的 OHOS NCP 监督：私有 0700 会话目录、AF_UNIX
  listener、displayfd 就绪握手；仅显式传入 listener 和 ready-write FD。
  GNU 父子各自使用自己的 libc，不将 GNU 库加载进 OHOS 运行时。
- 私有套接字没有 TCP 监听。测试使用 `-ac`，访问范围受应用沙箱及独占目录限制。
  displayfd 确认就绪后才运行客户端，超时有界，退出后清理会话文件和 FD。
- `NativeExecOptions` 增加显式 FD 白名单与有界父侧任务，原调用默认不变。
  子程序执行期间，父 NCP 可顺序启动客户端，无需多线程并发 fork。
- `scripts/package_linux_steam_gnu_fixture.py` 校验明确依赖集合、哈希和架构，
  将真实原名的 Xwayland / X11 程序加入 unsigned HAP，再由官方工具签名。
  所有可执行映射仍来自签名安装代码目录；可写 data 仅放 XKB 数据与会话文件。
- XKB 数据 ZIP 随诊断包保存，本次用 HDC 将同一份纯数据送入本应用私有目录。
  **自动解压部署尚未接入产品**；不能将本次手动数据准备写成正式产品安装流程。
- 新增默认关闭的诊断入口 `probeLinuxSteamGnuXwayland` 和
  `linux-steam-gnu-xwayland` 套件。没有 Linux Steam 正式产品入口。

## 两处实机失败与修正

1. 初版实机返回 `STEAMOHOS_SIGSYS syscall=147`，即 AArch64 setresuid。
   Xserver 的身份检查走了尝试 seteuid 的兼容分支。补丁 0002 改为使用真实
   getresuid/getresgid 查询已保存身份；Fopen 在 real/effective uid 已相同时直接
   打开文件。未更改应用身份、APL、系统权限或沙箱策略。修正后进入键盘初始化。
2. 第二版键盘初始化崩溃。Xwayland 与 libxkbfile 的动态符号中均有 XkbInternAtom，
   服务端与 Xlib 版本参数签名不同。把编译器直接链接到服务器的全局命名空间会
   引入冲突。补丁 0001 改为只在 fork 出的短生命周期 GNU 子进程内使用
   `dlopen(RTLD_LOCAL | RTLD_DEEPBIND)` 加载编译器及其 Xlib 依赖，按该依赖组
   自身的 ABI 绑定符号。保留真实 xkbcomp 解析和 XKM 输出，不绕过键盘初始化。
   实机最终五轮均通过，未再发生该崩溃。

补丁路径：

- `patches/xwayland/0001-ohos-native-keymap-compiler.patch`
- `patches/xwayland/0002-ohos-query-credentials-without-changing-identity.patch`

上游 Xwayland 的常规路径保留；上述调整仅在诊断专用宏
`STEAMOHOS_NATIVE_XKBCOMP` 下启用。编译器子进程设置父进程死亡 SIGKILL。

## 实机证据

证据目录：`F:/VintagePomelo-Workspace/artifacts/steam-linux-arm64-xwayland-20261019`。
最终安装候选：`native-xwayland-v3-debug-signed.hap`。
对应 GNU 构建树：`/home/liufeng/steamohos-xwayland-20261019-v4/runtime`。

| 用例 | 实际结果 |
| --- | --- |
| native-xwayland-03 | PASS，18 帧，server/client exit 0，NCP signal 0 |
| native-xwayland-04 | PASS，18 帧，server/client exit 0，NCP signal 0 |
| native-xwayland-05 | PASS，18 帧，server/client exit 0，NCP signal 0 |
| native-xwayland-06 | PASS，18 帧，server/client exit 0，NCP signal 0 |
| native-xwayland-07 | PASS，18 帧，server/client exit 0，NCP signal 0；屏幕实际显示 |

HDC 取回本应用诊断数据目录后，未发现遗留的 x11-* 会话目录。

五轮均 `displayReady=true`、`x11Verified=true`、服务端与客户端
`hostTpUnchanged=true`、无超时。每轮服务端约 2.8 秒。
`tablet-x11-visible-07.png` 是第七轮实际截图的测试窗口裁剪；第四轮截图仍停留在
应用列表，第五轮截图处于页面切换，第六轮截图未进入桌面，均不作为可见画面证据。

服务器有 X11 最大 keycode 255 与虚拟修饰键重复的警告，但正常完成真实键盘映射
编译；本轮**未验收实际键鼠输入**，不能以警告或初始化通过推断输入已完成。
悬浮 FPS 目前不统计此 GNU 客户端，截图显示 0 FPS 不代表窗口没有显示。

最终候选还回归阶段 2 的 GNU main/TLS/双线程/动态加载、exit37 与超时 SIGKILL；
GNU Wayland 回归 PASS：48 帧提交、48 次释放、48 次 frame callback，FD 4→4，
NCP signal 0；实际按键仍未验收。结果见对应 regression JSON。
本机原生 Linux 测试通过 FD 白名单正例、未传 FD 关闭、重复/无效/过量 FD 拒绝、
父侧任务、父 FD 的 CLOEXEC 保持、退出与回收；既有 host 协议夹具也通过。
默认 OFF 的 smoke_napi 编译及链接通过，动态符号未包含诊断入口。

`candidate-verification.json` 核对 30 个候选源码输入和签名包中 32 个 ELF 全部一致；
已有 14 项暂存 raw diff 保持不变。本轮没有 stage、commit、reset、push、卸载、
清数据或操作手机。另一个任务的既有工作树修改保留。

## 复现与后续关口

```sh
python3 scripts/build_linux_steam_xwayland.py --artifacts <locked-sources-and-debs> \
  --cc <aarch64-none-linux-gnu-gcc> --base-runtime <phase3-runtime> \
  --wayland-build <phase3-build-root> --output <new-output> --jobs 12
```

诊断快照打开 HOST / GNU_EXEC 后构建 HAP，使用 package 脚本准备 unsigned HAP，
再官方签名安装；不得直接修改已签包。运行：

```sh
hdc -t <tablet> shell aa start -b com.vintage.pomelopro -a EntryAbility \
  --ps winehua.mode smoke --ps winehua.suite linux-steam-gnu-xwayland \
  --ps winehua.run_id <unique>
```

套件结果与 summary 位于本应用
`files/.wine/drive_c/smoke/results/<unique>/`，使用带 bundle 的 HDC file recv 获取。
窗口只存在约三秒；列表显示稳定后进入 Wine 桌面，才能取得实际可见截图。

下一步按以下独立关口继续，不以本次小程序结果外推 Steam 可用：

1. 标准 GNU Xlib/XCB 的 DISPLAY 与 Unix 套接字发现。本次小程序明确使用私有
   `xcb_connect_to_fd`，尚未解决原包通常使用的 `/tmp/.X11-unix/Xn` 路径。
2. 原版 Steam / CEF 动态依赖与文件、网络；当前只有静态审计。
3. GNU rtld handoff 的 `/proc/self/exe`、内核 auxv 与重执行语义；子进程 kernel
   execve 在本应用仍受限，CEF 多进程、崩溃处理器与 JIT 需独立验证。
4. GNU GL/EGL/Vulkan 到 OHOS 驱动的 ABI 桥与 GPU 加速；本阶段只有 SHM。
5. Steam 登录、库、更新及签名代码部署路径，随后才是 Wine/FEX、DXVK/Proton 互通。

ARM64 Steam 原生执行不等于 Windows x86 游戏原生执行；后者仍依赖已有游戏后端。
