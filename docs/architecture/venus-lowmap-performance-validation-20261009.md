# Venus WOW64 共享映射修复与真机性能验证（2026-10-09）

本轮已找到并绕过一个 OHOS 适配瓶颈：Venus 的共享 fd 可以 mmap，但平板沙箱对同一 fd 的 fstat 返回 EACCES。Wine 无法通过 inode/device 查找 backing fd，因而为 PE32 创建 CPU shadow，在 QueueSubmit 时重复复制。让 Venus 直接使用现有 Wine 管理的低地址共享映射后，资源上传／回读探针通过；同一存档、相同配置的 OFF→ON→OFF→ON 对照为 18.52→37.59→17.75→37.53 FPS。

这是本轮实测的特定游戏与场景结果，不能推广为所有游戏或手机的性能倍率。实验默认关闭，未改产品 Direct 自动选择、FEX/x87 或 WineD3D/VirGL 默认设置。

## 设备与构建身份

- 设备：此前同一 MatePad Mini USB 平板，Maleoon 910；手机离线，未做手机验证。
- 仓库：WSL `/home/liufeng/src/vpp-proton`，`feature/main_proton`，基础 HEAD `4cb9e7c8`，含前序未提交改动；本轮没有提交／推送。
- 当前版本名称／版本号沿用 `1.4.5.29 / 1004038`。
- 测试包：`venus-lowmap-signed-20261009.hap`。
- SHA256：`15efc9dc2738930816335a24dc14378a7e232c01752404bd0c4ad7867c523b17`。
- runtime content：`bee36a048201da89`；Wine `cd547f7a0e`，FEX `86ff33bbe2`。
- 新 Venus `.text` SHA256：`d68a94996a465fabf74d4d5e9faa4c8881dbdc462b35e396581aad239799789d`。
- `libentry.so`、`ntdll.so`、`win32u.so`、`winevulkan.so` 的 `.text` 均与前一 `venus-native-present-signed-20261009.hap` 相同，详见 `venus-lowmap-build-identity-20261009.json`。

只重编 Mesa Venus。Hvigor 在本轮没有注册 PackageHap 任务，因此没有冒充完整 HAP 构建通过：测试包基于核验过的既有未签名 HAP，替换新 Venus、匹配的 wine-data.zip／manifest／runtime 缓存标记，再重新签名。HDC 覆盖安装成功。完整正式发布构建仍需单独验证。

## 实际失败原因与修复

关闭实验的 PE32 资源探针日志：

```text
winehua venus map: sample=0 fd=45 size=16777216 ptr=0x7001901000 wine_owned=0 map_errno=0 fstat_result=-1 fstat_errno=13 inode=0 dev=0
WineHua: WOW64 could not open backing fd for map 0x7001901000 vma=1
WineHua: WOW64 copying vkMapMemory 0x7001901000 -> 0x2500000 (CPU fallback)
```

开启后：

```text
winehua venus map: sample=0 fd=47 size=16777216 ptr=0x2500000 wine_owned=1 map_errno=0 fstat_result=-1 fstat_errno=13 inode=0 dev=0
```

开关两侧的 fstat 均被拒绝；新路径使用 BO 已掌握的准确 fd，不依赖 `/proc/self/map_files` 或按 inode 猜测 fd，没有放松身份匹配或尝试映射任意 fd。采到的五个探针 BO 均返回 Wine 管理的低地址共享视图。游戏诊断会话也采到同样的 `wine_owned=1` 映射。

Wine 的 WOW64 remap 对已能用 32 位表示的指针直接返回，所以这些映射无需注册 CPU shadow copy list。保留现有 guest→GPU 上传／脏范围发布／回读同步；“消除复制”在本报告中特指 Wine→Venus 的重复 CPU 副本，不能称整个 Vulkan 路径零复制。

Wine `winehua_wow64_flush_copies()` 对仍存活的 non-precise CPU shadow 复制有效 map_size。如果五个 16MiB BO 都映射整个范围并同时存活，一次 flush 即可复制 80MiB；多次提交会放大。这个例子解释数量级，本轮没有测得游戏每帧的完整复制字节计数。

新补丁 `patches/mesa/0004-venus-wine-owned-low-map.patch`：

1. 仅 OHOS ARM64 的 vtest BO 尝试发现已加载的 `ntdll.so`（RTLD_NOLOAD），必须同时找到 v1 分配和释放接口。
2. 调用 `winehua_map_shared_buffer_v1(fd, size)`，由 Wine 持有完整虚拟分配并保证整个范围低于 2GiB。缺接口、PE64、地址空间不足或映射失败时回退到原 mmap。
3. 每个 BO 保存配对释放回调；缓存重用不重复分配，destroy 使用相同所有者释放，不能普通 munmap Wine 的分配。
4. 诊断默认关闭，最多输出每进程 24 个 map 样本；fstat 仅在诊断中调用，记录的是 BO 的真实 fd。
5. 在生产 Mesa patch runner 注册，并缩小 0001 对结构体和错误日志的修改上下文，避免新增 BO 字段和修正错误日志使重复应用误判。0001 的功能未改变。

现有 Wine ABI 来自 `0040-ntdll-owned-virgl-shared-low-map.patch`，本轮没有改 Wine 或 FEX，也没有重用旧 Wine 对象。

## 正确性与资源探针

同一个 `C:\vp-gumu10-evidence\direct-dxvk32-smoke.exe`，两侧都是 Venus + legacy DXVK。两侧均通过缓冲上传／GPU 回读四轮，以及纹理上传／回读四轮，所有 `bad=0`、`RESULT PASS`。120 次 Present 的耗时如下：

| 设置（诊断开启） | 120 次提交耗时 | 用途 |
| --- | ---: | --- |
| low map 关闭 | 2645.055ms | 资源正确性与回退确认 |
| low map 开启 | 994.950ms | 资源正确性与低地址共享确认 |

这是合成资源探针的提交耗时，**不是游戏 FPS**，也不使用它估算最终帧率。

证据：`venus-lowmap-probe-off-final-20261009.txt`、`venus-lowmap-probe-on-final-20261009.txt`，对应启动条件 JSON。

## 游戏性能对照

- `Z:\games\Tomb Raider\TombRaider.exe`（PE32/FEX），同一 legacy DXVK，800×600，画质／游戏配置不变。
- 两侧都用 `isolate-venus-native-buffer`，保持相同 WSI、host 上传模式、guest 脏范围与提交顺序；比较的是 low map。不能把结果直接当作默认 WSI 的完整性能验证。
- 两侧均关闭 map 诊断、guest perf summary 和 trace flow。Wine/FEX 完整结束后重启。
- 从“继续”加载同一存档（列表里 2026/10/9 07:33，0% 完成），等到洞穴倒挂角色出现“摇摆”提示后不再输入。未选择新游戏或主动覆盖存档。
- 各窗口同时保存开始／结束截图、温度与 hilog，计数差／时间差计算显示 FPS 和源提交 FPS。没有 GPU timestamp 或逐帧 P95。

| 窗口 | 显示 FPS | 源提交 FPS | 状态 |
| --- | ---: | ---: | --- |
| OFF1：09:36:26–09:37:01 | 18.521 | 18.544 | 洞穴倒挂角色稳态，35 秒 |
| ON1：09:43:50–09:44:25 | 37.594 | 37.601 | 同一洞穴位置，35 秒 |
| OFF2：09:53:06–09:53:41 | 17.754 | 17.763 | 完整重启后回到同一洞穴位置，35 秒 |
| ON2：10:06:03–10:06:38 | 37.526 | 37.516 | 第二次关闭诊断的干净 ON 启动，同一洞穴位置，35 秒 |

ON1 比 OFF1 显示 FPS 高约 103%。关闭开关后 OFF2 回落到 17.754 FPS，再次开启后 ON2 恢复至 37.526 FPS。两次干净 ON 启动结果相近，支持收益由共享映射开关带来；没有把它解释为严格锁频实验。源提交和显示统计接近，四段未检出 failed swap／present 失败。画面人工检查未见泛白或人物缺图。开始／结束截图中的角色有正常小幅动画，镜头与场景一致；无需以完全相同像素掩盖动画差异。

温度记录保留原始传感器名称与数值，不把 `ambient` 名称解释成实际室温。OFF1 system_h 46.733→46.533°C、电池38°C；ON1 system_h45.625→45.500°C、电池39°C；OFF2 system_h46.866→47.071°C、电池39°C；ON2 system_h45.937→45.875°C、电池39°C。未锁 CPU／GPU 频率，已完成 OFF→ON→OFF→ON 复测。

35 秒是墙钟采集窗口。统计只覆盖窗口内第一条与最后一条周期日志之间的完整计数区间，OFF2 为 27.036 秒／480 显示帧；不是把周期末尾的帧数当作全窗口平均。原始窗口与方法见各轮 `*-summary.json`。

加载、片头和动态镜头窗口单独标记，不计入稳态对照。例如 `venus-lowmap-game-off1-loading-window-20261009` 名称含 loading，其截图后来已经进入片头，不能列为洞穴帧率。

## 启动异常与尚未验证项

首个 ON 诊断游戏会话（09:26:52 启动，PID48797）在09:27:55退出，NCP exit=11；FEX JIT 对游戏只读代码区 `0xb60028` 的访问异常已保存。后续关闭诊断的 ON1、ON2 均正常进入菜单、加载存档并完成洞穴采样；ON2 游戏 PID61877，在 10:07:22 仍保持游戏画面。不能据此断言首轮异常由诊断或 low map 引起，也不能声称启动稳定性已完全修复。该会话未用于 FPS 对照。

Wine 初始化阶段其他子进程 SIGSEGV 在 OFF 和 ON 都有记录，需与游戏进程区分。对游戏 PID／退出码的判断不能只统计全局 SIGSEGV 行数。

随后补做 PE64 真机资源探针：`direct-dxvk64-smoke.exe`，ON + 诊断，09:56:35 生成新结果，报告 `pointer_bits=64` 和 `Virtio-GPU Venus (Maleoon 910)`；缓冲上传／回读四轮、纹理上传／回读四轮均 `bad=0`，`RESULT PASS`。同会话的五个 64MiB BO 都是 `wine_owned=0` 且地址约 `0x7002e01000`，确认 WOW64 专用 allocator 拒绝后继续走原 mmap。120 次 Present 为 1070.917ms，仅用于正确性检查，不用于宣称 PE64 性能不变。证据：`venus-lowmap-pe64-on-diag-20261009-conditions.json`、`venus-lowmap-pe64-result-20261009.txt`；该文件头部残留前序 PE32 日志，判断 PE64 时仅取本次新出现的五个 64MiB／高地址样本。

手机、其他游戏、长时间运行、地址空间耗尽后的真实回退与默认 WSI 的实机正确性仍未完成；PE64 已通过本轮资源正确性与回退验证，但未做性能 A/B。

## 本轮检查与复现

- `make test-venus-low-map`：执行生产 map/destroy 方法，验证双向共享写入、缓存不重复分配、设置变化不改变所有者、禁用／缺库／缺配对接口／分配失败／非 mappable／普通 mmap 失败的安全回退和释放配对。末尾补充 fstat 模拟 EACCES 时仍能共享写入与成对释放，以及诊断改变 errno 后仍报告原始 mmap 错误，均通过。
- 四个 Mesa 补丁从逆序还原后连续正向应用两次，结果与当前源码逐字节相同。
- WOW64 Vulkan map 回归在配对 Wine 源码上通过：破坏性 MAP_FIXED 失败恢复、精确上传／回读范围、partial map／guard page／whole size／溢出拒绝。
- 相邻 Venus present retry、native presenter 故障和异步所有权测试通过。
- OHOS ARM64 Venus 驱动编译通过。专项之外没有宣称全量测试通过。

真机性能数字对应上文 SHA256 已安装候选。采集过程中，源码仅另外修正了“开启诊断且 mmap 失败时，错误文案误用 fstat 的 errno”：改为已捕获的 map_error；并相应缩小 0001 的上下文以保证 runner 重复执行。该失败文案修正已通过新增回归、两次补丁重放与 Docker `vp-proton` 内 OHOS 编译，但未替换当前已安装候选，也没有重签另一份同名包。成功映射路径未改。`entry/libs` 与 raw runtime 仍保持上文实测包的配套身份；新源码构建输出留在 `build/guest_vulkan_build/aarch64/mesa-venus-offscreen-v2/`，正式打包时需要再同步。

实验开关需要同时设置，并完整重启 Wine/FEX：

```text
WINEHUA_VENUS_LOW_MAP=1
WINEHUA_VIRGL_LOW_MAP=1
```

诊断追加 `WINEHUA_VENUS_MAP_DIAG=1` 和 `WINEHUA_WINEDEBUG=-all,err+vulkan`，不用于 FPS 对照。关诊断时将 MAP_DIAG=0，guest summary=0。

本地自动化入口：`launch_venus_lowmap.py LABEL off|on|off-diag|on-diag|direct`，`capture_venus_window.py LABEL 35`。每轮完整停止应用，程序启动和输入由工具自动完成。设备命令、超时及脱敏记录在同目录 ledger，截图与原始日志留在本地证据目录。

本轮结束时保留 ON2 的干净游戏会话，low map=1、map 诊断=0、guest summary=0；未发送进一步游戏输入。应用正常入口的产品默认仍然关闭本实验，重启后不能假定继续使用 ON2 的启动参数。

仓库内随报告保留关键小型证据：`evidence/venus-lowmap-20261009/`，包含四轮采样摘要与窗口、对应启动条件、资源探针、构建身份及 SHA256 清单。完整截图／日志与测试 HAP 保留在本地 `F:\VintagePomelo-Workspace\workspace_temp\gumu10-dx11-20261008\`；未把二进制包作为源码提交内容。

后续产品化重点是独立于 VirGL 的 Venus opt-in／能力门槛、默认 WSI 与手机回归。共享映射已消除的一层成本无需继续靠更换 FEX 或调低分辨率来掩盖；尚余 guest→GPU 上传、range prepare 和 ring/fence 同步成本仍可单独测量。
