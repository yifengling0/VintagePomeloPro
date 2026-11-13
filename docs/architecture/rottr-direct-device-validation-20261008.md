# ROTTR DX11 Direct 接入与平板验证（2026-10-08）

## 当前结论

平板上的 Vulkan Direct 已实际生效：DXVK 设备名由
`Virtio-GPU Venus (Maleoon 910)` 变为 `Maleoon 910`；游戏生产者经
DirectWineSurface 与 Vulkan 桌面合成器绑定，实际显示雪山存档。
此前 Venus 的资源扫描与 ring 等待热点在 Direct 采样中不再占据前列。
用户反馈操作流畅度大幅改善。

Direct 的 FPS HUD 已修复并安装：先前 Direct 未发布显示帧率，接入后又被
Explorer 的同编号窗口零值覆盖。现在桌面 root 的统计由合成输出单独发布，
同 root 的 layer 不再覆盖它。新图像仅在成功输出且可见时计一次，重复重绘、
失败 present、隐藏窗口均不增加游戏帧数。

这仍是 **平板、原生 ARM64 Wine、DXVK 的 LAB 会话入口**。普通程序入口仍
选择产品原有配置；不是所有设备、游戏或后续普通启动已经默认 Direct。
FEX 的游戏 CPU 转译、既有 x87 设置、Wine/DXVK/Mesa/VirGL 子模块未修改。

## 设备、构建和复现

- MatePad Mini，Maleoon 910，HarmonyOS 7；测试 bundle `com.vintage.pomelopro`。
- 仓库 `/home/liufeng/src/vpp-proton`，`feature/main_proton`，基于 `4cb9e7c8`，
  本次修改尚未提交。
- 已安装候选 `direct-fps-root-signed.hap`，SHA256
  `c3b9e43ceff59d22a39ef7bdec55fd41540e4ce994b3e3c0e7627f70df8b5a20`。
- 游戏为 Wine 内部的 `Z:\games\GUMU10\ROTTR.exe`，AMD64 PE64，参数 `-dx11`。
- 沿用低画质、游戏 800×600、AA 关闭、VSync 关闭和
  `d3d11.disableMsaa = True`；未在路由对照期间继续改画质。
- 游戏日志中 swapchain/桌面缓冲为 1200×800，部分内部渲染纹理为 800×600。
  不应将内部渲染分辨率、交换链分辨率及 2560×1600 截图分辨率混为一项。
- 每次切路由完整结束应用/Wine/FEX 会话，再启动相同游戏并读取“山顶”存档。
  输入、安装、截图、采样均由代理执行；没有更新游戏或 Steam 文件。

外部原始证据目录：
`F:\VintagePomelo-Workspace\workspace_temp\gumu10-dx11-20261008`。
`launch_route.py direct|venus|bound` 保持游戏参数相同，只切 LAB 路由。
`capture_route_window.py <label> 30` 保存前后截图、CPU ticks、温度与时间切片。
命令记录中的序列号和凭据字段已脱敏，截图作为本地证据保存。

## 接入逻辑

1. `isolate-vulkan-direct` 选择 `WINEHUA_VULKAN_BACKEND=direct`，移除 Venus
   precise-shadow/remote-memory-sync 策略，保留所选 DXVK 版本。
2. NAPI 从同一会话 policy 派生 `directNcpSession` 与
   `desktopVulkanCompositor`，解决当前 ArkTS 调用一直传 false 的断链。
3. `BuildSessionEnv()` 在历史 stable overlay 之后应用 LAB Guest 环境；最终
   固定会话 transport，避免程序环境把生产者与桌面消费者切成不同协议。
4. Direct 桌面不启用 Venus present mode。VirGL host 可以继续服务 GDI/GL
   路径，不能单凭 VirGL 进程仍存在就否定游戏已经直通。
5. 游戏图像通过 NativeBuffer/Vulkan 导入与 GPU 合成显示。仍有缓冲队列、
   fence、跨进程图像交接和 GPU 合成，不代表全部 IPC 或 GPU 拷贝都消失。
   日志里的 CPU 图像字节零值是该路径无 CPU 回读/上传的声明，不能当作
   覆盖任意代码路径的独立计量器。

## 真机帧率与口径

Direct 雪山采集时间 `17:03:16–17:03:46`：30 个约一秒的 root 新画面样本，
平均 **11.31 FPS**，最小 10.00，最大 12.00。前后截图显示游戏实景，HUD
已经显示非零值。证据 `direct-rootfix-snow-summary.json`、
`direct-rootfix-snow-fps-window.txt` 和前后 PNG。

普通 Venus 雪山稳态采集 `17:12:20–17:12:50`：两条 120 帧窗口的
`[GL-PERF]` 分别为 **7.74、7.81 FPS**，`failed_swaps=0`，无画面 CPU 上传。
证据 `venus-snow-valid-fps-window.txt` 和前后 PNG。

Direct 主菜单约 28–32 FPS，片头出现更高值；这些均不计入雪山性能结论。
最初 `venus-snow` 和 `bound-snow` 的采集开始时仍为主菜单，其 summary
已标记无效，不能作为游戏对照。

这是同包、同存档、同画质的顺序诊断，**不是严格重复 A/B**：镜头、输入、
资源热态和采样窗口口径不同，不能据此发布精确百分比。温度已保存；Direct
有效窗口的 shell_front 约 43.3°C，Venus 约 43.3–43.7°C，未做固定温控。
结果支持路由改善，但尚未证明所有场景或所有游戏都会同幅受益。

## CPU 与二进制证据

此前 Venus 的 30 秒雪山采样 76,688 samples、lost=0：
`vkr_device_memory_prepare_shadow_upload` 全 object-table buffer 扫描占
17.81% cycles；renderer ring 线程 VirGL DSO 合计 28.77%，Guest 队列有
`vn_ring_wait_seqno` 等待。已有 `.text` 匹配证据 `profile-identity.json`。

Direct 的 `16:43:29–16:43:59` 实景采样 70,404 samples、lost=0：
`ntdll.so` 19.37%，系统驱动 `vulkan.hvgr_v200.so` 16.61%，
`libentry.so` 2.14%；VirGL/Venus 未进入 >1% DSO 列表。
证据 `perf-direct.data`、`direct-perf-report.txt`、`direct-hotspots-final.txt`。
采样占比变化不能直接换算成 FPS。

当前候选与首版 Direct HAP 内的 `ntdll.so` SHA256 同为
`47a6a4919be2c28ee42e8682550ed4bfe7314e0b244f5a3c720cc22aa335169a`。
包内被 strip 的库与本地符号库整个 `.text`（445,104 bytes）一致，SHA256
`88666245dfda76cc0aa9036c588b29d4aaf479749e1fe725039c10d44a697245`。
`direct-ntdll-identity.json` 与 `verify_direct_ntdll.py` 可复核。
`0x68490/0x6848c` 对应 `send_request()` 的 `write()` 请求发送，
`0x6b840` 对应 `read_reply_data()`。这支持继续调查 Wine server 请求，
尚未确定具体哪一种请求是游戏瓶颈。

当前两个有效 30 秒窗口的累计进程 CPU ticks：Direct 游戏 8247，
VirGL host 0，wineserver 1593；Venus 游戏 7135，VirGL host 3317，
wineserver 1521。保留原始 ticks，不假设未核验的 CLK_TCK 换算绝对秒数，
也不把工作移入游戏进程后的占比直接解释为 FEX 回退。

## 保留 Venus 的扫描实验

新增 `isolate-shadow-bound-buffers`，只选已有 renderer 的
`VKR_WINEHUA_BOUND_BUFFER_LIST=1`，保留 inline upload、coverage sort、
generation serialize、batch flush 与原 per-buffer 上传规则，不启用诊断或
覆盖式整内存上传。绑定、销毁、内存释放链表维护已有生产实现。
设备 Host 参数已核对该 selector 与 flag 真实进入 VirGL child。

本实验 `17:26:22–17:26:52` 的雪山 GL-PERF 为 8.26、8.39 FPS，
failed_swaps=0，前后截图有实景；镜头与其余两轮不同，不能发布精确收益。
另采 20 秒 CPU profile（55,092 samples、lost=0），
`vkr_device_memory_prepare_shadow_upload` 仍占 16.24% cycles，说明该实验
并未消除整个资源同步函数的成本，原“全表扫描”归因还需继续细分到指令和分支。
证据 `perf-bound.data`、`bound-route-hiperf-report.txt` 和
`bound-route-host-proof.txt`。包内 VirGL/Venus 库 SHA256 与原设备证据完全
相同，对应符号映射可信。本实验维持默认关闭；长期正确性未完成充分验证，
不能提升为默认。

## 已完成验证及后续方向

### 窗口模式补测

用户追问窗口变小是否更快。本次保存游戏 Graphics 注册表的完整备份
`C:\vp-gumu10-evidence\graphics-before-window.reg`，只改 Fullscreen=0 和
窗口尺寸为 800×600，画质、AA、VSync、Direct 路由保持。新 DXVK 日志确认
交换链为 **800×600**，不再是全屏时的 1200×800。

`17:48:32–17:49:02` 的 30 个 Direct 一秒实景样本：平均 **15.64 FPS**，
最小 14.92，最大 16.88。证据 `direct-window-snow-summary.json`、前后 PNG、
`direct-window-ROTTR_d3d11.log`、`window-mode-settings.txt`。
Direct 全屏此前为平均 11.31 FPS。这说明本例窗口模式值得保留测试，
但两轮间隔较长、镜头/热态不同，不能把差值当作严格的因果收益百分比。
当前留在 **Direct + 800×600 窗口** 游戏现场，未改应用全局默认；原显示
设置可从上述 scoped 注册表备份恢复，另已取回主机备份。

窗口模式的可能收益来自实际缓冲缩小，不能泛化为“显示窗口小就快”。
交换链像素从 960,000 降到 480,000，内部 render target 的总工作是否同比
减少尚需 GPU 计时。桌面 Vulkan 合成输出仍是大屏尺寸。
可继续研究将游戏图像尺寸与桌面全屏几何解耦，由 GPU 放大低分辨率图像；
不应通过伪造 Vulkan format/MSAA 支持或删除同步来获得表面上的提升。

`make test-direct-session test-displayed-fps test-direct-viewport test-dll-overrides`
通过，Direct viewport 87 项检查零失败；Native/ArkTS/PackageHap 编译成功，
运行时闭包检查通过。Hvigor 原本的本地签名材料解密配置失败，使用匹配证书
手工签名完成并成功覆盖安装，不把 SignHap 的失败描述为整个构建成功。
未运行完整聚合测试；方向 TypeScript 测试受原容器依赖配置限制。

接下来应分别测 Wine server 请求类型/频率、请求等待时间、原生驱动提交时间
与 GPU 执行时间，再选择减少冗余查询或改善同步的改动。OHOS 的
`futex_waitv` 有 SIGSYS 限制，不能直接用启用 fsync 代替证据。

仍出现 format 13（R16G16B16A16_SNORM）、800×600、samples=1 的纹理创建
失败。旧 Venus probe 同样失败，说明并非此次 Direct 独有；其重复频率、
与帧耗时的关联及通用格式能力报告需要单独验证。不能直接换成 FLOAT 格式
或删除 usage bit 来掩盖错误，因为会改变着色器/资源语义。

未验证手机 Direct、32 位游戏、Steam、多游戏切换、长期挂起恢复和全场景
缺图；本次可确认的是 ROTTR 实景 Direct 路由、FPS 显示和可恢复的路由切换。

## 18 点后补采：低分辨率仍约 11 FPS 的瓶颈判断

用户在走路时仍观察到约 11 FPS，并指出此前 1280×800、开启 AA/双缓冲时
也接近这个数值。此前窗口模式站立时的 15.64 FPS 不能代表移动表现，
更不能作为窗口模式本身提升性能的严格证据。全屏低分辨率渲染后放大是
正常方案；本例交换链缩小也没有证明全部主要 render target 的工作同比减少。

本轮保持当前 Direct + 800×600 窗口，未重启、未安装新包、未改变画质、
未发送游戏输入。前后截图均为雪山实景，角色姿态变化。未录制完整输入轨迹，
不能声称整段都有连续走路，也未做新的固定镜头分辨率 A/B。

### 新采样与线程占用

`18:03:29–18:03:56` 的 27 个 root 新图像样本平均 **10.55 FPS**，
范围 **9.93–11.00**。同期采游戏、wineserver、应用、VirGL 的 25 秒
hiperf offcpu 记录：72,407 samples，lost=0。随后 `18:04:53` 开始补采
20 秒普通 CPU cycles，58,161 samples，lost=0；两份原始数据已取回。

线程占用通过前后 `/proc/task/stat` 差值除以每逻辑 CPU 的
`/proc/stat` 平均 tick 增量计算，100% 表示占用一个逻辑核，不假设 CLK_TCK。
快照覆盖约 27 秒，包含 profiler 的启动/结束开销：

| 对象 | 一个逻辑核的占用比例 |
| --- | ---: |
| 游戏所有线程合计 | 296.23% |
| wineserver | 53.86% |
| 应用所有线程合计 | 16.17% |
| VirGL | 0.00% |
| 游戏最忙线程（35712，名称 ROTTR.exe） | 56.63% |
| 游戏 Flush 线程 | 45.30% |
| dxvk-cs | 31.41% |
| 游戏主线程 | 29.81% |
| dxvk-submit | 5.14% |

没有看到线程在整个窗口中持续占满单核。这排除了“已经测得某单核持续
100%”的说法，**不排除 CPU 临界路径、线程串行依赖、等待、调度或 GPU
瓶颈**。整机总利用率低也不等于没有 CPU 侧瓶颈。

普通 CPU cycles 采样的可解析 DSO：wineserver 13.41%，ntdll 13.12%，
系统 Vulkan 驱动 10.69%，libentry 1.79%。部分 JIT/内核等地址未完整解析，
不能把 ROTTR.exe 的可见比例当作全部 FEX/游戏开销。
**驱动 10.69% 是 CPU cycles 占比，不是 GPU 利用率或 GPU 帧耗时。**

wineserver 热点是 `send_reply`、`read_request`、`send_thread_wakeup`；
ntdll 的 `send_request` / `read_reply_data` 仍出现。offcpu 栈为
Flush 的 `NtResetEvent`、主线程的 `NtReleaseSemaphore` 和 server 的
`req_release_semaphore` / `req_select` 提供候选线索。offcpu report 的
累计时间跨多个线程，优化过的 FP 栈也有局限，不能直接把百分比换算成
某帧的阻塞毫秒数。

四个符号库 ntdll、wineserver、winevulkan、win32u 的 `.text` 均已与
当前安装候选 HAP 的对应库逐字节核对一致，详见
`direct-bottleneck-symbol-identity.json`。

当前 Wine 的 OHOS `fsync_check_support()` 明确返回 0，设备未见
`/dev/ntsync`。这与热点中的 server 同步往返一致。不能通过强开
WINEFSYNC 解决：已有移植限制为沙箱拒绝 `futex_waitv`。后续应先测
具体 event/semaphore 请求次数与等待时长，再评估 OHOS 支持的同步
快速路径，完整保留 WaitAny/WaitAll、APC、取消、跨进程及句柄语义。

### 持续纹理创建失败：新的可复核线索

两次 bundle-aware 取回同一游戏日志，相隔 **71.24 秒**，后一个文件
完整以前一个为前缀。`Cannot create texture` 从 21,046 增至 21,897，
即新增 **851 次，11.95 次/秒**。同一时间范围的 71 个 Direct FPS 样本
平均 **11.87 FPS**。重试频率与出帧频率很接近，支持调查每帧资源创建失败
的循环，但还没有调用次数与 frame ID 的直接关联，不能认定耗时占比。

所有失败签名为 DXGI format 13（R16G16B16A16_SNORM）、800×600×1、
samples=1、日志 Usage=28。源代码这里打印的是 **十六进制 D3D BindFlags**，
即 **0x28 = SHADER_RESOURCE | RENDER_TARGET**，不是 Vulkan image usage。
单采样说明关闭 AA 后该失败仍存在。当前已有的可选 RGBA8 SNORM RT 实验
仅适用于另一种格式，不覆盖此 RGBA16 SNORM 失败。

应核查该格式的真实 image format / attachment / view 能力、游戏的
CheckFormatSupport 与 CreateTexture2D 请求，并测失败返回和重复建资源
在 CPU 临界路径上的成本。若需要格式仿真，必须处理 SNORM 的归一化、
精度、写入及视图语义；不能直接改 FLOAT 或假报支持。
本轮没有改格式映射或关掉报错来掩盖问题。

### 实质优化顺序与 935 的判断边界

1. 对齐帧编号，分别测游戏/DXVK CPU 准备、提交、Wine 同步、Direct
   acquire/fence/present 和 GPU timestamp，寻找约 91 ms 帧中的临界路径。
   查询结果异步读取，不为了计时新增每帧 WaitIdle。
2. 将纹理失败重试作为单独兼容性问题修复并做相同场景 A/B；以同一帧的
   耗时变化验证收益，不能仅凭错误数量预报 FPS。
3. 为已确认高频的 event/semaphore 操作评估同步快速路径；减少 server
   往返和串行等待。保持 FEX 和既有 x87 默认，不笼统改转译器。
4. GPU 若确实满载，再优化渲染目标/后处理/屏幕合成开销与驱动提交。
   Direct 已避开 Venus 命令转发，但游戏和上层 DXVK CPU 工作、NativeBuffer
   队列、fence、GPU 合成都仍存在。

本轮系统 GPU 频率/利用率接口不可读，RenderService 的 `gles` 只提供
Maleoon 910 身份；没有得到硬件 GPU busy 或实际执行时间。
DXVK 1.10 的 `gpuload` HUD 源码是依据 finish queue idle 时间估计，
不能单凭该指标替代硬件 GPU 时间。本轮没有为了开启 HUD 重启游戏。

因此目前可以确认 CPU 同步存在优化候选，**不能确认这是纯 CPU 瓶颈，
也不能承诺换 Maleoon 935 后翻倍或达到 30 FPS**。GPU 执行若占临界路径
的大头，935 才可能显著受益；若被 Wine 同步、FEX/DXVK CPU 或资源重试
限制，单独提高 GPU 算力收益受限。另一台设备的 CPU、驱动、散热也会同时
变化，需用相同存档和镜头对照。11 FPS 约 91 ms/帧，30 FPS 需约
33 ms/帧，必须消除主要临界路径成本，不能用微小缩放优化保证可玩。

新证据均在上述外部目录：`direct-bottleneck-walk-*`、
`direct-bottleneck-cpu-*`、`direct-bottleneck-wait-stacks.txt`、
`direct-bottleneck-error-rate.json`、两份 ROTTR 日志及符号身份 JSON。
本轮仅新增调查脚本、原始采样和本报告，没有修改运行时代码。
