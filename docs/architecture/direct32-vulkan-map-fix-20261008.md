# 32 位 FEX + DXVK Direct：映射修复与范围复制实验

日期：2026-10-08。主分支 `feature/main_proton`，基线 HEAD
`4cb9e7c8be2acf7545976f0699ef064e796cb2fd`，包含本地未提交的 Direct 工作。
本轮修改也尚未提交。没有改用 WowBox64，没有修改 FEX、x87 设置或
PAL4 / RichMan8 的 WineD3D / VirGL 配置。

## 已确认的结论

32 位程序仍由 FEX 执行 x86 指令；WOW64 是它调用 64 位 Wine Unix 层的桥。
真机日志有 `starting FEX based libwow64fex.dll`，与实际 PE32 测试对应。

修复前：PE32 已成功创建 Maleoon 910 D3D11 设备及 RTV，却在创建缓冲时崩溃。
相同代码编译出的 PE64 对照可完成上传、回读和 Present。

修复后：PE32 和 PE64 均通过 4 轮 256 KiB 缓冲及 4 轮 RGBA 纹理上传／回读，
所有字节匹配，120 次 Present 返回成功。PE32 测试窗口的蓝色画面已实际显示。
这是资源正确性和 Direct 路由验证；不能把测试的 API 提交速率当成游戏 FPS。

最终同源 PE64 探针于 19:22:18 启动，19:22:54 结束；本轮取回的 TSV
含 `pointer_bits=64`、全部 `bad=0`、`RESULT PASS` 及最终版本的
`hold_ms=30000`。这次结果为 `direct64-final-check.tsv`，并非沿用此前的
PE64 PASS 文件。同期日志确认使用 `FEX based libarm64ecfex.dll`。

本轮没有证明实际 32 位游戏的帧率提升，也没有证明所有缓冲均实现零复制。
平板样本的 4 个驱动映射仍使用 CPU shadow；优化针对重复整块复制的开销。

## 真机崩溃定位

设备：MatePad Mini，HDC target `5KPBB25818203996`，Maleoon 910。
进程 48110 的原生故障记录：

```text
SIGSEGV addr=0x24420000
memcpy destination=0x24420000 source=0x70dae55000 size=0x1000000
0x24420000–0x25420000 在 /proc/maps 中整段缺失
lr=0x5a5e8eb938，win32u ELF address=0x1ab938
```

ELF 地址用 PT_LOAD 的 `p_vaddr/p_offset` 推导：可执行段映射 offset 为
`0x94000`，对应虚拟页 `0x95000`。不能直接把文件 offset 当 ELF 虚拟地址。
反汇编确认 `0x1ab934` 调用 memcpy，返回到 `0x1ab938`；addr2line 指向
`winehua_wow64_remap_map`。

诊断复现记录（进程 52221）明确显示：

```text
mremap alias failed errno=22
mmap alias failed host=0x70dae55000 dest=0x24420000 errno=22
随后 memcpy 写入失效低地址并触发 SIGSEGV
```

直接原因是固定地址映射失败后，回退代码继续使用已失效的目标页。
这是 Wine/OHOS 映射适配层的错误，不能归因于 FEX 指令转译性能。

## 代码改动

`patches/wine/0047-win32u-wow64-vulkan-map-safety.patch`：

- 移除对驱动映射的 mremap 尝试，保持其原始指针及 VMA。
- 重开共享 backing fd 时同时核对设备号和 inode，避免误用其他文件描述符。
- 固定地址映射失败或共享验证失败后，重新映射 Wine 已拥有的低地址页；
  恢复失败则返回映射错误，不再对失效指针复制。
- 记录 MapMemory 的实际 offset/size，处理 VK_WHOLE_SIZE；复制及再次映射
  均限制在已映射范围内，不再把部分映射指针误当整块 allocation 的起点。
- 将 FlushMappedMemoryRanges / InvalidateMappedMemoryRanges 接入 win32u：
  flush 先按范围上传，再调用驱动；invalidate 先调用驱动，成功后按范围回读。
- 实验模式停止 QueueSubmit 和 Unmap 的整块 shadow 上传，避免重复复制以及
  用旧 shadow 覆盖 GPU 写入的数据。未开启实验的路径保留整块提交机制。
- 更新 Vulkan thunk 生成规则及内部接口；新接口字段追加在 vulkan_funcs 尾部，
  与配套构建的 win32u.so / winevulkan.so 一起部署。

`scripts/build_wine.sh` 注册 0047，并在 make_vulkan 更新后重新生成 thunks。
新增 `--unix-modules win32u winevulkan`，仍执行正常补丁、身份、configure 检查，
使用独立 BUILD_DIR；其构建身份与完整 Wine 构建区分，不能被当成完整运行时。

## LAB 开关及适用范围

| 实验入口 | 驱动 | DXVK 显式同步 | 桥接层提交复制 |
| --- | --- | --- | --- |
| isolate-vulkan-direct | Direct | 关闭 | 整块复制；PE32 回读对照失败 |
| isolate-vulkan-direct-whole-map | Direct | 开启 | 整块复制，正确性对照 |
| isolate-vulkan-direct-precise-map | Direct | 开启 | 只复制显式 flush / invalidate 范围 |

后两个入口仅允许 dxvk_legacy。它们配对设置
`DXVK_WINEHUA_PRECISE_SHADOW=1` / `DXVK_WINEHUA_FLUSH_DYNAMIC_MAPPED=1`；
只有按范围入口设置 `WINEHUA_VK_PRECISE_MAP=1`。
普通产品入口强制设置 `WINEHUA_VK_PRECISE_MAP=0`，保持现有图形路由。

最终测试包中，完整退出应用／Wine/FEX 后分别运行正确性对照：

| PE32 同一资源／清屏负载 | 数据验证 | 120 次清屏／Present 提交耗时 |
| --- | --- | --- |
| whole-map，19:15 启动 | 缓冲／纹理全部 bad=0，PASS | 1992.737 ms |
| precise-map，19:17 启动 | 缓冲／纹理全部 bad=0，PASS | 1201.126 ms |

这次单组对照的提交阶段耗时减少约 39.7%。两侧保留相同的 DXVK 显式同步，
只切换桥接层是否额外进行整块复制；诊断日志关闭。没有测 GPU 执行时间，
没有控制温度／后台负载到严格实验标准，也没有多轮统计，因此这里只能确认
此合成负载有改善迹象，不能推算真实游戏 FPS 或宣称普遍提升 66%。

已验证的是本项目 legacy DXVK 的 D3D11 显式同步路径。D3D9、现代 DXVK、
原生 Vulkan 程序不能仅凭这个实验推断支持；尤其 Vulkan HOST_COHERENT
并不要求应用主动 flush，shadow 模式不能普遍冒充共享的 coherent 映射。
因此本轮没有将范围复制设为普通入口默认行为。

PAL4、RichMan8 的现有 WineD3D / VirGL 路径不会自动获得这项 Direct 优化。
后续应选择真实的 PE32 DX11 游戏验证，而不是修改这些游戏的默认配置。

## 验证与证据

`host_tests/wow64_vulkan_map_test.py` 提取生产 helper，在真实 POSIX VMA 上运行：
注入 MAP_FIXED 失败并移除目标页、共享文件双向访问、部分映射紧贴 guard page、
VK_WHOLE_SIZE、越界拒绝、范围复制以及提交／释放不覆盖未上传范围均通过。

`make test-direct-session` 通过：Direct FPS、图形策略、实际 session 环境序列化，
包括按范围／整块对照参数以及普通 Venus 入口的恢复。

Wine 从独立源码及 `build-direct32-map-v3-20261008` 构建，未复用 Wine 对象。
两个 Unix 模块通过编译、链接及构建身份复入检查。没有完整重编全部 Wine PE DLL。

HAP：1.4.5.29 / 1004038，`direct32-map-v3-signed.hap`，已覆盖安装。
最终 SHA256：`48052285c7f519c3fd97376285e44917204499dd7acc52d144ba1f9f42736d46`。
Hvigor PackageHap 成功；其 SignHap 因本机加密材料环境验证失败，随后通过既有
Windows 签名流程生成测试包，HDC 安装明确返回成功。

包内模块 `.text` 与独立 Wine 构建完全一致：

```text
win32u.so    5392e1cf74a337740124c096c9622c81a38807c5b73991cab09ec398fd39fa11
winevulkan.so 380a0d9f37396a6da9a304c091a997808c06d43464d0a57c64a393f96e501a5f
```

原始证据目录：`F:/VintagePomelo-Workspace/workspace_temp/gumu10-dx11-20261008/`。
关键文件：

- direct32-crash-registers.txt、direct32-fault-maps.txt、direct32-diag-focused.txt：旧包故障。
- direct32-fixed-precise.tsv、direct32-fixed-clean.tsv：PE32 正确性。
- direct64-fixed-precise.tsv：PE64 正确性。
- direct64-final-check.tsv、direct64-final-launch-evidence.txt：最终同源 PE64 正确性和本轮启动时间。
- direct32-visible-sanitized.png：PE32 Direct 最终画面，已裁掉通知区域。
- direct32-whole-copy-control.tsv：旧 Direct 参数的失败对照，不可用于有效性能比较。
- direct32-valid-whole-control.tsv：正确同步、整块复制的对照。
- direct32-valid-precise-ab.tsv：最终包的正确同步、按范围复制对照。
- direct32-map-hap-identity.json：最终包及 Unix 模块身份。

故障 maps 原始转储受系统 524287 字节上限截断；故障地址附近的局部 VMA 及
PC/LR 映射另有完整专用记录。截图中的静态保留阶段为 0 FPS 属正常无新帧提交。

验证结束已 force-stop 测试应用，结束进程内 LAB 会话；没有自动启动 Steam
或其他游戏。范围复制保持默认关闭，普通入口会显式清除对应实验变量。

## 下一步

1. 在实际 PE32 D3D11 游戏中，对最后两个 LAB 入口做同场景冷启动对照，检查
   纹理、动态顶点／常量缓冲、菜单、加载、设备重建及长期运行。
2. 用实际复制字节和时间统计验证收益，不能以清屏测试的提交速率替代游戏 FPS。
3. 对本设备无法共享的原生驱动映射，继续调查 VK_EXT_map_memory_placed、
   VK_EXT_external_memory_host 或合法的外部内存导入能力。未具备这些能力时，
   保留正确范围复制，不虚报零复制。
4. D3D9 / 现代 DXVK 在补齐各自的上传和回读协议前，保持原入口。
5. 64 位 ROTTR 的 SNORM 纹理失败、GPU 执行时间与约 10–12 FPS 瓶颈仍是独立问题；
   这项仅在 WOW64 高地址映射发生时触发的修复不能解决它们。
