# main → Proton 迁移处置（2026-10-16）

适用：付费集成分支 `feature/account-cloud-membership-mvp`，基线 `09ca128c1fd738a32b2303c50c4f7bff82305c1a`；main `5383b7849190c563c0e79007fb9c33da64b326e4` 已为祖先。本次补齐语义遗漏，无需重复 merge。保持 ARM64 native Wine + FEX、API 23、guest graphics、多 producer、consumed-frame identity，版本保持 1.4.8 / 1004080。

| 项目 | 处置与必要性 | 验证入口 |
|---|---|---|
| MIG-01 | VirGL 0005 按 surface/view/storage 决定 sRGB；恢复首相关 FBO 兼容策略，按附件处理 manual encode，统一 draw/clear。保留新 Mesa shadow/import/raw-copy；单目标 float clear 保留 fast path | virgl_srgb_policy_test.py；完整 ARM64 release VirGL 编译；0001..0005 干净重放、重复 apply |
| MIG-02..05 | 保留像素重放、完整 fit/cache identity、直传失效、局部 GPU child/viewport 几何和直传资格。Direct 分离可见性与决定 canvas 的主内容，局部视频不能缩小父窗口 canvas；CPU planner/GPU scene 复用 fit 判定，输入映射一致 | compositor_migration_test.py；gpu_scene_input_test.py |
| MIG-06 | 每资源输入状态、Wayland 线程队列、焦点 epoch、销毁/arm 边界清理、真实 NAPI 返回；跨线程旧文本不可穿越焦点代次 | run_text_input_unit_tests.sh；run_text_input_focus_tests.sh |
| MIG-07/09 | 后台继续 drain，跳过 CPU snapshot/upload、输出 acquire/present；恢复时重画。renderer move 同步 atomic identity，先 erase 后 insert，创建前缓存暂停状态。后台归还 owned buffer 前提交 release-to-FOREIGN barrier 并等完成；未 owned 的原 acquire fence 直接归还 | renderer_lifecycle_test.py；ARM64 release/debug native 编译 |
| MIG-08 | exact session 绑定 PID 代次；多窗口、早到事件、window generation、防止复用窗口复活；只接受可证 ancestry。NCP 调用在 registry 锁外。stop 返回后端 accepted，实际退出由权威回调确认；ArkTS 不回退 recycled PID，reconcile 核对 native generation | test-process-lifecycle；run_app_session_unit_tests.cjs |
| MIG-10 | GL 失败返回有界未来 deadline，防止错误路径忙转；健康帧限速、geometry-only EGL reuse 保留 | gl_present_failure_test.py |
| MIG-11 | 不移入裸 DxvkImage 指针假共享句柄：缺少生命周期契约，当前默认路径无消费者。锁中明确 unsupported；未来需要真实共享协议设计 | graphics-stack.lock.yaml 决策记录 |
| MIG-12 | Wine 0064 仅 WINEHUA_MF_FALLBACK_NONSHARED 精确为 1，且 shared texture 创建失败时去 shared flags 重试；默认不改变行为 | mf_nonshared_retry_test.py；完整有效 Wine 重放；ARM64X+i386 MF 新编译及最终 HAP 身份检查 |
| MIG-13 | 更正旧审查：Valve resolver 默认 fallback 已是 MPEG4，无需迁回旧 prefer 补丁；移除失效 UI/env，保留旧设置数据可读 | 有效 mfplat/main.c；模型测试 |
| MIG-14 | schema 3 机器锁固定 pins、patch 顺序/内容、有效源码及 WHIP 9/11 契约；build key 纳入 overlay 内容。release strip 前校验输入配对 SHA，标准化后核对 allocated sections/program headers 不变，再更新清单；最终 HAP 对 MF、VirGL、native 输出和锁逐项核验 | graphics-contract-check；graphics-migration-check；migration-hap-check |
| 补查 locale | 托管子程序继承当前容器桌面语言；LANG/LC_ALL 显式覆盖仍最后写入，无重复设置 | wine_locale_test.py，指向有效 Wine |

复评中额外修正了三个边界：局部 Direct 视频不应决定全桌面尺寸；GPU fence 完成不等于已归还 FOREIGN ownership；停止请求 accepted 不等于进程已死亡。对应行为用例均已加入回归。

前台健康路径未新增 GPU readback、GPU 同步等待或全局 sRGB storage 回退。附件策略更新位于 FBO 状态/clear 路径。后台在等待既有帧完成、释放已 owned 的新 drain buffer 时才提交必要的 ownership release；前台正常提交结构保持。

五轮交替、同负载的 Host CPU 中位耗时（微秒）：直传取帧 65.7327 → 62.4480，合成 36.6321 → 36.6480，GPU scene 构造 0.4154 → 0.4118。未见明显 Host CPU 退化，不能据此推断设备 GPU 帧率。

验证已通过：compositor 49、真实进程生命周期 169、真实 Wayland IME 133、GL 故障/恢复 43、renderer 生命周期 698、xdg 真实销毁/断连 72 种排列、完整生产 GPU scene/input 20 场景、VirGL 实际 FBO/clear 40、MF 27 故障场景；模型/付费云/升级保护/Direct/Venus 契约；compositor、renderer 和 Wayland 生命周期的 ASan/UBSan。ARM64 release/debug native、完整 VirGL、ARM64X+i386 MF、release HAP 均编译成功。最终 HAP 的 runtime guard、closure、清单/锁/native/MF 身份及 stale MF/VirGL/lock 拒绝测试通过。

会话限制：沙箱隐藏 ancestry、启动器退出后重挂父进程时，无法证明归属便不猜测。停止只请求目标已登记进程，不扩大共享进程树。

设备验收仍待完成：Maleoon 920/9010 Rich8/PAL5 启动画面、菜单、场景颜色/透明 blend、帧率及前后台切换。缺少原生 sRGB write-control/view 能力时，混合真实 sRGB/raw UNORM 的 manual encode 不能完全等价替代线性 blend。Host 测试和编译不能证明这些设备项目通过；手机不测试 Direct。

未升版、未 commit/push。迁移初始候选未签名，后续真机候选使用 release 优化与本地 debug 签名，非上架签名。构建/门禁及性能证据见 workspace 的 artifacts/main-proton-migration-implementation-20261016/IMPLEMENTATION.md。


## 真机补查：基本功能、Steam 与星露谷

在 MLR-AL10 / Maleoon 910 / API26 完成有界验证：记事本启动、输入、后台恢复和关闭保存提示；DXVK 1.10.3 D3D11 cube 彩色画面，20 秒 2398 帧、S_OK；Steam 登录、库/详情/滚动、两次后台恢复可操作。该平板请求 DXVK 2.6 会回退 legacy，不能作为 2.6 验收。920/9010 颜色和 Direct 项目仍待目标设备。

真机发现并补修两处生命周期问题：xdg/wl resource 销毁监听避免 UAF，真实 Wayland 销毁/断连 72 种排列及 ASan/UBSan 通过；Steam 后台 drain 保留每层最后 acquired 图像以便恢复重画，替换/销毁继续退休，renderer 生命周期扩展至 698 checks 及 ASan/UBSan 通过。健康前台未新增 GPU readback 或同步等待。

Steam 的 stderr 系统日志镜像新增每 reader 每秒 32 条普通日志和独立 8 条错误配额，省略给摘要，WINEHUA_STDERR_HILOG=full 可恢复全量；完整文件先写入并处理短写/EINTR。提取真实 reader 的 20000 条 pipe 追踪、后续错误、完整文件、窗口复位与 full 模式回归通过，ARM64 release native/HAP 已编译安装。信号处理、图形同步不变；系统 MUSL-SIGCHAIN 日志不受该限流控制，实机交互提速尚未证明。

1.4.7 和当前候选均观察到秒级 Steam 操作迟滞，现有样本不能量化版本回退或修复收益。多进程、显式软件渲染和库低性能模式均作了实际操作试验，仍有迟滞，未改默认批处理。软件渲染和多进程的最终参数已从 Steam 自身日志核对；仅通过启动环境请求的软件模式那一组没有实际生效，不纳入模式比较。CPU 采样出现队列/提交线程 sched_yield 热点、CEF 高 CPU/内存压力及可恢复 SIGBUS/system sigchain 日志，具体调用归属仍需补证；未盲改全局同步或 TSO。

星露谷 1.6.15 / .NET6 的两个阻塞在 1.4.7 同样存在：ICU 符号缺失；FEX self-XOR 零值复用导致投影参数污染。隔离实验仅在三个 self-XOR 入口用 uncached zero，并单次启用 NLS，动态 x64、.NET warm 矩阵与 MonoGame 探针正确；游戏已加载 VPTest、移动并打开背包。更根本的 SSA/寄存器原因、通用 SSE/AVX 回归、正式构建和 ICU/NLS 策略尚未完成。实验 FEX 已恢复，当前候选不含该修复，不能称已产品化解决。

本地完整实测报告、候选包身份、有效与作废采样、截图和后续修复顺序见 artifacts/tablet-steam-stardew-20261016/REPORT.md。最后安装 migration-stderr-budget-debug-signed.hap，SHA256 2d91625bed781a69f7bd17764518558790bc76454755965b642b1e23ff635f05；与前一 retained-frame 候选相比仅 libwine_child.so 改变。原始日志及截图含账号信息，仅保留本地复核。


## 通用 CPU / ICU 集成复评（2026-10-16，后续候选）

本节更新上文“实验未集成”的历史状态。本轮已撤销按游戏名强制 NLS 的方案，生产代码不新增游戏白名单或 NLS/invariant override。FEX 三个 self-XOR 入口使用独立零值；完整 ICU 的 ARM64X/i386 组件和匹配数据进入标准构建与 prefix 升级。ICU 的编译器 memory libcalls 保留类型导入，以修复应用自带 x64 CRT 时的 ARM64EC 转接异常。构建检查同时验证复用 Makefile 的实际选项。

新增通用 SSE/AVX、动态 RX/RWX JIT、ICU 与 .NET6 探针。平板上 x64 SSE/AVX 和 x86 SSE 通过，x86 AVX 不可用而 SKIP；.NET6 默认环境的大小写、归一化、比较、日期/数字、中文与补充平面 Unicode 通过。最终包中的两份 FEX、六份 ICU 与语言数据共 9 个文件，已与安装后 prefix/runtime SHA 全部核对。Host 升级失败/重试、native override 和 10 项包损坏/缺编译策略拒绝通过。release native、ArkTS/HAP、runtime closure 与原图形身份门禁通过。

星露谷在最终候选 env=[] 下完成 VPTest 读档、室内移动、背包开关和出门切场景。本轮尚未保存并回读，不扩大为完整游戏验收。平板自动化明确使用 dxvk_legacy；不是 DXVK2.6、920/9010 颜色或 Direct 验收。

另有通用缺口尚未修复：889 个 ICU 文化名全部可正常 canonicalize/getLanguage，但 24 个被 Wine GetLocaleInfoEx(LOCALE_SNAME) 拒绝；本机 Windows 对相同 24 项全部成功。.NET6 Windows 在 ICU 初始化之后仍调用该 NLS API 校验，导致 GetCultures 整体抛出。main/Proton 的 locale.nls 当前相同，重新复制不能解决。探针保留该失败，后续需补齐 Wine 的 BCP47/自定义文化 API 与数据一致性，不过滤 ICU 枚举、不只伪造 LOCALE_SNAME 成功。

Steam 最终候选可进入完整库并切换详情，仍有秒级等待。临时 quiet 诊断试验与基线的准备/缓存不同，不能认定稳定提速，产品默认和原批处理未改。新增有效 10 秒 FP profile（20218 samples、lost=0）将热点对应到实际库的 vn_ring_wait_seqno/vn_relax；宿主栈进入 vkr_ring_thread/vkQueueSubmit/厂商驱动。后续需在相同状态关联输入、CEF 执行、提交和 present；未修改全局 GPU 同步、等待或 TSO。

当前本地 debug 签名候选 generic-runtime-crt-safe-debug-signed.hap，400258639 bytes，SHA256 1113d8316ee504441e3a08bbe9696ff9ce70ee1c959354aa00c88eaeac80033a；payload SHA256 6cc8436e833150b3a8d90df1d38fb47527524a3b89f997492c88229a63f47213。保持 1.4.8/1004080，未 commit/push；未完成上架验收。首个 generic-runtime-debug-signed.hap 含已发现的 CRT 调用问题，已替代。完整本地报告、身份及真实截图在 artifacts/runtime-perf-integration-20261016/REPORT.md，原始账号日志仅保留本地。
