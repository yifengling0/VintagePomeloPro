# VirGL / GLES 的 UBO 与条件渲染补齐

2026-10-09，`feature/main_proton`，基线 HEAD `f47090663537b1e70ad1ebe2f76bd2c34cfbb6af`。
平板 Maleoon 910 / Vulkan 1.2.53；本轮改动尚未提交。

已补齐两项真实扩展能力：12 个 uniform blocks 和 `GL_NV_conditional_render`。
最终候选真机 PE32 / PE64 各 45 项，共 90 项检查通过。
实际 desktop GL 仍为 **2.1**；没有伪报 3.3 / 4.6，也没有切换到软件 renderer。
本轮验证的是小型功能和一款游戏回归，尚非 CTS / Piglit 全量合规结果。

## UBO 的真实原因与修复

宿主各 stage 的物理 UBO 上限是 12。Mesa 的普通 uniform 使用逻辑 slot 0；
VirGL 自己还需要内部 `VirglBlock`，所以原路径只余 11 个用户 blocks。
上游提交 `dd301caf7e05ec9c09634fb7872067542aad89b7` 将内部 uniforms 改成该 block，
直接把 cap 加一会超出真实硬件限制。

`patches/virglrenderer/0002-gles-minimum-ubo-plain-sysvals.patch` 在 GLES 最小
UBO 配置下改用普通 GPU uniforms 上传 winsys Y、clip planes、alpha ref、stipple 和 draw ID，
释放内部 block，并按真实 stage / combined blocks / binding points 限制 guest。
本机 capset 为 13（包含逻辑 slot 0），guest 实际得到 12。
为这些辅助值预留默认 uniform components：非 fragment 48、fragment 176；
默认 uniforms 可用上限会略减，这是明确的资源取舍。
分离 pipeline、cookie 更新和 compute locations 也已处理。

PE32 / PE64 探针在 vertex / fragment 各同时使用 12 个 blocks，验证
初始上传、BufferSubData、range 重绑、普通 uniform 更新、数组动态索引；各 9 项通过。
旧 HAP 同版探针无法使用 UBO，最终候选可以。
`WINEHUA_VIRGL_UBO_FIX=0` 是 renderer 宿主环境开关；目前未接应用 host 参数，
不能认为只在 Wine Want 中设置它就能关闭。旧 / 新 HAP 是本轮有效对照。
Wine 仍拒绝 WGL ES profile；本探针成功部分使用 GLSL 1.20 + ARB UBO，
没有把 ES3 请求失败记成 GLES3 成功。

## GLES 条件渲染兼容路径

`patches/virglrenderer/0003-gles-query-conditional-render.patch` 仅在 GLES 有
occlusion-query boolean 且缺少 native conditional rendering 时启用。
每次开始条件区间读取一个查询结果，draw / clear / blit 依据该布尔值执行或跳过。
GPU 继续绘制；没有整帧像素回读、CPU 合成或纹理复制替代绘制。

- WAIT：读取已结束查询的结果，可能等待 GPU 完成。
- NO_WAIT：先查 available，未就绪时允许绘制，不读取会阻塞的 RESULT。
- BY_REGION：可保守使用整个查询结果，符合允许的实现方式。
- End：恢复正常绘制；明确忽略条件的 clear / blit、普通 copy 和 compute 不被抑制。
- Native conditional-render 路径保持原实现。暂未宣告 inverted 扩展；反转决策只做了主机测试。

这项是兼容性补齐，**不宣称提升 FPS**；WAIT 的标量查询同步可能增加延迟。
主机测试直接编译生产 resolver / render-condition 生命周期；
真机各 26 项检查覆盖四个模式、正负查询、绘制/清屏/blit、End 恢复和无条件 copy。
未就绪 NO_WAIT 的精确行为用可控主机 mock 验证；真机 NO_WAIT 检查使用已完成查询。

## 最终包和验证

未签名候选：`opengl-conditional-clean-unsigned-20261009.hap`。
SHA256：`99e2038c766d85c65cdc8f894db9cd7d426219e67c2b5ec5262f321617bd7a13`。
renderer SHA256：`165790221a950dda5b032e0c07873302a0780756686885d7dcea4e09fe032030`。
已覆盖安装同内容签名包，bundle 版本 `1.4.5.29 / 1004038`。
本次为替换 renderer 的候选打包；不是本轮全产品干净构建，也没有生成上架 APP。

| 检查 | PE32 | PE64 |
| --- | --- | --- |
| 条件渲染像素 / GL error | 26/26 | 26/26 |
| UBO 真实绘制 | 9/9 | 9/9 |
| D3D9 颜色、linear/sRGB、ping-pong 和混合 | 10/10 | 10/10 |

生产主机测试 `virgl_conditional_render_test.py`、`virgl_ubo_sysval_test.py` 通过。
相邻 socket 事务测试复现旧包交错并验证新实现，scene/input 10 场景通过。
三项 renderer patch 在 HEAD 干净源码上重放两次，与生产三个文件逐字节一致。
`build_native.sh` 已调用重放入口；顶层和 renderer 的 diff-check 通过。

Chaos 由原启动器进入实际剧情，黑色背景和文字正常。
30 秒新图像呈现平均 **59.03 FPS**，范围
57.79–59.96，30 个样本。
日志 `glSources=1`、`glDraws` 持续增长、`releaseErrors=0`，
本段 `uiUploadBytes` 增量为 0，确认仍消费 GL GPU 队列。
本段是静态剧情及提示动画，不是此前首张剧情卡的严格 A/B，
不能据此声称 UBO / 条件渲染带来额外性能增益，或所有游戏都到 60 FPS。
本轮未重新跑 DXVK Direct 游戏内性能对照。

## 继续提升 desktop GL 的具体方向

`opengl-version-gates-final-20261009.json` 依据最终 legacy 扩展快照与
Mesa `src/mesa/main/version.c` 条件生成；只统计扩展门槛，资源限制和 GLSL 另算。

| 版本增量 | 当前缺失的扩展门槛 | 下一项工作 |
| --- | --- | --- |
| 3.0 | framebuffer sRGB | 实现同 backing 的 linear / sRGB destination 切换 |
| 3.1 | 本级扩展已齐，但依赖 3.0 | 先完成 sRGB 与各项资源/GLSL验证 |
| 3.2 | depth clamp | 实现正确深度钳制，包含裁剪与插值语义 |
| 3.3 | dual-source blending | 查真实 Vulkan / GLES feature，或验证 GPU blend emulation |

`pipe_caps.dest_surface_srgb_control` 要求 draw 和 blit 目标能在 linear / sRGB 间切换。
Mesa `_mesa_update_renderbuffer_surface()` 会根据 `GL_FRAMEBUFFER_SRGB` 选择 surface format。
只给 fragment 输出套 gamma 不够：还要正确线性化混合目的颜色、MSAA、clear、blit、import 和 readback。
不能直接打开 cap，否则前轮修复的重复编码泛白可能回来。

后续先做 GPU backing 的两种颜色视图实验：若宿主 texture-view 可用，验证
linear / sRGB alias；若不可用，研究 NativeBuffer + 两种 EGLImage 或 Vulkan mutable-format views。
必须先验证真实共享存储、读写可见性、mipmap/layer、混合与同步，才能接资源分配路径。
该实验本轮未实施，不能把既有 `egl_image_srgb_import=1` 当作任意纹理 view 已可用。
Zink 在本机仍有 transform-feedback / dualSrcBlend / logicOp 等缺口和纹理小测失败，暂不设默认。

920 / Vulkan 1.3 设备当前未在线。接入后先按同一探针实测可选 features，
1.3 版本号不会自动带来 dualSrcBlend、transform feedback、multiViewport 或更大纹理限制。
Mesa 26.x 分支仍是未来独立合并候选，本轮没有合并或覆盖子模块脏状态。

## Steam 调查边界

始终使用 `Z:\games\Steam-legacy\Steam\steam.exe`，没有切最新版。
已观察 ANGLE 表面创建 `0x80070057 / EGL_BAD_ALLOC`，以及 WineD3D 对照的着色器问题。
本轮 UBO 修复关闭的旧候选也出现同类 surface failure；
该旧候选不是用户最早的稳定包，所以不构成完整的历史回归排除。
用户明确要求弹窗先不处理，本轮随即转回 OpenGL；没有修改生产 CEF GPU policy，
没有用自动 software fallback 的 FPS 证明 Steam GPU 成功。
Steam 主程序稳定性不能写成已经完全通过。

## 可复核入口

```sh
python3 host_tests/virgl_ubo_sysval_test.py
python3 host_tests/virgl_conditional_render_test.py
python3 host_tests/virgl_socket_transaction_test.py --verify-old
python3 host_tests/gpu_scene_input_test.py
bash scripts/apply_virglrenderer_ohos_patches.sh thirdparty/virglrenderer
```

设备探针源：`scripts/probes/wgl_ubo_draw_probe.c`、
`scripts/probes/wgl_conditional_render_probe.c`、
`scripts/probes/d3d9_srgb_render_target_probe.c`。
本目录证据含实际输出、包身份、重放摘要及 SHA256 清单；HAP、账号截图和完整 stderr 只留本地。
