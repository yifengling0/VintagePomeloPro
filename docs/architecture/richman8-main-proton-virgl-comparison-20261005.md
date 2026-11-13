# main 与 Proton 的 RichMan8 / VirGL 路径对照（2026-10-05）

## 判断

目前不能认定“FEX 比 Box64 慢”，也不能认定“VirGL 驱动整体退化”。两边的 D3D9 后端相同，但执行边界、WineD3D 实现、32 位缓冲地址和窗口合成方式有差异。现有设备材料更适合定位新帧生成之前的资源准备、映射和同步成本，尚不支持量化两个转译器的速度差。

本次发现一个具体的 WineD3D 差异：当前实现把从 1 开始的 WGL 像素格式 ID 直接用于紧凑数组下标，可能读错 `swap_method` 或越界。这是静态代码缺陷；尚未证明它在本次 RichMan8 卡顿中实际触发。

本次只进行了源码、构建配置及已有采样的对照，保存快照和报告。没有启动应用、Steam 或游戏，没有修改运行配置、构建、安装或提交代码。

## 对照身份与证据范围

已用 `git ls-remote` 核对两条远端分支，未 checkout / reset 工作树：

- main：`5383b7849190c563c0e79007fb9c33da64b326e4`，提交时间 2026-09-30。
- feature/main_proton：`0a877ea3724cf02e248d90c413aac92afbb38fd0`，当前工作树另含已经安装的窗口、字体、zlib、OpenGL 修复。
- 9 月 25 日 RichMan8 旧验收对应 `1ba0381b3aa3937c028f81419cdebbd8f910bd8f`。从该提交到当前 main，上述 graphics 文件及 Wine / Mesa / virglrenderer pins 没有差异；这不等于当时包的全部文件或配置完全相同。
- 当前已安装候选：`entry-default-richman8-buffer-flush-debug-signed.hap`，SHA256 `c04c99c764418ce3e10599e3d831db3d3e8f4f625f1a89eee0f3700689a8d8c7`，15:47 安装。
- 引用的 FPS / hiperf 来自 15:00–15:45、安装这份缓冲修复候选之前。尚未取得其人物完整性和性能验收，不能把旧采样当成修复后效果。
- 权威源码 `/home/liufeng/src/vpp-proton`；本地证据目录 `F:\VintagePomelo-Workspace\workspace_temp\virgl-main-compare-20261005\` 中的 `main/`、`current/`、`diff/` 保存所查源码和 diff。该目录的 `verified-refs.txt` 与 `current-build-flags.txt` 保存分支与编译配置；132 个文件的 SHA256 清单保留在同处，原始采样与源码快照不随本报告提交。

| 组件 | main pin | Proton pin |
| --- | --- | --- |
| Wine | wine `1cb1ac93e464` | wine-valve `cd547f7a0ee9`，另含 overlays |
| Mesa | `2939cbb816d3` | `330124bf18f8` |
| virglrenderer | `fde243e144d3` | `bb33e52aa031` |
| Box64 | `0411b38569d3` | `e970ee6f2179`，当前 RichMan8 不使用它作为基座 |
| FEX | 此架构没有 FEX pin | `86ff33bbe299` |

## 实际路径

| 环节 | main | 当前 RichMan8 |
| --- | --- | --- |
| 游戏与 D3D9 | 32 位游戏，内建 D3D9 / WineD3D，走 Box64 运行架构 | 32 位游戏和 i386 D3D9 / WineD3D DLL 由 FEX 转译 |
| Wine Unix / OpenGL 层 | x86_64 Wine Unix 层经 Box64 | ARM64 Wine Unix / opengl32 层原生运行，经 WoW64 边界与 32 位 DLL 交互 |
| guest Mesa | x86_64 Mesa；GL/EGL 显式列入 `BOX64_EMULATED_LIBS` | aarch64 Mesa，原生运行 |
| GPU 服务 | native ARM64 vtest / virglrenderer → 系统 EGL / GLES | native ARM64 vtest / virglrenderer → 系统 EGL / GLES |
| 呈现 | GPU blit → NativeWindow 队列 → NativeImage → 宿主合成 | 同样的队列链路，当前支持多个 GPU 子面共同合成 |
| 已有现场的窗口模式 | 9 月 25 日日志 `DESKTOP_MODE=0`，独立窗口 / 融合模式 | 本轮日志 `desktop D3D=...`，桌面根窗口合成；不是完全相同的窗口配置 |

两边均为 **D3D9 → WineD3D → OpenGL → VirGL**。UI 的 `dxvk_1_10` / `dxvk_legacy` 是会话档位，不能证明 D3D9 使用 DXVK。main 明确保留 D3D9 使用 WineD3D；当前 DXVK overlay 同样未接管 d3d9。

必须纠正一个执行边界的证据解读：stderr 中 `load_builtin_unixlib ... d3d9.so / wined3d.so` 是加载尝试，不能证明这些库成功加载，更不能证明整个 WineD3D 原生化。当前 staging 没有这两颗 Unix `.so`；`dlls/d3d9/Makefile.in` 和 `dlls/wined3d/Makefile.in` 编译 Windows DLL，现场 maps 显示实际的 `i386-windows/wined3d.dll`。因此，WineD3D 的状态处理、着色器生成和命令流代码仍属于 FEX 需要执行的 32 位 DLL，原生的是后面的 OpenGL Unix 层与 Mesa。

main 并非直接把 GL/EGL 包装成系统 GLES 调用。`wine_env.h:25` 的 `Box64EmulatedLibs()` 列出 `libGL.so`、`libEGL.so` 等，guest Mesa 构建也固定 x86_64。这两处证据说明 main 的 guest Mesa 本身经转译。当前把这个环节改为原生，减少了该部分的指令转译，但总耗时仍取决于前面的 DLL、边界切换、内存复制和同步，不能仅凭架构给出 FPS 排名。

## 找到的差异与优化方向

### 1. WineD3D 像素格式 ID 与数组位置混用

当前生产代码形成了以下组合：

- `adapter_gl.c:3973` 用 `pixel_format_count` 将成功查询的格式紧凑存入数组；在 `3975` 取数组槽位，在 `3981` 设置 `cfg->iPixelFormat = i + 1`。
- `context_gl.c:1703` 返回 `cfg->iPixelFormat`，这是 WGL 格式 ID。
- `swapchain.c:633` 却使用 `pixel_formats[context_gl->pixel_format]`。

最小静态反例：只有一个格式、ID=1 时，数组有效下标只有 0，当前表达式访问下标 1；有多个格式时也会偏到其他格式。查询失败会使数组变紧凑，此时 ID 和数组下标的关系更不能依赖减一。

main 返回 `i + 1`、使用 `[pixel_format - 1]`，连续完整枚举时能对应，但同样不能正确处理枚举缺口。**修复方向应按 `iPixelFormat` 查找记录，校验找不到的情况，或在 context 中分别保存格式 ID 与数组位置。不能简单把下标改回减一。**

错误的 `swap_method` 会影响部分区域 Present 是否走 `swapchain_blit_gdi()`，可能引入读回、CPU 位图处理和提交方式差异；越界还有正确性风险。不过现场的 zero-copy 增长、低 readback 数量不支持将全部低帧直接归因于这条 GDI 路径。应同时记录实际格式 ID / 数组位置、partial Present 次数、GDI 分支次数。

### 2. 32 位 GL 映射副本和资源等待

main 和当前都具有 WoW64 高地址 buffer shadow 分支。驱动返回不能表示为 32 位地址的指针时，Wine 为游戏创建低地址副本，可能在 Map 时复制原内容，并在 Flush / Unmap 时复制写入。

guest Mesa 的 vtest resource backing 使用 `os_mmap(NULL, ..., MAP_SHARED, ...)`；Box64 路径则通过自己的 mmap 包装和地址搜索，当前 ARM64 Mesa 直接使用本机分配。地址选择确实不同，**但尚未测到两边 shadow 命中比例，不能声称 main 避免了复制或当前每次都复制**。main 的 `find47bitBlock()` 也不保证 32 位可表示地址。

当前安装的 0035 已修复 explicit flush 在发布前遗漏 shadow 字节的问题，并避免显式 map 在 Unmap 时再整段复制。这个缺口在旧 main 的 shadow 实现中也存在，不能称为当前分支独有回归；也不能用正确性测试替代 FPS 验收。

下一步应先加按秒汇总的 map / flush / unmap 次数、shadow 命中数、双向复制字节数与耗时，以及 driver map 的等待时间。若数据证明复制明显，评估为这类共享 buffer 提供受控低地址映射或保持 shadow 复用；若等待明显，优化动态 buffer 的 orphan / discard / no-overwrite 路径及同步粒度。不要对未知 buffer 删除必要复制或刷新。

### 3. WineD3D 和宿主合成不是相同实现

Wine pin 不同，`buffer.c` 中 streaming buffer 带有 `WINED3DUSAGE_CS` 的处理不同；`glsl_shader.c` 有旧 shader 指令语义和生成代码差异；纹理 raw/FBO blit 与 resolve 分支也不完全相同。这些可能改变资源上传和首次 shader 准备成本，目前没有对应场景的计时证据。

宿主 main 用单一 GPU consumer，当前需要正确合成多个 GPU 子面、SHM 父面及统一窗口状态。几何重建和层数可能增加成本。但已采到的低帧段宿主单帧通常约 2.5–6 ms，`failed_swaps=0`、zero-copy 持续工作，没有持续全帧 SHM 上传证据；仅减少宿主 quad 合成不能解释地图约 20–24 FPS 的全部差额。

`GL-PERF total_us` 不包含整个上游等待或所有 GPU 执行时间。因此它只能说明宿主这段已计时工作较轻，不能排除 vtest / GPU 渲染和队列同步成为瓶颈。

建议先在当前 FEX 基座上比较同一游戏的独立窗口与桌面模式，同时采游戏 Present、vtest 完成、NativeWindow 发布、NativeImage 消费和宿主显示的同一时间线。多窗口身份、代次与前台修复应保留。

### 4. FEX 与 Box64 的成本要单独量化

main 有 BIGBLOCK=3、CALLRET=2、FORWARD=1024 等 Box64 档位；9 月 25 日启动日志有 WEAKBARRIER=2。当前 FEX 配置默认 TSOEnabled=true、MaxInst=5000、SMC=mtrack，且 WoW64 会尝试启用硬件 TSO。是否在本设备生效、是否被每游戏配置覆盖尚未记录，不能把配置默认值当成现场最终值。

它们的 block 编译、内存顺序、SMC 跟踪和跨边界调用实现不同。资源首次使用可能同时触发游戏解压、i386 WineD3D 工作、FEX JIT、Mesa / host shader 编译；“第一次慢、第二次好”无法独自区分这些来源。

若要判断转译器成本，应分别取游戏主线程与 `wined3d_cs` 的有效 on-CPU 时间、off-CPU 等待、编译 / invalidation 次数和调用边界时间，并补足 JIT / 特殊 IP 的归类。已有采样存在匿名和未知特殊样本，不能用已符号化的小部分百分比推导全部 CPU 开销。

不把关闭 TSO、SMC 或内存屏障作为默认优化：它们影响正确性。先固定 FEX 和已修复图形路径，解决可量化的图形及资源成本；若确需转译器 A/B，要保持 Wine、Mesa、virglrenderer、窗口模式、前缀和游戏数据相同，另做兼容性验证。整个 main 包对整个 Proton 包的比较只说明产品差异，不能隔离 Box64 / FEX。

## 两边相同、不能当成本次新增回归的设置

- Mesa 构建均为 release、virgl + softpipe；`GALLIUM_DRIVER=virpipe` 的选择意味着渲染器为 virgl，编进 softpipe 不等于现场退回 CPU 光栅化。现场 renderer 为 `virgl (Maleoon 910)`。
- 两边均 `shader-cache=disabled`。当前生成的 Meson 配置确认 guest Mesa optimization=3、host virglrenderer optimization=3。这是可以研究的首次加载优化空间，但不是当前分支新关闭缓存的证据。恢复磁盘缓存前还需确认这条驱动链支持哪些缓存以及 OHOS 可写目录与版本失效策略，不能只设置环境变量就声称生效。
- 两边 host launch 均设置 `VTEST_SYNC_GL_FINISH=1`，默认 sync mode 为 `egl-main`。实际处理 WAIT 请求时，vtest 执行 context finish，再检查 fence。这是共同的串行化候选，需要计时后按资源 fence 优化，不是当前新增同步退化的证据。此开关按存在性判断，设置为字符串 `0` 仍会开启。
- 两边 VirGL NativeImage queue 的默认周期为约 16.67 ms、相同的周期钳制和 pacing，不存在查到的固定 15 FPS 限制。不能单凭 15 FPS 现象认定是 60/4 的节流。
- main 保留 GLES direct 代码，但 `kGlesDirectQualified=false`，默认仍用 EGL / NativeWindow 队列。不能说当前删掉这部分代码导致从“已启用 direct”回退。
- RichMan8 当前 GL2.1 context 的 fshack capability 日志 `enabled=0`，没有证据支持把完整 Vulkan / fshack gamma 链作为这款游戏当前瓶颈。
- 新候选实际 Makefile 的 Unix、i386、ARM64EC 和 ARM64 CFLAGS 都有 `-O2`。旧脚本存在 CROSSCFLAGS 只带 include 路径而覆盖默认优化参数的风险，该守卫已随既有修复添加；当前候选不属于未经优化编译。旧设备包实际 flags 未证实，不能据源码假定它的优化等级。

## 建议推进顺序和验证判据

1. **先修像素格式查找的正确性，并验收已安装的 0035。** 用连续 ID、缺口 ID、唯一 ID、无匹配格式验证安全查找；检查 partial Present 是否命中额外 GDI 路径。用户仍从原入口手动启动。
2. **增加轻量资源与阶段计时。** 只做窗口内汇总，不开全量 OpenGL / msg trace；分开计首次角色、已访问角色和地图。记录复制字节、map 等待、shader compile/link、vtest busy wait 及从 Present 到消费的间隔。
3. **保持 FEX，先做窗口模式和同步路径的单变量对照。** 同分辨率、相同前缀和内容，记录前台状态与温度；如果游戏提交很慢，继续上游，如果提交快而消费慢，再查队列和合成。不能为了 FPS 丢掉多窗口正确性。
4. **按计时结果实施优化。** 高频 shadow 用低地址 backing / 复用及有效范围发布；首次 shader 耗时用受支持的缓存；资源解压 / 读取耗时再处理对应游戏加载；真正 fence 等待长时再缩小同步范围。
5. **最后量化转译器差异。** 没有相同图形链和最终配置的匹配 A/B 前，不作“FEX 应回退 Box64”的结论。

本轮没有性能改动，也没有新真机性能测试。可确定的是：后端名称相同不代表执行成本相同；当前有可修的具体图形实现问题，也有待测量的 FEX 与资源准备成本，尚未找到能解释全部回退的单一根因。
