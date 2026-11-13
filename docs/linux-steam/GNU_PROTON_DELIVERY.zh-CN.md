# GNU Proton 自动交付（1.5.2）

Linux ARM64 Steam 客户端、上游 Proton 运行包与鸿蒙适配层分别维护。HAP 保留已验证的 GE-Proton ARM64 运行包兜底；组件页支持独立下载安装、导入、版本选择及回退。首次准备 Steam 会校验适配层并注册兼容工具，已有 Steam 游戏库及用户选择保持独立，维护结束不会自动启动 Steam。

本版内置组件为社区 GE-Proton11-11 aarch64 的选择性闭包，非完整 SteamOS/rootfs。组件 983 个文件，压缩 116676942 字节，展开 432748944 字节；不含 Valve Steam 客户端、Windows Steam、游戏、账号、用户前缀或诊断 marker。GNU libc、Wayland 和媒体公共依赖复用已校验的 Linux Steam 支持运行库。游戏代码中的 x86/x64 仍由 GE 的 FEX 执行，ARM64 Steam 客户端不使用 QEMU。

## 安装与升级

1. `prepareLinuxSteamClientData` 在注册兼容工具前调用 `LinuxGnuProtonComponent.ensureReady`。启动状态展示提取百分比、解压、逐文件校验、启用和错误。
2. rawfile 压缩包及每个展开文件均校验 SHA-256、大小和路径，检查可用空间。可信归档只在专用 staging 解压，异步解压及每批校验让出 UI；不把整个压缩包读入 ArkTS 内存。
3. 只替换清单内的 package、graphics、locale、许可、适配库，以及明确指定的字体、locale helper 和 DXVK DLL。保留 prefix 的注册表、用户字体、home、Steam 游戏库和存档。
4. 激活前写事务，旧的托管文件先以 rename 保留。失败回滚；进程中断在下次准备时恢复。成功提交后删除安装 ZIP、staging 与旧托管文件，避免长期保存整份运行时副本。
5. 成功清单记录文件大小/mtime；常规启动复用未变化文件，变化文件重新哈希，缺失或损坏时重装。进行中的 Steam 会话不替换库。组件版本由内容标识决定，与 APP 版本号分开。
6. 只有完整安装通过才注册 GNU 工具，不能以 `manual-launch.version` 单一 marker 判定可用。Steam 自己保存全局和逐游戏的选择，已有 OHOS Wine 选项保留。失败明确提示，不静默换后端。

## 独立运行版本管理

入口：Steam 组件页 → Proton 运行版本。默认使用内置兜底，无需额外操作。维护前正常退出 Steam 和游戏；启动准备、适配层安装与活跃 GNU 子进程均有维护锁，Steam 意外退出也不会提前允许替换仍在运行的游戏库文件。

- **检查更新**：用户主动读取项目固定 HTTPS 版本列表，拒绝列表降级、身份冲突和未支持的架构/ABI。不在后台强制下载或切换版本。
- **在线安装／修复、导入此版本 ZIP**：按列表校验归档及逐文件 SHA-256，校验和空间检查全部通过才激活。暂停下载保留有界的断点文件。导入只接受列表内已知归档，不执行任意用户提供的 Proton。
- **已安装版本、回退**：通过同一文件系统的 rename 切换上游 package，保留当前和上一版本，支持移除其他闲置版本，最多保留 8 个。事务覆盖首次激活、普通切换、相同版本修复；中断时下次准备先恢复。切换不会复制整份前缀。
- **查找 Steam 中的 Proton**：只读扫描本应用 Linux Steam 游戏库的 common 目录。当前只接受文件哈希匹配已维护列表的 GE-Proton ARM64，按清单复制到私有版本目录后激活；已知上游库别名读取原始普通文件，不复制符号链接。不会改动 Steam 的原包；Steam 更新原包后需重新检查/导入。

目前目录仅发布 **GE-Proton11-11 ARM64**。版本切换/回退已用合成多版本文件系统回归，尚无第二个经过真机验证的版本。不支持直接启用普通 x86_64 Proton、任意 Proton Experimental 或其他未知 ABI。当前适配 ABI 为 `hmosdeck-ge-wine11-arm64ec-v1`，前缀 ABI 为 `wine11-arm64ec-v1`。这一阶段选择的是 GNU 后端的全局运行包；逐游戏 GNU/OHOS 后端仍交由 Steam 自身保存，并未新增一套冲突的游戏设置。

上游包指纹由其文件清单决定，独立于适配库版本。活跃路径保持 `steamohos-native/proton-probe/package`，其他版本存于 `proton-runtimes/versions/<id>`，选择及事务分别在 `selection.json`、`activation.json`。适配层升级仅核对/替换非 package 文件，旧的完整组件安装也能原地登记，无需另存一份。首次初始化状态会在版本切换后重新确认，但注册表、用户字体、home、游戏库和存档不属于运行包维护范围。回退仅允许相同前缀 ABI，不能撤销新 Wine 或游戏自身对前缀/存档的迁移；未来跨 ABI 版本需要独立前缀方案。

初期独立安装复用已发布的完整 `gnu-proton.zip`，只激活其中 package，上游字节不变，其余适配文件不会由独立更新覆盖。因此本轮无需重做 Linux Steam 客户端离线包或重发 GNU 二进制依赖。后续可发布只含上游的 ZIP，继续使用同一个运行身份。版本列表是固定项目 HTTPS 信任源及 SHA-256 校验，并非已实现离线签名验证的任意第三方仓库。

当前仍由自有启动器调用 GE Wine/FEX，接入既有鸿蒙 GPU/音频后端。没有完整执行上游 Proton Python、pressure-vessel 与 Steam Linux Runtime 流程；能够管理受支持运行包不代表任意官方 Proton 可直接由 Steam 安装后运行。

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

版本清单由 `scripts/build_proton_runtime_catalog.py --write` 从锁定上游文件生成，源码 feed 位于 `docs/linux-steam/proton-runtimes/catalog.json`，HAP fallback 清单位于 `rawfile/linux-proton/runtime-catalog.json`。新增已验证版本必须递增 revision，并保持当前内置运行包在 fallback 列表首项；同一 ID 不允许换字节。CI 校验清单一致性和输入已跟踪状态。

```sh
# 从已校验组件生成仅含上游的 ZIP；相同成果会复用。
python3 scripts/build_proton_runtime_catalog.py --write \
  --split-output /path/to/proton-runtime.zip \
  --archive-url https://github.com/yifengling0/VintagePomeloPro/releases/download/RELEASE/proton-runtime.zip
```

先上传不可变归档并核验公开下载哈希，再发布 feed。更改适配 ABI/图形通信协议仍需更新 HAP，不能只修改 catalog 伪装支持。上游运行包更新本身不要求重新发布 HAP；客户端离线包仍沿用已有兼容校验。

本轮新增主机回归覆盖运行版本独立性、旧安装登记、在线/本地/Steam 导入、哈希错误、空间不足、切换每阶段中断及同 ID 修复、上一版回退、闲置版移除、输入链接拒绝、会话/启动/游戏互斥、前缀与存档保留、原 Valve CDN 下载行为。CI 在 HAP 构建前执行实际 ArkTS 服务回归。此次未操作手机或平板，不将历史游戏结果视作新管理界面真机验收。

既有真实手机结果及最终 HAP/APP 身份记录在工作区交付 verification.json。不能将成功进菜单外推为所有游戏、硬件或完整 SteamOS 兼容。

## 首次初始化验收

真实手机空前缀初始化为 26,351 ms、退出码 0，无超时、无输出截断；测试完成后恢复原用户前缀。首次 Wine 初始化会多次修改线程执行权限，适配层每次执行页权限切换都重新请求 OHOS 策略，不能缓存线程曾获准执行的结果。保留真实 mprotect 错误，未将失败伪装为成功。

## 自动交付图形闭包回归

此前自动交付清单只枚举少数图形库，启用时整体替换 graphics 目录，遗漏原增量图形构建输出的 8 个 XCB/GLX 依赖，以及 GLX 的 Xwayland 和 libGL 产物。已存在的注册表项仍在，启动却停在注册程序触发的桌面/驱动初始化，不能将它归因于注册丢失。

打包现读取 proton-graphics-build.json 全部经哈希校验的产物，按实际加载名称交付，并递归核验图形/适配层 DT_NEEDED 依赖，只允许清单声明的共享 GNU 运行库满足外部依赖；不依赖开发设备残留文件。linux_proton_component_closure_test.py 对全部遗漏的 8 个库逐个移除，必须拒绝该闭包。此检查针对图形/适配层，不代表 USB、pcap、VR、TTS 等可选 Wine 后端都在 OHOS 可用。

准备辅助程序沿用游戏的显示驱动，不能同时禁用两个驱动并假设其依赖不会初始化桌面。每个辅助程序完成后重置单次桌面防递归状态；成功的 WoW64 注册缓存于私有前缀，失败保留真实阶段及超时数据。

## 本候选实际验收范围

自动交付后的手机候选已由正常 Linux ARM64 Steam 入口启动《仙剑四》（681820），进入中文主菜单及问答页面，可交互并正常退出，GNU waitpid 返回 0、子进程已回收。GPU 共享链路记录 9240 帧、pixel_readbacks=0；没有通过手工补库替代自动交付验收。

用户随后结束手机测试，《新仙剑》（695630）本候选的最终启动、交互及退出回归未完成，不将历史成功结果算作本轮通过。仙剑四退出后曾出现一次 Steam 内部 Unexpected Transport Error（0x3008），后续客户端会话恢复游戏库，根因尚未确认。该版本供阶段性测试，不宣称长期稳定性或全部游戏兼容已通过。当前 GNU DXVK 交付为 i386 DLL，64 位 DXVK 不在本轮验收范围。
