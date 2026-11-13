# 62ca61f 本地复核（2026-10-06）

本页记录较早的静态评审阶段；诊断出口与构建身份随后已修复，且已完成真机探针和有限游戏 OFF/ON 对照。最新结果和限制见 [真机评价与验证链路修复](fex-performance-measurement-followup-20261006.md)，下面的“尚未真机验证”描述只对应当时阶段。

复核提交：`62ca61faa87351307daa18b59c7d6fd26cbc0db6`。权威分支已快进到该提交；本地 Makefile、FEX 构建脚本及此前 Wine／控制器／Mesa 改动均保留，构建脚本冲突已解决，未暂存或推送生产代码。

结论：exact-store 的方向与已采到的 RichMan8 热点一致，转换条件保守，值得继续做同包对照。本轮没有发现所检查 F32/F64 精确正常值转换的位运算错误。但新增命中率输出有确定的 CRT 生命周期问题，`make fex` 也没有接入诊断构建身份变化。两项需要处理后再依赖诊断结果或重复构建的包身份。没有新的游戏 FPS 结论。

## 1. [P2] 析构函数中的命中率汇总无法按设计输出

位置：`scripts/patches/fex-wow64-exact-store-diagnostics.patch:37`。

新增 `ExactStoreStatsReporterInstance` 把唯一汇总放在静态对象析构函数里。实际 Windows FEX 并不使用通常的 MinGW CRT：WOW64 以 `-nostdlib -nostartfiles -nodefaultlibs` 链接自带 `CommonWindowsRuntime`。

三个相关事实：

- 固定版本 `Source/Windows/Common/CRT/Misc.cpp:76` 的 `atexit(void (*)(void))` 直接返回 0，没有保存回调。
- `Source/Windows/Common/CRT/CRT.cpp:25` 的 `DllMainCRTStartup` 只禁用线程通知后返回；没有 `DLL_PROCESS_DETACH` 的析构调用，也没有运行 `__DTOR_LIST__`。
- `Source/Windows/WOW64/Module.cpp:597` 的 `BTCpuProcessTerm` 为空。

用本地官方 llvm-mingw 20260826 / Clang 23.1.0 对同样的静态析构模式编译 ARM64 汇编，初始化函数明确执行 `b atexit`。结合实际空实现，即使游戏正常结束，新增 reporter 也没有可执行的退出注册路径。这是编译器和 CRT 的证据，尚未运行新的诊断 DLL 真机复现。

影响：计数代码可以编译、命中率也可能在内存中增长，但按文档等待进程退出并不能得到那条汇总；没有日志不能被解释为命中率为零。强制结束整个应用会进一步绕过正常 Wine 退出流程。

建议：为诊断显式接入实际执行的 WOW64 终止回调，或提供运行中的有界汇总出口；验证汇总满足 `attempt = hit + fallback`。先用 PE32 probe 正常结束测试，再验证结束宿主会话时的数据保存。不要为了一个诊断 reporter 扩展整个 CRT 析构机制。

证据：`reporter-registration.cpp`、`reporter-registration-arm64.s`、`review-validation.json`。

## 2. [P2] 构建 stamp 会绕过诊断身份检查

位置：提交版 `Makefile` 的 `fex` stamp 配方，以及 `scripts/build_fex.sh` 新增的 `prepare_exact_store_cache()`。

脚本本身比较 `FEX_EXACTSTORE_DIAGNOSTICS` 和 CMake cache，能够在直接调用脚本时刷新缓存。但外层 `make fex` 的 up-to-date 分支只检查 stamp、ARM64EC DLL、脚本时间和 FEX 源码时间，完全不检查诊断开关或新 overlay 的时间。因此当前干净构建之后执行 `FEX_EXACTSTORE_DIAGNOSTICS=1 make fex`，可能直接返回 up to date；诊断构建再切回普通 make 也有同样问题。

本轮从提交本身导出 Makefile，在隔离的 stamp fixture 中设置现存 DLL、干净 CMake flags 和当前 stamp，再请求诊断构建；实际输出 `[fex] up to date`。没有调用任何编译脚本。复现未使用本地 dirty Makefile。

影响：以为切换了干净／诊断 DLL，实际仍复用上一个构建；或者修改 exact-store overlay 后不触发构建。文档直接运行 `scripts/build_fex.sh` 的做法可以绕过这个问题，但正常工程构建入口仍不可靠。

建议：让 stamp 比较并记录构建身份（至少包含诊断模式和 overlay 哈希），并把两个新 patch 加入构建依赖。不要只依赖脚本内的 CMake cache 检查，因为脚本可能根本没有被执行。当前评审阶段可继续使用独立 BUILD_DIR 和直接脚本调用。

证据：`verify.py`、`stamp-diagnostics-repro.txt`、`review-validation.json`。

## 3. 已核对的实现边界

- F32：要求 ext80 显式整数位为 1、低 40 位为零，指数在 `0x3f81..0x407e`，只接受可精确表示的正常 float。
- F64：低 11 位为零，指数在 `0x3c01..0x43fe`，只接受可精确表示的正常 double。
- 不符合条件的零、denormal、非规范值、NaN/Inf、需要舍入或超出目标正常范围的数仍走旧 helper；快路使用整数构造结果，不依赖 ARM FPCR。
- 分支前后保存／恢复 NZCV，保留 fallback 指针 TMP4 和慢路源 VTMP1。正常精确转换不会生成新的 invalid exception，保留已有 sticky IE 与当前 helper 行为一致。
- 运行开关只接受精确字符串 `1`，在 Context 创建时读取；切换后必须新建整个 Wine/FEX 进程树。
- `_WIN32` 和 `FEX_WOW64_EXACT_STORE` 编译守卫只接入 WOW64 构建。ARM64EC 干净产物不包含开关或统计标记。
- 新的白名单允许通过 GameHook 传递两个开关；产品 EntryAbility 自动化入口也把解析环境传入启动进程。静态检查不是新的真机参数传递证明。

与之前已部署的 `fex-exact-clean-20261006` 候选相比，去掉编译守卫和注释后，两条 F32/F64 快路的指令生成代码一致（`algorithm-comparison.json`）。因此这一提交主要完成工程接入和验证组织，不是另一个更快的转换算法。

## 4. 本轮验证与构建身份

`make test-fex-exact-store` 通过：位转换 oracle、拒绝向量、patch 重放和编译隔离结构检查。`bash -n scripts/build_fex.sh` 通过。本地旧 0037 patch 以及新 patch 的空白 context 行会被普通 diff whitespace 检查报告；专项测试检查的是 patch 引入的源代码行，未为了消除报告破坏 unified diff。

干净构建使用提交本身导出的构建脚本、独立 FEX 源码副本和新的 `build-fex-62ca61f-review-20261006/clean`，没有带入本地未提交的五个 FEX 补丁。FEX 基线为 `86ff33bbe299cd8959a6610198c169b67ec419db`，应用提交脚本的七个既有兼容补丁和 exact-store overlay。

第一次 configure 因该 checkout 的 FEX 依赖子模块为空而失败；随后从已有产品源码复制依赖，每个依赖先核对 HEAD 与 FEX 固定 gitlink 一致并确认没有 tracked diff（`dependency-identities.json`）。只移除了失败 configure 留下的 cache，未复用旧编译对象。第二次构建成功。

编译设置为 `RelWithDebInfo`、`-O2 -g -DNDEBUG`、`ENABLE_LTO=False`、`FEX_EXACTSTORE_DIAGNOSTICS=0`，与这次提交脚本一致。平板之前候选使用 ThinLTO，不能与本次新产物跨包直接做性能比较。

| 产物 | 架构 | SHA256 |
| --- | --- | --- |
| libarm64ecfex.dll | COFF-ARM64EC | `2984d5b07a0adcd82885ea2774942221842f4f7fb745498d004a4fb88302be12` |
| libwow64fex.dll | COFF-ARM64 | `3642ba026af7e0325a710fcb551397397d13d73a61feefad93a26934a04ba742` |
| fex_exact_store_sequence_probe.exe | COFF-i386 / PE32 | `ce57165d67f3d85187ac81a991dbb59eaeabca83304a1cfafd5c8542993dd593` |

以上为本地未剥离构建产物身份，不应与文档中另一环境的哈希强行相等。隔离源码没有携带 FEX `.git`，CMake 自动 Git 版本串发现了上层产品仓库；源代码身份以固定 FEX commit、overlay 和依赖清单为准。

完整记录：`build-clean.log`、`build-clean-retry.log`、`build-identity.json`。没有制作／安装新 HAP，没有执行这次 PE32 probe，也没有新的游戏 OFF/ON 对照。新诊断 DLL和关闭编译宏的 DLL在本轮未重建。

## 5. 与 Steam 泛白的关系

本轮此前已在同一个已安装包内做过渲染后端对照，两个会话均显式设置 `FEX_EXACTSTORE=0`：

- Wine builtin D3D11 → WineD3D / OpenGL / VirGL：内容可见，深色背景和封面泛白。
- DXVK legacy 1.10.3 → Vulkan：主库页深色背景恢复正常；fault maps 确认 CEF 进程加载 DXVK `arm64x/d3d11.dll`、`dxgi.dll`。

同一标题背景的固定像素位置 `(900,45)` 与 `(900,95)`：DXVK 为 RGB `(23,29,37)`，WineD3D 为 `(85,95,107)`。对前者施加一次 sRGB 编码得到 `(84.706,94.718,106.336)`，误差小于 1 个字节。这个证据强烈支持一次额外的 sRGB 编码，但还没有证明发生在 WineD3D framebuffer、VirGL presenter 或宿主 NativeImage 合成中的哪一步；不是已定位某一行代码。

这次 exact-store 提交没有修改 D3D11、Mesa、VirGL presenter 或合成 shader，不能作为 Steam 泛白的修复。应继续检查 framebuffer 的实际 color encoding、`GL_FRAMEBUFFER_SRGB`、source texture decode 和 NativeWindow 颜色空间；不要给全局 shader 添加经验 gamma 系数。

颜色证据仍在 Windows `workspace_temp/steam-white-20261006/`：`wined3d-ready.png`、`dxvk-library-final.png`、`dxvk-maps-save.txt`、`color-comparison.json`。两份截图不是相同库内容，因此只比较固定标题背景；不用于帧率结论。DXVK 测试中的后续一次触摸进入了游戏启动状态，该轮会话已结束，后续截图未用作同内容颜色对照。

## 6. 下一步顺序

1. 先解决 stats 输出与 make 构建身份，避免用无日志或错误包身份判断快路是否生效。
2. 使用同一个干净 DLL／HAP，完整退出进程树后做交错 OFF/ON，多轮匹配温度、游戏入口、人物和缓存状态；先跑 sticky IE 与已有 PC/RC/边界 probe。
3. 再换独立诊断包，仅统计实际游戏 F32/F64 attempt/hit/fallback。换回干净包后验证哈希，才测 FPS。
4. 保留人物缺块和长片头单独验收，不能由 exact-store 单点改善推断全部 VirGL 32 位游戏问题已解决。
5. Steam 泛白按上面的 sRGB 证据继续追踪，不与 x87 性能实验混合修改。
