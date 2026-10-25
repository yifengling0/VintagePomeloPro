# GNU Proton 自动交付（1.5.2）

Linux ARM64 Steam 客户端与 GNU Proton 分开维护。客户端仍可官网下载或导入既有兼容离线包；GNU Proton 的锁定闭包和 OHOS 适配随 HAP 提供，首次准备 Linux Steam 时自动校验、安装，完成后注册兼容工具，不自动打开购买、下载或切换用户已选的后端。

本版内置组件为社区 GE-Proton11-11 aarch64 的选择性闭包，非完整 SteamOS/rootfs。组件 983 个文件，压缩 116676942 字节，展开 432748944 字节；不含 Valve Steam 客户端、Windows Steam、游戏、账号、用户前缀或诊断 marker。GNU libc、Wayland 和媒体公共依赖复用已校验的 Linux Steam 支持运行库。游戏代码中的 x86/x64 仍由 GE 的 FEX 执行，ARM64 Steam 客户端不使用 QEMU。

## 安装与升级

1. `prepareLinuxSteamClientData` 在注册兼容工具前调用 `LinuxGnuProtonComponent.ensureReady`。启动状态展示提取百分比、解压、逐文件校验、启用和错误。
2. rawfile 压缩包及每个展开文件均校验 SHA-256、大小和路径，检查可用空间。可信归档只在专用 staging 解压，异步解压及每批校验让出 UI；不把整个压缩包读入 ArkTS 内存。
3. 只替换清单内的 package、graphics、locale、许可、适配库，以及明确指定的字体、locale helper 和 DXVK DLL。保留 prefix 的注册表、用户字体、home、Steam 游戏库和存档。
4. 激活前写事务，旧的托管文件先以 rename 保留。失败回滚；进程中断在下次准备时恢复。成功提交后删除安装 ZIP、staging 与旧托管文件，避免长期保存整份运行时副本。
5. 成功清单记录文件大小/mtime；常规启动复用未变化文件，变化文件重新哈希，缺失或损坏时重装。进行中的 Steam 会话不替换库。组件版本由内容标识决定，与 APP 版本号分开。
6. 只有完整安装通过才注册 GNU 工具，不能以 `manual-launch.version` 单一 marker 判定可用。Steam 自己保存全局和逐游戏的选择，已有 OHOS Wine 选项保留。失败明确提示，不静默换后端。

本版没有独立远端 Proton 自动更新服务；更新 HAP 可交付匹配的适配层，旧 Steam 客户端离线包继续按既有身份与 ABI 校验规则使用，不因 Proton 改动而强制重做客户端包。

## 运行路径

- 默认 GNU 工具：D3D9/DirectDraw → WineD3D → GNU Mesa virpipe → 现有 OHOS virgl 接收端；D3D11 → DXVK 2.6 → GNU Venus → 现有 Vulkan 接收端。
- GNU WineD3D 工具：用于在 Steam 中主动选择 GL 路径。
- HmosDeck (OHOS Wine)：继续保留原后端，用户可在 Steam 兼容性页面选择。
- 游戏图像复用 NativeImage/NativeBuffer 的既有接收端，避免逐帧 CPU 读回；仍有接收端 GPU 合成，不是直接 scanout。
- 原 Wine 音频后端、FEX 浮点配置、locale/ACP 与字体别名在组件内统一交付，不按游戏名称打补丁。

首次使用会补齐空前缀依赖并执行有界初始化。`proton-x86-locale.exe --configure` 通过 Windows API 设置 NLS 和缺失字体替代，首次允许 120 秒并保留取消机制，成功缓存；实际游戏不会套用诊断阶段的 600 秒上限。

## 构建与核验

源码：`scripts/build_linux_proton_component.py`、`entry/src/main/ets/service/LinuxGnuProtonComponent.ets`；上游身份见 `gnu-proton-probe.lock.json`。二进制归档忽略 Git，生成清单随源码保留。构建器复用原 GE 归档、已验证适配产物、locale 和字体，拒绝不符合锁的输入，产生固定时间戳 ZIP、逐文件哈希与来源许可。

```sh
python3 scripts/build_linux_proton_component.py \
  --inputs /mnt/f/VintagePomelo-Workspace/workspace_temp/linux-proton \
  --wine-source /home/liufeng/src/wine-valve-linked-backup-20260926 \
  --runtime /home/liufeng/steamohos-client-20261019-external-v3-build/runtime
```

`scripts/run_linux_proton_component_tests.cjs <typescript目录>` 覆盖空安装、重复启动、并发准备、空间不足、ZIP/文件损坏、激活失败回滚、中断恢复、修复和用户数据保留。显式 `linux-proton / init-fresh` smoke 入口临时保留原前缀，用自动交付组件初始化空前缀，再恢复原前缀；中断后普通安装准备会先恢复原前缀。

真实手机结果及最终 HAP/APP 身份记录在工作区交付 verification.json。不能将成功进菜单外推为所有游戏、硬件或完整 SteamOS 兼容。

## 首次初始化验收

真实手机空前缀初始化为 26,351 ms、退出码 0，无超时、无输出截断；测试完成后恢复原用户前缀。首次 Wine 初始化会多次修改线程执行权限，适配层每次执行页权限切换都重新请求 OHOS 策略，不能缓存线程曾获准执行的结果。保留真实 mprotect 错误，未将失败伪装为成功。

## 自动交付图形闭包回归

此前自动交付清单只枚举少数图形库，启用时整体替换 graphics 目录，遗漏原增量图形构建输出的 8 个 XCB/GLX 依赖，以及 GLX 的 Xwayland 和 libGL 产物。已存在的注册表项仍在，启动却停在注册程序触发的桌面/驱动初始化，不能将它归因于注册丢失。

打包现读取 proton-graphics-build.json 全部经哈希校验的产物，按实际加载名称交付，并递归核验图形/适配层 DT_NEEDED 依赖，只允许清单声明的共享 GNU 运行库满足外部依赖；不依赖开发设备残留文件。linux_proton_component_closure_test.py 对全部遗漏的 8 个库逐个移除，必须拒绝该闭包。此检查针对图形/适配层，不代表 USB、pcap、VR、TTS 等可选 Wine 后端都在 OHOS 可用。

准备辅助程序沿用游戏的显示驱动，不能同时禁用两个驱动并假设其依赖不会初始化桌面。每个辅助程序完成后重置单次桌面防递归状态；成功的 WoW64 注册缓存于私有前缀，失败保留真实阶段及超时数据。

## 本候选实际验收范围

自动交付后的手机候选已由正常 Linux ARM64 Steam 入口启动《仙剑四》（681820），进入中文主菜单及问答页面，可交互并正常退出，GNU waitpid 返回 0、子进程已回收。GPU 共享链路记录 9240 帧、pixel_readbacks=0；没有通过手工补库替代自动交付验收。

用户随后结束手机测试，《新仙剑》（695630）本候选的最终启动、交互及退出回归未完成，不将历史成功结果算作本轮通过。仙剑四退出后曾出现一次 Steam 内部 Unexpected Transport Error（0x3008），后续客户端会话恢复游戏库，根因尚未确认。该版本供阶段性测试，不宣称长期稳定性或全部游戏兼容已通过。当前 GNU DXVK 交付为 i386 DLL，64 位 DXVK 不在本轮验收范围。
