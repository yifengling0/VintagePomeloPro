# 透明纹理与汉化启动器：通用修复和实机验证（2026-10-10）

> 后续更新：剩余发白和彩色底块已定位为宿主桌面合成误用 framebuffer alpha，并由用户确认修复。见 [普通 Wine GL 窗口透明度修复](native-gl-window-opacity-20261010.md)。下面保留此前调查阶段的证据与限制。

本轮找到并修复两个独立、可重复的代码问题：WineD3D legacy OpenGL alpha-test 状态遗漏；OHOS Broker 子进程请求丢失已经解析的完整 EXE 路径。均已编译、安装平板并验证。不能把这些结果扩大为全部游戏缺层、颜色和性能问题已解决。

基于 `feature/main_proton` / `f47090663537b1e70ad1ebe2f76bd2c34cfbb6af` 的现有 dirty 工作树。保留 FEX、OpenGL 3.1、Vulkan Direct 自动选择、共享低地址映射和显示节拍优化，没有新增整帧回读／复制。本轮没有 commit/push。

## 1. 透明像素被写入：缺失 GL_ALPHA_TEST enable/disable

32／64 位 D3D9 探针在修复前均为 16/18；纯采样及 SRCALPHA/INVSRCALPHA 混合正常，只有 alpha-test 应丢弃的透明像素写入了目标。MANAGED 和 dynamic DEFAULT 两种 texture pool 都复现。实际读取 `00336699`，期望背景 `ff204060`，见 baseline TSV。

`dlls/wined3d/glsl_shader.c:glsl_fragment_pipe_alpha_test_func()` 只设置 glAlphaFunc，没有启用 GL_ALPHA_TEST。设备 VirGL context 为 `3.1 Mesa 25.0.1`，WineD3D 将其识别为 legacy context，GLSL 依赖原生 alpha test。上游 `14f39c299c26f163a1d281075c3f2d26beb1d2e3`（2025-02-04，Move alpha test func to wined3d_extra_ps_args）移除了原函数的 enable/disable。此处是实际状态遗漏，不需要降低 GL 版本、禁用 UBO 或恢复 CPU 复制。

新增 `0053-wined3d-restore-legacy-alpha-test-state.patch`：比较函数 ALWAYS 时禁用 alpha test（也覆盖 D3DRS_ALPHATESTENABLE=0），其他有效比较启用，再设置比较和引用值。调用仍受 legacy context 限制，不对 core context 调用已移除的 API。

| 实机验证 | 结果 |
| --- | --- |
| 32／64 位原始 A8R8G8B8，MANAGED/DEFAULT | 各 18/18，修复前各 16/18 |
| 32 位 X8R8G8B8 目标 | 18/18，仅验证 RGB，X 通道不作 alpha 断言 |
| 32 位 A4R4G4B4 纹理、独立 alpha 混合及 8 种比较 | 72/72 |
| 64 位 A8R8G8B8 纹理、独立 alpha 混合及 8 种比较 | 72/72 |

短期 D3D9 trace 确认 TH18 实际使用 A4R4G4B4（format 0x1a）和 A8R8G8B8（0x15），启用独立 alpha 混合。因此新增覆盖来源于实际调用。`scripts/probes/d3d9_texture_alpha_probe.c` 是实际 GPU 像素测试；它的测试读回不属于产品渲染路径。

游戏标题背景和 Logo 白色矩形明显恢复，前后截图已保存。演示中部分精灵／光效仍显得过亮，尚未取得同帧正确基准，不能宣布颜色完全正确。关闭 sRGB emulation 的会话参数对照也有类似外观，但未证明参数传入 renderer host，不能据此排除 sRGB。后续应核对具体 draw 的输入纹理、diffuse/texture-stage、fog 和 RGB/alpha 输出；不应先修改全局 gamma 或复制规则。

《灰色的果实》能进入菜单和开篇背景，正文装饰透明层可见；截图没有明显矩形底色。但未到用户此前异常的同一人物／场景，无法证明其全部缺图或卡顿已修复。

## 2. 中文启动器失败：解析后的 EXE 路径没有交给 Broker

失败记录：launcher 使用 `CreateProcessW(L"th18.exe", ..., CREATE_SUSPENDED)`；Wine 已解析完整 `Z:\games\[th18] 东方虹龙洞\th18.exe`，但 build_broker_argv 的字符串匹配失败（consumed=0）便返回原始 `th18.exe`。Broker 无法在 Windows cwd 中查找这个裸文件名，因而无法读取固定基址 PE 的 header、也没有触发已有 fresh-process 策略。子进程最终 `STATUS_CONFLICTING_ADDRESSES / c0000018`，不是渲染崩溃。

设备 Win32 只读 PE 探针确认：th18.exe ImageBase=0x00400000、SizeOfImage=0x00174000、Characteristics=0x0103、DllCharacteristics=0x8100，relocation directory RVA/size 都为 0；确实需要固定基址。th18c.exe 本身可重定位。手动 `WINEHUA_DIRECT_NCP=0` 的独立对照成功进入汉化游戏，并记录 mode=start。这是原生子进程创建模式，不是 Vulkan Direct 开关。

新增 `0054-ntdll-broker-preserve-resolved-image-path.patch`：完整路径无法匹配命令行首段时仍使用 ImagePathName 作 Broker 的 EXE 参数，只替换 argv[0] 并保留其余参数。Windows 程序的原始 ProcessParameters 继续由 wineserver 传递，未改游戏的 GetCommandLine 语义。

新包不带 NCP=0 也实际命中：

```text
broker executable path repaired to Z:\games\[th18] 东方虹龙洞\th18.exe
[PROC-IMAGE] preferred PE32 range occupied; fresh native process
[PROC-SPAWN] ... mode=start
```

汉化版进入演示画面，未再出现该子进程的 c0000018。保留其他可重定位程序快速创建；没有全局改为慢创建或按游戏名称特判。

## 3. 构建、配套资产和性能边界

WineD3D 两架构使用独立 `build-alpha-test-20261010`；ntdll 使用新独立 `build-child-path-20261010`。ntdll 隔离源码以已验证 alpha source 为基准，仅更新 process.c。WineD3D source 对比此前 V8，仅 glsl_shader.c 不同；权威 dirty buffer.c 没被覆盖。

最终候选 `alpha-child-path-fixed-20261010-signed.hap` 覆盖安装成功。HAP runtime closure 为 aarch64，通过；未做完整发行 APP 构建。

| 文件 | SHA256 |
| --- | --- |
| unsigned HAP | aa4335a8a081de3826067c5581e3693b344dd1237929d20de8eb48a6c02304d8 |
| signed HAP | f019ddb89346e05149d962afd5a021ac377e2aacd79f40dc9e1927a0241308d6 |
| ntdll.so | 9c2ef6bce25bdcf702ed60064bc782b41dcb89d3d2e1be4d7a7a4ad6ac1ed4d3 |
| i386 wined3d.dll | 996a5eeef62b1e10afcaeca711a41dd9c3787a8b0059f14c96412d2fbdc9077d |
| aarch64 wined3d.dll | b9db32b8db07b83c607373cdd0d5665b6086f159e9d010622bc45b90723584ec |

相对 V8，仅 ntdll.so 与 wine-data.zip／marker／manifest 四项资产变化；zip 内只改两架构 WineD3D，其他 member 和 native renderer/Mesa/FEX/Direct 内容逐字节相同。已将这四项同步到产品 entry，记录 before/after hash 并备份。设备 shell 无权限读 native ELF，没有声称直接核验设备库哈希。

最终候选 32／64 位 D3D11 Direct probe 都自动走 direct=1、glSources=0，120 帧完成，buffer/texture 读写 bad=0、RESULT PASS。probe FPS 不等于游戏 FPS，本轮没有匹配温度、场景的游戏性能 A/B，不能宣称帧率提升或所有游戏零降速。

## 4. 回归、限制和后续

生产函数主机回归通过：alpha test 启停、NEVER/ALWAYS、引用变化、blit 后重新应用以及 core context 保护；完整路径／裸 EXE／替代 argv0／中文／空命令行／Steam 引号和参数。两个旧代码失败均由测试重新复现。

相邻 PE image policy、VirGL UBO、conditional render、socket serialization、GPU front present 和 pbuffer storage 测试通过。51 个注册 Wine patch 在 pinned source 上连续重放两遍、49 个文件字节一致。旧 `wine_proton_stability_test.py` 聚合 executable fixture 的 Wayland stub 缺少 minimized／xdg_surface／restore 定义，setUpClass 编译失败；此聚合没有通过，patch replay 已独立验证。未把旧 stub 阻塞写成产品运行失败。

剩余问题按证据处理：核对 TH18 演示光效的正确颜色基准；复现《灰色的果实》之前缺图场景；对真实资源更新继续区分必要下载、NOOVERWRITE/CS 同步和不必要 location 转换。保留此前减少复制的默认路径，不能用整帧回读或全局 sysmem 实验掩盖状态错误。本轮未复测 PAL2 战斗缺层或所有游戏。

本地大日志／HAP 留在 workspace_temp，仓库内只保存小型相关证据和 SHA256SUMS。使用的 USB 设备标识不进入小型证据包；没有启动 Steam、上传或提交代码。
