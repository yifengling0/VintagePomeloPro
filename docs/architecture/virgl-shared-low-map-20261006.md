# 消除 32 位 OpenGL 映射的 shadow 复制

日期：2026-10-06。父仓库基线 `284026ce64c631961394f2aee6fadb3a4380e579`；候选改动在本地，默认关闭。

## 结论

复制不是 FEX 使用 ARM64 Mesa 的必然成本。当前额外复制来自地址契约：Mesa 返回的原生指针超过 32 位可表示范围，Wine 必须给 PE32 调用者提供低地址 shadow，并在映射及写入发布时搬运内容。让共享缓冲本身位于 Wine 管理的低地址，可以沿用 Wine 的 direct-map 分支，消除这一层双向复制。

main 锁定的 Box 确实具备影响外部库 mmap 的低地址机制。但是还没有匹配条件下的 main 真机 direct/shadow 统计；不能把“Box 有此机制”写成“RichMan8 在 Box 下始终没有复制”。即使本候选命中，也不能推断 FEX 与 Box 的全部帧率差距会消失。

本轮实现了窄范围生产原型：VirGL vtest 在创建共享缓冲时，通过一对版本化的 Wine 原生接口分配低地址、建立 MAP_SHARED 映射；旧接口或映射失败回到原路径。它不更改 FEX 默认后端，不修改 TSO、x87 精度、画质、0035 显式 flush 或 VirGL 等待策略。

## Box 为什么有机会避开这层复制

检查的是 main 的 Box pin `0411b38569d3b70a0a8eb64ffd80e7ef72f9cb67`，不只检查当前子模块 HEAD。

- `src/custommmap.c:35` 导出 `mmap64`，同时把 `mmap` 作为别名导出。无指定地址且已运行 32 位代码、`BOX64_MMAP32` 开启时，调用 `box_mmap(..., flags | MAP_32BIT, ...)`。
- `src/include/env.h:110` 的 `BOX64_MMAP32` 默认值为 1。`docs/CHANGELOG.md` 明确说明该机制面向 Mesa 等外部库。
- `src/dynarec/dynarec.c`、`src/emu/x64run.c` 在运行 32 位代码时设置进程的 `running32bits` 状态。
- `src/custommem.c` 在 ARM64 上不直接依赖内核提供 x86 的 MAP_32BIT，而是自己选低地址。该树还包含 OHOS 的 `/proc/self/maps` 空洞扫描和 MAP_FIXED fallback。
- `src/wrapped32/wrappedlibc.c` 对被转译的 32 位 mmap 也补 MAP_32BIT。这和外部原生 Mesa 的 mmap 拦截是两件事，不能混为一谈。
- `wrappedlibgl_private.h`、`wrapped32/wrappedlibgl_private.h` 的 `glMapBuffer*` 是普通 GO 包装，没有在这些入口发现专门的映射副本实现。

Box 的机制依赖最终动态链接作用域、符号绑定、运行状态和地址分配结果。本项目使用 `dlopen("box64.so", RTLD_NOW)`，不能仅凭源码中导出 mmap 就证明安装包中的原生 Mesa 必然由它拦截。对照时应记录实际返回指针及 direct/shadow 次数，或直接记录 mmap 符号绑定。

main 的 Wine 同样存在“高地址则 shadow”的代码。差异可能发生在原始内存分配，而不是 GL API 表面上有没有 memcpy。

## 当前瓶颈和归因边界

原生 ARM64 Wine + FEX 的 PE32 链路为：

```text
PE32 游戏 / WineD3D / opengl32（FEX）
  → WoW64 UnixCall
  → 原生 ARM64 Wine OpenGL
  → 原生 ARM64 Mesa VirGL
  → vtest 共享资源 / renderer / 宿主驱动
```

Mesa 进入 ARM64 原生侧后，其系统调用不再经 FEX 转译。32 位 WineD3D 的管理代码、桥接频率和 FEX 自身执行成本仍可能是另一部分热点。

之前 RichMan8 **主菜单**样本的高地址 shadow，每次初始化复制 256 KiB，平均实际 flush 约 4.4 KiB；约 1 GiB/s 入向搬运、约 0.88 ms/swap。0038 只省掉 DISCARD 的少量初始化，NOOVERWRITE 仍大量复制整块。它证明存在无效内存流量，不是受控切角色样本，也不足以解释所有长停顿或一倍帧率差距。

0039 的 sysmem 候选复用脏区上传，可以减少映射 shadow，但仍保留独立 CPU 暂存及上传。0040/本 Mesa overlay 是更直接的共享地址方案。A/B 本方案时应关闭 0039，以免两种策略叠加使结果难以归因。

## 本候选的内存和生命周期契约

```text
原路径：游戏低地址 shadow ↔ memcpy ↔ Mesa 高地址共享映射 → GPU 上传
本候选：游戏和 Mesa → 同一个低地址 MAP_SHARED 映射 → GPU 上传
```

1. vtest 协议 ≥ 2、资源 `target == PIPE_BUFFER` 才尝试新路径。纹理资源、协议 0、非 OHOS ARM64 都保留原路径；PBO 等本身属于缓冲资源，可按同一地址契约获益。
2. 只通过 RTLD_NOLOAD 获取**已经加载**的 `ntdll.so`，查找 `winehua_map_shared_buffer_v1` 和 `winehua_unmap_shared_buffer_v1`。两者都存在才启用；没有接口时不加载 Wine，也不使用残缺的一半接口。
3. Wine 检查进程为 WoW64，且 `WINEHUA_VIRGL_LOW_MAP` 的值严格为 `1`。
4. Wine 用 `NtAllocateVirtualMemory`、ZeroBits=1、MEM_RESERVE|MEM_COMMIT 注册整个低 2 GiB 分配，按宿主页大小覆盖映射长度。比低 4 GiB 更保守，避免非 LAA 程序的高位地址问题。
5. MAP_FIXED **只替换本次已归属 Wine 的分配**，不扫描猜测空洞，不依赖 MAP_FIXED_NOREPLACE，也不把 mmap hint 当所有权凭据。原子占用阶段仍由 Wine 的虚拟地址锁保护。
6. 收到的同一个 fd 建立 MAP_SHARED 映射后，Mesa 继续关闭该 fd；映射自身持有 backing store，无需长期保存 fd、扫描 `/proc/self/fd` 或逐帧建立别名。
7. Wine 返回的 driver pointer 可由 PE32 表示，所以 opengl32 原有 direct-map 判断通过。游戏写入和 Mesa 读取落在同一份 CPU 共享页，不需要 shadow 初始化和回写。
8. 显式 flush、脏区、VirGL 传输及 fence 仍按原逻辑执行。共享 CPU 页不等于内容已在 GPU 完成上传，也不等于可移除同步。
9. 资源记录分配时的 release callback。vtest 缓存持有资源时继续持有映射；最终销毁走配对的 Wine NtFreeVirtualMemory，释放地址记录及映射。不会因为开关后来变化而误用普通 munmap。
10. 地址不足、无接口、64 位进程、非法/过大长度、mmap 失败均回退到普通 Mesa mmap，之后 Wine 可照常使用 shadow。失败的低地址尝试会释放它占用的 Wine 分配，不消费调用者的 fd。

两份补丁必须一起构建：

- Wine：`patches/wine/0040-ntdll-owned-virgl-shared-low-map.patch`，含低地址映射接口，以及进程启动时的试验开关同步。
- Mesa：`patches/mesa/0002-virgl-vtest-wine-owned-low-map.patch`，含可选接口发现、资源创建和销毁。

Wine `init_peb` 在 DLL 初始化前，把子进程 Windows 环境中的**这个单独开关**同步到 Unix 环境。这样用户手动运行 BAT 能影响该游戏；缺失、0、空值或其他值均关闭，不能继承一个应被清除的旧 Unix 开关。没有导入 PATH 等任意 Windows 环境变量。

## 验证

专项入口：

```sh
make test-virgl-shared-low-map
```

`host_tests/virgl_shared_low_map_test.py` 从固定 pin 重放生产 overlays，提取真实 Wine 映射函数和 Mesa 创建/销毁函数。Wine 地址预约及 vtest 平台是测试边界；共享 fd、mmap、读写及关闭是真实内核操作。不是把复制函数改成空函数的模拟。

11 项专项测试通过，覆盖：同页双向可见；非页对齐请求；关闭 fd 后继续访问；失败清理；4 线程共 400 次分配/释放；默认/64 位/纹理/协议0/零长度范围；持久缓冲生命周期；旧 peer/缺少 release/低地址耗尽回退；BAT 环境；两套 overlays 重放两遍一致。另以真实共享库和 RTLD_LOCAL/RTLD_NOLOAD 验证接口发现及导出可见性。

28 项相邻回归通过：0035 显式 flush、DISCARD、0039 sysmem、上传计时、字体兼容、计时默认关闭及边界。独立 Wine 构建身份回归通过。ASan/UBSan 的专项及相关内存测试通过；沿用已有环境设置关闭 LSan，因此没有 LSan 全量通过结论。

Mesa 已在独立源/构建目录完成 OHOS AArch64 Release 构建；真实 `libgallium-25.0.1.so` 含两个 v1 查找名称及 dlopen/dlsym/dlclose 依赖。Wine 也在全新独立 BUILD_DIR 完成 OHOS 构建及顶层缓存身份复核；真实 AArch64 `ntdll.so` 的动态符号表中，两个 v1 接口均为 GLOBAL DEFAULT。未复用旧 Wine 对象，也未在默认 dirty Wine 源上重放新补丁。

构建入口也把 Mesa overlay 文件和应用脚本纳入 deps 的依赖及时间检查，避免常规增量入口跳过新 Mesa。检查脚本支持 Docker 中外部 Git 元数据不可达的独立源码目录，仍逐补丁验证正向或反向重放。

Wine source pin `cd547f7a0ee9d3d59b3ec47317a7852dedf0443b` 在 WSL 核对。Docker 内 linked worktree 的外部 Git 元数据不可达，身份记录明确采用 `full-source-tree-sha256`、`pin_verified=false`，并核对父仓库 gitlink；没有跳过身份检查，也没有把容器内的 pin 写成已验证。

独立构建路径：`build-virgl-lowmap-20261006/`；源：`workspace_temp/wine-lowmap-source-20261006/` 和 `workspace_temp/mesa-lowmap-source-20261006/`。构建日志、身份文件和 SHA256 清单位于 `workspace_temp/virgl-lowmap-20261006/`。

配对 HAP 已构建、签名并在 MatePad Mini 的 `com.vintage.pomelopro` 覆盖安装，保留应用数据。版本仍为 `1.4.5-proton.26-alpha / 1004035`，签名包 SHA256 为 `42428843bdd0de79113f05264e8ff796050d5c9cd0753a3a5b08e258f629c812`。包内 Wine/Mesa 的实际加载段与本轮独立编译输出一致，FEX 和 ArkTS 与既有包一致，真实 zlib、runtime guard 及官方签名检查通过。安装输出明确成功，`bm dump` 复核版本，未发送应用、Steam 或游戏启动命令。

打包时发现 Mesa 脚本在读取 `env.sh` 后覆盖 `BUILD_DIR`，使独立目录的 zlib  provenance 丢失；现已保留调用者的 `BUILD_DIR`。常规 make 的依赖检查和 Wine 构建身份检查继续生效。Vulkan 使用先前已验证的 bundle；本轮重建 VirGL Mesa。设备直接读取已安装 el1 库受限，因此实际文件内容的证据来自签名 HAP 加载段核对，不能写成已读取设备 ELF。

手动 on/off 及 on-perf/off-perf 入口已部署到 C: 根目录，四个文件均经发送、取回及 SHA256 核对。首轮 on 入口关闭计时，且四组都固定 `WINEHUA_DYNAMIC_BUFFER_SYSMEM=0`。详细安装和现场状态见证据目录中的 `installation-test.md`、`package-identity.json` 及设备命令 ledger。

用户已手动测试。首轮反馈没有明显变快；诊断轮的选人截图与计时确认目标路径命中：约 45.22 秒内 `map_direct=164964`、mapped requested bytes 为 `43244322816`、该计时的 copied bytes 为 0，未出现 shadow 系列记录。这是目标缓冲直接映射的真机证据，不代表所有游戏/驱动复制都消失，也不能把 requested bytes 直接当作节省量。

用户仍反馈整体卡顿，尤其首次选择未缓存人物；已加载人物更流畅。该选人段宿主采样为 16.41、21.79、56.87、57.23 FPS；没有匹配的 off/on FPS 提升结论。`wgl_swap` 平均约 2.527 ms、单次最大 24.614 ms；`tex_subimage` 单次最大 4.363 ms，但计时未覆盖所有纹理初次分配、压缩上传、文件读取/解压或宿主 shader 编译，不能据此排除首次纹理准备。

补采的 35 秒、200 Hz、23 个应用自有进程的 hiperf 发现 `winebus.so+0x5e50/0x5e54` 合计占 37.84% 的采样 cycles。使用实际编译 ELF 的符号与反汇编定位到 `ohos_bus_wait()` 的 `poll(fd, 1, 20)` 附近；同期 winedevice 增长 3666 user ticks。它是需要继续查明的后台 CPU 热点，尚未记录实际 poll 返回值/errno，不能写成已确认根因。完整现场限定、截图、计数及采样 SHA 见 `installation-test.md` 与各 capture/profile 子目录。主机共享页验证仍不替代长会话地址压力及地图画质验收。

## 真机验证和后续方向

配对候选包必须包含新 `ntdll.so` 和新 Mesa `libgallium-25.0.1.so`，仅换 Wine 不足以触发新资源分配。开关默认 0；首次应对比同一包的开关 0/1，且 `WINEHUA_DYNAMIC_BUFFER_SYSMEM=0`，由用户从原本地入口启动游戏。

先关闭计时诊断，检查人物完整性、重复切角色、地图、最小化/恢复；随后两组都用 `WINEDEBUG=-all,+err,+winehua_perf`，同内容、温度、分辨率、刷新率和操作顺序采首次/重复/稳态。程序、Steam、BAT 和测试输入均由用户操作。

成功的直接指标是目标 buffer 的 map_direct 增长、map_shadow 和 shadow_in/shadow_flush/shadow_out 字节明显下降；再看 p50/p95 帧耗时、长停顿、GPU 上传等待和画质。FPS 未变化时也必须如实记录，不能把主机零复制当游戏收益。

待验证的两项架构扩展：

- 如果低地址缓存压力或频繁 resource-create 成为新热点，再设计有额度的资源池/子分配。复用要绑定 backing/resource 代次，并等 GPU/传输使用结束，不能仅按 GL name 复用。
- 若剩余热点在 renderer 的上传，才考虑外部内存导入或 host-visible GPU backing；这需要驱动、VirGL 协议和同步共同支持。本候选只消除 CPU 地址桥接的复制，尚未实现 GPU 上传零拷贝。

如果复制下降但角色切换仍卡，继续按已分层的图形调用、WineD3D/FEX、资源读取/解压和等待证据排查。FEX/Box 的 TSO、x87、桥接和缓存差异另做匹配对照，不把所有差距压到一个 memcpy 原因上。
