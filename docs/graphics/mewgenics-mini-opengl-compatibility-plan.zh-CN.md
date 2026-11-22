# Mini 上 MewGenics 的 OpenGL 适配推进计划

首期目标是在 Mini（MLR-AL10、Maleoon 910）上，用旧柚 Pro 的 ARM64 Wine/FEX 路线运行现有 x86_64 MewGenics，复现主菜单动画并完成一场战斗。推进顺序是 **softpipe 建立出图对照 → VirGL 核对并补齐 3.2 Core 能力 → 有实测依据时接入逐进程 Zink**。游戏验收和驱动验收各有判据。OpenGL 4.4 可在 3.2 Core 的结果明确后另行评估。

## 起点与已有证据

| 证据 | 对实施的约束 |
| --- | --- |
| Mini 上已部署的 EXE SHA-256 为 `4127cd6a792ae528bca6f65a8873dd61789591937d87656c2b586a5e30eb77ea`。它通过 SDL 请求 3.2 Core 和 accelerated visual。 | 固定同一游戏及资源，探针要覆盖 SDL/WGL 使用的像素格式、临时上下文和 3.2 Core 请求。 |
| 旧柚 Pro 2.0.6 的 VirGL 实测已启动游戏进程，随后弹出 `Could not create GL context`。Mini 的独立 VirGL 探针能创建 3.1 Core，显式请求 3.2 Core 时失败。同期扩展表里，Mesa 的八个 3.2 扩展门槛中仅 `GL_ARB_depth_clamp` 缺失，GLSL 1.50 仍须单独核对。 | 先查上下文和 capset，再查绘制。宿主 GLES 非空扩展、Guest GL 版本和实际像素结果都要记录。 |
| 两次选择现有 Zink 后端的冷启动都在 Wine 桌面就绪阶段超时。日志显示 Venus 路由生效，Explorer 提前退出，游戏进程尚未创建。旧样本另测得 Zink 暴露 GL 2.1，系统 Vulkan 直连样本也未创建出 3.2 Core。 | 逐进程 Zink 能验证桌面与游戏配置的耦合。3.2 能力仍需在当前运行时上重新验证。 |
| 历史 softpipe 探针请求 3.2 Core 时得到 Mesa 3.3 Core，并通过基础像素检查。 | 先复测当前库及游戏的 accelerated visual。真实游戏绘制、帧时间和热稳定性仍需实测。 |

本地证据位于工作区 `artifacts/mewgenics-mini/`，包括 `deployment.json`、`game-gl-context-request.json`、`startup-evidence.log` 及两条失败路线的截图。对照探针位于 `workspace_temp/bc-upload-integration/mesa-wgl-candidate.tsv` 和 `workspace_temp/gumu10-dx11-20261008/opengl-softpipe-wgl-capabilities64-20261009.tsv`。Zink 的历史 Vulkan 扩展与资源限制可查仓库内的逐级缺口记录。 [Mini Zink 能力记录](zink-opengl-version-research-20261116.zh-CN.md)

当前 main 的核对起点为 `e90752e2`，Mesa pin 为 `557fa3e781ea6e78fcff3ee759c25f60c3055064`，virglrenderer pin 为 `bb33e52aa03199926f662e50ff22bd00d19d7e0a`。实施时以实际 checkout、设备包与打包产物清单为准。已有并行修改的构建脚本要从当前任务 diff 中隔离。

## 第一步　复测同包基线和软件渲染对照

1. 复用设备里的 `Z:\games\MewGenics\Mewgenics.exe` 及原有资源，校验 EXE 和 `resources.gpak`。为对照固定同一前缀、游戏工作目录、分辨率、显示模式、存档或新档路线。记录 Mini 系统版本、包版本及哈希、实际 Wine/Mesa/virglrenderer 产物身份。
2. 复用 x64 `wgl_capability_probe`，先用产品 VirGL 确认 3.1/3.2 Core 上下文结果。记录 `ChoosePixelFormat`、临时 legacy context、`wglMakeCurrent`、`wglCreateContextAttribsARB` 和最终 vendor、renderer、GL、GLSL。探针如需补游戏的像素格式请求，仅扩展探针的测试组合，加入 accelerated visual，保留普通组合做对照。 [WGL 能力探针](../../scripts/probes/wgl_capability_probe.c) · [SDL 的 WGL 上下文创建](https://github.com/libsdl-org/SDL/blob/release-3.2.0/src/video/windows/SDL_windowsopengl.c#L743-L819)
3. 在已有诊断入口使用经过校验的 ARM64 Mesa 库。由测试进程覆盖 `GALLIUM_DRIVER=softpipe`、`LIBGL_ALWAYS_SOFTWARE=1` 和 `MESA_LOADER_DRIVER_OVERRIDE=swrast`，DRI 搜索路径沿用该库实际安装位置。检查最终 renderer 为 `softpipe`，桌面先走能稳定就绪的配置。若当前签名包没有相应诊断入口，优先复用测试包或已有测试入口的最小参数扩展。产品游戏库入口用于最终验收。 [Mesa 驱动构建选项](../../scripts/build_ohos_guest_gfx.sh) · [GraphicsBroker 的 EGL/DRI 环境](../../entry/src/main/cpp/graphics/graphics_broker.cpp)
4. 依次跑 3.2 Core 像素测试、真游戏菜单、设置页、战斗。保存首次失败环节、错误、两张不同时间的画面或画面统计，以及实际 renderer。菜单帧率供记录，玩法性能以固定战斗场景的帧时间为准。

softpipe 出图即可作为像素与功能对照。若停在像素格式，先确定 SDL 的 accelerated visual 选择、临时上下文或 Core 创建中的第一个失败点。若进入绘制后才失败，记录首个缺失的 GL 入口或异常帧。VirGL 的能力审计可以同时推进。

## 第二步　VirGL 补齐实际 3.2 Core 能力

### 从宿主能力到 Guest 版本逐段核对

| 层 | 核对位置与结果 |
| --- | --- |
| 宿主 EGL/GLES | 在真正执行 VirGL 的 Host EGL context 中记录 GLES 版本、renderer 和 `GL_EXT_depth_clamp`、`GL_NV_depth_clamp` 等扩展。用像素探针验证禁用和启用 depth clamp 的区别。Vulkan physdev 的 `depthClamp` 只能用于评估 Vulkan 路线。 |
| virglrenderer | 核对 `feat_depth_clamp` 是否命中、宿主是否实际设置对应 GL 状态、vtest capset 是否带出该位。检查 GLSL feature level 的来源和 geometry 路径的实现情况。 [Host 能力表](https://github.com/winehua/virglrenderer/blob/bb33e52aa03199926f662e50ff22bd00d19d7e0a/src/vrend/vrend_renderer.c#L265-L281) |
| Guest Mesa | 对照 Guest VirGL 收到的 capset、Gallium caps、Mesa 版本判定。用 `WINEHUA_GUEST_CAPS_LOG=1` 获取实际 capset 信息，并记录 GLSL 1.50 及全部 3.2 扩展门槛。 [Guest caps 入口](https://github.com/winehua/mesa-ohos/blob/557fa3e781ea6e78fcff3ee759c25f60c3055064/src/gallium/drivers/virgl/virgl_screen.c#L1101-L1118) · [Mesa 版本条件](https://github.com/winehua/mesa-ohos/blob/557fa3e781ea6e78fcff3ee759c25f60c3055064/src/mesa/main/version.c#L291-L309) |

### 根据差异选择修改层

1. **宿主能正确执行，capset 或 Guest 丢掉了能力**。修复丢失的那一层，并用同一探针证明改前失败、改后正确。能由现有 capset 表达的功能沿用该位的协议语义。实际生效的 GLSL 等级由后端能力计算，并通过 GLSL 150 编译、链接及绘制验证。
2. **宿主能报告扩展但行为异常**。先用独立像素用例确认具体语义。depth clamp 至少测跨 near/far 平面的三角形、夹紧后与深度测试的交互，以及关闭状态下的裁剪。两个状态要有可区分的结果。若只把版本抬高，或在 fragment shader 中单独改深度而丢失光栅化前的语义，按失败处理。
3. **宿主缺功能且无已验证的等价实现**。本轮停止扩大 VirGL 能力承诺，转入第三步的 Zink 能力复测。已有 VirGL 3.1 可继续作为对照。若后续能实现完整语义，再独立提交并复验。

通过条件是同一签名候选包在无 Mesa 版本 override 时创建 3.2 Core，并通过 GLSL 150 和补齐特性的探针。随后用 MewGenics 和另一款使用这些能力的 OpenGL 程序检查连续出图。 [virglrenderer 补丁注册](../../scripts/apply_virglrenderer_ohos_patches.sh) · [Mesa 补丁注册](../../scripts/apply_mesa_ohos_patches.sh) · [图形产物合同](../../scripts/verify_graphics_contract.py)

## 第三步　按实际能力决定逐进程 Zink

1. 首先用当前 ARM64 运行时在同设备上比对原生 Vulkan 和 Venus。记录物理设备查询值及 Zink 的实际 WGL 结果。重点检查 Transform Feedback 是否仍缺、Guest 实际看到哪些格式和限制、GL 3.0/3.1 的累计条件是否成立。原生有而 Venus 缺的项目查协议透传，原生和 Venus 都缺的项目按功能实现成本评估。 [Vulkan 能力探针](../../scripts/probes/vulkan_opengl_capability_probe.c) · [历史缺口](zink-opengl-version-research-20261116.zh-CN.md)
2. **第一组只验证游戏进程**。先让桌面用稳定配置达到 ready，再给一个 WGL 探针进程注入当前 Zink/Mesa/Venus 环境，显式用 `WINEHUA_ZINK_PRESENT=0` 验证 GL drawable/readback。这组记录 Host 是否真的建立了所需的 Venus 上下文，并记录 Guest Vulkan ICD、GL renderer 和最终帧来源。若 Host 现有模式无法同时服务两种客户端，先用现有测试隔离方式验证，再评估独立 Host context 或端点的成本。 [#78 的游戏环境](https://github.com/lan207/WineHua/blob/56c6a472140921d874f4e7a4993377ceacba6a19/entry/src/main/cpp/wine/mewgenics_graphics.h#L61-L98) · [#78 的 GL 上屏路径](https://github.com/lan207/WineHua/blob/56c6a472140921d874f4e7a4993377ceacba6a19/docs/pr/compatibility-history-rollup.md#L76-L88)
3. **接入产品入口时分别传递三项选择**。桌面会话的图形配置决定 Explorer 和宿主会话的启动。游戏的 GL 驱动配置由启动请求传入 `RunWineExe`，沿 `BuildSessionEnv` 下发。呈现路径由实际帧源及 Host 协议能力决定。重点改动入口是 `AppSessionService` 在游戏启动前的 Host 重启判定、`WineEngineService` 调用 native 时传入的后端、以及 `wine_launch.cpp` 对 Explorer 的环境组装。也复核 `RunWineProgram` 的调用者，避免游戏库与自动化路径出现不同选择。 [游戏启动路由](../../entry/src/main/ets/service/AppSessionService.ets) · [引擎选择](../../entry/src/main/ets/service/WineEngineService.ets) · [桌面启动](../../entry/src/main/cpp/wine/wine_launch.cpp) · [程序启动](../../entry/src/main/cpp/wine/wine_exe.cpp) · [环境组装](../../entry/src/main/cpp/wine/env_profiles.cpp)
4. 边界验证覆盖继承全局设置、显式游戏覆盖、回退、切换前后另一个游戏的实际后端、运行时缺库，以及连续启动退出。宿主路由确需重启时，走既有会话生命周期并验证回退。自动选择后端以设备能力与有效运行库为依据，沿用已有游戏配置入口。

逐进程探针无法创建 3.2 Core 时，保留第一项能力差异和实际错误，先估算功能缺口。GL 回读路线通过真游戏后，再另起对照验证 GPU 呈现。

## 第四步　按首个失败点吸收 #78 的补丁

| 观察 | 最小变更与复验 |
| --- | --- |
| Core context 创建成功，`glVertexAttribDivisor` 缺失，`GL_ARB_instanced_arrays` 存在 | 在 Wine WGL 层对该名字做扩展门控的 ARB 别名。测核心入口存在、允许别名及扩展缺失三种路径，并验证两个实例像素。 [#78 Wine 别名补丁](https://github.com/lan207/WineHua/blob/56c6a472140921d874f4e7a4993377ceacba6a19/scripts/patches/wine-instanced-arrays-alias.patch) |
| Zink 已到 shader 或 pipeline 创建，Maleoon 拒绝带未使用 `ShaderLayer` 的模块 | 捕获失败模块并保留能触发问题的最小样本，只处理未使用声明，同时验证真实 Layer 输出仍正确。 [#78 Mesa 补丁](https://github.com/lan207/WineHua/blob/56c6a472140921d874f4e7a4993377ceacba6a19/scripts/patches/mesa-maleoon-unused-layer.patch) |
| 绘制开始后黑屏、首帧冻结或间歇花屏 | 先核实候选包实装了 Mesa `0026/0027` 和 virglrenderer `0015`，分别对照非零偏移映射、图像 staging、显式读回，再比较 FBO 像素和连续上屏。重放参考探针时优先覆盖实例步进、间接绘制和 resize。 [旧柚已有传输修复](zink-shadow-transfer-fix-20261116.zh-CN.md) · [#78 绘制探针](https://github.com/lan207/WineHua/blob/56c6a472140921d874f4e7a4993377ceacba6a19/smoke/programs/graphics/wgl_core_transfer.c) |

## 合入与交付判据

- **游戏通过**。建议两次冷启动、一次退出重进，均显示主菜单动画，设置页能打开，固定测试存档或新档路线完成一场战斗。记录同分辨率、同设置、同场景的帧时间 P50/P95 及明显画面异常。首轮据此确定可玩目标，菜单瞬时帧率单独记录。
- **通用后端通过**。同一候选保存实际 renderer、GL/GLSL、3.2 Core 上下文、GLSL 150、补齐特性的像素结果和连续上屏结果。再跑一个独立 OpenGL 程序、内建 Wine 桌面、已有 VirGL 3.1 探针和一个 DXVK 程序，检查跨后端启动与回退。
- **提交顺序**。先做最小诊断和能力复现。Host/Guest 能力或映射修复做独立提交，逐进程启动策略和游戏行为补丁各自保留回退点。构建走现有增量目录及图形合同，核对 HAP 中实际库的架构、版本与哈希。
- **证据与空间**。复用设备游戏和同一临时目录 `workspace_temp/mewgenics-mini/`。把小型版本、包哈希、路由和首个失败点记到 `artifacts/mewgenics-mini/`。依工作区规则登记测试包目录，保留当前及上一份签名候选，限制单日志 32 MiB，实施结束运行磁盘维护。
