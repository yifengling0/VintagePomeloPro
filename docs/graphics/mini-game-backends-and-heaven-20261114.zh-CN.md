# Mini 每游戏后端与 Heaven 准入结果

2026-11-14。此轮完成后端设置与兼容性准入检查，没有获得 DXVK D3D9 / Zink 的有效 Heaven FPS，也没有达到或证明 30% 性能提升。

## 已完成的设置

- 系统默认后端保留 DXVK 1.10、设备支持时的 DXVK 2.6、VirGL；游戏可选择跟随系统或单独覆盖。
- 旧配置默认跟随系统，非法值回退；持久化保存用户选择，启动时解析继承并保留已有 GPU 能力回退。
- 每次普通启动重置上一游戏的覆盖；应用启动请求串行，避免跨异步初始化/重启污染后端选择。
- 游戏图形 API 参数预设与翻译后端分开。现有 D3D9 仍使用 WineD3D/VirGL/GLES；DXVK D3D9 与 Zink 未通过准入，不作为可用产品选项。
- 切换宿主后端可能重启 Wine 并结束其他 Windows 程序，设置页已明确说明。

## Heaven 准入结果

| 路径 | 实际结果 | 帧率资格 |
| --- | --- | --- |
| 原 VirGL/GLES | 最近已验证 Low 720p D3D9 完整基线 16.3 FPS；本轮未重跑完整基准 | 历史基线，不能冒充新成绩 |
| DXVK 1.10.3 D3D9 | Heaven `Direct3D9 error: not available / CreateDevice(): failed` | 未进入场景，无有效 FPS |
| Zink + WineD3D D3D9 | 两次有界检查均在 `vkCreateInstance` 返回 `VK_ERROR_INCOMPATIBLE_DRIVER` | 未进入场景，无有效 FPS |

DXVK9 请求 Direct 实验，但 Heaven 是 PE32，低地址区占用后走 fresh native child。现有 fresh-process 策略因未验证 native driver/producer IPC，将 Direct 回退到 Venus；日志和独立 PE32 Vulkan 探针实际均为 `Virtio-GPU Venus (Maleoon 910)`，不能将它记作已验证 Direct。

PE32 探针成功创建 Vulkan instance、枚举设备；DXVK9 所请求的 core features 中 `textureCompressionBC=0`、`multiViewport=0`，按必需集合创建 device 返回 -8 (`VK_ERROR_FEATURE_NOT_PRESENT`)。D3D11 的已有 BC 兼容层尚未接入 D3D9 的资源创建、上传、锁定与复制，不能只删除 feature 请求。单 viewport 路径也须审计 meta/SPIR-V 的真实使用再决定是否移除依赖。

Zink 单独复用增量依赖构建 Mesa 25.0.1，含 zink/virgl/softpipe。显式设置系统 Vulkan loader 后仍失败；范围受限的清理 Venus ICD 环境变量改动没有证明解决此问题。下一步应在实际 fresh child 中确认 loader/driver 路径、环境、instance version/API 请求与驱动加载结果，而非继续盲跑 Heaven。

## 图标错乱事故与修复

本轮前两个实验 HAP 只更新 `ets/modules.abc`，未同时更新 `resources.index` 和 `module.json`。两次编译的数字资源 ID 重排，造成图标引用错位：资源索引同为 7306 bytes 但 SHA 不同，Ability 的 `startWindowIconId` 从 16777282 变为 16777255。非 rawfile 的 UI 素材内容没有变化。

发现后先覆盖恢复已验证基线，未卸载或清空数据。修正为 ABC、资源索引和模块信息来自同一次编译，核对全部非 rawfile UI 资源集合与内容。新增持久校验函数 `scripts/check_candidate_hap.py::verify_ui_payload`，可通过 `--compiled-ui-hap` 在候选检查中启用；本任务打包器签名前后校验同一编译来源。回归测试覆盖原来的新 ABC/旧 index、旧 module、缺失/错误图片。

修正实验包 v3 的应用库、系统设置、Wine 引擎设置和 Heaven 设置页面已逐页截图查看；每游戏 VirGL 覆盖重新打开仍保留，验证后恢复跟随系统。附带的 70 秒 VirGL smoke 在 40 秒显示加载画面，70 秒应用已退到后台、native/audio 日志仍持续，未验证到实际场景；不能宣称这次 smoke 完整通过，也不能仅凭该截图认定崩溃。游戏目录的 `bin/log.html` 仍是旧 18:49 D3D11 日志，不作为本轮运行证据。

最终修正版保留基线全部 native/图形库、raw runtime，仅替换来自同一编译的三项 UI 文件；不把尚未准入的 Zink 实验库放入日常候选。它属于可回退的测试 overlay，不是完整 release parity 声明。

- 基线：`VPP-2.0.2-mini-virgl-egl-signed.hap`，627114490 bytes，SHA256 `d172b43c5f1bb024e03ae8dd7dff7424f3041353e9e829c80c14eef8348277e2`。
- 修正版：`VPP-2.0.2-mini-backends-ui-signed.hap`，627132471 bytes，SHA256 `64d2580bfb123b2cc96f8f3d33a50d9896b2c9c9ddcf4cf5f15743e5baf139db`。
- 均 2.0.2/2000020，release/internaltesting，debug=false、profileable=false；签名和 profile/证书匹配已校验。

## 验证与证据

- ArkTS release assembleHap 成功；host 设置/后端回归、启动串行及失败恢复、既有 Godot 图形启动回归通过。
- C++ Zink loader opt-in 范围测试通过；该测试不等于 Zink 驱动真机通过。
- HAP 检查器 6 项回归通过，真实签名候选与编译 UI 内容一致；其余 payload 对基线逐 SHA 一致。
- 仅操作 Mini；所有设备动作持固定任务 mini.lock。无 push、CI 或其他设备操作。
- 固定证据：`F:/VintagePomelo-Workspace/workspace_temp/mini-virgl-performance/backend-evaluation/`；包收据 `backend-artifact.json`、`signing-backends.json`，界面验证 `backend-ui-validation.json`。
- `vulkan9-capabilities.json` 是独立 PE32 probe；对应 DXVK9/Zink 截图、有限日志及请求参数均保留。启动失败不记作 0 FPS。

## 后续顺序

1. DXVK9：先补齐/验证 D3D9 BC 资源兼容与 viewport 依赖，再验证纹理和呈现正确性，最后才比较 FPS。
2. Zink：先完成实际 fresh-child Vulkan loader/instance 诊断；失败的初始化不支持性能推论。
3. 原 VirGL：继续围绕 busy-query socket mutex 往返、guest CS 等待和 draw/FBO 内部成本做时间线关联。没有连续 GPU busy/时间戳证据，仍不能判断 GPU 已满或排除 GPU。
4. 只有候选正确性通过且有明确关键路径收益，才做同设置完整对照。原 16.3 FPS 基线的 30% 目标仍是 21.19 FPS。
