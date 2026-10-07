# RichMan8：新场景卡顿的 JIT 编译和辅助代码定位

日期：2026-10-06。基线 feature/main_proton / 284026ce，FEX pin86ff33bbe2。本轮没有fetch、提交或推送。

## 当前结论

新人物／新场景卡顿和人物缺块仍未解决。真机已确认此前CRT/JIT-map候选实际加载，同进程采样抓到了18–21FPS人物段和11.46FPS场景段；交换缓冲最大133.786ms、BufferSubData最大76.086ms。已记录纹理上传／shader／WGL锁耗时不足以单独解释全部停顿。详细原始采样与边界见 [fex-jit-crt-diagnostics-20261006.md](fex-jit-crt-diagnostics-20261006.md)。

本轮新增定位能力，不降低TSO、SMC、x87精度、画质，不改代码缓存或呈现政策。没有确认帧率改善。

## 生产补丁

`scripts/patches/fex-windows-jit-cost.patch` 通过build_fex.sh独立、可重复地应用，并加入Makefile依赖与时间检查。

- Windows仅显式 `FEX_JITPERF=1` 开启编译计时；每线程拥有独立状态。关闭时不分配统计对象、不读诊断时钟、不输出诊断；在编译入口／阶段有空指针分支。Linux默认不启用该环境开关。
- 记录编译入口、GenerateIR／优化、后端编译的次数／总微秒／最大微秒，以及L3命中、新编译、并发已编译、失败、single-step、cache clear／rollover。每个编译线程最多约1秒一个聚合窗口，在最外层编译返回且释放代码失效锁后输出。
- frontend/backend是entry的子阶段，entry还包括PreCompile、代码锁／查询、符号登记、SMC注册和插入缓存。嵌套编译的包容耗时可重叠；这些是墙钟跨度，不是CPU占比。没有timer和退出时强制flush，最后不足1秒的尾部可能不输出。
- 现有GlobalJITNaming／BlockJITNaming下新增非重叠的dispatcher控制、128bit除法、F64辅助和每个fallback ABI区间。静态名字指出参数／结果类型，不等同于具体被调用函数；调用栈落在ABI辅助上也不证明采样leaf正在软件浮点计算。默认命名仍关闭。
- 不把新的helper地址反套到旧进程，仍要求同进程map，并保留无栈、未知地址及冲突。

手动BAT `vp-richman8-jit-cost-20261006.bat` 只对该次启动设 `FEX_JITPERF=1`、`FEX_SILENTLOG=0`、library/global命名和原Unix OpenGL计时；保持low-map off与原游戏cwd，检查两条已核对的游戏路径。已发送至设备并回读逐字节核对，工具没有执行。诊断自身会增加开销，因此这轮用于定位，收益必须另用关闭诊断、同条件包验证。

## 验证与产物

`make test-fex-jit-cost` 提取生产header验证关闭时零时钟读取、三个阶段独立计数、早于阈值不输出、嵌套编译不重置窗口、释放锁后报告、错误／并发／单步分支记账、窗口重置、cache计数和时钟逆序。补丁正反重放两轮逐字节一致；相邻JIT符号、CRT文件接口和synthetic-return回归通过。主机测试没有执行ARM JIT，不是性能微基准。

独立 `build-fex-jit-cost-20261006` 完成AArch64 PE和ARM64EC RelWithDebInfo / O2 / NDEBUG构建。首次编译暴露两项带小写x的ABI枚举未转换为命名对，已修正生成器；staged source与最终完整生产补丁的反向dry-run一致，随后同一独立目录构建通过。没有复用旧Wine对象，也没有重建或改动Wine。

候选以当前已安装的 `ebc72140...` 包为基底，只更新WOW64 DLL及配套payload manifest／runtime.version；ARM64EC、Wine、Mesa、宿主、手柄及ArkTS内容逐字节继承。非discardable PE加载节与新编译DLL一致、导入依赖不变、官方verify-app通过；签名没有改变既有archive内容。

| 产物 | SHA256 |
|---|---|
| 编译WOW64 DLL | `c779ea659af27e0daedfaed5009cf439900a734b0c4473e09b56722bffa44c25` |
| 包内WOW64 DLL | `e7d852ff7abf0d2821ca94d734eeb0aaa8295c14c9fa7709b28dbec6133fb166` |
| 未签名HAP | `30c078e63072709d7e0ef1be543ac1b9f5cb26c289cf677028bc0450f9e03fab` |
| 签名HAP | `e0c1caa769468a3003c25eb2595ebb4114ee58493ba016f6fa2228119a53e338` |

版本仍1.4.5-proton.26-alpha / 1004035。新marker：`content=839a53023981c7f0;wine-valve=cd547f7a0e;fex=86ff33bbe2;arch=arm64-v8a:aarch64`。包在 `F:\VintagePomelo-Workspace\workspace_temp\fex-jit-cost-20261006\`；14:47:39 已成功覆盖安装。用户手动打开后，14:51:38 真机 marker 和 WOW64 DLL SHA256 `e7d852ff...` 均与候选一致；性能和 JIT 计时验收尚未完成。

## 设备与后续修复方向

此前 Windows 只枚举到 MTP 接口、HDC Offline；用户切换 USB 调试后，14:46:25 恢复 HDC Connected 和 HDC Interface。未重置共享 HDC 服务。当前连接正常，不再把旧 Offline 状态作为阻塞。

同 payload 的 `wowbox64.dll` 已确认存在，设备 SHA256 为 `adece8ddc39f4a87fee038ebcd554a39a3b7f2149ca0fd6dddc87c8ed7a94e11`；尚未执行或证明加载 Box 对照，默认保持 FEX。先取得有效 CPU probe 的 complete／precision／checksum，再以同一个 PE32 二进制测试。若纯计算／x87慢则定位合法执行快路径；若计算接近但API边界慢则查32／64桥接；若新场景编译窗口很长或反复清缓存则查JIT缓存失效；若编译开销很低而BufferSubData／swap长等待则对齐VirGL资源完成和呈现节奏。

14:52 用户手动运行原 CPU BAT，测试进程 OS PID34075 在主函数前退出，没有 TSV。控制台虽显示 Exit code: 0，不能作为成功证据。第一异常 PC `0x6ffc44aa40` 对应候选 PE 的 `ForcedAssert`，LR `0x6ffc453980` 对应 `Config::JSON::LoadJSonConfig` 的无效 JSON 断言；之后异常处理在 `BTCpuResetToConsistentStateImpl` 读取未就绪的线程状态，反复访问 `0x42`，最终触及栈保护页。尚未确认具体配置路径或读取失败原因，不能把它称为游戏性能根因或本轮新引入回归。

配置隔离测试也已失败：OS PID37195 与原测试同类断言，独立 Config.json 实读 15 字节、内容有效，不能归因于用户配置格式错误。Windows 文件读取和早期异常保护候选已构建、签名，部署与后续验收详见 [fex-windows-file-loading-followup-20261006.md](fex-windows-file-loading-followup-20261006.md)。

缺块在截图52FPS时仍出现，须独立检查上传范围、stride、透明／裁剪／深度、动态缓冲有效范围和生命周期；提速不能当作画质修复。
